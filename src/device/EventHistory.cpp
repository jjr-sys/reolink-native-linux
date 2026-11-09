#include "EventHistory.h"

#include <QDateTime>

#include <algorithm>

namespace rl::device {

TimeSpan localDay(const QDate &day, const QTimeZone &zone)
{
    return {day.startOfDay(zone).toSecsSinceEpoch(),
            day.addDays(1).startOfDay(zone).toSecsSinceEpoch()};
}

TimeSpan dayContaining(qint64 epoch, const QTimeZone &zone)
{
    return localDay(QDateTime::fromSecsSinceEpoch(epoch, zone).date(), zone);
}

QVector<TimeSpan> notRunningGaps(const QVector<std::pair<qint64, qint64>> &runs, qint64 from,
                                 qint64 to, qint64 now, qint64 minGapSecs)
{
    QVector<TimeSpan> out;
    to = std::min(to, now);
    if (from >= to)
        return out;
    auto sorted = runs;
    std::sort(sorted.begin(), sorted.end());
    auto addGap = [&](qint64 a, qint64 b) {
        a = std::max(a, from);
        b = std::min(b, to);
        if (b - a >= minGapSecs)
            out.append({a, b});
    };
    qint64 covered = from; // everything before this is running or already a gap
    for (const auto &[start, lastSeen] : sorted) {
        if (lastSeen <= covered)
            continue;
        if (start > covered)
            addGap(covered, start);
        covered = std::max(covered, lastSeen);
        if (covered >= to)
            break;
    }
    if (covered < to)
        addGap(covered, to);
    return out;
}

QVector<TimeSpan> storedNotRunningGaps(const Database &db, qint64 from, qint64 to, qint64 now)
{
    const qint64 first = db.firstAppRunStart();
    if (first <= 0)
        return {};
    from = std::max(from, first);
    return notRunningGaps(db.appRuns(from, to), from, to, now, kMinNotRunningSecs);
}

QVector<EventRecord> statusRows(qint64 hostId, int channel, const QString &name, bool online,
                                const QVector<CameraRef> &cameras, qint64 now)
{
    auto row = [&](int ch, const QString &camera) {
        EventRecord r;
        r.hostId = hostId;
        r.channel = ch;
        r.timestamp = now;
        r.type = online ? QStringLiteral("online") : QStringLiteral("offline");
        r.camera = camera;
        return r;
    };
    QVector<EventRecord> out;
    if (channel >= 0) {
        out.append(row(channel, name));
    } else {
        for (const CameraRef &c : cameras)
            out.append(row(c.channel, c.name));
    }
    return out;
}

} // namespace rl::device
