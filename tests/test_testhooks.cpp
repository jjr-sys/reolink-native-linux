#include "ui/TestHooks.h"

#include <QtTest>

using namespace rl;

// The on-screen check hooks QML reads (wayfinder/architecture ticket 06): one object, read
// once from the RL_* environment; nothing set means every hook is off.
class TestTestHooks : public QObject
{
    Q_OBJECT

    static void clearAll()
    {
        for (const char *v : {"RL_INITIAL_PAGE", "RL_EVENTS_AUTOSCROLL", "RL_EVENTS_PLAY",
                              "RL_EVENTS_KEYS", "RL_SETTINGS_CATEGORY", "RL_MOCK_RECORDINGS",
                              "RL_MOCK_DOORBELL", "RL_OPEN_CLIP", "RL_PLAYBACK_AUTOPLAY",
                              "RL_PLAYBACK_STOP_MS", "RL_PLAYBACK_AT", "RL_PLAYBACK_HD",
                              "RL_PLAYBACK_BACK", "RL_PLAYBACK_FAST", "RL_PANE_BARS",
                              "RL_LIVE_REPLAY"})
            qunsetenv(v);
    }

private slots:
    void init() { clearAll(); }
    void cleanup() { clearAll(); }

    void nothingSetMeansEveryHookIsOff()
    {
        const TestHooks h;
        QCOMPARE(h.initialPage(), 0);
        QVERIFY(!h.eventsAutoScroll() && !h.eventsAutoPlay() && !h.mockRecordings());
        QVERIFY(!h.mockDoorbell() && !h.playbackAutoplay() && !h.playbackHd() && !h.paneBarsShown());
        QVERIFY(h.eventsKeys().isEmpty() && h.liveReplayTest().isEmpty() && h.openClipTarget().isEmpty());
        QCOMPARE(h.playbackAtSecs(), -1);   // unset: not "midnight"
        QCOMPARE(h.playbackStopMs(), 0);
    }

    void eachHookReadsItsVariable()
    {
        qputenv("RL_INITIAL_PAGE", "2");
        qputenv("RL_EVENTS_AUTOSCROLL", "1");
        qputenv("RL_MOCK_RECORDINGS", "1");
        qputenv("RL_OPEN_CLIP", "1:4");
        qputenv("RL_PLAYBACK_AUTOPLAY", "1");
        qputenv("RL_PLAYBACK_STOP_MS", "5000");
        qputenv("RL_PLAYBACK_HD", "1");
        qputenv("RL_PLAYBACK_FAST", "2");
        qputenv("RL_EVENTS_PLAY", "1");
        qputenv("RL_EVENTS_KEYS", "J,+1,K");
        qputenv("RL_SETTINGS_CATEGORY", "mosaics");
        qputenv("RL_MOCK_DOORBELL", "3:0@5");
        qputenv("RL_PLAYBACK_AT", "0");
        qputenv("RL_PLAYBACK_BACK", "3");
        qputenv("RL_PANE_BARS", "1");
        qputenv("RL_LIVE_REPLAY", "2:2;J");
        const TestHooks h;
        QCOMPARE(h.initialPage(), 2);
        QVERIFY(h.eventsAutoPlay());
        QCOMPARE(h.eventsKeys(), QStringLiteral("J,+1,K"));
        QCOMPARE(h.initialSettingsCategory(), QStringLiteral("mosaics"));
        QVERIFY(h.mockDoorbell());
        QCOMPARE(h.mockDoorbellTarget(), QStringLiteral("3:0@5"));
        QCOMPARE(h.playbackAtSecs(), 0);    // set to midnight
        QCOMPARE(h.playbackBackSteps(), 3);
        QVERIFY(h.paneBarsShown());
        QCOMPARE(h.liveReplayTest(), QStringLiteral("2:2;J"));
        QVERIFY(h.eventsAutoScroll() && h.mockRecordings() && h.playbackAutoplay() && h.playbackHd());
        QCOMPARE(h.openClipTarget(), QStringLiteral("1:4"));
        QCOMPARE(h.playbackStopMs(), 5000);
        QCOMPARE(h.playbackFastSteps(), 2);
    }
};

QTEST_GUILESS_MAIN(TestTestHooks)
#include "test_testhooks.moc"
