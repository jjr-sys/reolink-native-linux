#pragma once

#include <QDateTime>
#include <QRegularExpression>
#include <QString>

#include <vector>

namespace rl {

// The parts of the downloads queue that are pure decisions (which item may start next,
// what to call the file), kept free of Qt event loops and the network so they can be
// tested on their own.

enum class DlState { Queued, Preparing, Downloading, Done, Failed, Cancelled };

inline bool dlActive(DlState s) { return s == DlState::Preparing || s == DlState::Downloading; }

struct DlSlot {
    qint64 hostId = -1;
    DlState state = DlState::Queued;
};

// The oldest queued item whose NVR is idle, or -1. An NVR serves one download at a time
// (it is connection-limited and cuts each clip before sending it); different NVRs are
// independent, so they run in parallel.
inline int nextRunnable(const std::vector<DlSlot> &items)
{
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].state != DlState::Queued)
            continue;
        bool busy = false;
        for (const DlSlot &o : items)
            if (o.hostId == items[i].hostId && dlActive(o.state)) {
                busy = true;
                break;
            }
        if (!busy)
            return int(i);
    }
    return -1;
}

// "Woorabinda_Kitchen_20260919_142000.mp4", or "..._part2.mp4" when the NVR splits a range
// across several recording files. The site is in the name because two sites can have a
// camera with the same name.
inline QString downloadFileName(const QString &site, const QString &camera, const QDateTime &start,
                                int part = 1, int parts = 1)
{
    static const QRegularExpression bad(QStringLiteral("[^\\w-]+"));
    auto clean = [](QString s) {
        s.replace(bad, QStringLiteral("_"));
        while (s.startsWith(u'_')) s.remove(0, 1);
        while (s.endsWith(u'_')) s.chop(1);
        return s.isEmpty() ? QStringLiteral("camera") : s;
    };
    QString name = clean(site) + u'_' + clean(camera) + u'_'
                   + start.toString(QStringLiteral("yyyyMMdd_HHmmss"));
    if (parts > 1)
        name += QStringLiteral("_part%1").arg(part);
    return name + QStringLiteral(".mp4");
}

// How long to wait for the NVR to cut a clip before replying. Measured at about 1.6 s per
// minute of footage; allow six times that, with a floor for short clips.
inline long prepareTimeoutSecs(qint64 rangeSecs)
{
    const long t = 60 + long(rangeSecs / 10);
    return t > 3600 ? 3600 : t;
}

} // namespace rl
