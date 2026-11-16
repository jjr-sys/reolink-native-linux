#include "media/CatchUp.h"

namespace rl {

void CatchUp::begin(bool decoderReady)
{
    m_ready = decoderReady;
    m_missed = false;
    m_held = 0;
}

CatchUp::Hold CatchUp::onPacket(bool key)
{
    if (key) {
        m_ready = true;
        m_held = 1;
        return Hold::Restart;
    }
    if (!m_ready) {
        m_missed = true;
        return Hold::Drop;
    }
    if (m_held >= kMaxHeld) {
        m_ready = false;
        m_missed = true;
        m_held = 0;
        return Hold::Overflow;
    }
    ++m_held;
    return Hold::Keep;
}

CatchUp::Resume CatchUp::end(bool key)
{
    const int held = m_held;
    const bool missed = m_missed;
    m_held = 0;
    m_missed = false;
    if (key)
        return Resume::Continue;
    if (held > 0)
        return Resume::FeedHeld;
    return missed ? Resume::AwaitKeyframe : Resume::Continue;
}

} // namespace rl
