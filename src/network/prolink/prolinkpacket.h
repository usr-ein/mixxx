#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>

#include "network/prolink/prolinkdefs.h"
#include "network/prolink/prolinkdevice.h"

namespace mixxx {
namespace prolink {

/// Decoding of UDP-50000 datagrams into `ProLinkDevice`.
///
/// The byte-level work is done by the Kaitai parser generated from
/// `prolinks-compat/ksy/prolink_djl.ksy`; this is the adapter that turns its
/// `std::string` fields into Qt types and applies the interpretation. Keeping
/// the two apart means a schema change regenerates cleanly and the meaning lives
/// in one reviewable place.
namespace packet {

/// Cheap pre-filter: does this datagram even claim to be Pro DJ Link?
///
/// Worth having separately from a parse attempt because the discovery socket
/// sees everything broadcast to port 50000, and constructing a parser per
/// stray datagram to find out it is not ours is wasteful on a Pi.
bool hasMagic(const QByteArray& datagram);

/// Fill *pDevice* from an announcement, returning false if the datagram is not
/// one we can learn a device from.
///
/// Accepts keep-alive, hello and both claim stages that carry a MAC: a player
/// that boots after us is first seen mid-handshake, and waiting for its first
/// keep-alive would delay it appearing by up to two seconds for no reason.
///
/// A datagram we cannot interpret is *skipped, not fatal*. Mixers, CDJ-3000s
/// and firmware we have never captured all emit types absent from our schema,
/// and refusing to parse the rest of the network because of one of them would
/// be a worse outcome than ignoring it.
bool parseAnnouncement(const QByteArray& datagram, ProLinkDevice* pDevice);

/// The type byte at offset 0x0a, or -1 if this is not a Pro DJ Link datagram.
int packetType(const QByteArray& datagram);

/// The device number a claim or conflict packet is *about*.
///
/// Different from the sender's own number, and at a different offset per type:
/// a stage-2 claim proposes one at 0x2e, a stage-3 claim and a conflict carry
/// one at 0x24. Needed to answer the two questions the announcer asks of every
/// datagram — "is somebody contesting my number?" and "is somebody proposing a
/// number I already hold?".
bool parseContestedNumber(const QByteArray& datagram, int* pNumber);

// -- builders --------------------------------------------------------------
//
// Hand-written, because Kaitai generates readers only for C++. Every offset
// below is the one the schema parses, and the round-trip is what the unit tests
// check: build a packet, parse it with the generated reader, compare.
//
// **These are the only functions in the tree that put bytes on a DJ-Link port.**
// Everything else about ProLink is passive by construction, so a keep-alive that
// is subtly wrong is the one way this feature can disturb a live rig — which is
// why the acceptance test for the keep-alive is a byte-for-byte diff against a
// real CDJ-2000nexus one (F1, F9, C3).

QByteArray buildHello(const QString& name, DeviceKind kind);
QByteArray buildClaimMac(const QString& name,
        DeviceKind kind,
        int iteration,
        const QByteArray& mac);
QByteArray buildClaimIp(const QString& name,
        DeviceKind kind,
        const QHostAddress& ip,
        const QByteArray& mac,
        int deviceNumber,
        int iteration);
QByteArray buildClaimNumber(const QString& name,
        DeviceKind kind,
        int deviceNumber,
        int iteration);
/// *firstOnNetwork* is latched at start-up and never revisited: a real deck
/// decides it once at boot and holds it for the session, and it is what byte
/// 0x25 carries (F9).
QByteArray buildKeepAlive(const QString& name,
        DeviceKind kind,
        int deviceNumber,
        const QByteArray& mac,
        const QHostAddress& ip,
        int peerCount,
        bool firstOnNetwork);
/// Sent unicast to whoever proposed a number we hold. A device that takes a
/// number and never defends it simply loses it to the next player that boots.
QByteArray buildNumberConflict(const QString& name,
        DeviceKind kind,
        int deviceNumber,
        const QHostAddress& ip);

} // namespace packet
} // namespace prolink
} // namespace mixxx
