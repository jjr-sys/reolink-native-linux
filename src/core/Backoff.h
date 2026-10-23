#pragma once

#include <QtGlobal>
#include <algorithm>

namespace rl {

// Grow-and-cap retry delay. Units are the caller's (ms for the stream and socket loops,
// seconds for DeviceManager's host probe). Each user keeps its own start, cap and reset
// rule; this module only owns the arithmetic and holds no clock: callers sleep or
// schedule, and anything time-dependent takes "now" as an argument so tests can fake it.
struct BackoffPolicy
{
    int start;
    int cap;
    int growth = 2;

    // One step from `delay`. A delay already at or past the cap is left alone, so a
    // deliberate longer wait set with Backoff::setDelay() is not shrunk back.
    int grow(int delay) const { return delay < cap ? std::min(delay * growth, cap) : delay; }
};

class Backoff
{
public:
    explicit Backoff(BackoffPolicy policy) : m_policy(policy), m_delay(policy.start) {}

    // The delay the next failure will wait. Does not advance.
    int current() const { return m_delay; }

    // Record a failure: returns the delay to wait now, then grows for the next one.
    int fail()
    {
        const int d = m_delay;
        m_delay = m_policy.grow(d);
        return d;
    }

    // fail(), as a deadline: `nowMs` + the delay.
    qint64 failAt(qint64 nowMs) { return nowMs + fail(); }

    // Success (or "try now"): the next failure starts over at the policy start.
    void reset() { m_delay = m_policy.start; }

    // Force the next delay, even beyond the cap. Stays until reset().
    void setDelay(int delay) { m_delay = delay; }

private:
    BackoffPolicy m_policy;
    int m_delay;
};

} // namespace rl
