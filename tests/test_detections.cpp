#include "device/Detections.h"

#include <QtTest>

using namespace rl;
using namespace rl::device;

namespace {
api::DetectionState st(bool motion, bool person = false, bool vehicle = false, bool pet = false,
                       bool visitor = false)
{
    api::DetectionState s;
    s.motion = motion;
    s.person = person;
    s.vehicle = vehicle;
    s.pet = pet;
    s.visitor = visitor;
    return s;
}
QStringList types(const QVector<Detection> &d)
{
    QStringList out;
    for (const Detection &x : d)
        out.append(x.type);
    return out;
}
frigate::Event ev(const char *camera, const char *label, double start)
{
    frigate::Event e;
    e.id = QStringLiteral("e");
    e.camera = QString::fromLatin1(camera);
    e.label = QString::fromLatin1(label);
    e.start = start;
    return e;
}
const QHash<QString, int> kCams{{"front", 0}, {"back", 1}};
} // namespace

// One host's detections from every source the app listens to: Reolink poll, Baichuan
// push, Frigate poll and Frigate websocket, normalised into Detection events. These are
// the rules DeviceManager used to spread across pollDetections, onAlarmEvents,
// pollFrigate and raiseFrigateEvent.
class TestDetections : public QObject
{
    Q_OBJECT

private slots:
    // ---- Reolink: edges ----
    void risingEdgesAreEventsAndHeldStatesAreNot()
    {
        HostDetections h;
        QCOMPARE(types(h.onPush(0, st(true), "Door")), QStringList{"motion"});
        QCOMPARE(types(h.onPush(0, st(true), "Door")), QStringList{});
        QCOMPARE(types(h.onPush(0, st(true, true), "Door")), QStringList{"person"});
        QCOMPARE(types(h.onPush(0, st(false), "Door")), QStringList{});
        QCOMPARE(types(h.onPush(0, st(true), "Door")), QStringList{"motion"});
    }
    void channelsAreIndependentAndCarryTheirCameraName()
    {
        HostDetections h;
        const auto a = h.onPush(0, st(true), "Door");
        const auto b = h.onPush(3, st(true), "Yard");
        QCOMPARE(a.size(), 1);
        QCOMPARE(a[0].channel, 0);
        QCOMPARE(a[0].camera, QStringLiteral("Door"));
        QCOMPARE(b[0].channel, 3);
        QCOMPARE(b[0].camera, QStringLiteral("Yard"));
    }

    // ---- Reolink: push takes over from the poll ----
    void pollQueriesStatesUnlessPushCoversAMultiChannelHost_data()
    {
        QTest::addColumn<bool>("pushLive");
        QTest::addColumn<int>("channels");
        QTest::addColumn<bool>("queries");
        QTest::newRow("no push, camera") << false << 1 << true;
        QTest::newRow("no push, nvr") << false << 8 << true;
        QTest::newRow("push, camera") << true << 1 << true; // the poll is its health check
        QTest::newRow("push, nvr") << true << 8 << false;
    }
    void pollQueriesStatesUnlessPushCoversAMultiChannelHost()
    {
        QFETCH(bool, pushLive);
        QFETCH(int, channels);
        QFETCH(bool, queries);
        HostDetections h;
        h.setPushLive(pushLive);
        QCOMPARE(h.pollQueriesStates(channels), queries);
    }
    void pollRaisesEventsWhilePushIsDown()
    {
        HostDetections h;
        QCOMPARE(types(h.onPoll(0, st(false, true), "Door", false)), QStringList{"person"});
    }
    void pollIsIgnoredOnceItsHostIsPushLive_data()
    {
        QTest::addColumn<bool>("liveAtDispatch");
        QTest::addColumn<bool>("liveNow");
        QTest::newRow("live when sent") << true << true;
        QTest::newRow("went live meanwhile") << false << true;
        QTest::newRow("dropped meanwhile") << true << false; // that poll saw nothing: skip it
    }
    void pollIsIgnoredOnceItsHostIsPushLive()
    {
        QFETCH(bool, liveAtDispatch);
        QFETCH(bool, liveNow);
        HostDetections h;
        h.setPushLive(liveNow);
        QCOMPARE(types(h.onPoll(0, st(true), "Door", liveAtDispatch)), QStringList{});
        // ...and it left the state alone, so push's next report is still an edge.
        h.setPushLive(true);
        QCOMPARE(types(h.onPush(0, st(true), "Door")), QStringList{"motion"});
    }
    void pollResumesWhenPushDrops()
    {
        HostDetections h;
        h.setPushLive(true);
        h.onPush(0, st(true), "Door");
        h.setPushLive(false);
        QCOMPARE(types(h.onPoll(0, st(true), "Door", false)), QStringList{}); // same motion
        QCOMPARE(types(h.onPoll(0, st(true, true), "Door", false)), QStringList{"person"});
    }
    void takeoverDoesNotRepeatWhatThePollAlreadyRaised()
    {
        HostDetections h;
        QCOMPARE(types(h.onPoll(0, st(true), "Door", false)), QStringList{"motion"});
        h.setPushLive(true);
        QCOMPARE(types(h.onPush(0, st(true), "Door")), QStringList{}); // still the same motion
    }
    void pollCannotSeeADoorbellPressSoItKeepsWhatPushSaid()
    {
        HostDetections h;
        QCOMPARE(types(h.onPush(0, st(false, false, false, false, true), "Bell")),
                 QStringList{"visitor"});
        h.setPushLive(false);
        // The poll reports visitor=false; carried over, that is not a new press.
        QCOMPARE(types(h.onPoll(0, st(false), "Bell", false)), QStringList{});
        // A press while the poll holds: visitor stays carried over, not re-raised.
        QCOMPARE(types(h.onPoll(0, st(true), "Bell", false)), QStringList{"motion"});
    }

