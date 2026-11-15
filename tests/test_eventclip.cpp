#include "ui/EventClip.h"

#include <QtTest>

using namespace rl;

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
};

QTEST_GUILESS_MAIN(TestEventClip)
#include "test_eventclip.moc"
