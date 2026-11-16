#include "ui/EventClip.h"

#include <QtTest>

using namespace rl;


namespace {
// An EventClip (a recorder, or a Frigate camera on host 3) with what it asks the player to do
// written down, one line per command.
struct Rig
{
    EventClip c;
    QStringList log;
    explicit Rig(bool frigate = false)
    {
        c.setClipResolver([frigate](qint64 h, int, qint64 ts, int pre, int post) {
            return frigate && h == 3 ? QStringLiteral("f/%1/%2").arg(ts - pre).arg(ts + post) : QString();
        });
        QObject::connect(&c, &EventClip::openWanted, [this](double at) { log << QStringLiteral("open %1").arg(at); });
        QObject::connect(&c, &EventClip::stopAllWanted, [this] { log << "stopAll"; });
        QObject::connect(&c, &EventClip::stopStreamWanted, [this] { log << "stopStream"; });
        QObject::connect(&c, &EventClip::pauseMediaWanted, [this] { log << "pauseMedia"; });
        QObject::connect(&c, &EventClip::seekMediaWanted, [this](double ms) { log << QStringLiteral("seekMedia %1").arg(ms); });
        QObject::connect(&c, &EventClip::retryHdWanted, [this] { log << "retryHd"; });
        QObject::connect(&c, &EventClip::notice, [this](EventClip::Notice n) {
            log << (n == EventClip::StartOfClip ? "notice start" : "notice missing");
        });
    }
    // Commands since the last call.
    QStringList took() { QStringList out; out.swap(log); return out; }
    // A recorder event playing at 1× from 995, picture at wall 5000 ms.
    void playing()
    {
        c.play(1, 7, 1000);
        c.firstPicture(5000);
    }
};
} // namespace

// The Events page's clip player (wayfinder/events-feed step 22): which footage an event
// plays and over what window. A Reolink recorder plays its sub stream from 5 s before the
// event to 30 s after; a Frigate camera plays Frigate's own clip from 15 s before (its
// events arrive up to 10 s late) to 30 s after.
class TestEventClip : public QObject
{
    Q_OBJECT

    struct Asked
    {
        qint64 host = 0;
        int channel = 0;
        qint64 ts = 0;
        int pre = 0, post = 0;
    };

private slots:
    void aRecorderPlaysFromFiveSecondsBefore()
    {
        EventClip c;
        c.setClipResolver([](qint64, int, qint64, int, int) { return QString(); });
        c.setEvent(1, 7, 1000);
        QVERIFY(!c.frigate());
        QVERIFY(c.clipUrl().isEmpty());
        QCOMPARE(c.from(), qint64(995));
        QCOMPARE(c.to(), qint64(1030));
        QCOMPARE(c.progress(995), 0.0);
        QCOMPARE(c.progress(1012.5), 0.5);
        QCOMPARE(c.progress(1030), 1.0);
        QCOMPARE(c.progress(900), 0.0);   // clamped
        QCOMPARE(c.progress(2000), 1.0);
        QVERIFY(!c.atEnd(1029.9));
        QVERIFY(c.atEnd(1030));
    }

    void aFrigateCameraPlaysItsOwnClip()
    {
        Asked asked;
        EventClip c;
        c.setClipResolver([&asked](qint64 h, int ch, qint64 ts, int pre, int post) {
            asked = {h, ch, ts, pre, post};
            return h == 3 ? QStringLiteral("http://f/api/Bosch/start/%1/end/%2/clip.mp4")
                                .arg(ts - pre).arg(ts + post)
                          : QString();
        });
        QSignalSpy spy(&c, &EventClip::changed);
        c.setEvent(3, 1, 1000);
        QVERIFY(spy.count() >= 1);
        QVERIFY(c.frigate());
        QCOMPARE(asked.host, qint64(3));
        QCOMPARE(asked.channel, 1);
        QCOMPARE(asked.ts, qint64(1000));
        QCOMPARE(asked.pre, 15);
        QCOMPARE(asked.post, 30);
        QCOMPARE(c.clipUrl(), QStringLiteral("http://f/api/Bosch/start/985/end/1030/clip.mp4"));
        QCOMPARE(c.from(), qint64(985));
        QCOMPARE(c.to(), qint64(1030));
        QCOMPARE(c.progress(1007.5), 0.5);
    }