    // ---- Frigate: the poll and its watermark ----
    void firstPollRaisesNothingAndSetsTheWatermark()
    {
        HostDetections h;
        QVERIFY(!h.frigatePlan().initialized);
        const auto plan = h.frigatePlan();
        // No events yet: the server's clock is ours.
        QVERIFY(h.onFrigatePoll(plan, {}, kCams, 5000, 0).isEmpty());
        QVERIFY(h.frigatePlan().initialized);
        QCOMPARE(h.frigatePlan().after, 5000.0);
    }
    void firstPollTakesTheNewestEventStartAsTheWatermark()
    {
        HostDetections h;
        const QVector<frigate::Event> events{ev("front", "person", 900), ev("front", "person", 800)};
        QVERIFY(h.onFrigatePoll(h.frigatePlan(), events, kCams, 5000, 0).isEmpty());
        QCOMPARE(h.frigatePlan().after, 900.0);
    }
    void backfillTreatsTheLastSecondsAsNew()
    {
        HostDetections h;
        h.onFrigatePoll(h.frigatePlan(), {}, kCams, 5000, 300);
        QCOMPARE(h.frigatePlan().after, 4700.0);
    }
    void laterPollsRaiseOldestFirstSkippingTheUnmodelled()
    {
        HostDetections h;
        h.onFrigatePoll(h.frigatePlan(), {}, kCams, 1000, 0);
        const auto plan = h.frigatePlan();
        // Frigate answers newest first.
        const QVector<frigate::Event> events{
            ev("back", "car", 1100),       // vehicle
            ev("front", "person", 1090),   // person
            ev("front", "bird", 1080),     // not a modelled label
            ev("shed", "person", 1070),    // not one of this host's cameras
            ev("front", "person", 1000),   // not after the watermark
        };
        const auto out = h.onFrigatePoll(plan, events, kCams, 1200, 0);
        QCOMPARE(types(out), (QStringList{"person", "vehicle"}));
        QCOMPARE(out[0].camera, QStringLiteral("front"));
        QCOMPARE(out[0].channel, 0);
        QCOMPARE(out[1].channel, 1);
        QCOMPARE(h.frigatePlan().after, 1100.0);
    }
    void watermarkNeverGoesBackwards()
    {
        HostDetections h;
        h.onFrigatePoll(h.frigatePlan(), {}, kCams, 1000, 0);
        h.onFrigatePoll(h.frigatePlan(), {ev("front", "bird", 500)}, kCams, 1100, 0);
        QCOMPARE(h.frigatePlan().after, 1000.0);
    }

