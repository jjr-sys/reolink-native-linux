#include "ui/LiveReplay.h"

#include <QSignalSpy>
#include <QtTest>

#include <algorithm>

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
    QStringList askedBy;  // which camera asked each (in step with `asked`)
    QList<QDate> ever;    // every search asked for
    explicit Recorder(LiveReplay &replay) : r(replay)
    {
        QObject::connect(&r, &LiveReplay::searchWanted, &r,
                         [this](const QString &key, int y, int m, int d) {
            asked << QDate(y, m, d);
            askedBy << key;
            ever << QDate(y, m, d);
        });
    }
    // Answer the searches asked so far (and any they lead to), in order.
    void answerAll(int limit = 100)
    {
        while (!asked.isEmpty() && limit-- > 0) {
            const QDate d = asked.takeFirst();
            const QString key = askedBy.takeFirst();
            r.dayFound(key, d.year(), d.month(), d.day(), book.value(d),
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
        QCOMPARE(changed.count(), 4);   // 5:0, back to live, 1:6, back to live
    }
    // ● Live, or replay moving to another pane, lets the replaying pane go before the group
    // turns back to forwards (live-replay 08d): turning while that pane still replays made its
    // recording open a forward stream just before it closed.
    void backToLiveLetsThePaneGoBeforeTheGroupTurnsForwards()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        // What QML sees, in order: the camera replaying (replayingChanged also notifies
        // cameraKey) and each turn of the group.
        QStringList seen;
        connect(&r, &LiveReplay::replayingChanged, this,
                [&] { seen << QStringLiteral("pane ") + r.cameraKey(); });
        connect(r.group(), &PlaybackGroup::speedChanged, this,
                [&] { seen << QStringLiteral("turn"); });
        r.start(QStringLiteral("5:0"));
        r.group()->playBackward();
        seen.clear();
        r.stop();
        QCOMPARE(seen.first(), QStringLiteral("pane "));
        QVERIFY(seen.contains(QStringLiteral("turn")));

        r.start(QStringLiteral("5:0"));
        r.group()->playBackward();
        seen.clear();
        r.start(QStringLiteral("1:6"));
        QCOMPARE(seen.first(), QStringLiteral("pane "));
        QVERIFY(seen.indexOf(QStringLiteral("turn")) < seen.indexOf(QStringLiteral("pane 1:6")));
        QCOMPARE(r.cameraKey(), QStringLiteral("1:6"));
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
    // Pausing (the doorbell, ticket 14) drops a play still waiting for the recorder: the answer
    // fills the day book but doesn't start the video under the "paused" bar.
    void pausingDropsAPlayStillWaitingForTheRecorder()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}, {28800, 35900}});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.start(QStringLiteral("5:0"));
        r.playFrom(kNow, -1);
        r.setPaused(true);
        rec.answerAll();
        QCOMPARE(play.count(), 0);
        QCOMPARE(r.daySegments().size(), 2);      // the day book still fills
        r.setPaused(false);                        // resuming plays from the playhead
        r.playFrom(r.playhead(), -1);
        QCOMPARE(play.count(), 1);
    }
    // ---- the live buffer (ticket 08b) ----
    // With the pane's live buffer, ◀ from now starts at once at the buffer's newest frame (the
    // camera's clock may run a little ahead of or behind this machine's), no search needed.
    void backwardsFromNowStartsInTheLiveBuffer()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 35900}});   // the recorder's newest: 09:58:20
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.start(QStringLiteral("5:0"));
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 240}, {"end", kNow - 1}}});
        r.playFrom(kNow, -1);
        QCOMPARE(play.count(), 1);                  // no waiting for today's search
        QCOMPARE(play.last().at(0).toDouble(), kNow - 1);
        QVERIFY(r.inBuffer(r.playhead()));
        // A camera clock ahead of this machine's: from now, still the buffer.
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 240}, {"end", kNow + 0.8}}});
        r.playFrom(kNow, -1);
        QCOMPARE(play.last().at(0).toDouble(), kNow);
    }

    // The buffer counts as footage: on the bar's strip and in the day's segments (so the pane
    // doesn't call it a gap); backwards from its oldest frame the recorder's footage follows,
    // where it overlaps straight on, else from the recorder's newest.
    void backwardsFromTheBuffersOldestCarriesOnIntoTheRecorder()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 35900}});   // to 09:58:20 = now − 100 s
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 60}, {"end", kNow}}});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(kNow, -1);
        QVERIFY(r.covered(kNow - 30));
        QCOMPARE(r.daySegments().size(), 2);
        QCOMPARE(r.strip().size(), 2);
        r.frameAt(r.cameraKey(), kNow - 60, 0);
        // the buffer's oldest frame: a gap behind it
        r.carryOn(r.cameraKey(), -1);
        QCOMPARE(play.last().at(0).toDouble(), kNow - 100);   // the recorder's newest footage
        QVERIFY(!r.inBuffer(r.playhead()));
        // Overlapping (RLN36: the recorder is only seconds behind): straight on.
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 120}, {"end", kNow}}});
        r.playFrom(kNow, -1);
        r.frameAt(r.cameraKey(), kNow - 120, 0);
        r.carryOn(r.cameraKey(), -1);
        QCOMPARE(play.last().at(0).toDouble(), kNow - 121);
    }

    // Forwards from before the buffer with nothing recorded between: on into the buffer (the
    // RLN8 keeps the newest minutes from its search), not back to the recorder's last 10 s.
    void forwardsIntoTheBufferFromAGapBeforeIt()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 35700}});   // to now − 300 s
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 200}, {"end", kNow}}});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(kNow - 250, 1);
        QCOMPARE(play.last().at(0).toDouble(), kNow - 200);
        // ...and from the recorder's end, carrying on.
        r.playFrom(kNow - 310, 1);
        r.frameAt(r.cameraKey(), kNow - 300, 0);
        r.carryOn(r.cameraKey(), 1);
        QCOMPARE(play.last().at(0).toDouble(), kNow - 200);
        // Backwards from the buffer's start with a gap behind: the recorder's end.
        r.frameAt(r.cameraKey(), kNow - 200, 0);
        r.carryOn(r.cameraKey(), -1);
        QCOMPARE(play.last().at(0).toDouble(), kNow - 300);
    }

    // A slow stream (Farm's sub stream arrives up to 8 s after its camera time, ticket 08a):
    // ◀ from now still starts at the buffer's newest frame, not at the recorder.
    void backwardsFromNowOnASlowStreamStillStartsInTheBuffer()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 35700}});
        r.start(QStringLiteral("2:2"));
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 200}, {"end", kNow - 9}}});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(kNow, -1);
        QCOMPARE(play.last().at(0).toDouble(), kNow - 9);
    }

    // Backwards out of the buffer's newest run into an older one (the SD footage before a
    // maximise, kept by ticket 08b) — even across a break of several seconds — before the
    // recorder.
    void backwardsCarriesOnIntoAnOlderBufferRun()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 35700}});   // to now − 300 s
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 200}, {"end", kNow - 60}},
                                      QVariantMap{{"start", kNow - 52}, {"end", kNow}}});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(kNow, -1);
        r.frameAt(r.cameraKey(), kNow - 52, 0);
        r.carryOn(r.cameraKey(), -1);
        QCOMPARE(play.last().at(0).toDouble(), kNow - 60);   // the older run's newest
    }

    // Forwards in the recorder's footage, reaching the buffer: the buffer takes over there
    // (a reopen), rather than the recorder running on to its stall at the newest footage.
    void forwardsFromTheRecorderSwitchesToTheBufferOnReachingIt()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 35880}});   // to now − 120 s
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 150}, {"end", kNow}}});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        QSignalSpy pane(&r, &LiveReplay::paneWanted);
        r.playFrom(kNow - 400, 1);                  // the recorder
        QCOMPARE(play.count(), 1);
        r.frameAt(r.cameraKey(), kNow - 200, 0);
        QCOMPARE(pane.count(), 0);
        r.frameAt(r.cameraKey(), kNow - 150, 0);    // the buffer's oldest frame: only this pane
        QCOMPARE(play.count(), 1);
        QCOMPARE(pane.count(), 1);
        QCOMPARE(pane.last().at(0).toString(), QStringLiteral("5:0"));
        QCOMPARE(pane.last().at(1).toDouble(), kNow - 150);
        QCOMPARE(r.playhead(), kNow - 150);
        r.frameAt(r.cameraKey(), kNow - 140, 0);    // now playing the buffer: no more reopens
        QCOMPARE(pane.count(), 1);
    }

    // Forwards, the buffer's end is now: playing into it returns the pane to live, rather than
    // reopening at each newest second as live keeps filling it.
    void forwardsAtTheBuffersNewestFrameReturnsToLive()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 35900}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll(1);
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 60}, {"end", kNow - 0.4}}});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(kNow - 30, 1);
        QCOMPARE(play.count(), 1);
        // the stream ended near the newest frame
        r.frameAt(r.cameraKey(), kNow - 1.2, 0);
        r.carryOn(r.cameraKey(), 1);
        QVERIFY(!r.replaying());
        // Further back in the buffer, a stream that ended carries on in it.
        r.start(QStringLiteral("5:0"));
        rec.answerAll();
        r.setBufferSpans(r.cameraKey(),
                          QVariantList{QVariantMap{{"start", kNow - 60}, {"end", kNow - 0.4}}});
        r.playFrom(kNow - 30, 1);
        r.frameAt(r.cameraKey(), kNow - 20, 0);
        r.carryOn(r.cameraKey(), 1);
        QVERIFY(r.replaying());
        QCOMPARE(play.last().at(0).toDouble(), kNow - 19);
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
        r.frameAt(r.cameraKey(), at(2026, 11, 15, 8, 0), 0);
        // at 08:00, the recording's start
        r.carryOn(r.cameraKey(), -1);
        QCOMPARE(play.count(), 1);                     // waits for the 13th (14th has none)
        rec.answerAll();
        QCOMPARE(play.count(), 2);
        // The 13th's end, at midnight: played from its own day's last second.
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 13, 23, 59, 59));
        QCOMPARE(r.dayEpoch(), at(2026, 11, 13, 0));
        // ...and from there backwards it carries on within the 13th.
        r.frameAt(r.cameraKey(), at(2026, 11, 13, 20, 0), 0);
        r.carryOn(r.cameraKey(), -1);
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
        // the run stopped at the day's start
        r.frameAt(r.cameraKey(), at(2026, 11, 15, 0, 0, 0) + 0.2, 0);
        r.carryOn(r.cameraKey(), -1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 14, 23, 59, 59));
        QCOMPARE(r.dayEpoch(), at(2026, 11, 14, 0));
        QVERIFY(r.covered(at(2026, 11, 14, 23, 30)));
        const QVariantMap last = r.daySegments().last().toMap();
        QCOMPARE(last["start"].toDouble(), 82802.0);
        QCOMPARE(last["end"].toDouble(), 86400.0);
        r.group()->playForward();
        r.frameAt(r.cameraKey(), at(2026, 11, 14, 23, 59, 59) + 0.6, 0);
        r.carryOn(r.cameraKey(), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 0));
        QCOMPARE(r.dayEpoch(), at(2026, 11, 15, 0));
        // A run that ends again at the same place (the recorder refused it) isn't reopened
        // there forever: the second time it goes to the previous recording's end.
        r.group()->playBackward();
        r.playFrom(at(2026, 11, 15, 0, 0, 30), -1);
        r.frameAt(r.cameraKey(), at(2026, 11, 15, 0, 0, 0) + 0.2, 0);
        r.carryOn(r.cameraKey(), -1);
        r.frameAt(r.cameraKey(), at(2026, 11, 14, 23, 59, 59), 0);
        r.carryOn(r.cameraKey(), -1);
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
        r.frameAt(r.cameraKey(), at(2026, 11, 14, 23, 59, 59) + 0.9, 0);
        // the 14th ran out at midnight
        r.carryOn(r.cameraKey(), 1);
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
        r.frameAt(r.cameraKey(), at(2026, 11, 15, 9, 58, 20), 0);
        QSignalSpy live(&r, &LiveReplay::replayingChanged);
        r.carryOn(r.cameraKey(), 1);
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
        r.frameAt(r.cameraKey(), at(2026, 11, 15, 8, 0), 0);
        r.carryOn(r.cameraKey(), -1);
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
        r.opened(r.cameraKey());
        QVERIFY(!r.stalledAtEdge(10'000));             // no picture yet
        r.frameAt(r.cameraKey(), kNow - 120, 10'000);
        r.frameAt(r.cameraKey(), kNow - 119, 11'000);
        QVERIFY(!r.stalledAtEdge(14'000));
        QVERIFY(!r.stalledAtEdge(18'000));            // a hiccup (or a window refill)
        QVERIFY(r.stalledAtEdge(19'500));
        r.setPaused(true);
        QVERIFY(!r.stalledAtEdge(19'500));
        r.setPaused(false);
        r.group()->playBackward();
        QVERIFY(!r.stalledAtEdge(19'500));
        r.group()->playForward();
        // an hour back: a gap, not the edge
        r.frameAt(r.cameraKey(), kNow - 3600, 20'000);
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
        rec.askedBy.takeFirst();
        QCOMPARE(rec.asked.takeFirst(), kToday);      // today's search fails twice
        r.dayFailed(r.cameraKey(), 2026, 11, 15);
        rec.askedBy.takeFirst();
        QCOMPARE(rec.asked.takeFirst(), kToday);
        r.dayFailed(r.cameraKey(), 2026, 11, 15);
        rec.answerAll();
        QCOMPARE(none.count(), 0);
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 14, 2, 0));
        QVERIFY(r.oldestFound());
        QCOMPARE(r.oldest(), at(2026, 11, 14, 1, 0));
    }
    // Answers are shared with Playback (wayfinder/architecture tickets 01, 07): failures for a
    // day replay isn't searching don't cost it that day's footage or extra searches.
    void anotherPagesFailureIsNotReplays()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({14, 15});
        rec.months[202610] = days({});
        rec.book[QDate(2026, 11, 14)] = segs({{3600, 7200}});
        rec.book[QDate(2026, 11, 15)] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(kNow, -1);
        QCOMPARE(rec.asked.first(), kToday);
        for (int i = 0; i < 3; ++i)   // not in flight: someone else's
            r.dayFailed(r.cameraKey(), 2026, 11, 14);
        rec.answerAll();
        QVERIFY(rec.ever.count(QDate(2026, 11, 14)) <= 1);   // nothing asked on its account
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 2, 0));   // today's footage
        r.playFrom(at(2026, 11, 15, 0, 30), -1);   // back over midnight: the 14th still has it
        rec.answerAll();
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 14, 2, 0));
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
        r.frameAt(r.cameraKey(), at(2026, 11, 15, 9, 58, 20), 0);
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.carryOn(r.cameraKey(), 1);
        QVERIFY(r.replaying());                       // asked again first
        rec.answerAll(1);
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 9, 58, 21));
        r.frameAt(r.cameraKey(), at(2026, 11, 15, 10, 6, 40), 0);
        r.carryOn(r.cameraKey(), 1);                                 // just asked: back to live
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
        r.frameAt(r.cameraKey(), kNow - 90, 0);
        QCOMPARE(r.playhead(), kNow - 90);
        QCOMPARE(r.behind(), 90.0);
        r.setNow(kNow + 10);
        QCOMPARE(r.behind(), 100.0);
    }

    // ---- several panes in step (ticket 15a) ----
    // A pane joins a running replay (up to four); it searches its own recorder, starting with
    // today; the first camera leads.
    void panesJoinUpToFourAndSearchTheirOwnRecorders()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        QVERIFY(!r.join(QStringLiteral("1:6")));          // nothing replaying: start first
        r.start(QStringLiteral("5:0"));
        QVERIFY(r.join(QStringLiteral("1:6")));
        QVERIFY(!r.join(QStringLiteral("1:6")));          // already in
        QVERIFY(r.join(QStringLiteral("1:2")));
        QVERIFY(r.join(QStringLiteral("2:2")));
        QVERIFY(!r.join(QStringLiteral("3:0")));          // a fifth
        QCOMPARE(r.cameraKeys(), (QStringList{"5:0", "1:6", "1:2", "2:2"}));
        QCOMPARE(r.cameraKey(), QStringLiteral("5:0"));   // the lead
        QCOMPARE(rec.askedBy, (QStringList{"5:0", "1:6", "1:2", "2:2"}));   // one each, at once
        QVERIFY(std::all_of(rec.asked.begin(), rec.asked.end(),
                            [](const QDate &d) { return d == kToday; }));
    }
    // Leaving: another pane leaves alone; the lead leaving hands the lead on; the last one
    // leaving ends replay.
    void leavingHandsTheLeadOnAndTheLastEndsReplay()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        r.join(QStringLiteral("1:2"));
        QSignalSpy changed(&r, &LiveReplay::replayingChanged);
        r.leave(QStringLiteral("1:6"));
        QCOMPARE(r.cameraKeys(), (QStringList{"5:0", "1:2"}));
        QCOMPARE(changed.count(), 1);
        r.leave(QStringLiteral("5:0"));
        QCOMPARE(r.cameraKey(), QStringLiteral("1:2"));
        QVERIFY(r.replaying());
        r.leave(QStringLiteral("1:2"));
        QVERIFY(!r.replaying());
        QVERIFY(r.cameraKeys().isEmpty());
    }
    // The lead's footage decides where a play lands, and only its frames move the playhead;
    // setLead hands it to another replaying camera (the selected pane, as Playback).
    void theLeadDecidesWhereAPlayLands()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}});              // both cameras: 01:00–02:00
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        rec.answerAll();
        // 1:6 also has 08:00–09:00 (answered as its own).
        r.dayFound(QStringLiteral("1:6"), 2026, 11, 15, segs({{3600, 7200}, {28800, 32400}}), {});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(at(2026, 11, 15, 8, 30), -1);              // the lead has nothing then
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 2, 0));
        QVERIFY(r.covers(QStringLiteral("1:6"), at(2026, 11, 15, 8, 30)));
        QVERIFY(!r.covers(QStringLiteral("5:0"), at(2026, 11, 15, 8, 30)));
        r.frameAt(QStringLiteral("1:6"), at(2026, 11, 15, 1, 50), 0);   // not the lead's frame
        QCOMPARE(r.playhead(), at(2026, 11, 15, 2, 0));
        QVERIFY(!r.setLead(QStringLiteral("3:0")));                      // not replaying
        QVERIFY(r.setLead(QStringLiteral("1:6")));
        QCOMPARE(r.cameraKey(), QStringLiteral("1:6"));
        r.playFrom(at(2026, 11, 15, 8, 30), -1);              // now 1:6's footage decides
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 8, 30));
        QCOMPARE(r.daySegmentsFor(QStringLiteral("1:6")).size(), 2);
        QCOMPARE(r.daySegmentsFor(QStringLiteral("5:0")).size(), 1);
    }
    // Only the lead's stream running out moves every pane on.
    void onlyTheLeadsEndCarriesEveryPaneOn()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}, {28800, 32400}});
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        rec.answerAll();
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(at(2026, 11, 15, 8, 30), -1);
        QCOMPARE(play.count(), 1);
        r.frameAt(QStringLiteral("5:0"), at(2026, 11, 15, 8, 0), 0);
        r.carryOn(QStringLiteral("1:6"), -1);                 // another pane's end: nothing
        QCOMPARE(play.count(), 1);
        r.carryOn(QStringLiteral("5:0"), -1);                 // the lead's: the previous recording
        QCOMPARE(play.count(), 2);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 15, 2, 0));
    }
    // Forwards, another pane reaching its own live buffer switches only that pane.
    void anotherPaneSwitchesToItsOwnBuffer()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 35880}});
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        rec.answerAll();
        r.setBufferSpans(QStringLiteral("1:6"),
                          QVariantList{QVariantMap{{"start", kNow - 150}, {"end", kNow}}});
        QSignalSpy play(&r, &LiveReplay::playWanted);
        QSignalSpy pane(&r, &LiveReplay::paneWanted);
        r.playFrom(kNow - 400, 1);
        r.frameAt(QStringLiteral("5:0"), kNow - 160, 0);
        r.frameAt(QStringLiteral("1:6"), kNow - 150, 0);
        QCOMPARE(play.count(), 1);
        QCOMPARE(pane.count(), 1);
        QCOMPARE(pane.last().at(0).toString(), QStringLiteral("1:6"));
        QCOMPARE(r.playhead(), kNow - 160);                    // the lead's time stays
        r.frameAt(QStringLiteral("1:6"), kNow - 140, 0);
        QCOMPARE(pane.count(), 1);
    }
    // The time line reaches the oldest any camera has; found once every camera's is.
    void theOldestIsTheOldestAnyCameraHas()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({15});
        rec.months[202610] = days({});
        rec.book[kToday] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        rec.answerAll();
        QVERIFY(r.oldestFound());
        QCOMPARE(r.oldest(), at(2026, 11, 15, 1, 0));
        r.join(QStringLiteral("1:6"));
        QVERIFY(!r.oldestFound());                             // 1:6's not yet
        r.dayFound(QStringLiteral("1:6"), 2026, 11, 15, segs({{1800, 7200}}), days({15}));
        rec.asked.clear();
        rec.askedBy.clear();
        r.dayFound(QStringLiteral("1:6"), 2026, 10, 1, {}, days({}));
        QVERIFY(r.oldestFound());
        QCOMPARE(r.oldest(), at(2026, 11, 15, 0, 30));
        r.dayFound(QStringLiteral("3:0"), 2026, 11, 15, segs({{0, 60}}), {});   // not replaying
        QCOMPARE(r.oldest(), at(2026, 11, 15, 0, 30));
    }
    // Every replaying camera searches the day the playhead moves to (not only the lead), so
    // its pane knows its footage there.
    void everyCameraSearchesThePlayheadsDay()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({14, 15});
        rec.months[202610] = days({20});   // the oldest scan ends on 20 Oct, not this day
        rec.months[202609] = days({});
        rec.book[QDate(2026, 11, 14)] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        rec.answerAll();
        rec.ever.clear();
        QStringList by;
        connect(&r, &LiveReplay::searchWanted, this,
                [&](const QString &key, int, int, int d) { if (d == 14) by << key; });
        r.playFrom(at(2026, 11, 14, 1, 30), 1);
        rec.answerAll();
        QVERIFY(by.contains(QStringLiteral("5:0")));
        QVERIFY(by.contains(QStringLiteral("1:6")));
        QCOMPARE(r.daySegmentsFor(QStringLiteral("1:6")).size(), 1);
    }
    // The lead with no footage at the playhead: the next camera in order that has some moves
    // it (Playback's frameDriverSecs).
    void anotherCamerasFramesMoveThePlayheadWhereTheLeadHasNone()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        rec.answerAll();
        r.dayFound(QStringLiteral("1:6"), 2026, 11, 15, segs({{3600, 7200}, {28800, 32400}}), {});
        r.playFrom(at(2026, 11, 15, 1, 30), 1);
        r.frameAt(QStringLiteral("1:6"), at(2026, 11, 15, 1, 31), 0);   // the lead covers: ignored
        QCOMPARE(r.playhead(), at(2026, 11, 15, 1, 30));
        QVERIFY(r.setLead(QStringLiteral("1:6")));
        r.playFrom(at(2026, 11, 15, 8, 30), 1);                          // 1:6's footage only
        QVERIFY(r.setLead(QStringLiteral("5:0")));                       // a lead with none here
        r.frameAt(QStringLiteral("1:6"), at(2026, 11, 15, 8, 31), 0);
        QCOMPARE(r.playhead(), at(2026, 11, 15, 8, 31));
    }
    // A play waiting for the lead's search, when the lead changes or leaves: retried under the
    // new lead, not dropped.
    void aWaitingPlayIsRetriedUnderTheNewLead()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.months[202611] = days({13, 15});
        rec.months[202610] = days({20});   // the oldest scan ends on 20 Oct, not this day
        rec.months[202609] = days({});
        rec.book[kToday] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        rec.answerAll();
        QSignalSpy play(&r, &LiveReplay::playWanted);
        r.playFrom(at(2026, 11, 13, 1, 30), 1);                  // the 13th: a search for 5:0
        QCOMPARE(play.count(), 0);
        // 1:6 leads: its search, then plays
        r.leave(QStringLiteral("5:0"));
        rec.book[QDate(2026, 11, 13)] = segs({{3600, 7200}});
        rec.answerAll();
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.last().at(0).toDouble(), at(2026, 11, 13, 1, 30));
    }
    // A hidden pane (another maximised) plays nothing: its frames can't move the playhead, so
    // the frame driver passes over it (ticket 15b).
    void aHiddenPaneDoesntDriveThePlayhead()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        r.join(QStringLiteral("1:2"));
        rec.answerAll();
        r.dayFound(QStringLiteral("1:6"), 2026, 11, 15, segs({{28800, 32400}}), {});
        r.dayFound(QStringLiteral("1:2"), 2026, 11, 15, segs({{28800, 32400}}), {});
        r.setLead(QStringLiteral("1:6"));
        r.playFrom(at(2026, 11, 15, 8, 30), 1);
        r.setLead(QStringLiteral("5:0"));                 // no footage at 08:30
        QCOMPARE(r.frameDriver(), QStringLiteral("1:6"));
        r.setShown(QStringLiteral("1:6"), false);
        QCOMPARE(r.frameDriver(), QStringLiteral("1:2"));
        QVERIFY(r.anyShown());
        r.setShown(QStringLiteral("1:2"), false);
        r.setShown(QStringLiteral("5:0"), false);
        QVERIFY(!r.anyShown());
    }
    // A pane that isn't driving starts again at the playhead where it has footage and isn't
    // playing, or has drifted more than 5 s × speed (PaneSync, shared with Playback); never the
    // driver, never while paused, not within 3 s of its last restart.
    void aPaneOutOfStepStartsAgainAtThePlayhead()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        rec.answerAll();
        r.dayFound(QStringLiteral("1:6"), 2026, 11, 15, segs({{3600, 7200}}), {});
        const double t = at(2026, 11, 15, 1, 30);
        r.playFrom(t, 1);
        // The pane as the page reports it; frameSecs in epoch seconds.
        auto pane = [](const char *key, bool busy, double frame) {
            return QVariantMap{{"key", key}, {"busy", busy}, {"frameSecs", frame}};
        };
        QVERIFY(r.needsRestart(pane("1:6", false, -1), 100000));        // not playing
        QVERIFY(!r.needsRestart(pane("1:6", true, t + 4), 100000));     // in step
        QVERIFY(r.needsRestart(pane("1:6", true, t + 6), 100000));      // 6 s out
        QVariantMap p = pane("1:6", true, t + 6);
        p["aligning"] = true;                                           // catching up itself
        QVERIFY(!r.needsRestart(p, 100000));
        p = pane("1:6", false, -1);
        p["lastOpenMs"] = 98000;                                     // just restarted
        QVERIFY(!r.needsRestart(p, 100000));
        p = pane("1:6", false, -1);
        p["frozen"] = true;
        QVERIFY(!r.needsRestart(p, 100000));
        QVERIFY(!r.needsRestart(pane("5:0", true, t + 9), 100000));     // the driver, drifted
        QVERIFY(r.needsRestart(pane("5:0", false, -1), 100000));        // the driver, not playing
        r.setPaused(true);
        QVERIFY(!r.needsRestart(pane("1:6", false, -1), 100000));
    }
    // start() on a camera already in the replay makes it the lead; it doesn't restart from now.
    void startOnAJoinedCameraLeadsInstead()
    {
        LiveReplay r;
        r.setNow(kNow);
        Recorder rec(r);
        rec.book[kToday] = segs({{3600, 7200}});
        r.start(QStringLiteral("5:0"));
        r.join(QStringLiteral("1:6"));
        rec.answerAll();
        r.playFrom(at(2026, 11, 15, 1, 30), 1);
        r.start(QStringLiteral("1:6"));
        QCOMPARE(r.cameraKeys(), (QStringList{"5:0", "1:6"}));
        QCOMPARE(r.cameraKey(), QStringLiteral("1:6"));
        QCOMPARE(r.playhead(), at(2026, 11, 15, 1, 30));
        QCOMPARE(r.maxCameras(), 4);
    }
};

QTEST_GUILESS_MAIN(TestLiveReplay)
#include "test_livereplay.moc"