    // Keep playing: a recorder plays on with no end; Frigate fetches the next minute.
    void keepPlayingRunsOn()
    {
        EventClip c;
        c.setClipResolver([](qint64 h, int, qint64 ts, int pre, int post) {
            return h == 3 ? QStringLiteral("f/%1/%2").arg(ts - pre).arg(ts + post) : QString();
        });
        c.setEvent(1, 7, 1000);
        c.keepPlaying();
        QVERIFY(c.openEnded());
        QVERIFY(!c.atEnd(5000));
        QCOMPARE(c.from(), qint64(995)); // the clock still counts from the event

        c.setEvent(3, 1, 1000);
        QVERIFY(!c.openEnded()); // a new event starts a fresh window
        c.keepPlaying();
        QVERIFY(c.openEnded()); // plays on, a minute's clip at a time
        QCOMPARE(c.clipUrl(), QStringLiteral("f/1030/1090"));
        QCOMPARE(c.from(), qint64(1030));
        QCOMPARE(c.to(), qint64(1090));
        QVERIFY(!c.atEnd(1090));
        c.keepPlaying(); // the next minute
        QCOMPARE(c.clipUrl(), QStringLiteral("f/1090/1150"));
        QCOMPARE(c.from(), qint64(1090));
    }

    void noEventIsEmpty()
    {
        EventClip c;
        c.setClipResolver([](qint64, int, qint64, int, int) { return QString(); });
        c.setEvent(1, 7, 1000);
        c.clear();
        QVERIFY(!c.active());
        QCOMPARE(c.from(), qint64(0));
        QCOMPARE(c.progress(1000), 0.0);
        QCOMPARE(c.timeAt(0.5), 0.0);
    }

    // The pane bar's time line and ↺ / ↻ (wayfinder/live-replay ticket 11): a point on the
    // line is a moment in the window; a skip stays inside it.
    void aPointOnTheLineIsAMomentInTheWindow()
    {
        EventClip c;
        c.setClipResolver([](qint64, int, qint64, int, int) { return QString(); });
        c.setEvent(1, 7, 1000);   // 995 .. 1030
        QCOMPARE(c.timeAt(0), 995.0);
        QCOMPARE(c.timeAt(1), 1030.0);
        QCOMPARE(c.timeAt(0.2), 1002.0);
        QCOMPARE(c.timeAt(-1), 995.0);
        QCOMPARE(c.timeAt(2), 1030.0);
    }

    void aSkipStaysInsideTheWindow()
    {
        EventClip c;
        c.setClipResolver([](qint64, int, qint64, int, int) { return QString(); });
        c.setEvent(1, 7, 1000);   // 995 .. 1030
        QCOMPARE(c.skipFrom(1010, -10), 1000.0);
        QCOMPARE(c.skipFrom(1000, -10), 995.0);
        QCOMPARE(c.skipFrom(1025, 10), 1030.0);
        // Kept playing (open-ended): forward runs on past the window's end.
        c.keepPlaying();
        QCOMPARE(c.skipFrom(1025, 10), 1035.0);
        QCOMPARE(c.skipFrom(998, -10), 995.0);
    }

    // ---- The clip as a state machine (architecture review #2, candidate 2) ----------------
    // The player tells the clip what happened (a picture, a stream that stopped, a tick of the
    // wall clock) and does what it's told (open, stop, pause, seek). Positions are epoch seconds;
    // wall times are milliseconds from any clock.

    // A recorder event opens at the window's start; the first picture starts the clock.
    void anEventOpensAtItsWindowsStart()
    {
        Rig r;
        r.c.play(1, 7, 1000);
        QCOMPARE(r.took(), QStringList({"stopAll", "retryHd", "open 995"}));
        QCOMPARE(r.c.phase(), EventClip::Starting);
        QVERIFY(r.c.firstPicture(0));
        QCOMPARE(r.c.phase(), EventClip::Playing);
        QVERIFY(!r.c.firstPicture(10));   // once per open
        r.c.tick(1000);
        QCOMPARE(r.c.position(), 996.0);
        r.c.group()->stepFast(1);          // 2× forwards, no frame times: the wall clock
        r.c.tick(2000);
        QCOMPARE(r.c.position(), 998.0);
    }

