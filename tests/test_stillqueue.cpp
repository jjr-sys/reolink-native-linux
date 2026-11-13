#include "ui/StillQueue.h"

#include <QSignalSpy>
#include <QtTest>

using namespace rl;

// Playback stills (wayfinder/fisheye-and-framing step 08): each pane asks for a picture at
// the playhead; at most a few at once per recorder (an NVR has a connection limit), the
// newest request per pane wins, and playing cancels them all.
class TestStillQueue : public QObject
{
    Q_OBJECT

    static QStringList started(const QSignalSpy &spy)
    {
        QStringList out;
        for (const QList<QVariant> &args : spy)
            out << args.at(0).toString();
        return out;
    }

private slots:
    void requestsStartAtOnceUpToTheLimitPerRecorder()
    {
        StillQueue q;
        q.setPerRecorder(2);
        QSignalSpy spy(&q, &StillQueue::startStill);
        q.request("1:0", 1);
        q.request("1:3", 1);
        q.request("1:6", 1);
        QCOMPARE(started(spy), QStringList({"1:0", "1:3"}));
        QCOMPARE(q.running(1), 2);
        QCOMPARE(q.waiting(), 1);
    }

    void recordersHaveTheirOwnLimit()
    {
        StillQueue q;
        q.setPerRecorder(1);
        QSignalSpy spy(&q, &StillQueue::startStill);
        q.request("1:0", 1);
        q.request("1:3", 1);
        q.request("2:7", 2);
        QCOMPARE(started(spy), QStringList({"1:0", "2:7"}));
    }

    void aFinishedStillStartsTheNextInOrder()
    {
        StillQueue q;
        q.setPerRecorder(1);
        QSignalSpy spy(&q, &StillQueue::startStill);
        q.request("1:0", 1);
        q.request("1:3", 1);
        q.request("1:6", 1);
        q.done("1:0");
        QCOMPARE(started(spy), QStringList({"1:0", "1:3"}));
        q.done("1:3");
        QCOMPARE(started(spy), QStringList({"1:0", "1:3", "1:6"}));
        q.done("1:6");
        QCOMPARE(q.running(1), 0);
        QCOMPARE(q.waiting(), 0);
    }

    void askingAgainWhileWaitingKeepsOnePlaceInLine()
    {
        StillQueue q;
        q.setPerRecorder(1);
        QSignalSpy spy(&q, &StillQueue::startStill);
        q.request("1:0", 1);
        q.request("1:3", 1);
        q.request("1:3", 1);
        QCOMPARE(q.waiting(), 1);
        q.done("1:0");
        QCOMPARE(started(spy), QStringList({"1:0", "1:3"}));
    }

    void askingAgainWhileRunningRestartsItInItsOwnSlot()
    {
        // Dragging: the pane's newer moment replaces the one it is fetching.
        StillQueue q;
        q.setPerRecorder(1);
        QSignalSpy spy(&q, &StillQueue::startStill);
        q.request("1:0", 1);
        q.request("1:3", 1);
        q.request("1:0", 1);
        QCOMPARE(started(spy), QStringList({"1:0", "1:0"}));
        QCOMPARE(q.running(1), 1);
        QCOMPARE(q.waiting(), 1);
    }

    void playingCancelsEverything()
    {
        StillQueue q;
        q.setPerRecorder(1);
        QSignalSpy spy(&q, &StillQueue::startStill);
        q.request("1:0", 1);
        q.request("1:3", 1);
        q.cancelAll();
        QCOMPARE(q.running(1), 0);
        QCOMPARE(q.waiting(), 0);
        q.done("1:0");   // a late answer changes nothing
        QCOMPARE(started(spy), QStringList({"1:0"}));
        q.request("1:6", 1);
        QCOMPARE(started(spy), QStringList({"1:0", "1:6"}));
    }

    void cancellingOnePaneFreesItsPlace()
    {
        StillQueue q;
        q.setPerRecorder(1);
        QSignalSpy spy(&q, &StillQueue::startStill);
        q.request("1:0", 1);
        q.request("1:3", 1);
        q.request("1:6", 1);
        q.cancel("1:3");          // waiting: just leaves the line
        QCOMPARE(q.waiting(), 1);
        q.cancel("1:0");          // running: frees the slot for the next
        QCOMPARE(started(spy), QStringList({"1:0", "1:6"}));
    }

    void anUnknownAnswerIsIgnored()
    {
        StillQueue q;
        QSignalSpy spy(&q, &StillQueue::startStill);
        q.done("9:9");
        q.cancel("9:9");
        QCOMPARE(spy.count(), 0);
    }

    void theDefaultLimitIsFour()
    {
        StillQueue q;
        QCOMPARE(q.perRecorder(), 4);
    }
};

QTEST_GUILESS_MAIN(TestStillQueue)
#include "test_stillqueue.moc"
