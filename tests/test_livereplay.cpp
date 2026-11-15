#include "ui/LiveReplay.h"

#include <QSignalSpy>
#include <QtTest>

using namespace rl;

namespace {

double at(int y, int m, int d, int h, int min = 0, int s = 0)
{
    return double(QDateTime(QDate(y, m, d), QTime(h, min, s)).toSecsSinceEpoch());
}

// A day's recordings as the device search answers them: seconds into the day.
QVariantList segs(std::initializer_list<std::pair<int, int>> spans)
{
    QVariantList out;
    for (const auto &[s, e] : spans) {
        QVariantMap m;
        m["start"] = double(s);
        m["end"] = double(e);
        out << m;
    }
    return out;
}

QVariantList days(std::initializer_list<int> ds)
{
    QVariantList out;
    for (int d : ds)
        out << d;
    return out;
}

// Answer every search the controller asks for from `book` (date -> segments) and the
// month's recording days from `months` (year*100+month -> days); unknown dates are empty.
struct Recorder {
    LiveReplay &r;
    QHash<QDate, QVariantList> book;
    QHash<int, QVariantList> months;
    QList<QDate> asked;   // not answered yet
    QList<QDate> ever;    // every search asked for
    explicit Recorder(LiveReplay &replay) : r(replay)
    {
        QObject::connect(&r, &LiveReplay::searchWanted, &r, [this](int y, int m, int d) {
            asked << QDate(y, m, d);
            ever << QDate(y, m, d);
        });
    }
    // Answer the searches asked so far (and any they lead to), in order.
    void answerAll(int limit = 100)
    {
        while (!asked.isEmpty() && limit-- > 0) {
            const QDate d = asked.takeFirst();
            r.dayFound(d.year(), d.month(), d.day(), book.value(d),
                       months.value(d.year() * 100 + d.month()));
        }
    }
};

const QDate kToday(2026, 11, 15);
const double kNow = at(2026, 11, 15, 10, 0);

} // namespace

// Live replay (wayfinder/live-replay ticket 07): one pane at a time steps back from now into
// its recorder's footage, across days, as far back as storage holds.
class TestLiveReplay : public QObject
{
    Q_OBJECT
private slots:
    // ---- the "time behind live" slider: a stretched scale ----
    // Marks at now · 1 min · 10 min · 1 h · 1 day · 1 week · oldest, evenly spaced; linear
    // between marks; marks past the oldest dropped.
    void sliderMarksAreEvenlySpaced()
    {
        const double oldest = 30 * 86400.0;   // 7 marks: 6 equal spans
        QCOMPARE(LiveReplay::posForBehind(0, oldest), 0.0);
        QCOMPARE(LiveReplay::posForBehind(60, oldest), 1.0 / 6);
        QCOMPARE(LiveReplay::posForBehind(3600, oldest), 3.0 / 6);
        QCOMPARE(LiveReplay::posForBehind(604800, oldest), 5.0 / 6);
        QCOMPARE(LiveReplay::posForBehind(oldest, oldest), 1.0);
        QCOMPARE(LiveReplay::posForBehind(330, oldest), 1.5 / 6);   // halfway 1 → 10 min
        QCOMPARE(LiveReplay::posForBehind(-5, oldest), 0.0);         // clamped
        QCOMPARE(LiveReplay::posForBehind(oldest * 2, oldest), 1.0);
    }
    void sliderDropsMarksPastTheOldest()
    {
        const double oldest = 2 * 3600.0;     // now · 1 min · 10 min · 1 h · oldest: 4 spans
        QCOMPARE(LiveReplay::posForBehind(3600, oldest), 0.75);
        QCOMPARE(LiveReplay::posForBehind(5400, oldest), 0.875);
        QCOMPARE(LiveReplay::posForBehind(60, 0), 0.0);              // nothing known
    }
    void sliderRoundTrips()
    {
        const double oldest = 40 * 86400.0;
        for (double behind : {0.0, 30.0, 61.0, 900.0, 7200.0, 90000.0, 700000.0, oldest}) {
            const double back = LiveReplay::behindForPos(LiveReplay::posForBehind(behind, oldest), oldest);
            QVERIFY2(qAbs(back - behind) < 0.01, qPrintable(QString::number(behind)));
        }
        QCOMPARE(LiveReplay::behindForPos(1.5, oldest), oldest);
        QCOMPARE(LiveReplay::behindForPos(-1, oldest), 0.0);
    }

