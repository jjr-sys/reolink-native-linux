#include "device/ActivityPolicy.h"

#include <QtTest>

using namespace rl::activity;

namespace {
constexpr qint64 T0 = 1'000'000; // fake clock, ms
constexpr qint64 s(int n) { return n * 1000LL; }

// 4 tiles showing cameras 0..3 (baseline). Cameras 0,1 on host 1; 2,3 on host 2;
// cameras 10..19 are off-grid (host 1 for 10..14, host 2 for 15..19).
ActivityPolicy make()
{
    ActivityPolicy p;
    p.setHostLookup([](int row) -> qint64 { return (row < 2 || (row >= 10 && row < 15)) ? 1 : 2; });
    p.setBaseline({0, 1, 2, 3});
    return p;
}
int shown(const ActivityPolicy &p, int tile) { return p.tile(tile).shown; }
} // namespace

class TestActivity : public QObject
{
    Q_OBJECT
private slots:
    void idleGridChangesNothing()
    {
        ActivityPolicy p = make();
        QVERIFY(p.tick(T0).isEmpty());
        QCOMPARE(shown(p, 2), 2);
    }

    // Acceptance 3: the replay starts about 10 s before the trigger.
    void personTakesAFreeTileAndReplaysFromBeforeTheTrigger()
    {
        ActivityPolicy p = make();
        p.detect(10, Kind::Person, T0);
        const auto ch = p.tick(T0 + 500);
        QCOMPARE(ch.size(), 1);
        QCOMPARE(ch[0].row, 10);
        QVERIFY(ch[0].activity);
        QCOMPARE(ch[0].replayFromMs, T0 - s(10));
        QCOMPARE(shown(p, ch[0].tile), 10);
    }

    // Acceptance 2: hold respected. After it the tile keeps its camera on screen (no
    // churn) but is free for new activity.
    void holdIsThirtySecondsThenTheTileKeepsItsCameraButIsFree()
    {
        ActivityPolicy p = make();
        p.detect(10, Kind::Person, T0);
        const int tile = p.tick(T0)[0].tile;
        QVERIFY(p.tick(T0 + s(29)).isEmpty());
        QVERIFY(p.tile(tile).active);
        QVERIFY(p.tick(T0 + s(31)).isEmpty()); // nothing to switch back to
        QVERIFY(!p.tile(tile).active);
        QCOMPARE(shown(p, tile), 10);
    }

    // The same camera again lands on the tile it already has, not a second one.
    void sameCameraAfterTheHoldIsHighlightedWhereItIs()
    {
        ActivityPolicy p = make();
        p.detect(10, Kind::Person, T0);
        const int tile = p.tick(T0)[0].tile;
        p.tick(T0 + s(40));
        p.detect(10, Kind::Person, T0 + s(60));
        QVERIFY(p.tick(T0 + s(60)).isEmpty());
        QVERIFY(p.tile(tile).active);
        QCOMPARE(shown(p, tile), 10);
    }

    // New activity replaces the tile that has been inactive longest.
    void newActivityReplacesTheMostInactiveTile()
    {
        ActivityPolicy p = make();
        p.setPinned(2, true);
        p.setPinned(3, true);
        p.detect(10, Kind::Person, T0);
        const int a = p.tick(T0)[0].tile;   // tile 0 (all idle since never)
        p.detect(11, Kind::Person, T0 + s(1));
        const int b = p.tick(T0 + s(1))[0].tile; // the other free tile
        QVERIFY(a != b);
        p.tick(T0 + s(35)); // both released: a idle since 31 s, b since 32 s
        p.detect(12, Kind::Person, T0 + s(40));
        const auto ch = p.tick(T0 + s(40));
        QCOMPARE(ch.size(), 1);
        QCOMPARE(ch[0].tile, a);            // idle longest
        QCOMPARE(shown(p, b), 11);          // the other keeps its camera
    }

    void untrackedKindsAreIgnored()
    {
        ActivityPolicy p = make();
        p.setTracked({Kind::Person});
        p.detect(10, Kind::Motion, T0);
        p.detect(11, Kind::Vehicle, T0);
        QVERIFY(p.tick(T0).isEmpty());
        QCOMPARE(p.queued(), 0);
        p.detect(12, Kind::Person, T0);
        QCOMPARE(p.tick(T0).size(), 1);
    }