    // ---- Frigate: bursts, websocket over poll ----
    void burstRuleTable_data()
    {
        QTest::addColumn<double>("first");
        QTest::addColumn<double>("second");
        QTest::addColumn<bool>("raisedAgain");
        QTest::newRow("same instant") << 1000.0 << 1000.0 << false;
        QTest::newRow("10 s later") << 1000.0 << 1010.0 << false;
        QTest::newRow("29.9 s later") << 1000.0 << 1029.9 << false;
        QTest::newRow("exactly 30 s") << 1000.0 << 1030.0 << true;
        QTest::newRow("a minute") << 1000.0 << 1060.0 << true;
    }
    void burstRuleTable()
    {
        QFETCH(double, first);
        QFETCH(double, second);
        QFETCH(bool, raisedAgain);
        HostDetections h;
        QCOMPARE(h.onFrigateLive("front", "person", first, kCams).size(), 1);
        QCOMPARE(h.onFrigateLive("front", "person", second, kCams).size(), raisedAgain ? 1 : 0);
    }
    void burstsAreKeptPerCameraAndPerType()
    {
        HostDetections h;
        QCOMPARE(h.onFrigateLive("front", "person", 1000, kCams).size(), 1);
        QCOMPARE(h.onFrigateLive("back", "person", 1001, kCams).size(), 1);
        QCOMPARE(h.onFrigateLive("front", "car", 1002, kCams).size(), 1);
        QCOMPARE(h.onFrigateLive("front", "dog", 1003, kCams).size(), 1);
        QCOMPARE(h.onFrigateLive("front", "person", 1004, kCams).size(), 0);
    }
    void liveIgnoresUnmodelledLabelsAndUnknownCameras()
    {
        HostDetections h;
        QVERIFY(h.onFrigateLive("front", "bird", 1000, kCams).isEmpty());
        QVERIFY(h.onFrigateLive("shed", "person", 1000, kCams).isEmpty());
    }
    void socketThenPollRaisesTheEventOnce()
    {
        HostDetections h;
        h.onFrigatePoll(h.frigatePlan(), {}, kCams, 1000, 0);
        const auto plan = h.frigatePlan();
        QCOMPARE(h.onFrigateLive("front", "person", 1010, kCams).size(), 1);
        // The poll then lists the same event: the burst rule drops it.
        QVERIFY(h.onFrigatePoll(plan, {ev("front", "person", 1010)}, kCams, 1020, 0).isEmpty());
        QCOMPARE(h.frigatePlan().after, 1010.0); // ...but the watermark still moves
    }
    void pollThenSocketRaisesTheEventOnce()
    {
        HostDetections h;
        h.onFrigatePoll(h.frigatePlan(), {}, kCams, 1000, 0);
        QCOMPARE(h.onFrigatePoll(h.frigatePlan(), {ev("front", "person", 1010)}, kCams, 1020, 0)
                     .size(), 1);
        QVERIFY(h.onFrigateLive("front", "person", 1010, kCams).isEmpty());
    }

    // ---- the burst gate shared with the history catch-up ----
    void burstGateAdmitsOncePerBurst()
    {
        BurstGate g;
        QVERIFY(g.admit("front", "person", 100));
        QVERIFY(!g.admit("front", "person", 129));
        QVERIFY(g.admit("front", "person", 130));
        QVERIFY(g.admit("front", "car", 131));
    }
    void historyRowCoversAMomentWithinTheBurstWindow_data()
    {
        QTest::addColumn<qint64>("existing");
        QTest::addColumn<qint64>("candidate");
        QTest::addColumn<bool>("covers");
        QTest::newRow("same second") << qint64(1000) << qint64(1000) << true;
        QTest::newRow("29 s before") << qint64(1000) << qint64(971) << true;
        QTest::newRow("29 s after") << qint64(1000) << qint64(1029) << true;
        QTest::newRow("30 s after") << qint64(1000) << qint64(1030) << false;
        QTest::newRow("30 s before") << qint64(1000) << qint64(970) << false;
    }
    void historyRowCoversAMomentWithinTheBurstWindow()
    {
        QFETCH(qint64, existing);
        QFETCH(qint64, candidate);
        QFETCH(bool, covers);
        QCOMPARE(sameBurstWindow(existing, candidate), covers);
    }
};

QTEST_APPLESS_MAIN(TestDetections)
#include "test_detections.moc"