    // Off 1× the playhead follows the frame on screen — a fresh one, not the frame left from
    // before the stream was opened.
    void offNormalFollowsTheFreshFrame()
    {
        Rig r;
        r.c.setFrameEpochMs(500000);       // the last frame of an earlier stream
        r.c.play(1, 7, 1000);
        r.c.group()->stepFast(1);
        r.c.firstPicture(0);
        r.c.tick(1000);
        QCOMPARE(r.c.position(), 997.0);   // stale frame: wall clock at 2×
        r.c.setFrameEpochMs(1003500);
        r.c.tick(1200);
        QCOMPARE(r.c.position(), 1003.5);
    }

    // At the window's end the stream stops with the last picture; ❚❚ / ▶ there plays it again.
    void theWindowsEndStopsThenPlaysAgainFromTheStart()
    {
        Rig r;
        r.playing();
        r.took();
        r.c.tick(40000);   // 1030
        QCOMPARE(r.c.phase(), EventClip::Ended);
        QVERIFY(r.c.paused());
        QCOMPARE(r.took(), QStringList({"stopStream"}));
        r.c.togglePause();
        QCOMPARE(r.took(), QStringList({"retryHd", "stopStream", "open 995"}));
    }

    // ❚❚ closes a recorder's stream (freeing the recorder) and keeps the moment; ▶ reopens there.
    void pauseKeepsTheMomentAndPlayReopensThere()
    {
        Rig r;
        r.playing();
        r.c.tick(10000);   // 1000
        r.took();
        r.c.togglePause();
        QCOMPARE(r.c.phase(), EventClip::Paused);
        QCOMPARE(r.c.shownPosition(), 1000.0);
        QCOMPARE(r.took(), QStringList({"stopStream"}));
        r.c.skip(-10);    // from where it paused, kept inside the window
        QCOMPARE(r.took(), QStringList({"retryHd", "stopStream", "open 995"}));
        r.c.togglePause();   // playing again (Starting): ❚❚ does nothing until a picture
        QCOMPARE(r.took(), QStringList());
    }

    // A speed button on a paused clip plays from the paused moment; on an ended one, backwards
    // from its end, forwards from its start.
    void aSpeedPlaysOnFromWherePaused()
    {
        Rig r;
        r.playing();
        r.c.tick(10000);   // 1000
        r.c.togglePause();
        r.took();
        r.c.group()->playBackward();
        r.c.speedChanged();
        QCOMPARE(r.took(), QStringList({"retryHd", "stopStream", "open 1000"}));

        Rig e;
        e.playing();
        e.c.tick(40000);   // ended at 1030
        e.took();
        e.c.group()->stepFast(1);
        e.c.speedChanged();
        QCOMPARE(e.took(), QStringList({"retryHd", "stopStream", "open 995"}));
        e.c.firstPicture(40000);
        e.c.tick(60000);   // 2× for 20 s: past the end, ended again
        QCOMPARE(e.c.phase(), EventClip::Ended);
        e.took();
        e.c.group()->playBackward();
        e.c.speedChanged();
        QCOMPARE(e.took().last(), QStringLiteral("open %1").arg(e.c.position()));
        QVERIFY(e.c.position() >= 1030);
    }

    // Backwards reaches the window's start (within a quarter second): paused there, with a note.
    void backwardsPausesAtTheWindowsStart()
    {
        Rig r;
        r.c.play(1, 7, 1000);
        r.c.group()->playBackward();
        r.c.seekTo(1005);
        r.c.firstPicture(0);
        r.took();
        r.c.tick(-200);       // a clock that went backwards doesn't move the playhead
        QCOMPARE(r.c.position(), 1005.0);
        r.c.tick(1000);
        QCOMPARE(r.c.phase(), EventClip::Playing);
        r.c.tick(10000);
        QCOMPARE(r.c.phase(), EventClip::Paused);
        QCOMPARE(r.c.shownPosition(), 995.0);
        QCOMPARE(r.took(), QStringList({"stopStream", "notice start"}));
        // ◀ again from the start: nothing earlier, it stays paused with the note.
        r.c.togglePause();
        QCOMPARE(r.took(), QStringList({"retryHd", "stopStream", "notice start"}));
        QCOMPARE(r.c.phase(), EventClip::Paused);
    }

