#include "ui/PaneSync.h"
#include "ui/PlaybackDay.h"

#include <QtTest>

using namespace rl;

// Playback's day (architecture review #2, candidate 1): the selected cameras' footage for one
// day, the camera that leads, where a play lands and carries on, the playhead's clock and the
// searches — what PlaybackPage.qml held untested. Positions are seconds into the day.
// And the rules Playback and Live replay share (PaneSync): whose frames move the playhead, and
// when a pane that isn't driving starts again at the playhead.
namespace {
QVariantList segs(std::initializer_list<std::pair<double, double>> spans)
{
    QVariantList out;
    for (const auto &[s, e] : spans)
        out << QVariantMap{{"start", s}, {"end", e}, {"type", "timer"}};
    return out;
}
QVariantMap pane(const QString &key, bool shown, bool frozen, double frameSecs)
{
    return {{"key", key}, {"shown", shown}, {"frozen", frozen}, {"frameSecs", frameSecs}};
}
} // namespace

class TestPlaybackDay : public QObject
{
    Q_OBJECT
private slots:
    // A day's answer (wayfinder/architecture ticket 01) is taken only for a pane's camera and
    // the day shown: a search still out for the day before, or another camera's (a Live
    // replay search), is left alone.
    void takesOnlyAnswersForItsCamerasAndDay()
    {
        PlaybackDay d;
        d.setPaneOrder({QStringLiteral("1:6"), QStringLiteral("2:2")});
        d.setDate(2026, 11, 16);
        QVERIFY(d.takes(QStringLiteral("1:6"), 2026, 11, 16));
        QVERIFY(d.takes(QStringLiteral("2:2"), 2026, 11, 16));
        QVERIFY(!d.takes(QStringLiteral("1:6"), 2026, 11, 15));   // the day before
        QVERIFY(!d.takes(QStringLiteral("1:6"), 2026, 10, 16));
        QVERIFY(!d.takes(QStringLiteral("3:0"), 2026, 11, 16));   // not a pane
        d.setDate(2026, 11, 15);
        QVERIFY(d.takes(QStringLiteral("1:6"), 2026, 11, 15));
        QVERIFY(!d.takes(QStringLiteral("1:6"), 2026, 11, 16));
    }

    // The selected camera leads; a Frigate one (1× forward only, no reverse) passes it to the
    // first recorder camera in pane order. None: no lead.
    void theSelectedRecorderCameraLeads()
    {
        PlaybackDay d;
        d.setPaneOrder({"3:0", "1:6", "1:2"});
        d.setDirectKeys({"3:0"});
        d.setSelectedKey("1:2");
        QCOMPARE(d.leadKey(), QStringLiteral("1:2"));
        d.setSelectedKey("3:0");
        QCOMPARE(d.leadKey(), QStringLiteral("1:6"));
        d.setSelectedKey("");
        QCOMPARE(d.leadKey(), QStringLiteral("1:6"));
        d.setPaneOrder({"3:0"});
        QCOMPARE(d.leadKey(), QString());
    }

    void footageAndItsEdges()
    {
        PlaybackDay d;
        d.setSegments("1:6", segs({{3600, 7200}, {28800, 32400}}));
        QVERIFY(d.known("1:6"));
        QVERIFY(!d.known("1:2"));
        QVERIFY(d.covered("1:6", 3600));
        QVERIFY(d.covered("1:6", 7200));
        QVERIFY(!d.covered("1:6", 8000));
        QVERIFY(!d.covered("1:2", 3600));
        QCOMPARE(d.previousEnd("1:6", 28800), 7200.0);
        QCOMPARE(d.previousEnd("1:6", 7200.4), -1.0);   // half a second: still that recording
        QCOMPARE(d.nextStart("1:6", 7200), 28800.0);
        QCOMPARE(d.nextStart("1:6", 28800), -1.0);
        d.clear();
        QVERIFY(!d.known("1:6"));
    }

    // A play backwards from where the lead has nothing starts at the end of its previous
    // recording, or nowhere (-1: nothing earlier that day). Forwards, every pane answers for
    // itself (play, or "no footage") where it is asked.
    void wherePlayLands()
    {
        PlaybackDay d;
        d.setPaneOrder({"1:6"});
        d.setSegments("1:6", segs({{3600, 7200}, {28800, 32400}}));
        QCOMPARE(d.landing(20000, 1), 20000.0);
        QCOMPARE(d.landing(5000, -1), 5000.0);
        QCOMPARE(d.landing(20000, -1), 7200.0);
        QCOMPARE(d.landing(1000, -1), -1.0);
        d.setPaneOrder({});
        QCOMPARE(d.landing(1000, -1), 1000.0);   // no lead: nothing to decide by
    }

