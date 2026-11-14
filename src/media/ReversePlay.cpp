#include "media/ReversePlay.h"

#include <algorithm>

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

Window windowFrom(qint64 fromMs, int lengthSecs, int slackSecs)
{
    Window w;
    const qint64 second = fromMs / 1000;
    w.startSecs = second - slackSecs;
    w.endSecs = second + lengthSecs;
    return w;
}

ForwardGops forwardGops(const QList<Frame> &window, qint64 fromMs, qint64 ceilingMs, bool last)
{
    ForwardGops g;
    const bool ahead = std::any_of(window.begin(), window.end(), [&](const Frame &f) {
        return f.epochMs >= fromMs && f.epochMs < ceilingMs;
    });
    if (!ahead)
        return g;
    // Start on the last keyframe at or before the playhead, else the first after it.
    qint64 startMs = -1;
    for (const Frame &f : window)
        if (f.key && f.epochMs >= 0 && f.epochMs <= fromMs && f.epochMs > startMs)
            startMs = f.epochMs;
    if (startMs < 0)
        for (const Frame &f : window)
            if (f.key && f.epochMs > fromMs && f.epochMs < ceilingMs) {
                startMs = f.epochMs;
                break;
            }
    if (startMs < 0) {             // frames ahead, no keyframe yet: a longer window
        if (!last)
            g.nextFromMs = fromMs;
        return g;
    }
    QList<QList<Frame>> groups;
    for (const Frame &f : window) {
        if (f.epochMs < startMs || f.epochMs >= ceilingMs)
            continue;
        if (f.key)
            groups.append(QList<Frame>());
        if (groups.isEmpty())
            continue;
        groups.last().append(f);
    }
    if (groups.isEmpty())
        return g;
    if (!last) {
        g.nextFromMs = groups.last().first().epochMs;
        groups.removeLast();
    }
    g.inOrder = std::move(groups);
    return g;
}

qint64 ForwardPacer::waitUs(qint64 epochMs, double speed, qint64 nowUs)
{
    if (epochMs < 0 || speed <= 0)
        return 0;
    const qint64 last = m_lastMs;
    m_lastMs = epochMs;
    auto anchor = [&] {
        m_anchorMs = epochMs;
        m_anchorUs = nowUs;
        m_speed = speed;
        return qint64(0);
    };
    if (m_anchorMs < 0 || speed != m_speed || epochMs < last || epochMs - last > 5000)
        return anchor();
    const qint64 dueUs = m_anchorUs + qint64(double(epochMs - m_anchorMs) * 1000.0 / speed);
    const qint64 wait = dueUs - nowUs;
    if (wait < -1'000'000) {
        if (m_lateMs >= 0 && epochMs - m_lateMs <= 20'000)
            m_tooSlow = true;
        m_lateMs = epochMs;
        return anchor();
    }
    return qBound(qint64(0), wait, qint64(1'000'000));
}

qint64 waitUs(qint64 prevMs, qint64 epochMs, double speed)
{
    if (prevMs < 0 || epochMs < 0 || epochMs >= prevMs || speed <= 0)
        return 0;
    return qMin<qint64>(1'000'000, qint64(double(prevMs - epochMs) * 1000.0 / speed));
}

} // namespace rl::reverse
