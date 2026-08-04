#include "network/prolink/keysync.h"

namespace mixxx {
namespace prolink {

bool KeySync::engage(const Link& link) {
    if (m_engaged) {
        // Already holding one. Re-reading the network here is exactly what
        // rule 4 forbids: it would turn every press into a re-latch and make
        // the button impossible to switch off.
        return true;
    }
    if (!canEngage(link)) {
        return false;
    }
    m_target = link.masterKey;
    m_engaged = true;
    return true;
}

void KeySync::release() {
    m_engaged = false;
    m_target = mixxx::track::io::key::INVALID;
}

} // namespace prolink
} // namespace mixxx
