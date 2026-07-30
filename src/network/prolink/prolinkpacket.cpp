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

} // namespace packet
} // namespace prolink
} // namespace mixxx