    void retriggerExtendsTheHoldAndDoesNotDuplicate()
    {
        ActivityPolicy p = make();
        p.detect(10, Kind::Person, T0);
        const int tile = p.tick(T0)[0].tile;
        p.detect(10, Kind::Person, T0 + s(25));
        QVERIFY(p.tick(T0 + s(26)).isEmpty());
        QVERIFY(p.tick(T0 + s(50)).isEmpty()); // 25 + 30 = 55
        QVERIFY(p.tile(tile).active);
        p.tick(T0 + s(56));
        QVERIFY(!p.tile(tile).active);
        QCOMPARE(shown(p, tile), 10);
    }

    // A camera already on screen is marked active in place, not swapped into a second tile.
    void cameraAlreadyVisibleIsMarkedInPlace()
    {
        ActivityPolicy p = make();
        p.detect(1, Kind::Person, T0);
        QVERIFY(p.tick(T0).isEmpty());
        QVERIFY(p.tile(1).active);
        QCOMPARE(p.tile(1).kind, Kind::Person);
        QCOMPARE(shown(p, 1), 1);
        // and it goes quiet again without any stream change
        QVERIFY(p.tick(T0 + s(40)).isEmpty());
        QVERIFY(!p.tile(1).active);
    }

    // Motion only fills a free tile; it never displaces a person.
    void motionNeverDisplacesAPerson()
    {
        ActivityPolicy p = make();
        for (int r = 10; r < 14; ++r) p.detect(r, Kind::Person, T0);
        // Host 1 cap is 2 starts per window, so tick over several windows.
        for (int i = 0; i < 6; ++i) p.tick(T0 + i * 2100);
        for (int t = 0; t < 4; ++t) QVERIFY(p.tile(t).active);
        p.detect(14, Kind::Motion, T0 + s(15));
        QVERIFY(p.tick(T0 + s(16)).isEmpty());
        QCOMPARE(p.queued(), 1);
    }

    // Higher-priority activity may take a tile held for motion, after its minimum time.
    void personTakesAMotionHeldTileAfterTheMinimumShownTime()
    {
        ActivityPolicy p = make();
        for (int r = 10; r < 14; ++r) p.detect(r, Kind::Motion, T0);
        for (int i = 0; i < 6; ++i) p.tick(T0 + i * 2100);
        p.detect(14, Kind::Person, T0 + s(13));
        // Everything was shown by ~T0+8.4s, so at +13 s some tiles are under the 15 s minimum.
        p.tick(T0 + s(13));
        p.tick(T0 + s(14));
        QCOMPARE(p.queued(), 1);            // nothing displaceable yet
        const auto ch = p.tick(T0 + s(20)); // minimum shown time is over
        QCOMPARE(ch.size(), 1);
        QCOMPARE(ch[0].row, 14);
        QCOMPARE(p.queued(), 0);
    }

    // Acceptance 4: at most two new streams per NVR per window.
    void startsPerHostAreCapped()
    {
        ActivityPolicy p = make();
        for (int r = 10; r < 14; ++r) p.detect(r, Kind::Person, T0); // all host 1
        const auto first = p.tick(T0);
        QCOMPARE(first.size(), 2);
        QCOMPARE(p.queued(), 2);
        QCOMPARE(p.tick(T0 + 500).size(), 0); // still inside the window
        QCOMPARE(p.tick(T0 + 2500).size(), 2);
    }

    void hostBudgetsAreIndependent()
    {
        ActivityPolicy p = make();
        p.detect(10, Kind::Person, T0);
        p.detect(11, Kind::Person, T0);
        p.detect(12, Kind::Person, T0);
        p.detect(15, Kind::Person, T0);
        p.detect(16, Kind::Person, T0);
        // 3 on host 1 (cap 2) + 2 on host 2 -> 4 starts now, one waits.
        QCOMPARE(p.tick(T0).size(), 4);
        QCOMPARE(p.queued(), 1);
    }

    // Acceptance 5: pinned tiles never move (and never revert either).
    void pinnedTileIsNeverReplaced()
    {
        ActivityPolicy p = make();
        p.setPinned(0, true);
        p.setPinned(1, true);
        p.setPinned(2, true);
        p.detect(10, Kind::Person, T0);
        const auto ch = p.tick(T0);
        QCOMPARE(ch.size(), 1);
        QCOMPARE(ch[0].tile, 3);
        p.detect(11, Kind::Person, T0);
        p.tick(T0 + s(1));
        QCOMPARE(p.queued(), 1); // no free tile: queued, not yanked
        QCOMPARE(shown(p, 0), 0);
        QCOMPARE(shown(p, 1), 1);
        QCOMPARE(shown(p, 2), 2);
    }

