#include "media/ReversePlay.h"

#include <QtTest>

using namespace rl::reverse;

namespace {

// A window as the recorder sends it: `fps` frames a second from `startMs`, a keyframe every
// `gopSecs`, the first frame a keyframe.
QList<Frame> window(qint64 startMs, int secs, int gopSecs, int fps = 10)
{
    QList<Frame> out;
    const int step = 1000 / fps;
    for (int i = 0; i < secs * fps; ++i) {
        Frame f;
        f.epochMs = startMs + qint64(i) * step;
        f.key = i % (gopSecs * fps) == 0;
        f.annexB = QByteArray::number(f.epochMs);
        out << f;
    }
    return out;
}

QList<qint64> times(const QList<Frame> &frames)
{
    QList<qint64> out;
    for (const Frame &f : frames)
        out << f.epochMs;
    return out;
}

} // namespace

// Reverse play (wayfinder/playback-controls ticket 12): windows of the sub stream fetched
// behind the playhead (cmd 143), cut at the playhead, split into keyframe groups sent newest
// first; the player decodes each group forwards and shows it backwards.
class TestReversePlay : public QObject
{
    Q_OBJECT
private slots:
    // ---- the window to fetch ----
    // 10 s behind the cutoff plus a keyframe group of slack (the recorder starts a window on
    // a keyframe up to 3 s after the start asked for), ending at the cutoff's second.
    void windowEndsAtTheCutoffSecond()
    {
        const Window w = windowBefore(1'000'500);
        QCOMPARE(w.endSecs, qint64(1001));
        QCOMPARE(w.startSecs, qint64(1001 - 14));
        const Window exact = windowBefore(2'000'000);
        QCOMPARE(exact.endSecs, qint64(2000));
    }

