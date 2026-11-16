#pragma once

#include "media/LiveBuffer.h"

#include <QByteArray>
#include <QList>

#include <atomic>
#include <memory>

namespace rl {

// Live replay from the live buffer (wayfinder/live-replay ticket 08b): a camera's buffered live
// frames played as a stream, the way the recorder's windows are (media/ReversePlay.h), so the
// player paces, holds and shows them exactly as it does recorder footage.
//
//  - Backwards from a moment: the run holding it, cut at the moment, its keyframe groups newest
//    first, each in decode order; at the top speed (16×) keyframes only, as reverse play does.
//  - Forwards from a moment: from its group's keyframe, in order, on to the newest frame —
//    frames buffered while it plays included.
//  - One run per stream: its end is the stream's end (Live replay then carries on: an older run,
//    the recorder, or live).
// Every access unit carries its time (media/FrameTimeSei.h).
class BufferReplay
{
public:
    BufferReplay(std::shared_ptr<LiveBuffer> buffer, qint64 fromMs, int direction,
                 std::shared_ptr<std::atomic<double>> speed = {});

    bool main() const { return m_main; }
    BcMediaParser::Codec codec() const { return m_codec; }
    bool empty() const { return m_empty; }

    // The next access unit; empty at the end.
    QByteArray next();
    // For the player's packet source: bytes of the stream, AVERROR_EOF at the end or once stopped.
    int read(unsigned char *buf, int size);
    void stop() { m_stopped.store(true); }

private:
    std::shared_ptr<LiveBuffer> m_buffer;
    std::shared_ptr<std::atomic<double>> m_speed;
    int m_direction = 1;
    bool m_main = false;
    bool m_empty = true;
    BcMediaParser::Codec m_codec = BcMediaParser::Codec::Unknown;
    QList<reverse::Frame> m_queue;
    qint64 m_lastMs = -1;            // forwards: the newest frame sent
    QByteArray m_pending;            // read(): the rest of the access unit being read
    std::atomic<bool> m_stopped{false};
};

} // namespace rl
