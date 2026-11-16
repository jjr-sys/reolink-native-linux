#include "media/LiveBuffer.h"

namespace rl {

void LiveBuffer::push(const reverse::Frame &f, bool main)
{
    if (f.epochMs < 0)
        return;
    {
        QMutexLocker lock(&m_mutex);
        // A new run: another stream (quality, codec), or the camera's clock jumped.
        const bool otherStream = m_runOpen && (main != m_runMain || f.codec != m_runCodec);
        const bool jumped = m_runOpen && m_lastMs >= 0
                            && (f.epochMs < m_lastMs || f.epochMs - m_lastMs > kGapMs);
        if (otherStream || jumped)
            m_runOpen = false;
        if (!m_runOpen) {
            if (!f.key)
                return;   // a run starts on a keyframe
            ++m_run;
            m_runOpen = true;
            m_runMain = main;
            m_runCodec = f.codec;
        }
        if (f.key || m_gops.isEmpty() || m_gops.last().run != m_run)
            m_gops.append(Gop{m_run, main, 0, {}});
        Gop &g = m_gops.last();
        g.frames.append(f);
        g.bytes += f.annexB.size();
        m_bytes += f.annexB.size();
        m_lastMs = f.epochMs;
        // Five minutes back from the newest frame, in whole groups.
        const qint64 floor = f.epochMs - kSpanMs;
        while (m_gops.size() > 1 && m_gops.at(1).frames.first().epochMs <= floor) {
            m_bytes -= m_gops.first().bytes;
            m_gops.removeFirst();
        }
    }
    if (m_pool)
        m_pool->enforce();
}

void LiveBuffer::breakRun()
{
    QMutexLocker lock(&m_mutex);
    m_runOpen = false;
}

bool LiveBuffer::empty() const
{
    QMutexLocker lock(&m_mutex);
    return m_gops.isEmpty();
}

qint64 LiveBuffer::oldestMs() const
{
    QMutexLocker lock(&m_mutex);
    return m_gops.isEmpty() ? -1 : m_gops.first().frames.first().epochMs;
}

qint64 LiveBuffer::newestMs() const
{
    QMutexLocker lock(&m_mutex);
    return m_gops.isEmpty() ? -1 : m_gops.last().frames.last().epochMs;
}

qint64 LiveBuffer::bytes() const
{
    QMutexLocker lock(&m_mutex);
    return m_bytes;
}

int LiveBuffer::runs() const
{
    QMutexLocker lock(&m_mutex);
    int n = 0, last = -1;
    for (const Gop &g : m_gops)
        if (g.run != last) {
            ++n;
            last = g.run;
        }
    return n;
}

namespace {
// The group holding `ms`: from its first frame to the next group's first frame of the same run
// (or its own last frame); -1 when none.
template <typename Gops> int gopAt(const Gops &gops, qint64 ms)
{
    for (int i = int(gops.size()) - 1; i >= 0; --i) {
        const auto &g = gops.at(i);
        if (g.frames.first().epochMs > ms)
            continue;
        const bool sameRunNext = i + 1 < gops.size() && gops.at(i + 1).run == g.run;
        const qint64 end = sameRunNext ? gops.at(i + 1).frames.first().epochMs : g.frames.last().epochMs;
        return ms <= end ? i : -1;
    }
    return -1;
}
} // namespace

// gopAt, or in a break of less than kGapMs after a run, that run's last group.
template <typename Gops> int gopAtOrJustAfter(const Gops &gops, qint64 ms)
{
    const int i = gopAt(gops, ms);
    if (i >= 0)
        return i;
    for (int j = int(gops.size()) - 1; j >= 0; --j) {
        const auto &g = gops.at(j);
        if (g.frames.first().epochMs > ms)
            continue;
        const bool runEnds = j + 1 >= gops.size() || gops.at(j + 1).run != g.run;
        return runEnds && ms - g.frames.last().epochMs < LiveBuffer::kGapMs ? j : -1;
    }
    return -1;
}

bool LiveBuffer::covers(qint64 ms) const
{
    QMutexLocker lock(&m_mutex);
    return gopAt(m_gops, ms) >= 0;
}

bool LiveBuffer::mainAt(qint64 ms) const
{
    QMutexLocker lock(&m_mutex);
    const int i = gopAt(m_gops, ms);
    return i >= 0 && m_gops.at(i).main;
}

LiveBuffer::Run LiveBuffer::runAt(qint64 ms) const
{
    QMutexLocker lock(&m_mutex);
    Run out;
    const int at = gopAtOrJustAfter(m_gops, ms);
    if (at < 0)
        return out;
    const int run = m_gops.at(at).run;
    int first = at;
    while (first > 0 && m_gops.at(first - 1).run == run)
        --first;
    out.main = m_gops.at(at).main;
    out.startMs = m_gops.at(first).frames.first().epochMs;
    for (int i = first; i <= at; ++i)
        out.frames += m_gops.at(i).frames;   // implicitly shared bytes: a cheap copy
    return out;
}

QList<reverse::Frame> LiveBuffer::framesFrom(qint64 ms, bool fromKeyframe) const
{
    QMutexLocker lock(&m_mutex);
    QList<reverse::Frame> out;
    const int at = gopAt(m_gops, ms);
    if (at < 0)
        return out;
    const int run = m_gops.at(at).run;
    for (int i = at; i < m_gops.size() && m_gops.at(i).run == run; ++i)
        for (const reverse::Frame &f : m_gops.at(i).frames)
            if (fromKeyframe ? true : f.epochMs > ms)
                out.append(f);
    return out;
}

QList<LiveBuffer::Span> LiveBuffer::spans() const
{
    QMutexLocker lock(&m_mutex);
    QList<Span> out;
    for (const Gop &g : m_gops) {
        const qint64 a = g.frames.first().epochMs, b = g.frames.last().epochMs;
        if (!out.isEmpty() && a - out.last().endMs < kGapMs)
            out.last().endMs = qMax(out.last().endMs, b);
        else
            out.append({a, b});
    }
    return out;
}

qint64 LiveBuffer::spareOldestMs() const
{
    QMutexLocker lock(&m_mutex);
    return m_gops.size() > 1 ? m_gops.first().frames.first().epochMs : -1;
}

qint64 LiveBuffer::dropOldest()
{
    QMutexLocker lock(&m_mutex);
    if (m_gops.size() <= 1)
        return 0;
    const qint64 freed = m_gops.first().bytes;
    m_bytes -= freed;
    m_gops.removeFirst();
    return freed;
}

namespace livebuffer {

qint64 rtcpEpochMs(qint64 startRealtimeUs, qint64 ptsSinceStart, int tbNum, int tbDen)
{
    if (startRealtimeUs <= 0 || tbDen <= 0)
        return -1;
    return startRealtimeUs / 1000 + ptsSinceStart * 1000 * tbNum / tbDen;
}

namespace {
// Whether an Annex-B access unit holds a NAL unit of `type` (H.264: low 5 bits; H.265: bits 1–6).
bool hasNal(const QByteArray &au, int type, bool hevc)
{
    const int n = int(au.size());
    for (int i = 0; i + 3 < n; ++i) {
        if (au[i] != 0 || au[i + 1] != 0)
            continue;
        int at = -1;
        if (au[i + 2] == 1)
            at = i + 3;
        else if (au[i + 2] == 0 && i + 4 < n && au[i + 3] == 1)
            at = i + 4;
        if (at < 0 || at >= n)
            continue;
        const int t = hevc ? (uchar(au[at]) >> 1) & 0x3f : uchar(au[at]) & 0x1f;
        if (t == type)
            return true;
        i = at;
    }
    return false;
}
} // namespace

QByteArray withParameterSets(const QByteArray &annexB, const QByteArray &extradata,
                             BcMediaParser::Codec codec)
{
    if (extradata.isEmpty() || codec == BcMediaParser::Codec::Unknown)
        return annexB;
    const bool hevc = codec == BcMediaParser::Codec::H265;
    if (hasNal(annexB, hevc ? 32 : 7, hevc))
        return annexB;
    return extradata + annexB;
}

} // namespace livebuffer

std::shared_ptr<LiveBuffer> LiveBufferPool::buffer(const QString &key)
{
    QMutexLocker lock(&m_mutex);
    auto &b = m_buffers[key];
    if (!b)
        b = std::make_shared<LiveBuffer>(this);
    return b;
}

std::shared_ptr<LiveBuffer> LiveBufferPool::find(const QString &key) const
{
    QMutexLocker lock(&m_mutex);
    return m_buffers.value(key);
}

qint64 LiveBufferPool::bytes() const
{
    QMutexLocker lock(&m_mutex);
    qint64 n = 0;
    for (const auto &b : m_buffers)
        n += b->bytes();
    return n;
}

void LiveBufferPool::enforce()
{
    QMutexLocker lock(&m_mutex);
    qint64 total = 0;
    for (const auto &b : m_buffers)
        total += b->bytes();
    while (total > m_cap) {
        LiveBuffer *oldest = nullptr;
        qint64 oldestMs = -1;
        for (const auto &b : m_buffers) {
            const qint64 ms = b->spareOldestMs();
            if (ms >= 0 && (oldestMs < 0 || ms < oldestMs)) {
                oldestMs = ms;
                oldest = b.get();
            }
        }
        if (!oldest)
            return;   // every buffer is down to one group
        total -= oldest->dropOldest();
    }
}

} // namespace rl
