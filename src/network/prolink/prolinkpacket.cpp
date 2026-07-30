#include "network/prolink/prolinkpacket.h"

#include <kaitai/kaitaistream.h>

#include <QHostAddress>
#include <QStringList>
#include <string>

#include "prolink_djl.h"
#include "util/assert.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkPacket");

/// A Kaitai `std::string` field holding raw bytes, as a QByteArray.
QByteArray toBytes(const std::string& value) {
    return QByteArray(value.data(), static_cast<int>(value.size()));
}

/// The four bytes of an IPv4 address, in the order they appear on the wire.
QHostAddress toAddress(const std::string& value) {
    if (value.size() != 4) {
        return QHostAddress();
    }
    const auto* octets = reinterpret_cast<const quint8*>(value.data());
    return QHostAddress((quint32(octets[0]) << 24) | (quint32(octets[1]) << 16) |
            (quint32(octets[2]) << 8) | quint32(octets[3]));
}
} // namespace

namespace mixxx {
namespace prolink {

QString ProLinkDevice::macString() const {
    QStringList octets;
    octets.reserve(mac.size());
    for (const char byte : mac) {
        octets << QStringLiteral("%1").arg(
                static_cast<quint8>(byte), 2, 16, QLatin1Char('0'));
    }
    return octets.join(QLatin1Char(':'));
}

namespace packet {

bool hasMagic(const QByteArray& datagram) {
    return datagram.size() >= kMinPacketLength &&
            datagram.startsWith(QByteArray::fromRawData(kMagic, kMagicLength));
}

bool parseAnnouncement(const QByteArray& datagram, ProLinkDevice* pDevice) {
    VERIFY_OR_DEBUG_ASSERT(pDevice) {
        return false;
    }
    if (!hasMagic(datagram)) {
        return false;
    }

    // Kaitai throws on anything it cannot make sense of, including a truncated
    // datagram. Catch everything: this runs on every packet the network offers
    // us, some of which will be from devices we have never seen, and one
    // malformed frame must not take discovery down.
    try {
        const std::string buffer(datagram.constData(), datagram.size());
        kaitai::kstream stream(buffer);
        prolink_djl_t parsed(&stream);

        // `device_name()` is transcoded through bytes_to_str, which the vendored
        // runtime compiles as a pass-through (KS_STR_ENCODING_NONE). That is the
        // identity function for this ASCII field, so it is safe here -- but it
        // is why UTF-16 fields elsewhere must never use Kaitai's `encoding:`.
        pDevice->nameRaw = toBytes(parsed.device_name_raw());
        pDevice->name = QString::fromLatin1(toBytes(parsed.device_name())).trimmed();
        pDevice->kind = static_cast<DeviceKind>(
                static_cast<quint8>(parsed.device_kind()));

        switch (parsed.packet_type()) {
        case prolink_djl_t::PACKET_TYPE_KEEP_ALIVE: {
            const auto* body = static_cast<prolink_djl_t::keep_alive_body_t*>(
                    parsed.body());
            pDevice->deviceNumber = body->device_number();
            pDevice->mac = toBytes(body->mac());
            pDevice->address = toAddress(body->ip());
            break;
        }
        case prolink_djl_t::PACKET_TYPE_CLAIM_IP: {
            // Mid-handshake. The number here is *proposed*, not yet held, so a
            // deck seen this way may renumber a moment later -- which is fine,
            // the peer table keys on the MAC and updates the number in place.
            const auto* body = static_cast<prolink_djl_t::claim_ip_body_t*>(
                    parsed.body());
            pDevice->deviceNumber = body->device_number();
            pDevice->mac = toBytes(body->mac());
            pDevice->address = toAddress(body->ip());
            break;
        }
        case prolink_djl_t::PACKET_TYPE_CLAIM_MAC: {
            // Earliest we can learn of a device. No IP yet, so it stays out of
            // the table until a later stage supplies one; recording it now would
            // put a row in the sidebar we cannot reach.
            const auto* body = static_cast<prolink_djl_t::claim_mac_body_t*>(
                    parsed.body());
            pDevice->mac = toBytes(body->mac());
            break;
        }
        default:
            // hello, claim_number, number_in_use, number_conflict and the mixer
            // assignment types carry no MAC/IP pair, so they identify nobody.
            return false;
        }
    } catch (const std::exception& e) {
        kLogger.debug() << "undecodable datagram," << datagram.size()
                        << "bytes:" << e.what();
        return false;
    } catch (...) {
        kLogger.debug() << "undecodable datagram," << datagram.size() << "bytes";
        return false;
    }

    if (!pDevice->isValid()) {
        return false;
    }
    pDevice->touch();
    return true;
}

int packetType(const QByteArray& datagram) {
    if (!hasMagic(datagram)) {
        return -1;
    }
    return static_cast<quint8>(datagram.at(kOffsetType));
}

bool parseContestedNumber(const QByteArray& datagram, int* pNumber) {
    VERIFY_OR_DEBUG_ASSERT(pNumber) {
        return false;
    }
    if (!hasMagic(datagram)) {
        return false;
    }
    try {
        const std::string buffer(datagram.constData(), datagram.size());
        kaitai::kstream stream(buffer);
        prolink_djl_t parsed(&stream);
        switch (parsed.packet_type()) {
        case prolink_djl_t::PACKET_TYPE_CLAIM_IP:
            *pNumber = static_cast<prolink_djl_t::claim_ip_body_t*>(parsed.body())
                               ->device_number();
            return true;
        case prolink_djl_t::PACKET_TYPE_CLAIM_NUMBER:
            *pNumber = static_cast<prolink_djl_t::number_body_t*>(parsed.body())
                               ->device_number();
            return true;
        case prolink_djl_t::PACKET_TYPE_NUMBER_CONFLICT:
            *pNumber = static_cast<prolink_djl_t::number_conflict_body_t*>(
                    parsed.body())
                               ->device_number();
            return true;
        default:
            return false;
        }
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

// -- builders --------------------------------------------------------------

namespace {

/// The common 0x24-byte header, inside a zeroed buffer of *length*.
///
/// The `stype` byte at 0x23 equals the whole datagram length for every type we
/// have captured. research/02 lists claim_number as stype 0x26 but length 0x2a,
/// which would leave four undescribed trailing bytes; six real ones in the
/// dysentery captures are 0x26 bytes long, so the table's length column is
/// simply wrong there (C2).
QByteArray header(quint8 type, const QString& name, DeviceKind kind, int length) {
    QByteArray out(length, '\0');
    out.replace(0, kMagicLength, QByteArray::fromRawData(kMagic, kMagicLength));
    out[kOffsetType] = static_cast<char>(type);
    // 0x0b subtype stays 0: nonzero only marks a directed mixer reply, which is
    // a flow we do not participate in.

    // NUL-padded to exactly 20 bytes, and truncated rather than allowed to
    // overrun -- the padding is part of what makes an announcement look real
    // (F1), and the name is the first thing a DJ sees on the deck's screen.
    const QByteArray ascii = name.toLatin1().left(kDeviceNameLength);
    out.replace(kOffsetName, ascii.size(), ascii);

    out[kOffsetConstOne] = 0x01;
    out[kOffsetDeviceKind] = static_cast<char>(kind);
    // 0x22 pad stays 0.
    out[kOffsetStype] = static_cast<char>(length);
    return out;
}

/// 0x01 for a CDJ, 0x02 for a mixer. Recurs at 0x30 in the stage-2 claim and at
/// 0x34 in the keep-alive, tracking the device kind in both.
quint8 roleFor(DeviceKind kind) {
    return kind == DeviceKind::Mixer ? 0x02 : 0x01;
}

void putIp(QByteArray* pOut, int offset, const QHostAddress& ip) {
    const quint32 value = ip.toIPv4Address();
    (*pOut)[offset] = static_cast<char>((value >> 24) & 0xFF);
    (*pOut)[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    (*pOut)[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    (*pOut)[offset + 3] = static_cast<char>(value & 0xFF);
}

void putMac(QByteArray* pOut, int offset, const QByteArray& mac) {
    pOut->replace(offset, qMin(mac.size(), 6), mac.left(6));
}

} // namespace

QByteArray buildHello(const QString& name, DeviceKind kind) {
    QByteArray out = header(static_cast<quint8>(PacketType::Hello), name, kind, 0x25);
    out[0x24] = static_cast<char>(kind == DeviceKind::Mixer ? 0x02 : 0x01);
    return out;
}

QByteArray buildClaimMac(const QString& name,
        DeviceKind kind,
        int iteration,
        const QByteArray& mac) {
    QByteArray out =
            header(static_cast<quint8>(PacketType::ClaimMac), name, kind, 0x2C);
    out[0x24] = static_cast<char>(iteration);
    out[0x25] = static_cast<char>(roleFor(kind));
    putMac(&out, 0x26, mac);
    return out;
}

QByteArray buildClaimIp(const QString& name,
        DeviceKind kind,
        const QHostAddress& ip,
        const QByteArray& mac,
        int deviceNumber,
        int iteration) {
    QByteArray out =
            header(static_cast<quint8>(PacketType::ClaimIp), name, kind, 0x32);
    putIp(&out, 0x24, ip);
    putMac(&out, 0x28, mac);
    out[0x2E] = static_cast<char>(deviceNumber);
    out[0x2F] = static_cast<char>(iteration);
    // 0x30 is the CDJ/mixer role byte, not the "const 01" research/02 calls it:
    // a real DJM-2000nexus sends 02 here where a CDJ-2000nexus sends 01 (C1).
    out[0x30] = static_cast<char>(roleFor(kind));
    // 0x31 assignment mode. 01 = we picked the number ourselves, which is what
    // we do even in AUTO: "auto" here means we chose a free one after watching,
    // not that we asked a mixer to allocate one (that is the 02 flow, and it
    // needs a mixer to answer).
    out[0x31] = 0x01;
    return out;
}

QByteArray buildClaimNumber(const QString& name,
        DeviceKind kind,
        int deviceNumber,
        int iteration) {
    QByteArray out =
            header(static_cast<quint8>(PacketType::ClaimNumber), name, kind, 0x26);
    out[0x24] = static_cast<char>(deviceNumber);
    out[0x25] = static_cast<char>(iteration);
    return out;
}

QByteArray buildKeepAlive(const QString& name,
        DeviceKind kind,
        int deviceNumber,
        const QByteArray& mac,
        const QHostAddress& ip,
        int peerCount,
        bool firstOnNetwork) {
    QByteArray out =
            header(static_cast<quint8>(PacketType::KeepAlive), name, kind, 0x36);
    out[0x24] = static_cast<char>(deviceNumber);
    // 0x25 is latched at boot: 02 if we were first on the network, 01 if peers
    // were already there. Six observed boots, no exceptions (F9). Documented
    // elsewhere as a CDJ/mixer role byte, which real captures contradict (C4).
    out[0x25] = static_cast<char>(firstOnNetwork ? 0x02 : 0x01);
    putMac(&out, 0x26, mac);
    putIp(&out, 0x2C, ip);
    // Includes ourselves.
    out[0x30] = static_cast<char>(peerCount);
    // 0x31-0x33 stay zero.
    out[0x34] = static_cast<char>(roleFor(kind));
    // 0x35: research/02 calls 01 the "typical CDJ keep-alive" value, but every
    // real nexus packet captured -- 148 from a CDJ-2000nexus and 91 from a
    // DJM-2000nexus -- carries 00, and looking like a CDJ-2000nexus is the whole
    // point (C3). 0x64 is what CDJ-3000 coexistence would need instead.
    out[0x35] = 0x00;
    return out;
}

QByteArray buildNumberConflict(const QString& name,
        DeviceKind kind,
        int deviceNumber,
        const QHostAddress& ip) {
    QByteArray out =
            header(static_cast<quint8>(PacketType::NumberConflict), name, kind, 0x29);
    out[0x24] = static_cast<char>(deviceNumber);
    putIp(&out, 0x25, ip);
    return out;
}

} // namespace packet
} // namespace prolink
} // namespace mixxx
