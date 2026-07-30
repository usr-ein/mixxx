#include <gtest/gtest.h>

#include <QByteArray>
#include <QHostAddress>

#include "network/prolink/prolinkdefs.h"
#include "network/prolink/prolinkpacket.h"

namespace {

using namespace mixxx::prolink;

QByteArray hex(const char* text) {
    return QByteArray::fromHex(QByteArray(text));
}

const QByteArray kMac = hex("a0cec8e226de");
const QHostAddress kIp(QStringLiteral("169.254.99.100"));

/// A real 54-byte keep-alive from a CDJ-2000nexus, byte for byte, out of
/// captures/S24b-e9-control. The same literal the Python corpus test pins.
///
/// **This is the acceptance test for announcing.** Every byte we put on UDP
/// 50000 has to be justifiable against a real device, because a subtly wrong
/// keep-alive is the one way this feature can disturb a rig that was working
/// before Mixxx was plugged into it.
const char* kRealKeepAlive =
        "5173707431576d4a4f4c060043444a2d3230303"
        "06e6578757300000000000000010200360502a0"
        "cec8e226dea9fe6364010000000100";

TEST(ProLinkAnnounceTest, KeepAliveIsByteIdenticalToARealCdjs) {
    // Device 5 and "first on the network" are what that capture shows: byte
    // 0x24 is 05 and byte 0x25 is 02.
    const QByteArray ours = packet::buildKeepAlive(QStringLiteral("CDJ-2000nexus"),
            DeviceKind::Cdj,
            5,
            kMac,
            kIp,
            1,
            /*firstOnNetwork*/ true);
    EXPECT_EQ(hex(kRealKeepAlive), ours);
}

/// The counterpart: joining a network that already has peers flips exactly one
/// byte, and it is 0x25 (F9).
TEST(ProLinkAnnounceTest, JoiningPeersChangesOnlyByteTwentyFive) {
    const QByteArray first = packet::buildKeepAlive(
            QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, 5, kMac, kIp, 1, true);
    const QByteArray joined = packet::buildKeepAlive(
            QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, 5, kMac, kIp, 1, false);
    ASSERT_EQ(first.size(), joined.size());
    int differing = 0;
    for (int i = 0; i < first.size(); ++i) {
        if (first.at(i) != joined.at(i)) {
            ++differing;
            EXPECT_EQ(0x25, i);
        }
    }
    EXPECT_EQ(1, differing);
}

/// Round-tripping through the generated Kaitai reader is what keeps the
/// hand-written builders honest: Kaitai emits no C++ serializers, so the writers
/// carry their offsets independently of the schema and would otherwise be free
/// to drift away from it.
TEST(ProLinkAnnounceTest, EveryBuiltPacketParsesBackToWhatWentIn) {
    ProLinkDevice device;

    ASSERT_TRUE(packet::parseAnnouncement(
            packet::buildKeepAlive(QStringLiteral("CDJ-2000nexus"),
                    DeviceKind::Cdj,
                    3,
                    kMac,
                    kIp,
                    2,
                    true),
            &device));
    EXPECT_EQ(3, device.deviceNumber);
    EXPECT_EQ(kMac, device.mac);
    EXPECT_EQ(kIp, device.address);
    EXPECT_EQ(QStringLiteral("CDJ-2000nexus"), device.name);
    EXPECT_EQ(DeviceKind::Cdj, device.kind);

    device = ProLinkDevice();
    ASSERT_TRUE(packet::parseAnnouncement(
            packet::buildClaimIp(QStringLiteral("CDJ-2000nexus"),
                    DeviceKind::Cdj,
                    kIp,
                    kMac,
                    4,
                    1),
            &device));
    EXPECT_EQ(4, device.deviceNumber);
    EXPECT_EQ(kMac, device.mac);
    EXPECT_EQ(kIp, device.address);

    // A stage-1 claim is deliberately *not* accepted: it carries a MAC and no
    // IP, so it identifies a device we could not reach, and admitting it would
    // put a row in the sidebar with nothing behind it. The MAC is still at the
    // offset the schema reads it from, which is what this checks.
    const QByteArray claimMac = packet::buildClaimMac(
            QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, 2, kMac);
    device = ProLinkDevice();
    EXPECT_FALSE(packet::parseAnnouncement(claimMac, &device));
    EXPECT_EQ(kMac, claimMac.mid(0x26, 6));
    EXPECT_EQ(2, static_cast<quint8>(claimMac.at(0x24)));
}

/// The stype byte at 0x23 equals the whole datagram length for every type, which
/// is what a receiver length-checks against.
TEST(ProLinkAnnounceTest, StypeEqualsTheDatagramLength) {
    const QByteArray packets[] = {
            packet::buildHello(QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj),
            packet::buildClaimMac(QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, 1, kMac),
            packet::buildClaimIp(
                    QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, kIp, kMac, 3, 1),
            packet::buildClaimNumber(
                    QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, 3, 1),
            packet::buildKeepAlive(QStringLiteral("CDJ-2000nexus"),
                    DeviceKind::Cdj,
                    3,
                    kMac,
                    kIp,
                    1,
                    true),
            packet::buildNumberConflict(
                    QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, 3, kIp),
    };
    // The sizes research/02 documents, with claim_number corrected to 0x26 --
    // its length column says 0x2a, which would leave four undescribed trailing
    // bytes that six real packets do not have (C2).
    const int expected[] = {0x25, 0x2C, 0x32, 0x26, 0x36, 0x29};
    int index = 0;
    for (const QByteArray& datagram : packets) {
        EXPECT_EQ(expected[index], datagram.size()) << "packet " << index;
        EXPECT_EQ(datagram.size(), static_cast<quint8>(datagram.at(kOffsetStype)))
                << "packet " << index;
        ++index;
    }
}

/// The name is NUL-padded to exactly 20 bytes. The padding is part of what makes
/// an announcement look like a real one (F1), and a name longer than the field
/// must be truncated rather than allowed to overrun into the next header byte.
TEST(ProLinkAnnounceTest, TheNameFieldIsAlwaysTwentyNulPaddedBytes) {
    const QByteArray shortName = packet::buildHello(QStringLiteral("AB"), DeviceKind::Cdj);
    EXPECT_EQ(QByteArrayLiteral("AB") + QByteArray(18, '\0'),
            shortName.mid(kOffsetName, kDeviceNameLength));
    EXPECT_EQ(0x01, static_cast<quint8>(shortName.at(kOffsetConstOne)));

    const QByteArray longName = packet::buildHello(
            QStringLiteral("a-name-far-longer-than-the-field"), DeviceKind::Cdj);
    EXPECT_EQ(0x25, longName.size());
    EXPECT_EQ(0x01, static_cast<quint8>(longName.at(kOffsetConstOne)));
    EXPECT_EQ(static_cast<quint8>(DeviceKind::Cdj),
            static_cast<quint8>(longName.at(kOffsetDeviceKind)));
}

TEST(ProLinkAnnounceTest, ReadsTheNumberAClaimOrConflictIsAbout) {
    int number = 0;

    // Stage 2 carries the proposed number at 0x2e, stage 3 at 0x24, and a
    // conflict at 0x24 -- three offsets for the same quantity.
    ASSERT_TRUE(packet::parseContestedNumber(
            packet::buildClaimIp(
                    QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, kIp, kMac, 2, 1),
            &number));
    EXPECT_EQ(2, number);

    ASSERT_TRUE(packet::parseContestedNumber(
            packet::buildClaimNumber(
                    QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, 4, 1),
            &number));
    EXPECT_EQ(4, number);

    ASSERT_TRUE(packet::parseContestedNumber(
            packet::buildNumberConflict(
                    QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj, 1, kIp),
            &number));
    EXPECT_EQ(1, number);

    // A keep-alive is about nobody's claim, and must not be mistaken for one --
    // reading its 0x24 as a contested number would have us back off from our own
    // number every two seconds.
    EXPECT_FALSE(packet::parseContestedNumber(
            packet::buildKeepAlive(QStringLiteral("CDJ-2000nexus"),
                    DeviceKind::Cdj,
                    3,
                    kMac,
                    kIp,
                    1,
                    true),
            &number));
}

TEST(ProLinkAnnounceTest, PacketTypeRejectsThingsThatAreNotOurs) {
    EXPECT_EQ(-1, packet::packetType(QByteArray()));
    EXPECT_EQ(-1, packet::packetType(QByteArray(64, '\0')));
    EXPECT_EQ(static_cast<int>(PacketType::KeepAlive),
            packet::packetType(hex(kRealKeepAlive)));
    EXPECT_EQ(static_cast<int>(PacketType::Hello),
            packet::packetType(packet::buildHello(
                    QStringLiteral("CDJ-2000nexus"), DeviceKind::Cdj)));
}

} // namespace
