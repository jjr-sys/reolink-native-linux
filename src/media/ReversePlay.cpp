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

QSize holdSize(QSize decoded, QSize pane, bool fisheye)
{
    if (decoded.isEmpty())
        return decoded;
    const QSize box = fisheye ? QSize(1920, 1920) : pane;
    if (box.isEmpty())
        return decoded;
    const double k = qMin(1.0, qMin(double(box.width()) / decoded.width(),
                                    double(box.height()) / decoded.height()));
    if (k >= 1.0)
        return decoded;
    return QSize(int(decoded.width() * k) & ~1, int(decoded.height() * k) & ~1);
}

void LinkPace::window(double fetchSecs, double playSecs)
{
    if (playSecs <= 0)
        return;
    m_slowInARow = fetchSecs > 0.8 * playSecs ? m_slowInARow + 1 : 0;
    if (m_slowInARow >= 2)
        m_tooSlow = true;
}

qint64 waitUs(qint64 prevMs, qint64 epochMs, double speed)
{
    if (prevMs < 0 || epochMs < 0 || epochMs >= prevMs || speed <= 0)
        return 0;
    return qMin<qint64>(1'000'000, qint64(double(prevMs - epochMs) * 1000.0 / speed));
}

} // namespace rl::reverse
