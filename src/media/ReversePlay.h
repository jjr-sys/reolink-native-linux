#pragma once

#include "media/BcMediaParser.h"

#include <QByteArray>
#include <QList>
#include <QSize>
#include <QtGlobal>

#include <utility>

// Reverse play (wayfinder/playback-controls ticket 12). No recorder plays backwards, so the
// app builds it: the recorder client fetches windows of the sub stream behind the playhead
// (cmd 143), cuts each at the playhead, splits it into keyframe groups and sends them newest
// first, each still in decode order; the player decodes a group forwards, holds its frames
// and shows them backwards, paced by their recording times. Times are ms since the epoch.
namespace rl::reverse {

struct Frame {
    bool key = false;
    qint64 epochMs = -1;
    BcMediaParser::Codec codec = BcMediaParser::Codec::Unknown;
    QByteArray annexB;
};

// The recording seconds [startSecs, endSecs] to fetch for the frames before `cutoffMs`:
// `lengthSecs` plus `slackSecs` (a recorder starts a window on a keyframe up to a group
// after the start asked for; measured 3 s on the RLN36), ending at the cutoff's second.
struct Window {
    qint64 startSecs = 0;
    qint64 endSecs = 0;
};
Window windowBefore(qint64 cutoffMs, int lengthSecs = 10, int slackSecs = 4);

// A fetched window, cut: frames at or after `cutoffMs` dropped (already shown, or past the
// playhead: a window runs about a second past its end), frames before the first keyframe
// dropped (they can't be decoded), the rest grouped from each keyframe, newest group first.
// nextCutoffMs is the oldest kept keyframe's time, or -1 when nothing came before the
// cutoff (the start of the recording). Frames before `floorMs` (the selected day's midnight)
// are dropped too: reverse stops at the start of the day.
struct Gops {
    QList<QList<Frame>> newestFirst;
    qint64 nextCutoffMs = -1;
};
Gops reverseGops(const QList<Frame> &window, qint64 cutoffMs, qint64 floorMs = 0);

// Recording time of each frame of a window: only keyframes carry a usable wall-clock time
// (seconds), the others take the frame clock (micros) since their keyframe.
class FrameClock
{
public:
    // -1 until the first keyframe.
    qint64 epochMs(bool key, quint32 timeSecs, quint32 micros);

private:
    qint64 m_keyMs = -1;
    quint32 m_keyMicros = 0;
};

// The player's side: decoded frames in, in the order sent (newest group first, each group
// forwards); each group comes back out reversed once the next group's keyframe arrives,
// or at the end (flush).
template <typename T> class GopReverser
{
public:
    struct Item {
        T frame;
        qint64 epochMs;
    };
    QList<Item> push(T frame, bool key, qint64 epochMs)
    {
        QList<Item> out;
        if (key && !m_group.isEmpty())
            out = flush();
        m_group.append({std::move(frame), epochMs});
        return out;
    }
    QList<Item> flush()
    {
        QList<Item> out;
        out.reserve(m_group.size());
        for (auto it = m_group.rbegin(); it != m_group.rend(); ++it)
            out.append(std::move(*it));
        m_group.clear();
        return out;
    }

private:
    QList<Item> m_group;
};

// HD reverse (wayfinder/live-replay ticket 06): a decoded group is held scaled to fit the
// pane's size on screen in device pixels (never larger than the picture; the whole picture if
// the pane is unknown); a fisheye camera's to fit 1920×1920 whatever the pane, so its dewarped
// views and zoom stay sharp and its memory is capped. Even sizes, aspect kept.
QSize holdSize(QSize decoded, QSize pane, bool fisheye);

// Whether the link keeps up with HD backwards: each window's download time against the time
// its footage plays for (recording seconds ÷ speed). Two windows in a row above 80% mean it
// can't; that stays for the run (HD comes back on the next open).
class LinkPace
{
public:
    void window(double fetchSecs, double playSecs);
    bool tooSlow() const { return m_tooSlow; }

private:
    int m_slowInARow = 0;
    bool m_tooSlow = false;
};

// How long to wait before showing a frame of `epochMs` after one of `prevMs` (backwards):
// the gap in recording time divided by the speed; at once for the first frame or an
// out-of-order one; at most a second.
qint64 waitUs(qint64 prevMs, qint64 epochMs, double speed);

} // namespace rl::reverse
