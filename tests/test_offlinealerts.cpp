#include "device/OfflineAlerts.h"

#include <QtTest>

using namespace rl::device;
using Kind = OfflineAlert::Kind;

namespace {
constexpr qint64 t0 = 1'800'000'000;
const QVector<CameraRef> eight = {{0, "A"}, {1, "B"}, {2, "C"}, {3, "D"},
                                  {4, "E"}, {5, "F"}, {6, "G"}, {7, "H"}};

OfflineAlerts::Change camera(qint64 host, int ch, const QString &name, bool online)
{
    return {host, ch, name, online, {}, false};
}
OfflineAlerts::Change recorder(qint64 host, const QString &name, bool online,
                               const QVector<CameraRef> &cams = eight)
{
    return {host, -1, name, online, cams, true};
}
// A single-camera device: the whole host is that one camera.
OfflineAlerts::Change lone(qint64 host, const QString &name, bool online)
{
    return {host, -1, name, online, {{0, name}}, false};
}
} // namespace

class TestOfflineAlerts : public QObject
{
    Q_OBJECT
private slots:
    void cameraWaitsOutTheGrace()
    {
        OfflineAlerts a;
        QVERIFY(a.change(camera(1, 2, "Kitchen", false), t0).isEmpty());
        QVERIFY(a.tick(t0 + 29).isEmpty());
        const auto out = a.tick(t0 + 30);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].kind, Kind::Offline);
        QCOMPARE(out[0].hostId, 1);
        QCOMPARE(out[0].channel, 2);
        QCOMPARE(out[0].title, QStringLiteral("Kitchen offline"));
        QVERIFY(out[0].critical);
        QVERIFY(a.tick(t0 + 60).isEmpty());   // once
    }

    void backInsideTheGraceIsSilent()
    {
        OfflineAlerts a;
        a.change(camera(1, 2, "Kitchen", false), t0);
        QVERIFY(a.change(camera(1, 2, "Kitchen", true), t0 + 20).isEmpty());
        QVERIFY(a.tick(t0 + 40).isEmpty());
    }

    void backOnlineSaysHowLong()
    {
        OfflineAlerts a;
        a.change(camera(1, 2, "Kitchen", false), t0);
        a.tick(t0 + 30);
        const auto out = a.change(camera(1, 2, "Kitchen", true), t0 + 4 * 60);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].kind, Kind::BackOnline);
        QVERIFY(!out[0].critical);
        QCOMPARE(out[0].key, OfflineAlerts::keyOf(1, 2));   // replaces the offline bubble
        QCOMPARE(out[0].title, QStringLiteral("Kitchen back online"));
        QVERIFY2(out[0].body.contains(QStringLiteral("4 min")), qPrintable(out[0].body));
    }

    void recorderAlertsAtOnceWithCameraCount()
    {
        // The host's own detection (3 polls + probe) already served the grace.
        OfflineAlerts a;
        const auto out = a.change(recorder(5, "Woorabinda NVR", false), t0);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].kind, Kind::Offline);
        QCOMPARE(out[0].channel, -1);
        QCOMPARE(out[0].title, QStringLiteral("Woorabinda NVR offline · 8 cameras"));
        QVERIFY(out[0].critical);
    }

    void loneCameraHostIsWordedAsACamera()
    {
        OfflineAlerts a;
        const auto out = a.change(lone(3, "Front Door", false), t0);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].title, QStringLiteral("Front Door offline"));
    }

    void recorderSwallowsItsCamerasWaiting()
    {
        OfflineAlerts a;
        a.change(camera(5, 1, "Shed", false), t0);
        a.change(camera(9, 0, "Elsewhere", false), t0);
        a.change(recorder(5, "Woorabinda NVR", false), t0 + 10);
        const auto out = a.tick(t0 + 40);
        QCOMPARE(out.size(), 1);   // only the other host's camera
        QCOMPARE(out[0].hostId, 9);
    }

    void recorderBackOnline()
    {
        OfflineAlerts a;
        a.change(recorder(5, "Woorabinda NVR", false), t0);
        const auto out = a.change(recorder(5, "Woorabinda NVR", true), t0 + 120);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].kind, Kind::BackOnline);
        QCOMPARE(out[0].key, OfflineAlerts::keyOf(5, -1));
    }

    void thirdDropInTenMinutesIsUnstableThenQuiet()
    {
        OfflineAlerts a;
        // Drop 1: alerted, back. Drop 2: back inside the grace (still counts).
        a.change(camera(1, 2, "Kitchen", false), t0);
        a.tick(t0 + 30);
        a.change(camera(1, 2, "Kitchen", true), t0 + 60);
        a.change(camera(1, 2, "Kitchen", false), t0 + 120);
        a.change(camera(1, 2, "Kitchen", true), t0 + 130);
        const auto out = a.change(camera(1, 2, "Kitchen", false), t0 + 300);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].kind, Kind::Unstable);
        QVERIFY(out[0].critical);
        QCOMPARE(out[0].title, QStringLiteral("Kitchen unstable"));

        // Quiet: no back-online, no offline, no more unstable.
        QVERIFY(a.change(camera(1, 2, "Kitchen", true), t0 + 310).isEmpty());
        QVERIFY(a.change(camera(1, 2, "Kitchen", false), t0 + 320).isEmpty());
        QVERIFY(a.tick(t0 + 400).isEmpty());
        QVERIFY(a.change(camera(1, 2, "Kitchen", true), t0 + 400).isEmpty());
        QVERIFY(a.tick(t0 + 400 + 599).isEmpty());

        // Up 10 min: one quiet "stable again", and the count starts over.
        const auto stable = a.tick(t0 + 400 + 600);
        QCOMPARE(stable.size(), 1);
        QCOMPARE(stable[0].kind, Kind::Stable);
        QVERIFY(!stable[0].critical);
        QCOMPARE(stable[0].key, OfflineAlerts::keyOf(1, 2));
        QVERIFY(a.tick(t0 + 2000).isEmpty());
        QVERIFY(a.change(camera(1, 2, "Kitchen", false), t0 + 2000).isEmpty());   // grace again
        QCOMPARE(a.tick(t0 + 2030).value(0).kind, Kind::Offline);
    }

    void unstableStaysQuietWhileDown()
    {
        OfflineAlerts a;
        for (int i = 0; i < 2; ++i) {
            a.change(camera(1, 2, "Kitchen", false), t0 + i * 60);
            a.change(camera(1, 2, "Kitchen", true), t0 + i * 60 + 10);
        }
        QCOMPARE(a.change(camera(1, 2, "Kitchen", false), t0 + 200).value(0).kind, Kind::Unstable);
        QVERIFY(a.tick(t0 + 5000).isEmpty());   // still down: no "stable again"
    }

    void dropsOlderThanTheWindowDontCount()
    {
        OfflineAlerts a;
        a.change(camera(1, 2, "Kitchen", false), t0);
        a.change(camera(1, 2, "Kitchen", true), t0 + 5);
        a.change(camera(1, 2, "Kitchen", false), t0 + 100);
        a.change(camera(1, 2, "Kitchen", true), t0 + 105);
        QVERIFY(a.change(camera(1, 2, "Kitchen", false), t0 + 601).isEmpty());   // 1st aged out
        QCOMPARE(a.tick(t0 + 631).value(0).kind, Kind::Offline);
    }

    void nextDueForTheTimer()
    {
        OfflineAlerts a;
        QCOMPARE(a.nextDue(), qint64(-1));
        a.change(camera(1, 2, "Kitchen", false), t0);
        QCOMPARE(a.nextDue(), t0 + 30);
    }

    void clickTargetIsTheCameraOrTheRecorder()
    {
        OfflineAlerts a;
        a.change(camera(1, 2, "Kitchen", false), t0);
        const auto cam = a.tick(t0 + 30);
        QCOMPARE(cam[0].hostId, 1);
        QCOMPARE(cam[0].channel, 2);
        const auto rec = a.change(recorder(5, "NVR", false), t0);
        QCOMPARE(rec[0].hostId, 5);
        QCOMPARE(rec[0].channel, -1);
        const auto one = a.change(lone(3, "Front Door", false), t0);
        QCOMPARE(one[0].channel, 0);   // a lone camera opens that camera
    }
};

QTEST_GUILESS_MAIN(TestOfflineAlerts)
#include "test_offlinealerts.moc"
