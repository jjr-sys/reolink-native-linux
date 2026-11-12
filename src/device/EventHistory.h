#pragma once

#include "core/Database.h"

#include <QDate>
#include <QString>
#include <QTimeZone>
#include <QVector>

#include <utility>

namespace rl::device {

// How much Events history is kept (wayfinder/events-feed, "Decide how long events are kept
// after the switch-offs"): motion for 24 hours outside the cap; everything else, Status
// included, for 30 days under a per-recorder cap that is only a safety net.
inline constexpr qint64 kMotionKeepSecs = 24 * 3600;
inline constexpr qint64 kOtherKeepSecs = 30 * 24 * 3600;
inline constexpr int kCapPerRecorder = 50000;
// How often the running app stamps its run, and the shortest gap between runs shown as
// "app not running" (a quick restart is not one).
inline constexpr qint64 kRunHeartbeatSecs = 60;
inline constexpr qint64 kMinNotRunningSecs = 2 * kRunHeartbeatSecs;

// A half-open span of Unix seconds, [from, to).
struct TimeSpan
{
    qint64 from = 0;
    qint64 to = 0;
};

// One day, midnight to midnight in `zone` (23 or 25 hours on a daylight-saving change).
TimeSpan localDay(const QDate &day, const QTimeZone &zone);
// The local day `epoch` falls in.
TimeSpan dayContaining(qint64 epoch, const QTimeZone &zone);

// The parts of [from, to) that no run covers. A run is (started, last seen). Nothing
// after `now` is a gap, and gaps shorter than `minGapSecs` are dropped.
QVector<TimeSpan> notRunningGaps(const QVector<std::pair<qint64, qint64>> &runs, qint64 from,
                                 qint64 to, qint64 now, qint64 minGapSecs);

// The "app not running" spans of [from, to) from the store's runs. Time before the first
// recorded run is unknown (events from a build that kept no runs), so it is never a gap.
QVector<TimeSpan> storedNotRunningGaps(const Database &db, qint64 from, qint64 to, qint64 now);

// A camera on a host, as Status rows name it.
struct CameraRef
{
    int channel = 0;
    QString name;
};

// The Status rows for a connectivity change. channel >= 0 is one camera; channel < 0 is
// the whole host, which writes a row for each camera that flipped with it (`cameras`;
// none when every camera was already in that state).
QVector<EventRecord> statusRows(qint64 hostId, int channel, const QString &name, bool online,
                                const QVector<CameraRef> &cameras, qint64 now);

} // namespace rl::device