    // The lead's stream ran out: every pane goes on from its previous recording's end
    // (backwards) or next one's start (forwards), or none that day. Another pane's end, or an
    // end against the way it plays, is nothing to act on.
    void whereAStreamThatRanOutCarriesOn()
    {
        PlaybackDay d;
        d.setPaneOrder({"1:6", "1:2"});
        d.setSegments("1:6", segs({{3600, 7200}, {28800, 32400}}));
        QCOMPARE(d.carryOn("1:6", -1, 28800), 7200.0);
        QCOMPARE(d.carryOn("1:6", -1, 3600), double(PlaybackDay::Nothing));
        QCOMPARE(d.carryOn("1:6", 1, 7200), 28800.0);
        QCOMPARE(d.carryOn("1:6", 1, 32400), double(PlaybackDay::Nothing));
        QCOMPARE(d.carryOn("1:2", -1, 28800), double(PlaybackDay::Ignore));
    }

    // A recorder that has just connected may answer its first search empty: an empty day is
    // asked again, at most three times, 10 s apart; footage is never asked again. force asks
    // regardless (a new day, the page refreshed).
    void searchesAreRetriedForAnEmptyDay()
    {
        PlaybackDay d;
        QVERIFY(d.wantSearch("1:6", 0, false));
        QVERIFY(!d.wantSearch("1:6", 9999, false));   // asked less than 10 s ago
        d.setSegments("1:6", {});
        QVERIFY(d.wantSearch("1:6", 10000, false));
        QVERIFY(d.wantSearch("1:6", 20000, false));   // the third
        QVERIFY(!d.wantSearch("1:6", 30000, false));
        QVERIFY(d.wantSearch("1:6", 30001, true));
        d.setSegments("1:2", segs({{0, 10}}));
        QVERIFY(!d.wantSearch("1:2", 50000, false));
        d.clear();
        QVERIFY(d.wantSearch("1:2", 50001, false));
    }

    // The playhead runs with the clock at the group's speed and direction, inside the day; off
    // 1× the frames of the pane that drives it move it instead.
    void thePlayheadsClock()
    {
        QCOMPARE(PlaybackDay::advance(100, 0.5, 4, 1, -1), 102.0);
        QCOMPARE(PlaybackDay::advance(100, 0.5, 2, -1, -1), 99.0);
        QCOMPARE(PlaybackDay::advance(0.5, 1, 1, -1, -1), 0.0);
        QCOMPARE(PlaybackDay::advance(86398.8, 1, 1, 1, -1), 86399.0);
        QCOMPARE(PlaybackDay::advance(86399, 1, 1, 1, -1), 86399.0);
        QCOMPARE(PlaybackDay::advance(100, 0.5, 16, 1, 250), 250.0);
        QCOMPARE(PlaybackDay::advance(100, 0.5, 16, 1, 90000), 86399.0);
    }

    // ---- Shared with Live replay (PaneSync) ----

    // Whose frames move the playhead: the lead's pane while it is on screen, not frozen and has
    // footage at the playhead; else the first such pane in order; else none (the wall clock).
    void theFrameDriver()
    {
        PlaybackDay d;
        d.setPaneOrder({"1:6", "1:2", "3:0"});
        d.setSelectedKey("1:2");
        d.setSegments("1:6", segs({{0, 1000}}));
        d.setSegments("1:2", segs({{0, 100}}));
        const QVariantList panes{pane("1:6", true, false, 50), pane("1:2", true, false, 50),
                                 pane("3:0", true, false, 50)};
        QCOMPARE(d.frameDriver(panes, 50, false), QStringLiteral("1:2"));
        QCOMPARE(d.frameDriver(panes, 500, false), QStringLiteral("1:6"));   // the lead has none here
        QCOMPARE(d.frameDriver({pane("1:6", false, false, 50), pane("1:2", true, true, 50)}, 50, false),
                 QString());   // hidden, frozen
        QCOMPARE(d.frameDriverSecs({pane("1:6", true, false, -1)}, 50, true), -1.0);   // no frame yet
        QCOMPARE(d.frameDriverSecs({pane("1:6", true, false, 60)}, 50, true), 60.0);
        // A Frigate camera selected: the lead is the first recorder; off 1× a Frigate pane is
        // stopped (locked), so its stale frame never drives (review #2, standards review).
        d.setDirectKeys({"3:0"});
        d.setSelectedKey("3:0");
        d.setSegments("3:0", segs({{0, 1000}}));
        const QVariantList withFrigate{pane("3:0", true, false, 40), pane("1:6", true, false, 55)};
        QCOMPARE(d.frameDriver(withFrigate, 50, true), QStringLiteral("1:6"));
        QCOMPARE(d.frameDriverSecs(withFrigate, 50, true), 55.0);
        QCOMPARE(d.frameDriver({pane("3:0", true, false, 40)}, 50, true), QString());
    }