    // A recorder's stream ends at each window (backwards) or recording file (forwards): it opens
    // again where it got to, once per place; a second end there is the edge.
    void aStreamThatEndsReopensOnceWhereItGotTo()
    {
        Rig r;
        r.playing();
        r.c.tick(20000);   // 1010
        r.took();
        r.c.streamStopped(false);   // an ordered stop that was already superseded
        QCOMPARE(r.took(), QStringList());
        r.c.streamStopped(true);
        QCOMPARE(r.took(), QStringList({"open 1011"}));
        r.c.firstPicture(15000);
        r.c.streamStopped(true);   // ended again where it reopened
        QCOMPARE(r.took(), QStringList({"stopStream"}));
        QCOMPARE(r.c.phase(), EventClip::Ended);

        Rig b;   // backwards: the same place again
        b.c.play(1, 7, 1000);
        b.c.group()->playBackward();
        b.c.seekTo(1010);
        b.c.firstPicture(0);
        b.took();
        b.c.streamStopped(true);
        QCOMPARE(b.took(), QStringList({"open 1010"}));
        b.c.firstPicture(0);
        b.c.streamStopped(true);
        QCOMPARE(b.took(), QStringList({"stopStream", "notice start"}));
        QCOMPARE(b.c.phase(), EventClip::Paused);
        // A stream that stops while not playing (paused, ended) is not this clip's business.
        b.c.streamStopped(true);
        QCOMPARE(b.took(), QStringList());
    }

    // A recorder that sends nothing is given up on: 10 s at 1×, 30 s off 1×, 20 s for Frigate's
    // cut-on-request clip. With a picture already up the clip pauses there instead of failing.
    void aSilentRecorderIsGivenUpOn_data()
    {
        QTest::addColumn<bool>("frigate");
        QTest::addColumn<bool>("fast");
        QTest::addColumn<int>("limitMs");
        QTest::newRow("recorder 1×") << false << false << 10000;
        QTest::newRow("recorder off 1×") << false << true << 30000;
        QTest::newRow("frigate") << true << false << 20000;
    }
    void aSilentRecorderIsGivenUpOn()
    {
        QFETCH(bool, frigate);
        QFETCH(bool, fast);
        QFETCH(int, limitMs);
        Rig r(frigate);
        r.c.play(frigate ? 3 : 1, 7, 1000);
        if (fast)
            r.c.group()->stepFast(1);
        r.took();
        r.c.tick(50000);              // the first tick starts the count
        r.c.tick(50000 + limitMs - 1);
        QCOMPARE(r.c.phase(), EventClip::Starting);
        r.c.tick(50000 + limitMs);
        QCOMPARE(r.c.phase(), EventClip::Failed);
        QCOMPARE(r.took(), QStringList({"stopAll"}));
    }
    // The deadline runs from when starting began: reopening again before a picture (a seek, HD,
    // a new route) doesn't push it back.
    void reopeningWhileStartingKeepsTheDeadline()
    {
        Rig r;
        r.c.play(1, 7, 1000);
        r.c.tick(0);
        r.c.tick(6000);
        r.c.seekTo(1010);
        r.c.hdChanged();
        r.c.reopenNeeded();
        QCOMPARE(r.c.phase(), EventClip::Starting);
        r.c.tick(10000);
        QCOMPARE(r.c.phase(), EventClip::Failed);
    }
    void aSilentReopenPausesOnThePictureItHad()
    {
        Rig r;
        r.playing();
        r.c.tick(10000);   // 1000
        r.c.streamReconnecting();   // the NVR dropped it; the player retries
        QCOMPARE(r.c.phase(), EventClip::Starting);
        r.took();
        r.c.tick(20000);
        r.c.tick(30000);
        QCOMPARE(r.c.phase(), EventClip::Paused);
        QCOMPARE(r.c.shownPosition(), 995.0);   // where that stream was opened
        QCOMPARE(r.took(), QStringList({"stopAll", "notice missing"}));
    }

