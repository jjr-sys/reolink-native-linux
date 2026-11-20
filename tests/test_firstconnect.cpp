// What this run's first connect of a host found, as DeviceManager::applyValidation reports it
// to the offline alerts (connectivityKnown). Characterised from applyValidation as of 9b67132.
#include "device/FirstConnect.h"

#include <QtTest>

using namespace rl::device;

namespace {
const QVector<CameraRef> none;

bool isHost(const ConnectFact &f, const QString &name, bool online)
{
    return f.channel == -1 && f.name == name && f.online == online;
}
bool isCamera(const ConnectFact &f, int channel, const QString &name)
{
    return f.channel == channel && f.name == name && !f.online && f.cameras.isEmpty();
}
} // namespace

class TestFirstConnect : public QObject
{
    Q_OBJECT
private slots:
    void downAtFirstConnectThenRecovers()
    {
        FirstConnect fc;
        const auto down = fc.failed(1, "NVR", Problem::Unreachable, none);
        QCOMPARE(down.size(), 1);
        QVERIFY(isHost(down[0], "NVR", false));
        QVERIFY(down[0].cameras.isEmpty()); // start-up placeholder: no camera count
        const auto back = fc.listed(1, "NVR", none);
        QCOMPARE(back.size(), 1);
        QVERIFY(isHost(back[0], "NVR", true));
        QVERIFY(back[0].cameras.isEmpty());
    }

    void downAtFirstConnectCarriesKnownCameras()
    {
        FirstConnect fc;
        const auto down = fc.failed(1, "NVR", Problem::Unreachable, {{0, "Door"}, {3, "Yard"}});
        QCOMPARE(down.size(), 1);
        QCOMPARE(down[0].cameras.size(), 2);
        QCOMPARE(down[0].cameras[1].channel, 3);
        QCOMPARE(down[0].cameras[1].name, QStringLiteral("Yard"));
    }

    void repeatedFailureIsReportedOnce()
    {
        FirstConnect fc;
        QCOMPARE(fc.failed(1, "NVR", Problem::Unreachable, none).size(), 1);
        QVERIFY(fc.failed(1, "NVR", Problem::Unreachable, none).isEmpty());
        QCOMPARE(fc.listed(1, "NVR", none).size(), 1);
        QVERIFY(fc.listed(1, "NVR", none).isEmpty());     // back is said once
        QVERIFY(fc.failed(1, "NVR", Problem::Unreachable, none).isEmpty()); // the poll's job
    }

    void startFailedThenListedWithOfflineCameras()
    {
        FirstConnect fc;
        fc.failed(1, "NVR", Problem::Unreachable, none);
        const auto out = fc.listed(1, "Garage NVR", {{2, "Garage"}, {5, "Porch"}});
        QCOMPARE(out.size(), 3);
        QVERIFY(isHost(out[0], "Garage NVR", true)); // the host first, under its listed name
        QVERIFY(isCamera(out[1], 2, "Garage"));
        QVERIFY(isCamera(out[2], 5, "Porch"));
    }

    void firstListingReportsOfflineCameras()
    {
        FirstConnect fc;
        const auto out = fc.listed(1, "NVR", {{4, "Shed"}});
        QCOMPARE(out.size(), 1);
        QVERIFY(isCamera(out[0], 4, "Shed"));
    }

    void firstListingAllOnlineIsSilent()
    {
        FirstConnect fc;
        QVERIFY(fc.listed(1, "NVR", none).isEmpty());
    }

    void secondListingIsSilent()
    {
        FirstConnect fc;
        QCOMPARE(fc.listed(1, "NVR", {{4, "Shed"}}).size(), 1);
        QVERIFY(fc.listed(1, "NVR", {{4, "Shed"}, {6, "Gate"}}).isEmpty());
    }

    void failureAfterListingIsSilent()
    {
        FirstConnect fc;
        fc.listed(1, "NVR", none);
        QVERIFY(fc.failed(1, "NVR", Problem::Unreachable, {{0, "Door"}}).isEmpty());
        QVERIFY(fc.listed(1, "NVR", {{0, "Door"}}).isEmpty()); // not a recovery, not first
    }

    void refusedLoginIsNotDown_data()
    {
        QTest::addColumn<Problem>("problem");
        QTest::newRow("auth") << Problem::Auth;
        QTest::newRow("locked") << Problem::Locked;
        QTest::newRow("none (nothing listed)") << Problem::None;
        QTest::newRow("connecting") << Problem::Connecting;
    }
    void refusedLoginIsNotDown()
    {
        QFETCH(Problem, problem);
        FirstConnect fc;
        QVERIFY(fc.failed(1, "NVR", problem, none).isEmpty());
        // The first connect has landed: a later Unreachable is a change, not a start fact.
        QVERIFY(fc.failed(1, "NVR", Problem::Unreachable, none).isEmpty());
        // Listed at last: no "back" (never reported down), but this is the first listing.
        const auto out = fc.listed(1, "NVR", {{1, "Lane"}});
        QCOMPARE(out.size(), 1);
        QVERIFY(isCamera(out[0], 1, "Lane"));
    }

    // A degraded return (cameras listed over Baichuan while the API is down) is a listing:
    // it reports the host back, and the API's own listing later is no longer the first.
    void degradedListingIsTheFirstListing()
    {
        FirstConnect fc;
        fc.failed(1, "NVR", Problem::Unreachable, none);
        const auto degraded = fc.listed(1, "NVR", none); // Baichuan lists cameras online
        QCOMPARE(degraded.size(), 1);
        QVERIFY(isHost(degraded[0], "NVR", true));
        QVERIFY(fc.listed(1, "NVR", {{3, "Yard"}}).isEmpty());
    }

    void degradedFirstConnectIsNotDown()
    {
        FirstConnect fc;
        QVERIFY(fc.listed(1, "NVR", none).isEmpty()); // degraded at the first connect
        QVERIFY(fc.failed(1, "NVR", Problem::Unreachable, none).isEmpty());
    }

    void hostsAreSeparate()
    {
        FirstConnect fc;
        QCOMPARE(fc.failed(1, "NVR", Problem::Unreachable, none).size(), 1);
        QCOMPARE(fc.failed(2, "Cam", Problem::Unreachable, none).size(), 1);
        QVERIFY(fc.failed(3, "Bad", Problem::Auth, none).isEmpty());
        QCOMPARE(fc.listed(3, "Bad", {{0, "Bad"}}).size(), 1);
        QCOMPARE(fc.listed(2, "Cam", none).size(), 1);
        const auto nvr = fc.listed(1, "NVR", {{1, "Lane"}});
        QCOMPARE(nvr.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestFirstConnect)
#include "test_firstconnect.moc"
