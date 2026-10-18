#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

namespace rl {

// Holds the newest value until the consumer takes it and coalesces requests to
// wake that consumer. A slow consumer therefore retains at most one pending value.
template <typename T>
class LatestValueMailbox
{
public:
    // Returns true when the caller should schedule a consumer wake-up.
    bool publish(T value)
    {
        std::lock_guard lock(m_mutex);
        if (m_pending)
            ++m_overwritten;
        m_pending.emplace(std::move(value));
        if (m_wakeupPending)
            return false;
        m_wakeupPending = true;
        return true;
    }

    bool take(T &value)
    {
        std::lock_guard lock(m_mutex);
        m_wakeupPending = false;
        if (!m_pending)
            return false;
        value = std::move(*m_pending);
        m_pending.reset();
        return true;
    }

    void clear()
    {
        std::lock_guard lock(m_mutex);
        m_pending.reset();
        m_wakeupPending = false;
    }

    std::size_t overwrittenCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_overwritten;
    }

private:
    mutable std::mutex m_mutex;
    std::optional<T> m_pending;
    bool m_wakeupPending = false;
    std::size_t m_overwritten = 0;
};

} // namespace rl