    // Idle tile is picked by longest idle; a recently released tile is the last choice.
    void freeTileChoiceIsLongestIdle()
    {
        ActivityPolicy p = make();
        p.detect(10, Kind::Person, T0);
        const int a = p.tick(T0)[0].tile;
        p.tick(T0 + s(31)); // released at +31 s, now idle since then
        p.detect(11, Kind::Person, T0 + s(32));
        const auto ch = p.tick(T0 + s(32));
        QCOMPARE(ch.size(), 1);
        QVERIFY(ch[0].tile != a);
    }

    void queueDrainsInPriorityThenArrivalOrder()
    {
        ActivityPolicy p = make();
        p.setPinned(0, true); p.setPinned(1, true); p.setPinned(2, true);
        p.detect(10, Kind::Person, T0);
        p.tick(T0); // takes tile 3
        p.detect(11, Kind::Motion, T0 + s(1));
        p.detect(12, Kind::Person, T0 + s(2));
        p.detect(13, Kind::Person, T0 + s(3));
        QCOMPARE(p.tick(T0 + s(4)).size(), 0);
        QCOMPARE(p.queued(), 3);
        const auto ch = p.tick(T0 + s(31)); // tile 3 released: next is the first queued person
        QCOMPARE(ch.size(), 1);
        QCOMPARE(ch[0].row, 12);
    }

    // The 15 s / 30 s clock starts when the tile is actually shown, not when the event arrived.
    void holdClockStartsWhenShown()
    {
        ActivityPolicy p = make();
        p.setPinned(0, true); p.setPinned(1, true); p.setPinned(2, true);
        p.detect(10, Kind::Person, T0);
        p.tick(T0);
        p.detect(11, Kind::Person, T0 + s(1));
        p.tick(T0 + s(31));                    // 11 shown now, from a 30 s-old queue wait
        QVERIFY(p.tick(T0 + s(45)).isEmpty()); // still held
        QCOMPARE(p.tile(3).shown, 11);
        QVERIFY(p.tile(3).active);
        p.tick(T0 + s(62));
        QVERIFY(!p.tile(3).active);
    }

    void excludedCamerasNeverTrigger()
    {
        ActivityPolicy p = make();
        p.setExcluded(10, true);
        p.detect(10, Kind::Person, T0);
        QVERIFY(p.tick(T0).isEmpty());
        QCOMPARE(p.queued(), 0);
    }

    void changingTheBaselineMovesIdleTilesButNotHeldOnes()
    {
        ActivityPolicy p = make();
        p.detect(10, Kind::Person, T0);
        const int tile = p.tick(T0)[0].tile;
        p.setBaseline({5, 6, 7, 8});
        QCOMPARE(shown(p, tile), 10);          // held: keeps the activity camera
        for (int t = 0; t < 4; ++t)
            if (t != tile) QCOMPARE(shown(p, t), 5 + t);
        p.setBaseline({5, 6, 7, 8});           // same baseline again changes nothing
        QCOMPARE(shown(p, tile), 10);
    }

    // Acceptance 1 (no flicker): an activity tile is never replaced within its minimum
    // shown time (returning a released baseline tile to work is not part of that).
    void noActivityTileIsReplacedWithinTheMinimumTime()
    {
        ActivityPolicy p = make();
        QHash<int, qint64> shownAt; // tile -> when it last began showing activity
        int violations = 0;
        for (int i = 0; i < 400; ++i) {
            const qint64 now = T0 + i * 700;
            if (i % 3 == 0) p.detect(10 + (i / 3) % 10, i % 2 ? Kind::Person : Kind::Motion, now);
            for (const Change &c : p.tick(now)) {
                if (shownAt.contains(c.tile) && !c.activity && now - shownAt[c.tile] < s(15))
                    ++violations;
                if (shownAt.contains(c.tile) && c.activity && now - shownAt[c.tile] < s(15))
                    ++violations;
                if (c.activity)
                    shownAt[c.tile] = now;
                else
                    shownAt.remove(c.tile);
            }
        }
        QCOMPARE(violations, 0);
    }
};

QTEST_GUILESS_MAIN(TestActivity)
#include "test_activity.moc"