    // Frigate's clip is a file: ❚❚ pauses it in place, ▶ seeks it and plays; its end is the
    // window's end; Keep playing fetches the next minute while that minute is in the past.
    void aFrigateClipPausesAndSeeksTheFile()
    {
        Rig r(true);
        r.c.play(3, 1, 1000);   // 985 .. 1030
        QCOMPARE(r.took(), QStringList({"stopAll", "retryHd", "open 985"}));
        r.c.setMediaPositionMs(500);
        QVERIFY(r.c.firstPicture(0));
        r.c.setMediaPositionMs(5000);
        QCOMPARE(r.c.position(), 990.0);
        r.c.tick(99999);   // the wall clock doesn't move a file's playhead
        QCOMPARE(r.c.position(), 990.0);
        r.c.togglePause();
        QCOMPARE(r.took(), QStringList({"pauseMedia"}));
        r.c.togglePause();
        QCOMPARE(r.took(), QStringList({"retryHd", "seekMedia 5000"}));
        QCOMPARE(r.c.phase(), EventClip::Playing);
        r.c.setMediaPositionMs(45000);   // 1030: the end, nothing to stop
        QCOMPARE(r.c.phase(), EventClip::Ended);
        QCOMPARE(r.took(), QStringList());
        r.c.keepPlaying();
        QCOMPARE(r.took(), QStringList({"open 1030"}));
        r.c.firstPicture(0);
        r.c.mediaEnded(1090 + 4);   // the next minute isn't over yet: the end
        QCOMPARE(r.c.phase(), EventClip::Ended);
        r.c.keepPlaying();          // 1090 .. 1150
        r.c.firstPicture(0);
        r.took();
        r.c.mediaEnded(1150 + 5);
        QCOMPARE(r.took(), QStringList({"open 1150"}));
        QCOMPARE(r.c.from(), qint64(1150));
    }

    // A new Frigate clip starts at its window's start, before its file reports a position:
    // the last file's playhead must not carry over (it read 45 s ahead while loading).
    void aNewFrigateClipReadsItsStartWhileLoading()
    {
        Rig r(true);
        r.c.play(3, 1, 1000);   // 985 .. 1030
        r.c.firstPicture(0);
        r.c.setMediaPositionMs(45000);   // ended at 1030
        r.c.keepPlaying();               // 1030 .. 1090, loading
        QCOMPARE(r.c.position(), 1030.0);
        QCOMPARE(r.c.shownPosition(), 1030.0);
        r.c.firstPicture(0);
        r.c.setMediaPositionMs(60000);
        r.c.mediaEnded(1090 + 5);        // the next minute, on its own
        QCOMPARE(r.c.position(), 1090.0);
        r.c.play(3, 1, 2000);            // another event: 1985 ..
        QCOMPARE(r.c.position(), 1985.0);
    }

    // A recorder's Keep playing opens where the picture is, with no end.
    void keepPlayingARecorderOpensWhereThePictureIs()
    {
        Rig r;
        r.playing();
        r.c.tick(40000);   // ended at 1030
        r.took();
        r.c.keepPlaying();
        QVERIFY(r.c.openEnded());
        QCOMPARE(r.took(), QStringList({"open 1030"}));
    }

    // What the engines report: an open with nothing to play fails; a download that fails while
    // starting fails; the session asks for a reopen at the picture (a new route) only while up;
    // HD reopens a running stream, never moves it in place.
    void enginesReportBack()
    {
        Rig r;
        r.c.play(1, 7, 1000);
        r.c.openFailed();
        QCOMPARE(r.c.phase(), EventClip::Failed);
        r.c.loadFailed();
        QCOMPARE(r.c.phase(), EventClip::Failed);

        Rig f(true);
        f.c.play(3, 1, 1000);
        f.c.firstPicture(0);
        f.c.loadFailed();   // only while starting
        QCOMPARE(f.c.phase(), EventClip::Playing);

        Rig p;
        p.playing();
        p.c.tick(10000);
        p.took();
        p.c.reopenNeeded();
        QCOMPARE(p.took(), QStringList({"open 1000"}));
        p.c.hdChanged();
        QCOMPARE(p.took(), QStringList({"stopStream", "open 1000"}));
        p.c.firstPicture(10000);
        p.c.togglePause();
        p.took();
        p.c.reopenNeeded();
        p.c.hdChanged();
        QCOMPARE(p.took(), QStringList());   // paused: nothing running to reopen
    }

    // Open in Playback hands over the moment on screen; the event's own before a picture.
    void playbackTakesOverAtTheMomentOnScreen()
    {
        Rig r;
        r.c.play(1, 7, 1000);
        QCOMPARE(r.c.handOffAt(), 1000.0);
        r.c.firstPicture(0);
        r.c.tick(2000);
        QCOMPARE(r.c.handOffAt(), 997.0);
        r.c.togglePause();
        r.c.tick(9000);
        QCOMPARE(r.c.handOffAt(), 997.0);
        r.took();
        r.c.close();
        QCOMPARE(r.took(), QStringList({"stopAll"}));
        QVERIFY(!r.c.active());
        QCOMPARE(r.c.phase(), EventClip::Idle);
    }
};

QTEST_GUILESS_MAIN(TestEventClip)
#include "test_eventclip.moc"
