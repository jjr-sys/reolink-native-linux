#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <deque>
#include <utility>

namespace rl {

// Smooths bursty live Baichuan delivery before the Annex-B bytes reach FFmpeg.
// Unlike VideoPlayoutQueue, this queue must preserve every encoded access unit:
// dropping a P-frame can corrupt later frames until the next key frame.
class BcLiveFramePacer
{
public:
    static constexpr qint64 kDelayUs = 250000;

    struct Config {
        qint64 delayUs = kDelayUs;
        qint64 maxDepthUs = 1250000;
        qint64 lateToleranceUs = 100000;
        qint64 maxTimestampGapUs = 3000000;
        qint64 maxBytes = 32ll * 1024 * 1024;
        int maxFrames = 120;
        qint64 fallbackStepUs = 40000;
    };

    struct Stats {
        qint64 queued = 0;
        qint64 released = 0;
        qint64 overflowed = 0;
        int reanchors = 0;
    };

    BcLiveFramePacer() = default;
    explicit BcLiveFramePacer(Config config) : m_cfg(config) {}

    // Queue one encoded access unit with the camera's 32-bit microsecond clock and
    // monotonic arrival time. Returns false while waiting for an IDR after overflow.
    bool push(QByteArray annexB, quint32 ptsUs, qint64 arrivedUs, bool keyFrame)
    {
        if (m_waitingForKeyFrame && !keyFrame) {
            ++m_stats.overflowed;
            return false;
        }

        if (!m_anchored) {
            anchor(ptsUs, arrivedUs);
        } else {
            const quint32 rawStep = ptsUs - m_lastRawPts; // modulo handles the 32-bit wrap
            qint64 stepUs = rawStep;
            if (rawStep == 0)
                stepUs = m_lastStepUs > 0 ? m_lastStepUs : m_cfg.fallbackStepUs;

            if (stepUs > m_cfg.maxTimestampGapUs) {
                // A camera clock reset or long timestamp gap. Keep the access-unit
                // order intact and start a fresh wall-clock schedule for this frame.
                anchor(ptsUs, arrivedUs);
                ++m_stats.reanchors;
            } else {
                m_extendedPts += stepUs;
                m_lastRawPts = ptsUs;
                if (rawStep > 0)
                    m_lastStepUs = rawStep;

                qint64 due = dueAt(m_extendedPts);
                if (due < arrivedUs - m_cfg.lateToleranceUs) {
                    // Keep already queued encoded frames in order; move the schedule
                    // forward so they do not get dumped into the decoder in a burst.
                    const qint64 firstDue = m_queue.empty() ? due : m_queue.front().dueUs;
                    const qint64 shift = arrivedUs + m_cfg.delayUs - firstDue;
                    m_wallAnchorUs += shift;
                    ++m_stats.reanchors;
                } else if (due - arrivedUs > m_cfg.maxDepthUs) {
                    m_wallAnchorUs -= due - arrivedUs - m_cfg.delayUs;
                }
            }
        }

        if (m_waitingForKeyFrame) {
            // The key frame that recovered the queue starts a clean decode chain.
            m_queue.clear();
            m_queuedBytes = 0;
            anchor(ptsUs, arrivedUs);
            m_waitingForKeyFrame = false;
        }

        const QByteArray currentFrame = annexB; // Qt implicit sharing keeps the overflow path cheap
        enqueue(std::move(annexB), m_extendedPts, keyFrame);
        if (overLimit()) {
            ++m_stats.overflowed;
            m_queue.clear();
            m_queuedBytes = 0;
            m_anchored = false;
            m_waitingForKeyFrame = true;
            if (keyFrame) {
                anchor(ptsUs, arrivedUs);
                m_waitingForKeyFrame = false;
                enqueue(currentFrame, m_extendedPts, true);
            }
            return keyFrame;
        }

        ++m_stats.queued;
        return true;
    }

    // Release the oldest access unit once its timestamp is due.
    bool popDue(qint64 nowUs, QByteArray &annexB)
    {
        if (m_queue.empty() || m_queue.front().dueUs > nowUs)
            return false;
        annexB = std::move(m_queue.front().annexB);
        m_queuedBytes -= m_queue.front().bytes;
        m_queue.pop_front();
        ++m_stats.released;
        return true;
    }

    qint64 nextDueUs() const { return m_queue.empty() ? -1 : m_queue.front().dueUs; }
    bool hasAnchor() const { return m_anchored; }
    int size() const { return int(m_queue.size()); }
    qint64 queuedBytes() const { return m_queuedBytes; }
    const Stats &stats() const { return m_stats; }

    void clear()
    {
        m_queue.clear();
        m_queuedBytes = 0;
        m_anchored = false;
        m_waitingForKeyFrame = false;
        m_lastStepUs = 0;
        m_extendedPts = 0;
    }

private:
    struct Entry {
        QByteArray annexB;
        qint64 dueUs = 0;
        qint64 bytes = 0;
        bool keyFrame = false;
    };

    qint64 dueAt(qint64 ptsUs) const { return m_wallAnchorUs + (ptsUs - m_ptsAnchorUs); }

    void anchor(quint32 ptsUs, qint64 nowUs)
    {
        m_lastRawPts = ptsUs;
        m_extendedPts = ptsUs;
        m_ptsAnchorUs = m_extendedPts;
        m_wallAnchorUs = nowUs + m_cfg.delayUs;
        m_lastStepUs = 0;
        m_anchored = true;
    }

    void enqueue(QByteArray annexB, qint64 ptsUs, bool keyFrame)
    {
        const qint64 bytes = annexB.size();
        m_queuedBytes += bytes;
        m_queue.push_back({std::move(annexB), dueAt(ptsUs), bytes, keyFrame});
    }

    bool overLimit() const
    {
        return int(m_queue.size()) > m_cfg.maxFrames || m_queuedBytes > m_cfg.maxBytes;
    }

    Config m_cfg;
    std::deque<Entry> m_queue;
    qint64 m_queuedBytes = 0;
    bool m_anchored = false;
    bool m_waitingForKeyFrame = false;
    quint32 m_lastRawPts = 0;
    qint64 m_extendedPts = 0;
    qint64 m_ptsAnchorUs = 0;
    qint64 m_wallAnchorUs = 0;
    qint64 m_lastStepUs = 0;
    Stats m_stats;
};

} // namespace rl
