#include "media/ReversePlay.h"

namespace rl::reverse {

Window windowBefore(qint64 cutoffMs, int lengthSecs, int slackSecs)
{
    Window w;
    w.endSecs = (cutoffMs + 999) / 1000;
    w.startSecs = w.endSecs - lengthSecs - slackSecs;
    return w;
}

Gops reverseGops(const QList<Frame> &window, qint64 cutoffMs, qint64 floorMs)
{
    Gops g;
    QList<QList<Frame>> forward;
    for (const Frame &f : window) {
        if (f.epochMs < floorMs || f.epochMs >= cutoffMs)
            continue;
        if (f.key)
            forward.append(QList<Frame>());
        if (forward.isEmpty())
            continue; // before the first keyframe
        forward.last().append(f);
    }
    if (forward.isEmpty())
        return g;
    g.nextCutoffMs = forward.first().first().epochMs;
    for (auto it = forward.rbegin(); it != forward.rend(); ++it)
        g.newestFirst.append(std::move(*it));
    return g;
}

qint64 FrameClock::epochMs(bool key, quint32 timeSecs, quint32 micros)
{
    if (key && timeSecs != 0) {
        m_keyMs = qint64(timeSecs) * 1000;
        m_keyMicros = micros;
        return m_keyMs;
    }
    if (m_keyMs < 0)
        return -1;
    return m_keyMs + qMax<qint64>(0, qint64(micros) - qint64(m_keyMicros)) / 1000;
}

qint64 waitUs(qint64 prevMs, qint64 epochMs, double speed)
{
    if (prevMs < 0 || epochMs < 0 || epochMs >= prevMs || speed <= 0)
        return 0;
    return qMin<qint64>(1'000'000, qint64(double(prevMs - epochMs) * 1000.0 / speed));
}

} // namespace rl::reverse