    // A pane that doesn't drive the playhead plays at its own pace: where it has footage at
    // the playhead and isn't playing, or is more than 5 s × speed from it, it starts again at
    // the playhead — not more often than every 3 s, and not while hidden, frozen, on a still
    // or locked at 1× (Frigate off 1×). Both ways (Live replay's rule; Playback's was
    // backwards only, 5 s flat).
    void restartingAPaneAtThePlayhead_data()
    {
        QTest::addColumn<bool>("busy");
        QTest::addColumn<double>("frameSecs");
        QTest::addColumn<double>("speed");
        QTest::addColumn<double>("sinceLastMs");
        QTest::addColumn<QString>("block");
        QTest::addColumn<bool>("restart");
        QTest::newRow("in step") << true << 104.0 << 1.0 << 9000.0 << "" << false;
        QTest::newRow("stopped") << false << -1.0 << 1.0 << 9000.0 << "" << true;
        QTest::newRow("6 s out at 1x") << true << 106.0 << 1.0 << 9000.0 << "" << true;
        QTest::newRow("6 s out at 2x") << true << 94.0 << 2.0 << 9000.0 << "" << false;
        QTest::newRow("11 s out at 2x") << true << 89.0 << 2.0 << 9000.0 << "" << true;
        QTest::newRow("restarted 2 s ago") << false << -1.0 << 1.0 << 2000.0 << "" << false;
        // Its own open counts too: a pane opened a moment ago is still finding its feet.
        QTest::newRow("opened 1 s ago") << false << -1.0 << 1.0 << 1000.0 << "" << false;
        // The driver isn't pulled back to the playhead (its frames move it) — but a driver that
        // isn't playing moves nothing: it starts like any pane (a joined lead never opened).
        QTest::newRow("drives, drifted") << true << 106.0 << 1.0 << 9000.0 << "driver" << false;
        QTest::newRow("drives, stopped") << false << -1.0 << 1.0 << 9000.0 << "driver" << true;
        QTest::newRow("hidden") << false << -1.0 << 1.0 << 9000.0 << "hidden" << false;
        QTest::newRow("frozen") << false << -1.0 << 1.0 << 9000.0 << "frozen" << false;
        QTest::newRow("still") << false << -1.0 << 1.0 << 9000.0 << "still" << false;
        QTest::newRow("locked") << false << -1.0 << 1.0 << 9000.0 << "locked" << false;
        QTest::newRow("no footage") << false << -1.0 << 1.0 << 9000.0 << "uncovered" << false;
        // A pane catching up by itself (1.5× / 0.7× after a late first picture) isn't also
        // reopened: one alignment at a time (review #2, after the on-screen check).
        QTest::newRow("catching up") << true << 106.0 << 1.0 << 9000.0 << "aligning" << false;
    }
    void restartingAPaneAtThePlayhead()
    {
        QFETCH(bool, busy);
        QFETCH(double, frameSecs);
        QFETCH(double, speed);
        QFETCH(double, sinceLastMs);
        QFETCH(QString, block);
        QFETCH(bool, restart);
        panesync::Pane p;
        p.key = QStringLiteral("1:2");
        p.busy = busy;
        p.frameSecs = frameSecs;
        p.covered = block != "uncovered";
        p.shown = block != "hidden";
        p.frozen = block == "frozen";
        p.still = block == "still";
        p.locked = block == "locked";
        p.aligning = block == "aligning";
        p.lastOpenMs = 100000 - sinceLastMs;
        QCOMPARE(panesync::needsRestart(p, 100, speed, block == "driver", 100000), restart);
    }

    // Playback's panes, as the page reports them: the same rule, covered by this day's footage.
    void playbackAsksTheSameRule()
    {
        PlaybackDay d;
        d.setPaneOrder({"1:6", "1:2", "3:0"});
        d.setSelectedKey("1:6");
        d.setDirectKeys({"3:0"});
        d.setSegments("1:6", segs({{0, 1000}}));
        d.setSegments("1:2", segs({{0, 1000}}));
        d.setSegments("3:0", segs({{0, 1000}}));
        QVariantMap p{{"key", "1:2"}, {"shown", true}, {"busy", false}, {"frameSecs", -1},
                      {"lastOpenMs", -1}};
        // (pane, playhead, speed, off 1×, the pane driving the playhead, now)
        QVERIFY(d.needsRestart(p, 100, 1, false, "1:6", 50000));
        p["key"] = "1:6";   // the driver, not playing: started
        QVERIFY(d.needsRestart(p, 100, 1, false, "1:6", 50000));
        p["key"] = "3:0";   // Frigate: only at 1× forward
        QVERIFY(d.needsRestart(p, 100, 1, false, "1:6", 50000));
        QVERIFY(!d.needsRestart(p, 100, 2, true, "1:6", 50000));
        p["key"] = "1:2";
        QVERIFY(!d.needsRestart(p, 2000, 1, false, "1:6", 50000));   // no footage there
        p["busy"] = true;
        p["frameSecs"] = 108;   // 8 s out …
        QVERIFY(d.needsRestart(p, 100, 1, false, "1:6", 50000));
        p["aligning"] = true;   // … but catching up by itself
        QVERIFY(!d.needsRestart(p, 100, 1, false, "1:6", 50000));
    }
};

QTEST_GUILESS_MAIN(TestPlaybackDay)
#include "test_playbackday.moc"
