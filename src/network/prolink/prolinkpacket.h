#pragma once

#include <QByteArray>
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

} // namespace packet
} // namespace prolink
} // namespace mixxx
