#pragma once

#include <QtGlobal>

#include <algorithm>
#include <deque>
#include <utility>

namespace rl {

// Presentation clock for live video. Frames arrive from the network in bursts and
// stalls; showing each one the instant it arrives makes the picture freeze and then
// rush. This holds every frame until `pts + delay` on the wall clock instead, so a
// stall shorter than the delay is invisible.
//
// The first frame fixes the schedule: it is due `delay` after it arrived, and every later
// frame is due at the same offset plus its timestamp difference from the first. A frame
// that turns up long after its slot (a stall longer than the delay) or a timestamp
// discontinuity starts a fresh schedule, so the picture freezes once and recovers instead
// of trying to catch up. If the source runs faster than real time the queue would grow
// without bound, so the schedule is pulled earlier to keep the depth near `delay`.
//
// Pure logic: the caller supplies "now" (microseconds), so it is fully testable on a
// virtual clock. Not thread-safe; use it from one thread.
template <class Frame>
class VideoPlayoutQueue
{
public:
    struct Config {
        qint64 delayUs = 1000000;     // how far behind arrival the picture runs
        qint64 maxDepthUs = 2500000;  // queued-ahead limit; beyond it the schedule is pulled in
        qint64 lateToleranceUs = 150000; // a frame this far past its slot means the cushion is gone
        qint64 jumpUs = 3000000;      // a timestamp step bigger than this is a discontinuity
        int maxFrames = 120;          // hard cap on queued frames
        qint64 maxBytes = 0;          // hard cap on queued payload bytes (0 = no cap)
    };

    struct Stats {
        qint64 presented = 0;
        qint64 droppedLate = 0;     // due together with a newer frame: skipped, never shown
        qint64 droppedOverflow = 0; // discarded by the frame or byte cap
        int reanchors = 0;          // fresh schedules after a stall, jump or backwards timestamp
    };

    explicit VideoPlayoutQueue(Config config = {}) : m_cfg(config) {}

    void setConfig(const Config &config) { m_cfg = config; }
    const Config &config() const { return m_cfg; }

    // A decoded frame with camera timestamp `ptsUs`, arriving at `nowUs`.
    void push(Frame frame, qint64 ptsUs, qint64 nowUs, qint64 bytes = 0)
    {
        if (!m_anchored) {
            anchor(ptsUs, nowUs);
        } else {
            const bool jumped = ptsUs < m_lastPts || ptsUs - m_lastPts > m_cfg.jumpUs;
            if (jumped) {
                startOver(ptsUs, nowUs);
            } else if (dueAt(ptsUs) < nowUs - m_cfg.lateToleranceUs) {
                startOver(ptsUs, nowUs); // stall outlasted the cushion
            } else {
                const qint64 depth = dueAt(ptsUs) - nowUs;
                if (depth > m_cfg.maxDepthUs) // source faster than real time: pull in
                    m_wallAnchor -= depth - m_cfg.delayUs;
            }
        }
        m_lastPts = ptsUs;
        m_q.push_back({std::move(frame), ptsUs, bytes});
        m_bytes += bytes;
        while (int(m_q.size()) > m_cfg.maxFrames ||
               (m_cfg.maxBytes > 0 && m_bytes > m_cfg.maxBytes && m_q.size() > 1)) {
            m_bytes -= m_q.front().bytes;
            m_q.pop_front();
            ++m_stats.droppedOverflow;
        }
    }

    // The newest frame that is due by `nowUs`, if any. Older frames that were also due
    // (the timer ran late) are skipped rather than shown in a rush.
    bool poll(qint64 nowUs, Frame &out)
    {
        bool got = false;
        while (!m_q.empty() && dueAt(m_q.front().pts) <= nowUs) {
            if (got)
                ++m_stats.droppedLate;
            out = std::move(m_q.front().frame);
            m_bytes -= m_q.front().bytes;
            m_q.pop_front();
            got = true;
        }
        if (got)
            ++m_stats.presented;
        return got;
    }

    // When the next queued frame is due, or -1 if the queue is empty.
    qint64 nextDueUs() const { return m_q.empty() ? -1 : dueAt(m_q.front().pts); }

    // Drop everything and forget the schedule (stream stopped or replaced).
    void clear()
    {
        m_q.clear();
        m_bytes = 0;
        m_anchored = false;
    }

    int size() const { return int(m_q.size()); }
    qint64 queuedBytes() const { return m_bytes; }
    const Stats &stats() const { return m_stats; }

private:
    struct Entry {
        Frame frame;
        qint64 pts;
        qint64 bytes;
    };

    qint64 dueAt(qint64 ptsUs) const { return m_wallAnchor + (ptsUs - m_ptsAnchor); }

    void anchor(qint64 ptsUs, qint64 nowUs)
    {
        m_ptsAnchor = ptsUs;
        m_wallAnchor = nowUs + m_cfg.delayUs;
        m_anchored = true;
    }

    void startOver(qint64 ptsUs, qint64 nowUs)
    {
        // Everything queued belongs to the old schedule.
        m_q.clear();
        m_bytes = 0;
        anchor(ptsUs, nowUs);
        ++m_stats.reanchors;
    }

    Config m_cfg;
    std::deque<Entry> m_q;
    qint64 m_bytes = 0;
    bool m_anchored = false;
    qint64 m_ptsAnchor = 0;
    qint64 m_wallAnchor = 0;
    qint64 m_lastPts = 0;
    Stats m_stats;
};

} // namespace rl
