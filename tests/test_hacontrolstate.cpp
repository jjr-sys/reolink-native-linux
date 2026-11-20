#include "device/HaControlState.h"

#include <QtTest>

using namespace rl;

// The order / read-back bookkeeping shared by the Bosch wiper and tracking controls: an order
// bumps the revision when sent and when answered; a read captures the revision when sent and
// is dropped if it changed.
class TestHaControlState : public QObject
{
    Q_OBJECT

private slots:
    void startsIdle()
    {
        const HaControlState s;
        QVERIFY(!s.pending);
        QVERIFY(!s.readPending);
        QVERIFY(!s.busy());
    }

    void readWithNothingInBetweenIsKept()
    {
        HaControlState s;
        const quint64 captured = s.beginRead();
        QVERIFY(s.readPending);
        QVERIFY(s.busy());
        QVERIFY(s.endRead(captured));
        QVERIFY(!s.readPending);
        QVERIFY(!s.busy());
    }

    void readOvertakenByAnOrderIsDropped()
    {
        HaControlState s;
        const quint64 captured = s.beginRead();
        const quint64 sent = s.beginOrder();
        QVERIFY(s.pending);
        QVERIFY(!s.endOrder(sent));   // nothing overtook the order
        QVERIFY(!s.pending);
        QVERIFY(!s.endRead(captured));   // older than the order's answer
        QVERIFY(!s.readPending);
    }

    void readSentWhileAnOrderIsOutIsDroppedByTheAnswer()
    {
        HaControlState s;
        const quint64 sent = s.beginOrder();
        const quint64 captured = s.beginRead();
        QVERIFY(!s.endOrder(sent));
        QVERIFY(!s.endRead(captured));
    }

    void invalidateDropsAReadAndOvertakesAnOrder()
    {
        HaControlState s;
        const quint64 captured = s.beginRead();
        s.invalidate();
        QVERIFY(!s.endRead(captured));

        const quint64 sent = s.beginOrder();
        s.invalidate();
        QVERIFY(s.endOrder(sent));   // overtaken
        QVERIFY(!s.pending);
    }

    void revisionCountsEachBump()
    {
        HaControlState s;
        QCOMPARE(s.revision, quint64(0));
        const quint64 sent = s.beginOrder();
        QCOMPARE(sent, quint64(1));
        QCOMPARE(s.revision, quint64(1));
        s.endOrder(sent);
        QCOMPARE(s.revision, quint64(2));
        s.invalidate();
        QCOMPARE(s.revision, quint64(3));
        QCOMPARE(s.beginRead(), quint64(3));
        QCOMPARE(s.revision, quint64(3));   // a read never bumps
    }

    void resetClearsBothAndDropsWhatIsOut()
    {
        HaControlState s;
        const quint64 captured = s.beginRead();
        const quint64 sent = s.beginOrder();
        s.reset();
        QVERIFY(!s.pending);
        QVERIFY(!s.readPending);
        QVERIFY(!s.busy());
        QVERIFY(s.endOrder(sent));   // a late answer still sees it was overtaken
        QVERIFY(!s.endRead(captured));
    }
};

QTEST_GUILESS_MAIN(TestHaControlState)
#include "test_hacontrolstate.moc"