    // ---- cutting and ordering a window ----
    void groupsComeNewestFirstEachInDecodeOrder()
    {
        // 09:59:50.000 .. 10:00:00.9 with 1 s groups; cutoff at 10:00:00.000.
        const qint64 t0 = 1'000'000;
        const Gops g = reverseGops(window(t0, 11, 1), t0 + 10'000);
        QCOMPARE(g.newestFirst.size(), 10);
        QCOMPARE(g.newestFirst.first().first().epochMs, t0 + 9'000);   // the last whole group
        QVERIFY(g.newestFirst.first().first().key);
        QCOMPARE(g.newestFirst.first().last().epochMs, t0 + 9'900);
        QCOMPARE(g.newestFirst.last().first().epochMs, t0);
        QCOMPARE(g.nextCutoffMs, t0);
    }

    // Frames at or after the cutoff are already shown (or past the playhead): never sent.
    void framesFromTheCutoffOnAreDropped()
    {
        const qint64 t0 = 1'000'000;
        const Gops g = reverseGops(window(t0, 11, 4), t0 + 6'050);
        // Groups start at +0, +4 and +8 s; +8 is past the cutoff; +4 is cut at 6.05 s.
        QCOMPARE(g.newestFirst.size(), 2);
        const QList<qint64> newest = times(g.newestFirst.first());
        QCOMPARE(newest.first(), t0 + 4'000);
        QCOMPARE(newest.last(), t0 + 6'000);
        QCOMPARE(g.nextCutoffMs, t0);
    }

    // A window that starts mid-group (no keyframe yet) can't decode those frames: dropped.
    void framesBeforeTheFirstKeyframeAreDropped()
    {
        const qint64 t0 = 1'000'000;
        QList<Frame> w = window(t0, 12, 4).mid(15); // starts at +1.5 s, mid-group
        const Gops g = reverseGops(w, t0 + 12'000);
        QCOMPARE(g.newestFirst.size(), 2);
        QCOMPARE(g.newestFirst.last().first().epochMs, t0 + 4'000);
        QCOMPARE(g.nextCutoffMs, t0 + 4'000);
    }

    // Windows overlap (the recorder may start before the start asked for): the next window's
    // frames from the previous cutoff on are already shown.
    void overlappingWindowsNeverRepeatAFrame()
    {
        const qint64 t0 = 1'000'000;
        const Gops first = reverseGops(window(t0 + 10'000, 10, 1), t0 + 20'000);
        const Gops second = reverseGops(window(t0 + 7'000, 13, 1), first.nextCutoffMs);
        QCOMPARE(second.newestFirst.first().last().epochMs, t0 + 9'900);
        QCOMPARE(second.nextCutoffMs, t0 + 7'000);
    }

    // Nothing before the cutoff: the start of the recording (the page jumps or pauses).
    void nothingBeforeTheCutoffEndsTheRun()
    {
        const qint64 t0 = 1'000'000;
        QCOMPARE(reverseGops({}, t0).nextCutoffMs, qint64(-1));
        const Gops g = reverseGops(window(t0, 2, 1), t0);
        QVERIFY(g.newestFirst.isEmpty());
        QCOMPARE(g.nextCutoffMs, qint64(-1));
    }

    // Reverse stops at the start of the selected day: frames before `floorMs` (midnight) are
    // the day before. A group cut by the floor loses its keyframe, so all of it goes.
    void framesBeforeTheDayAreDropped()
    {
        const qint64 t0 = 1'000'000;
        const Gops g = reverseGops(window(t0, 10, 1), t0 + 10'000, t0 + 3'500);
        QCOMPARE(g.newestFirst.size(), 6);
        QCOMPARE(g.nextCutoffMs, t0 + 4'000);
        QCOMPARE(reverseGops(window(t0, 3, 1), t0 + 3'000, t0 + 5'000).nextCutoffMs, qint64(-1));
    }

    // ---- HD reverse (wayfinder/live-replay ticket 06) ----
    // A decoded group is held at the pane's size on screen (never larger than the picture);
    // fisheye cameras at up to 1920×1920 so dewarped views stay sharp. Even sizes.
    void heldAtThePaneSize()
    {
        QCOMPARE(holdSize(QSize(7680, 2160), QSize(1920, 1080), false), QSize(1920, 540));
        QCOMPARE(holdSize(QSize(2560, 1920), QSize(1001, 1001), false), QSize(1000, 750));
        QCOMPARE(holdSize(QSize(1024, 1024), QSize(1920, 1080), false), QSize(1024, 1024)); // no upscale
        QCOMPARE(holdSize(QSize(2560, 1920), QSize(), false), QSize(2560, 1920)); // pane unknown
    }
    void fisheyeHeldUpTo1920()
    {
        QCOMPARE(holdSize(QSize(2560, 2560), QSize(800, 800), true), QSize(1920, 1920));
        QCOMPARE(holdSize(QSize(2560, 2560), QSize(2400, 2400), true), QSize(1920, 1920)); // a cap
        QCOMPARE(holdSize(QSize(1024, 1024), QSize(800, 800), true), QSize(1024, 1024));
    }

    // The link can't feed HD backwards when two windows in a row take more than 80% of the
    // time they play for; it stays so for the run (HD comes back on the next open).
    void linkTooSlowAfterTwoSlowWindowsInARow()
    {
        LinkPace p;
        p.window(9.2, 10.0);           // 92%: slow
        QVERIFY(!p.tooSlow());
        p.window(3.0, 10.0);           // fine: the run of slow windows restarts
        p.window(8.5, 10.0);
        QVERIFY(!p.tooSlow());
        p.window(8.5, 10.0);
        QVERIFY(p.tooSlow());
        p.window(0.5, 10.0);           // stays slow for the run
        QVERIFY(p.tooSlow());
    }
    void linkPaceCountsTheSpeed()
    {
        LinkPace p;                    // 10 s of footage at 4× plays in 2.5 s
        p.window(2.4, 10.0 / 4);
        p.window(2.4, 10.0 / 4);
        QVERIFY(p.tooSlow());
        LinkPace q;
        q.window(0.6, 10.0 / 4);       // Lascelles: 0.6 s per window
        q.window(0.6, 10.0 / 4);
        QVERIFY(!q.tooSlow());
        LinkPace r;
        r.window(5.0, 0);              // nothing played (an empty window): not counted
        r.window(5.0, 0);
        QVERIFY(!r.tooSlow());
    }

    // ---- HD fast forward (wayfinder/live-replay ticket 09) ----
    // Windows fetched ahead of the playhead: from a group of slack before it, 10 s past it.
    void forwardWindowStartsAGroupBeforeThePlayhead()
    {
        const Window w = windowFrom(1'000'500);
        QCOMPARE(w.startSecs, qint64(1000 - 4));
        QCOMPARE(w.endSecs, qint64(1000 + 10));
        QCOMPARE(windowFrom(2'000'000, 30).endSecs, qint64(2030));
    }
    // A window's complete groups in order, from the last keyframe at or before the playhead;
    // the trailing group (cut by the window's end) is fetched again by the next window.
    void forwardGroupsStartAtTheKeyframeBeforeThePlayhead()
    {
        const qint64 t0 = 1'000'000;      // 2 s groups, 14 s of window
        const ForwardGops g = forwardGops(window(t0, 14, 2), t0 + 4'500, t0 + 86'400'000, false);
        QCOMPARE(g.inOrder.size(), 4);    // 4, 6, 8, 10 s; 12 s is the trailing group
        QCOMPARE(g.inOrder.first().first().epochMs, t0 + 4'000);
        QVERIFY(g.inOrder.first().first().key);
        QCOMPARE(g.inOrder.first().size(), 20);
        QCOMPARE(g.inOrder.last().last().epochMs, t0 + 11'900);
        QCOMPARE(g.nextFromMs, t0 + 12'000);
    }
    // The next window continues at the trailing group's keyframe: no frame twice, none missed.
    void forwardWindowsNeverRepeatOrSkipAFrame()
    {
        const qint64 t0 = 1'000'000;
        const QList<Frame> all = window(t0, 40, 2);
        QList<qint64> shown;
        qint64 from = t0 + 1'000;
        for (int i = 0; i < 3; ++i) {
            const Window w = windowFrom(from);
            QList<Frame> part;
            for (const Frame &f : all)
                if (f.epochMs >= w.startSecs * 1000 && f.epochMs < (w.endSecs + 1) * 1000)
                    part << f;
            const ForwardGops g = forwardGops(part, from, t0 + 86'400'000, false);
            for (const auto &gop : g.inOrder)
                shown << times(gop);
            from = g.nextFromMs;
        }
        for (int i = 1; i < shown.size(); ++i)
            QCOMPARE(shown[i] - shown[i - 1], qint64(100));
        QCOMPARE(shown.first(), t0);
    }
    // One group only (longer than the window): nothing yet, continue from it with a longer
    // window; `last` (the longest window, or the end of the day) sends the trailing group too.
    void aGroupLongerThanTheWindowIsNotSentUntilLast()
    {
        const qint64 t0 = 1'000'000;
        const ForwardGops g = forwardGops(window(t0, 10, 20), t0 + 2'000, t0 + 86'400'000, false);
        QVERIFY(g.inOrder.isEmpty());
        QCOMPARE(g.nextFromMs, t0);
        const ForwardGops last = forwardGops(window(t0, 10, 20), t0 + 2'000, t0 + 86'400'000, true);
        QCOMPARE(last.inOrder.size(), 1);
        QCOMPARE(last.nextFromMs, qint64(-1));
    }
    // Frames ahead but no keyframe in the window (a long group): nothing yet, continue from the
    // playhead with a longer window; ends only when `last`.
    void framesAheadWithoutAKeyframeAskForALongerWindow()
    {
        const qint64 t0 = 1'000'000;
        QList<Frame> noKey = window(t0, 10, 20);
        noKey.removeFirst();               // the group's keyframe came before the window
        const ForwardGops g = forwardGops(noKey, t0 + 2'000, t0 + 86'400'000, false);
        QVERIFY(g.inOrder.isEmpty());
        QCOMPARE(g.nextFromMs, t0 + 2'000);
        QCOMPARE(forwardGops(noKey, t0 + 2'000, t0 + 86'400'000, true).nextFromMs, qint64(-1));
    }
    // Nothing at or after the playhead (the newest footage, a gap) ends the run; frames from
    // the ceiling (the next midnight) on are dropped.
    void forwardEndsWithNothingAheadOrAtTheDaysEnd()
    {
        const qint64 t0 = 1'000'000;
        QCOMPARE(forwardGops(window(t0, 4, 2), t0 + 5'000, t0 + 86'400'000, false).nextFromMs,
                 qint64(-1));
        QVERIFY(forwardGops({}, t0, t0 + 86'400'000, false).inOrder.isEmpty());
        const ForwardGops g = forwardGops(window(t0, 10, 2), t0, t0 + 5'000, true);
        QCOMPARE(g.inOrder.size(), 3);
        QCOMPARE(g.inOrder.last().last().epochMs, t0 + 4'900);
        QCOMPARE(g.nextFromMs, qint64(-1));
    }
    // The player paces windowed forward play by the frames' recording times ÷ speed, and
    // re-anchors on a change of speed or a jump in the recording.
    void forwardPacerFollowsTheRecordingTimes()
    {
        ForwardPacer p;
        QCOMPARE(p.waitUs(10'000, 4.0, 0), qint64(0));            // the first frame: at once
        QCOMPARE(p.waitUs(10'400, 4.0, 0), qint64(100'000));      // 400 ms ÷ 4
        QCOMPARE(p.waitUs(10'800, 4.0, 150'000), qint64(50'000));
        QCOMPARE(p.waitUs(11'000, 2.0, 200'000), qint64(0));      // speed changed: re-anchor
        QCOMPARE(p.waitUs(11'200, 2.0, 200'000), qint64(100'000));
        QCOMPARE(p.waitUs(60'000, 2.0, 300'000), qint64(0));      // a jump: re-anchor
        QCOMPARE(p.waitUs(59'000, 2.0, 300'000), qint64(0));      // back in time: re-anchor
        QVERIFY(!p.tooSlow());
    }
    // More than a second behind twice within 20 s of footage: too slow (HD drops to SD).
    // A late frame re-anchors (no rush to catch up).
    void forwardPacerTooSlowWhenFallingBehindTwice()
    {
        ForwardPacer p;
        p.waitUs(0, 4.0, 0);
        QCOMPARE(p.waitUs(4'000, 4.0, 2'500'000), qint64(0));    // due at 1 s, 1.5 s late
        QVERIFY(!p.tooSlow());
        QCOMPARE(p.waitUs(4'400, 4.0, 2'500'000), qint64(100'000)); // re-anchored
        p.waitUs(8'000, 4.0, 4'600'000);                           // due 3.5 s: 1.1 s late
        QVERIFY(p.tooSlow());
        ForwardPacer q;
        q.waitUs(0, 1.0, 0);
        q.waitUs(1'000, 1.0, 2'500'000);                           // late once
        q.waitUs(30'000, 1.0, 2'500'000 + 29'000'000);             // on time
        q.waitUs(31'000, 1.0, 2'500'000 + 31'500'000);             // late again, 29 s later
        QVERIFY(!q.tooSlow());
    }

    // ---- frame times from the recorder's clocks ----
    // Only keyframes carry a usable wall-clock time; the others take the frame clock since.
    void frameClockTimesFramesFromTheLastKeyframe()
    {
        FrameClock c;
        QCOMPARE(c.epochMs(false, 0, 5'000'000), qint64(-1));      // no keyframe yet
        QCOMPARE(c.epochMs(true, 1000, 7'000'000), qint64(1'000'000));
        QCOMPARE(c.epochMs(false, 12345, 7'250'000), qint64(1'000'250)); // junk time ignored
        QCOMPARE(c.epochMs(false, 0, 6'000'000), qint64(1'000'000));   // clock went back: clamp
    }

    // ---- the player's side ----
    // Decoded frames arrive group by group (newest group first, each forwards); a group is
    // shown backwards once the next group's keyframe arrives, and the last at the end.
    void playerShowsEachGroupBackwards()
    {
        GopReverser<int> r;
        QVERIFY(r.push(90, true, 9000).isEmpty());
        QVERIFY(r.push(91, false, 9100).isEmpty());
        QVERIFY(r.push(92, false, 9200).isEmpty());
        const auto first = r.push(80, true, 8000);
        QCOMPARE(first.size(), 3);
        QCOMPARE(first[0].frame, 92);
        QCOMPARE(first[2].frame, 90);
        QVERIFY(r.push(81, false, 8100).isEmpty());
        const auto rest = r.flush();
        QCOMPARE(rest.size(), 2);
        QCOMPARE(rest[0].frame, 81);
        QCOMPARE(rest[1].frame, 80);
        QVERIFY(r.flush().isEmpty());
    }

    // Shown at the gaps between their recording times, divided by the speed.
    void pacingFollowsTheRecordingTimesBackwards()
    {
        QCOMPARE(waitUs(9200, 9100, 1.0), qint64(100'000));
        QCOMPARE(waitUs(9200, 9100, 4.0), qint64(25'000));
        QCOMPARE(waitUs(9200, 9100, 0.25), qint64(400'000));
        QCOMPARE(waitUs(9000, 8900, 2.0), qint64(50'000));
        QCOMPARE(waitUs(-1, 9100, 1.0), qint64(0));           // the first frame: at once
        QCOMPARE(waitUs(9100, 9200, 1.0), qint64(0));         // out of order: at once
        QCOMPARE(waitUs(20'000, 9100, 1.0), qint64(1'000'000)); // a jump: at most a second
    }
};

QTEST_GUILESS_MAIN(TestReversePlay)
#include "test_reverseplay.moc"
