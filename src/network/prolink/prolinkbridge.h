#pragma once

#include <QString>

#include "network/prolink/prolinktypes.h"
#include "prolink-cxx/src/lib.rs.h"

/// Between the Rust bridge's types and Mixxx's, for the files that talk to the
/// session: ProLinkNetworkService and ProLinkSync.
///
/// It includes the generated bridge header, so only a .cpp file includes it:
/// the headers keep the session opaque.
namespace mixxx {
namespace prolink {

inline QString toQString(const ::rust::String& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

inline MediaSlot toMixxxSlot(::prolink::Slot slot) {
    switch (slot) {
    case ::prolink::Slot::Cd:
        return MediaSlot::Cd;
    case ::prolink::Slot::Sd:
        return MediaSlot::Sd;
    case ::prolink::Slot::Usb:
        return MediaSlot::Usb;
    case ::prolink::Slot::Rekordbox:
        return MediaSlot::Rekordbox;
    default:
        return MediaSlot::Empty;
    }
}

inline ::prolink::Slot toRustSlot(MediaSlot slot) {
    switch (slot) {
    case MediaSlot::Cd:
        return ::prolink::Slot::Cd;
    case MediaSlot::Sd:
        return ::prolink::Slot::Sd;
    case MediaSlot::Rekordbox:
        return ::prolink::Slot::Rekordbox;
    case MediaSlot::Usb:
    default:
        // USB for anything unnamed: it is the slot a deck browses first, and
        // the one a caller means when it has not thought about it.
        return ::prolink::Slot::Usb;
    }
}

} // namespace prolink
} // namespace mixxx
