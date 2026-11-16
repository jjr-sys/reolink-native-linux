#include "media/BufferReplay.h"

#include "media/FrameTimeSei.h"

#include <cstring>

extern "C" {
#include <libavutil/error.h>
}

namespace rl {

BufferReplay::BufferReplay(std::shared_ptr<LiveBuffer> buffer, qint64 fromMs, int direction,
                           std::shared_ptr<std::atomic<double>> speed)
    : m_buffer(std::move(buffer)), m_speed(std::move(speed)), m_direction(direction < 0 ? -1 : 1)
{
    if (!m_buffer)
        return;
    if (m_direction < 0) {
        const LiveBuffer::Run run = m_buffer->runAt(fromMs);
        if (run.frames.isEmpty())
            return;
        m_main = run.main;
        const reverse::Gops g = reverse::reverseGops(run.frames, fromMs + 1, run.startMs);
        for (const QList<reverse::Frame> &gop : g.newestFirst)
            m_queue += gop;
    } else {
        m_queue = m_buffer->framesFrom(fromMs, true);
        if (!m_queue.isEmpty())
            m_main = m_buffer->mainAt(m_queue.first().epochMs);
    }
    m_empty = m_queue.isEmpty();
    if (!m_empty)
        m_codec = m_queue.first().codec;
}

QByteArray BufferReplay::next()
{
    for (;;) {
        if (m_queue.isEmpty() && m_direction > 0 && m_lastMs >= 0)
            m_queue = m_buffer->framesFrom(m_lastMs, false);   // buffered since
        if (m_queue.isEmpty())
            return {};
        const reverse::Frame f = m_queue.takeFirst();
        const double speed = m_speed ? m_speed->load(std::memory_order_relaxed) : 1.0;
        if (m_direction < 0 && speed >= 16.0 && !f.key)
            continue;   // backwards at the top speed: keyframes only
        m_lastMs = f.epochMs;
        return frametime::find(f.annexB, f.codec) >= 0 ? f.annexB
                                                       : frametime::tag(f.annexB, f.codec, f.epochMs);
    }
}

int BufferReplay::read(unsigned char *buf, int size)
{
    if (m_stopped.load())
        return AVERROR_EOF;
    if (m_pending.isEmpty())
        m_pending = next();
    if (m_pending.isEmpty())
        return AVERROR_EOF;
    const int n = qMin(size, int(m_pending.size()));
    std::memcpy(buf, m_pending.constData(), n);
    m_pending.remove(0, n);
    return n;
}

} // namespace rl