    // ---- starting, the one replaying pane, back to live ----
    void startAsksForTodayFirstAndOnePaneReplays()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        QSignalSpy changed(&r, &LiveReplay::replayingChanged);
        r.start(QStringLiteral("5:0"));
        QVERIFY(r.replaying());
        QCOMPARE(r.cameraKey(), QStringLiteral("5:0"));
        QVERIFY(!rec.asked.isEmpty());
        QCOMPARE(rec.asked.first(), kToday);
        r.start(QStringLiteral("1:6"));          // another pane: the first goes back to live
        QCOMPARE(r.cameraKey(), QStringLiteral("1:6"));
        r.stop();
        QVERIFY(!r.replaying());
        QCOMPARE(r.cameraKey(), QString());
        QCOMPARE(changed.count(), 3);
    }
    // A new camera starts with a fresh day book: one camera's recordings are not another's.
    void anotherCameraForgetsTheRecordings()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        QVERIFY(r.covered(at(2026, 11, 15, 1, 30)));
        r.start(QStringLiteral("1:6"));
        QVERIFY(!r.covered(at(2026, 11, 15, 1, 30)));
    }

    // ---- where a play lands ----
    // ◀ from now: today's newest footage ends before now (the recorder's lag), so backwards
    // starts from the end of the last recording.
    void backwardsFromNowStartsAtTheNewestFootage()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}, {28800, 35900}});   // ends 09:58:20
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.start(QStringLiteral("5:0"));
        r.playFrom(kNow, -1);
        QCOMPARE(play.count(), 0);                 // today not known yet: waits
        rec.answerAll(1);
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 9, 58, 20));
        QCOMPARE(r.playhead(), at(2026, 11, 15, 9, 58, 20));
        QCOMPARE(r.dayEpoch(), at(2026, 11, 15, 0));
        QCOMPARE(r.daySegments().size(), 2);
    }
    // Inside a recording: there. In a gap: forwards to the next recording, backwards to the
    // end of the previous one.
    void aPlayInAGapSnapsToFootage()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}, {28800, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(at(2026, 11, 15, 1, 30), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 1, 30));
        r.playFrom(at(2026, 11, 15, 4, 0), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 8, 0));
        r.playFrom(at(2026, 11, 15, 4, 0), -1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 2, 0));
    }
    // Forwards with nothing ahead before now (← 10 s from live, or a drag past the newest
    // footage): the last 10 s of the newest footage instead.
    void aForwardPlayPastTheNewestFootagePlaysItsLast10s()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{28800, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(kNow - 10, 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 9, 58, 10));
    }

    // ---- carrying on when a stream ends (across days) ----
    // Backwards at a recording's start: the previous recording's end, yesterday if need be
    // (asked for when not known), skipping days the month says have none.
    void backwardsCarriesOnIntoEarlierDays()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({12, 13, 15});
        rec.book[kToday] = segs({{28800, 35900}});
        rec.book[QDate(2026, 11, 13)] = segs({{72000, 86400}});
        rec.book[QDate(2026, 11, 12)] = segs({{0, 86400}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(at(2026, 11, 15, 9, 0), -1);
        r.frameAt(at(2026, 11, 15, 8, 0), 0);
        r.carryOn(-1);                                 // at 08:00, the recording's start
        QCOMPARE(play.count(), 1);                     // waits for the 13th (14th has none)
        rec.answerAll();
        QCOMPARE(play.count(), 2);
        // The 13th's end, at midnight: played from its own day's last second.
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 13, 23, 59, 59));
        QCOMPARE(r.dayEpoch(), at(2026, 11, 13, 0));
        // ...and from there backwards it carries on within the 13th.
        r.frameAt(at(2026, 11, 13, 20, 0), 0);
        r.carryOn(-1);
        rec.answerAll();
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 12, 23, 59, 59));
        QVERIFY(!rec.ever.contains(QDate(2026, 11, 14)));
    }
    // A recording file that runs over midnight is listed on its last day only, clipped to start
    // at 00:00 (its real start comes with it). Backwards from that day's start carries straight
    // on into the day before at 23:59:59, where the file shows as footage; forwards from the
    // day before's last second carries on into the next day.
    void aRecordingOverMidnightCarriesOnBothWays()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({14, 15});
        rec.book[QDate(2026, 11, 14)] = segs({{0, 82802}});              // ends 23:00:02
        QVariantList overMidnight = segs({{0, 3600}});                    // 23:00:02 → 01:00
        QVariantMap m = overMidnight.first().toMap();
        m["startEpoch"] = at(2026, 11, 14, 23, 0, 2);
        overMidnight[0] = m;
        rec.book[kToday] = overMidnight;
        r.start(QStringLiteral("5:0"));
        rec.answerAll();
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.group()->playBackward();
        r.playFrom(at(2026, 11, 15, 0, 0, 30), -1);
        r.frameAt(at(2026, 11, 15, 0, 0, 0) + 0.2, 0);    // the run stopped at the day's start
        r.carryOn(-1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 14, 23, 59, 59));
        QCOMPARE(r.dayEpoch(), at(2026, 11, 14, 0));
        QVERIFY(r.covered(at(2026, 11, 14, 23, 30)));
        const QVariantMap last = r.daySegments().last().toMap();
        QCOMPARE(last["start"].toDouble(), 82802.0);
        QCOMPARE(last["end"].toDouble(), 86400.0);
        r.group()->playForward();
        r.frameAt(at(2026, 11, 14, 23, 59, 59) + 0.6, 0);
        r.carryOn(1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 0));
        QCOMPARE(r.dayEpoch(), at(2026, 11, 15, 0));
        // A run that ends again at the same place (the recorder refused it) isn't reopened
        // there forever: the second time it goes to the previous recording's end.
        r.group()->playBackward();
        r.playFrom(at(2026, 11, 15, 0, 0, 30), -1);
        r.frameAt(at(2026, 11, 15, 0, 0, 0) + 0.2, 0);
        r.carryOn(-1);
        r.frameAt(at(2026, 11, 14, 23, 59, 59), 0);
        r.carryOn(-1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 14, 23, 0, 2));
    }
    // Forwards at a day's end: the next day's first recording.
    void forwardsCarriesOnIntoTheNextDay()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({14, 15});
        rec.book[QDate(2026, 11, 14)] = segs({{80000, 86400}});
        rec.book[kToday] = segs({{1800, 7200}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll();
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(at(2026, 11, 14, 23, 0), 1);
        rec.answerAll();
        r.frameAt(at(2026, 11, 14, 23, 59, 59) + 0.9, 0);
        r.carryOn(1);                                  // the 14th ran out at midnight
        rec.answerAll();
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 0, 30));
    }
    // Forwards with nothing ahead before now: back to live (the recorder's newest footage).
    void forwardsAtTheNewestFootageReturnsToLive()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{28800, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        r.playFrom(at(2026, 11, 15, 9, 50), 1);
        r.frameAt(at(2026, 11, 15, 9, 58, 20), 0);
        QSignalSpy live(&r, &LiveReplay::replayingChanged);
        r.carryOn(1);
        QVERIFY(!r.replaying());
        QCOMPARE(live.count(), 1);
    }
    // Backwards past the oldest recording: nothing earlier (the page pauses there).
    void backwardsAtTheOldestSaysNothingEarlier()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({15});
        rec.months[202610] = days({});
        rec.book[kToday] = segs({{28800, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll();
        QVERIFY(r.oldestFound());
        QSignalSpy none(&r, &LiveReplay::nothingEarlier);
        r.playFrom(at(2026, 11, 15, 9, 0), -1);
        r.frameAt(at(2026, 11, 15, 8, 0), 0);
        r.carryOn(-1);
        rec.answerAll();
        QCOMPARE(none.count(), 1);
        QVERIFY(r.replaying());
    }

    // ---- the oldest recording ----
    // Month by month backwards from today's month until one with no recordings; the oldest
    // is the first recording of the earliest day of the earliest month with any.
    void theOldestIsFoundMonthByMonth()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({1, 2, 15});
        rec.months[202610] = days({20, 31});
        rec.months[202609] = days({});
        rec.book[kToday] = segs({{28800, 35900}});
        rec.book[QDate(2026, 10, 20)] = segs({{43200, 50000}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll();
        QVERIFY(r.oldestFound());
        QCOMPARE(r.oldest(), at(2026, 10, 20, 12, 0));
        QVERIFY(rec.asked.isEmpty());
    }
    // Until then the oldest known recording stands in.
    void beforeTheScanEndsTheOldestKnownStandsIn()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({15});
        rec.book[kToday] = segs({{28800, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        QVERIFY(!r.oldestFound());
        QCOMPARE(r.oldest(), at(2026, 11, 15, 8, 0));
    }

    // ---- the strip of recordings along the slider ----
    void stripShowsKnownRecordingsAsSliderSpans()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({15});
        rec.months[202610] = days({});
        rec.book[kToday] = segs({{28800, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll();
        const QVariantList strip = r.strip();
        QCOMPARE(strip.size(), 1);
        const QVariantMap span = strip.first().toMap();
        // From 100 s behind (09:58:20) to the oldest (08:00) at the far end.
        QCOMPARE(span["from"].toDouble(), LiveReplay::posForBehind(100, 7200));
        QCOMPARE(span["to"].toDouble(), 1.0);
    }

    // ---- back to live at the recorder's edge (recorder-paced forwards stalls there) ----
    // Forwards, the picture's time stops moving for 8 s within 5 min of now: back to live. Not
    // before a first picture, not paused, not backwards, not far from now.
    void aForwardStallNearNowReturnsToLive()
    {
        LiveReplay r;
        r.setNow(kNow);
        r.start(QStringLiteral("5:0"));
        r.opened();
        QVERIFY(!r.stalledAtEdge(10'000));             // no picture yet
        r.frameAt(kNow - 120, 10'000);
        r.frameAt(kNow - 119, 11'000);
        QVERIFY(!r.stalledAtEdge(14'000));
        QVERIFY(!r.stalledAtEdge(18'000));            // a hiccup (or a window refill)
        QVERIFY(r.stalledAtEdge(19'500));
        r.setPaused(true);
        QVERIFY(!r.stalledAtEdge(19'500));
        r.setPaused(false);
        r.group()->playBackward();
        QVERIFY(!r.stalledAtEdge(19'500));
        r.group()->playForward();
        r.frameAt(kNow - 3600, 20'000);                // an hour back: a gap, not the edge
        QVERIFY(!r.stalledAtEdge(30'000));
    }
    // A failed search (twice) counts as a day with no recordings, but leaves what its month
    // is known to have alone: replay still reaches earlier days.
    void aFailedSearchDoesNotHideItsMonth()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({14, 15});
        rec.months[202610] = days({});
        rec.book[QDate(2026, 11, 14)] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        QSignalSpy play(&r, &LiveReplay::playWanted);
        QSignalSpy none(&r, &LiveReplay::nothingEarlier);
        r.playFrom(kNow, -1);
        QCOMPARE(rec.asked.takeFirst(), kToday);      // today's search fails twice
        r.dayFailed(2026, 11, 15);
        QCOMPARE(rec.asked.takeFirst(), kToday);
        r.dayFailed(2026, 11, 15);
        rec.answerAll();
        QCOMPARE(none.count(), 0);
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 14, 2, 0));
        QVERIFY(r.oldestFound());
        QCOMPARE(r.oldest(), at(2026, 11, 14, 1, 0));
    }
    // A recording that starts the evening before (a file over midnight, listed on its last
    // day) lands on its own day when played forwards to it.
    void forwardsToAFileStartingTheDayBeforeLandsOnThatDay()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({14, 15});
        rec.book[QDate(2026, 11, 14)] = segs({{0, 3600}});
        QVariantList overMidnight = segs({{0, 3600}});
        QVariantMap m = overMidnight.first().toMap();
        m["startEpoch"] = at(2026, 11, 14, 23, 0);
        overMidnight[0] = m;
        rec.book[kToday] = overMidnight;
        r.start(QStringLiteral("5:0"));
        rec.answerAll();
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(at(2026, 11, 14, 12, 0), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 14, 23, 0));
        QCOMPARE(r.dayEpoch(), at(2026, 11, 14, 0));
    }
    // Before going back to live, today's recordings are asked for again when the last answer
    // is old: a long replay may have more footage ahead than at the start.
    void forwardsRefreshesTodayBeforeGoingLive()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{28800, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        r.playFrom(at(2026, 11, 15, 9, 50), 1);
        r.setNow(kNow + 600);                         // ten minutes later
        rec.book[kToday] = segs({{28800, 36400}});    // the recording went on to 10:06:40
        r.frameAt(at(2026, 11, 15, 9, 58, 20), 0);
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.carryOn(1);
        QVERIFY(r.replaying());                       // asked again first
        rec.answerAll(1);
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 9, 58, 21));
        r.frameAt(at(2026, 11, 15, 10, 6, 40), 0);
        r.carryOn(1);                                 // just asked: back to live
        QVERIFY(!r.replaying());
    }
    // 10 s on: within footage, there; past the newest footage, back to live (not its last 10 s
    // again); 10 s back across a gap: the previous recording's end.
    void skipMovesTenSecondsOrGoesLive()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}, {28800, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(at(2026, 11, 15, 9, 0), 1);
        r.skipBy(10);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 9, 0, 10));
        r.playFrom(at(2026, 11, 15, 8, 0, 5), 1);
        r.skipBy(-10);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 2, 0));
        r.playFrom(at(2026, 11, 15, 9, 58, 15), 1);
        r.skipBy(10);
        QVERIFY(!r.replaying());
    }
    // "N behind live" as the pane shows it.
    void behindTextReads()
    {
        QCOMPARE(LiveReplay::behindText(42), QStringLiteral("42 s"));
        QCOMPARE(LiveReplay::behindText(100), QStringLiteral("1 min 40 s"));
        QCOMPARE(LiveReplay::behindText(7260), QStringLiteral("2 h 1 min"));
        QCOMPARE(LiveReplay::behindText(90000), QStringLiteral("1 d 1 h"));
        QCOMPARE(LiveReplay::sliderMarks(), (QVariantList{0.0, 60.0, 600.0, 3600.0, 86400.0, 604800.0}));
    }
    void behindFollowsThePlayheadAndTheClock()
    {
        LiveReplay r;
        r.setNow(kNow);
        r.start(QStringLiteral("5:0"));
        r.frameAt(kNow - 90, 0);
        QCOMPARE(r.playhead(), kNow - 90);
        QCOMPARE(r.behind(), 90.0);
        r.setNow(kNow + 10);
        QCOMPARE(r.behind(), 100.0);
    }
};

QTEST_GUILESS_MAIN(TestLiveReplay)
#include "test_livereplay.moc"
