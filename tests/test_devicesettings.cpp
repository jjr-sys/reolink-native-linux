#include "ScriptedTransports.h"
#include "device/DeviceSettings.h"

#include <QSignalSpy>
#include <QtTest>

using namespace rl;
using namespace rl::testing;

// A camera's settings, asked by camera key (wayfinder/architecture ticket 08): HTTP
// commands against a scripted transport, Baichuan ones against a scripted link.
class TestDeviceSettings : public QObject
{
    Q_OBJECT

    // One camera, "7:2", on the scripted transports; `admin` and `ready` as given.
    struct Camera {
        std::shared_ptr<ScriptedHttp> http = std::make_shared<ScriptedHttp>();
        BaichuanScript bc;
        bool ready = true;
        bool admin = true;
        std::unique_ptr<DeviceSettings> settings;
        Camera()
        {
            settings = std::make_unique<DeviceSettings>(
                [this](const QString &key) {
                    device::CameraLink l;
                    if (key != QLatin1String("7:2"))
                        return l;
                    l.ready = ready;
                    l.admin = admin;
                    l.channel = 2;
                    l.http = http;
                    l.baichuan.host = QStringLiteral("10.0.0.7");
                    return l;
                },
                scriptedOpener(bc));
        }
    };

    static QList<QVariant> waitFor(QSignalSpy &spy)
    {
        if (spy.isEmpty() && !spy.wait(3000))
            return {};
        return spy.takeFirst();
    }

private slots:
    // ---- HTTP settings ------------------------------------------------------------------
    void aCameraNotReadyAnswersAtOnce()
    {
        Camera c;
        c.ready = false;
        QSignalSpy failed(c.settings.get(), &DeviceSettings::settingsFailed);
        c.settings->fetchSettings(QStringLiteral("7:2"), {QStringLiteral("GetEnc")});
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(), QStringLiteral("7:2"));
        QVERIFY(c.http->sent.isEmpty());
    }

    void settingsComeBackByNameWithTheRefusedOnesListed()
    {
        Camera c;
        c.http->values.insert(QStringLiteral("GetEnc"), Json{{"Enc", {{"audio", 1}}}});
        c.http->values.insert(QStringLiteral("GetPushV20"), Json{{"Push", {{"enable", 1}}}});
        QSignalSpy loaded(c.settings.get(), &DeviceSettings::settingsLoaded);
        c.settings->fetchSettings(QStringLiteral("7:2"),
                                  {QStringLiteral("GetEnc"), QStringLiteral("GetPush"),
                                   QStringLiteral("GetOsd")});
        const QList<QVariant> got = waitFor(loaded);
        QCOMPARE(got.value(0).toString(), QStringLiteral("7:2"));
        const QVariantMap v = got.value(1).toMap();
        QVERIFY(v.contains(QStringLiteral("GetEnc")));
        QVERIFY(v.contains(QStringLiteral("GetPush")));   // the V20 answer, under the old name
        QCOMPARE(v.value(QStringLiteral("_failed")).toStringList(), QStringList{QStringLiteral("GetOsd")});
        QCOMPARE(c.http->sentParams.first().value("channel", -1), 2);
    }

    void aWriteNeedsAnAdministrator()
    {
        Camera c;
        c.admin = false;
        QSignalSpy applied(c.settings.get(), &DeviceSettings::settingApplied);
        c.settings->applySetting(QStringLiteral("7:2"), QStringLiteral("SetOsd"), {});
        QCOMPARE(applied.count(), 1);
        QCOMPARE(applied.first().at(2).toBool(), false);
        QVERIFY(applied.first().at(3).toString().contains(QStringLiteral("administrator")));
        QVERIFY(c.http->sent.isEmpty());
    }

    void aWriteAnswersWithItsCommand()
    {
        Camera c;
        c.http->values.insert(QStringLiteral("AudioAlarmPlay"), Json::object());
        QSignalSpy applied(c.settings.get(), &DeviceSettings::settingApplied);
        c.settings->applySetting(QStringLiteral("7:2"), QStringLiteral("AudioAlarmPlay"),
                                 {{QStringLiteral("channel"), 2}});
        const QList<QVariant> got = waitFor(applied);
        QCOMPARE(got.value(0).toString(), QStringLiteral("7:2"));
        QCOMPARE(got.value(1).toString(), QStringLiteral("AudioAlarmPlay"));
        QVERIFY(got.value(2).toBool());
    }

    // ---- Quick replies ------------------------------------------------------------------
    // HTTP refused: the clip plays over Baichuan.
    void aQuickReplyFallsBackToBaichuan()
    {
        Camera c;
        QSignalSpy played(c.settings.get(), &DeviceSettings::quickReplyPlayed);
        c.settings->playQuickReply(QStringLiteral("7:2"), 3);
        const QList<QVariant> got = waitFor(played);
        QCOMPARE(got.value(1).toInt(), 3);
        QVERIFY(got.value(2).toBool());
        QCOMPARE(c.bc.transacts.size(), 1);
        QCOMPARE(c.bc.transacts.first().channel, 2);
        QCOMPARE(c.bc.lastParams.host, QStringLiteral("10.0.0.7"));
    }

    // ---- The push switch ----------------------------------------------------------------
    void thePushSwitchIsKnownByCameraOnceReadOrWritten()
    {
        Camera c;
        QVERIFY(c.settings->pushEnabledFor(QStringLiteral("7:2")));   // unknown: allowed
        quint32 getPush = 0, setPush = 0;
        QVERIFY(device::CameraCommands::alertCommandIds(QStringLiteral("push"), getPush, setPush));
        c.bc.enables.insert(getPush, 0);
        QSignalSpy alerts(c.settings.get(), &DeviceSettings::alertsLoaded);
        c.settings->fetchAlerts(QStringLiteral("7:2"));
        QCOMPARE(waitFor(alerts).value(1).toMap().value(QStringLiteral("push")).toInt(), 0);
        QVERIFY(!c.settings->pushEnabledFor(QStringLiteral("7:2")));
        QVERIFY(c.settings->pushEnabledFor(QStringLiteral("7:3")));   // another camera

        QSignalSpy applied(c.settings.get(), &DeviceSettings::settingApplied);
        c.settings->setAlertEnable(QStringLiteral("7:2"), QStringLiteral("push"), true);
        QVERIFY(waitFor(applied).value(2).toBool());
        QVERIFY(c.settings->pushEnabledFor(QStringLiteral("7:2")));
    }

    // ---- Baichuan config and schedule ---------------------------------------------------
    void configComesBackWithItsCommandId()
    {
        Camera c;
        c.bc.flatConfigs.insert(26, {{QStringLiteral("bright"), 128}});
        QSignalSpy loaded(c.settings.get(), &DeviceSettings::bcConfigLoaded);
        c.settings->fetchBcConfig(QStringLiteral("7:2"), 26);
        const QList<QVariant> got = waitFor(loaded);
        QCOMPARE(got.value(1).toInt(), 26);
        QCOMPARE(got.value(2).toMap().value(QStringLiteral("bright")).toInt(), 128);
    }

    void aScheduleWriteNeedsAnAdministrator()
    {
        Camera c;
        c.admin = false;
        QSignalSpy applied(c.settings.get(), &DeviceSettings::settingApplied);
        c.settings->writeRecSchedule(QStringLiteral("7:2"), QStringLiteral("MD"), QString(168, u'1'));
        QCOMPARE(applied.count(), 1);
        QCOMPARE(applied.first().at(1).toString(), QStringLiteral("SetRecSchedule"));
        QCOMPARE(applied.first().at(2).toBool(), false);
        QCOMPARE(c.bc.opens, 0);
    }

    // ---- Motion zone grid ---------------------------------------------------------------
    void zoneBitsRoundTrip()
    {
        Camera c;
        const QString bits = QStringLiteral("1010000011");
        const QString table = c.settings->mdZoneTable(bits);
        QCOMPARE(c.settings->mdZoneBits(table, 10), bits);
        QCOMPARE(c.settings->mdZoneBits({}, 4), QStringLiteral("0000"));
    }
};

QTEST_GUILESS_MAIN(TestDeviceSettings)
#include "test_devicesettings.moc"
