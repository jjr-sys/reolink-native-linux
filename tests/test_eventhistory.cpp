#include "device/EventHistory.h"

#include <QTimeZone>
#include <QtTest>

using namespace rl;
using namespace rl::device;

class TestEventHistory : public QObject
{
    Q_OBJECT

private slots:
    // Days follow this computer's local time; the zone is passed in so the test means
    // the same on any machine. Perth is UTC+8 with no daylight saving.
    void localDayIsMidnightToMidnight()
    {
        const QTimeZone perth("Australia/Perth");
        QVERIFY(perth.isValid());
        const TimeSpan d = localDay(QDate(2026, 11, 9), perth);
        // 2026-11-09 00:00 +08:00 = 2026-11-08 16:00 UTC.
        QCOMPARE(d.from, QDateTime(QDate(2026, 11, 8), QTime(16, 0), QTimeZone::UTC).toSecsSinceEpoch());
        QCOMPARE(d.to - d.from, 24 * 3600);

        // 23:59:59 local belongs to that day, 00:00 the next.
        QCOMPARE(dayContaining(d.to - 1, perth).from, d.from);
        QCOMPARE(dayContaining(d.to, perth).from, d.to);
        QCOMPARE(dayContaining(d.from, perth).from, d.from);
    }

    // A daylight-saving day is 23 or 25 hours long, not 24.
    void localDayFollowsDaylightSaving()
    {
        const QTimeZone sydney("Australia/Sydney");
        QVERIFY(sydney.isValid());
        const TimeSpan start = localDay(QDate(2026, 10, 4), sydney); // clocks go forward
        QCOMPARE(start.to - start.from, 23 * 3600);
        const TimeSpan end = localDay(QDate(2026, 4, 5), sydney);    // clocks go back
        QCOMPARE(end.to - end.from, 25 * 3600);
    }

    void gapsBetweenRuns()
    {
        const qint64 minGap = 120;
        // Runs are (started, last seen).
        const QVector<std::pair<qint64, qint64>> runs = {
            {1000, 2000}, {5000, 6000}, {6050, 7000}};
        // Before the first run, between runs; the 50 s restart is not a gap; after the last.
        const QVector<TimeSpan> g = notRunningGaps(runs, 0, 10000, 20000, minGap);
        QCOMPARE(g.size(), 3);
        QCOMPARE(g[0].from, 0);    QCOMPARE(g[0].to, 1000);
        QCOMPARE(g[1].from, 2000); QCOMPARE(g[1].to, 5000);
        QCOMPARE(g[2].from, 7000); QCOMPARE(g[2].to, 10000);
    }

    void gapsClipToWindowAndNow()
    {
        const qint64 minGap = 120;
        // Overlapping runs merge; the window cuts the gaps.
        const QVector<std::pair<qint64, qint64>> runs = {{1000, 3000}, {2500, 4000}};
        QVERIFY(notRunningGaps(runs, 1500, 3800, 99999, minGap).isEmpty());

        // The current run's last heartbeat is up to a minute old: no gap at "now".
        const QVector<std::pair<qint64, qint64>> current = {{1000, 4950}};
        QVERIFY(notRunningGaps(current, 1000, 9000, 5000, minGap).isEmpty());

        // The future is not a gap; nothing ran at all: the whole past is one gap.
        const QVector<TimeSpan> none = notRunningGaps({}, 1000, 9000, 5000, minGap);
        QCOMPARE(none.size(), 1);
        QCOMPARE(none[0].from, 1000);
        QCOMPARE(none[0].to, 5000);
    }

    // A recorder or Frigate server going offline writes Status for each of its cameras,
    // not one row at channel 0 under the recorder's name.
    void outageWritesStatusForEachCamera()
    {
        const QVector<CameraRef> cams = {{0, "Gate"}, {2, "Kitchen"}, {8, "Driveway"}};
        const QVector<EventRecord> rows = statusRows(7, -1, "Farm", false, cams, 5000);
        QCOMPARE(rows.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(rows[i].hostId, 7);
            QCOMPARE(rows[i].channel, cams[i].channel);
            QCOMPARE(rows[i].camera, cams[i].name);
            QCOMPARE(rows[i].type, QStringLiteral("offline"));
            QCOMPARE(rows[i].timestamp, 5000);
        }
        QCOMPARE(statusRows(7, -1, "Farm", true, cams, 5000).first().type, QStringLiteral("online"));

        // One camera: one row.
        const QVector<EventRecord> one = statusRows(7, 2, "Kitchen", false, cams, 5000);
        QCOMPARE(one.size(), 1);
        QCOMPARE(one[0].channel, 2);
        QCOMPARE(one[0].camera, QStringLiteral("Kitchen"));

        // Cameras already offline did not flip with the recorder: no rows for them.
        QVERIFY(statusRows(7, -1, "Farm", false, {}, 5000).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestEventHistory)
#include "test_eventhistory.moc"
