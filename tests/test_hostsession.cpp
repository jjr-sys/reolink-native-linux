#include "ScriptedTransports.h"
#include "device/CameraCommands.h"
#include "device/HostSession.h"

#include <QtTest>

using namespace rl;
using namespace rl::device;
using namespace rl::testing;

namespace {
const QDateTime t0 = QDateTime(QDate(2026, 1, 1), QTime(12, 0), Qt::UTC);

HostRecord rec(const QString &addr = QStringLiteral("10.0.0.5"))
{
    HostRecord r;
    r.id = 7;
    r.kind = QStringLiteral("camera");
    r.name = QStringLiteral("Home");
    r.addr = addr;
    r.username = QStringLiteral("admin");
    return r;
}

Json devInfo(const char *name, int channels)
{
    return Json{{"DevInfo", {{"name", name}, {"model", "RLC-X"}, {"channelNum", channels}}}};
}
Json enc(int ch, const char *vtype, int w, int h)
{
    return Json{{"Enc", {{"channel", ch},
                         {"mainStream", {{"vType", vtype}, {"width", w}, {"height", h}}},
                         {"subStream", {{"vType", "h264"}, {"width", 640}, {"height", 360}}}}}};
}
} // namespace

class TestHostSession : public QObject
{
    Q_OBJECT

private slots:
    // ---- retry / lockout rules ----
    void backoffDoublesFromFifteenToThreeHundred()
    {
        HostSession s;
        QDateTime now = t0;
        const int expected[] = {15, 30, 60, 120, 240, 300, 300};
        for (int secs : expected) {
            s.validationFailed(Problem::Unreachable, now);
            QCOMPARE(s.nextRetryAt().value(), now.addSecs(secs));
            QVERIFY(!s.retryDue(now.addSecs(secs - 1)));
            QVERIFY(s.retryDue(now.addSecs(secs)));
            now = now.addSecs(secs);
        }
    }
    void authAndLockedAreNeverRetried_data()
    {
        QTest::addColumn<int>("problem");
        QTest::newRow("auth") << int(Problem::Auth);
        QTest::newRow("locked") << int(Problem::Locked);
    }
    void authAndLockedAreNeverRetried()
    {
        QFETCH(int, problem);
        HostSession s;
        s.validationFailed(Problem::Unreachable, t0); // a retry is pending...
        QVERIFY(s.nextRetryAt().has_value());
        s.validationFailed(Problem(problem), t0);     // ...the host then refuses the login
        QVERIFY(!s.nextRetryAt().has_value());
        QVERIFY(!s.retryDue(t0.addDays(30)));
        QVERIFY(!s.takeRetryIfDue(t0.addDays(30)));
    }
    void authResetsTheBackoffForTheNextOutage()
    {
        HostSession s;
        s.validationFailed(Problem::Unreachable, t0);
        s.validationFailed(Problem::Unreachable, t0);   // delay now 30
        s.validationFailed(Problem::Auth, t0);
        s.validationFailed(Problem::Unreachable, t0);
        QCOMPARE(s.nextRetryAt().value(), t0.addSecs(15));
    }
    void successClearsBackoff()
    {
        HostSession s;
        s.validationFailed(Problem::Unreachable, t0);
        s.validationSucceeded();
        QVERIFY(!s.nextRetryAt().has_value());
        s.validationFailed(Problem::Unreachable, t0);
        QCOMPARE(s.nextRetryAt().value(), t0.addSecs(15));
    }
    void degradedViewKeepsRetryingHttp()
    {
        HostSession s;
        s.keepRetrying(t0);
        s.keepRetrying(t0);
        QCOMPARE(s.nextRetryAt().value(), t0.addSecs(30));
    }
    void dueRetryIsTakenOnce()
    {
        HostSessions all;
        all.of(1).validationFailed(Problem::Unreachable, t0);
        all.of(2).validationFailed(Problem::Unreachable, t0.addSecs(100));
        all.of(3).validationFailed(Problem::Auth, t0);
        QCOMPARE(all.takeDueRetries(t0.addSecs(20)), QVector<qint64>{1});
        QVERIFY(all.takeDueRetries(t0.addSecs(20)).isEmpty());
        QCOMPARE(all.takeDueRetries(t0.addSecs(200)), QVector<qint64>{2});
    }
    void onlyOneValidationAtATime()
    {
        HostSession s;
        QVERIFY(s.beginValidation());
        QVERIFY(!s.beginValidation());
        s.endValidation();
        QVERIFY(s.beginValidation());
    }

    // ---- poll health ----
    void threeFailedPollsAskForAProbeOnce()
    {
        HostSession s;
        QCOMPARE(s.onPoll(false), HostSession::PollAction::None);
        QCOMPARE(s.onPoll(false), HostSession::PollAction::None);
        QCOMPARE(s.onPoll(false), HostSession::PollAction::ProbeAlive);
        QCOMPARE(s.onPoll(false), HostSession::PollAction::None);
    }
    void deadDeviceIsMarkedUnreachableAndRecovers()
    {
        HostSession s;
        for (int i = 0; i < 3; ++i)
            s.onPoll(false);
        QVERIFY(s.beginProbe());
        QVERIFY(!s.beginProbe());
        QCOMPARE(s.probeFinished(false), HostSession::ProbeAction::MarkUnreachable);
        QVERIFY(!s.apiOnly());
        QCOMPARE(s.onPoll(true), HostSession::PollAction::Recovered);
        QCOMPARE(s.onPoll(true), HostSession::PollAction::None);
    }
    void aliveDeviceWithDeadApiGoesApiOnlyThenComesBack()
    {
        HostSession s;
        for (int i = 0; i < 3; ++i)
            s.onPoll(false);
        QVERIFY(s.beginProbe());
        QCOMPARE(s.probeFinished(true), HostSession::ProbeAction::EnterApiOnly);
        QVERIFY(s.apiOnly());
        // While API-only, a liveness probe is repeated every sixth failure (~1 minute).
        QCOMPARE(s.onPoll(false), HostSession::PollAction::None);
        QCOMPARE(s.onPoll(false), HostSession::PollAction::None);
        QCOMPARE(s.onPoll(false), HostSession::PollAction::ProbeAlive);
        QVERIFY(s.beginProbe());
        QCOMPARE(s.probeFinished(true), HostSession::ProbeAction::Ignore); // already API-only
        QCOMPARE(s.onPoll(true), HostSession::PollAction::ApiBack);
        QVERIFY(!s.apiOnly());
    }
    void probeResultIsIgnoredIfTheApiCameBackMeanwhile()
    {
        HostSession s;
        for (int i = 0; i < 3; ++i)
            s.onPoll(false);
        QVERIFY(s.beginProbe());
        s.onPoll(true);
        QCOMPARE(s.probeFinished(false), HostSession::ProbeAction::Ignore);
    }
    void pollsDoNotOverlap()
    {
        HostSession s;
        QVERIFY(s.beginPoll());
        QVERIFY(!s.beginPoll());
        s.endPoll();
        QVERIFY(s.beginPoll());
    }

    // ---- logging in, through the HTTP seam ----
    void standaloneCameraValidates()
    {
        ScriptedHttp http;
        http.values["GetDevInfo"] = devInfo("Porch", 1);
        http.values["GetEnc"] = enc(0, "H265", 3840, 2160);
        http.values["GetBatteryInfo"] = Json{{"Battery", {{"batteryPercent", 80}}}};
        BaichuanScript bc;
        const Validation v = validateReolinkHost(http, rec(), QStringLiteral("pw"), false,
                                                 scriptedOpener(bc));
        QVERIFY(v.online);
        QCOMPARE(v.problem, Problem::None);
        QCOMPARE(v.hostName, QStringLiteral("Porch"));
        QCOMPARE(v.channels.size(), 1);
        QCOMPARE(v.channels[0].codec, QStringLiteral("h265"));
        QCOMPARE(v.channels[0].mainSize, QSize(3840, 2160));
        QCOMPARE(v.channels[0].subSize, QSize(640, 360));
        QCOMPARE(v.password, QStringLiteral("pw"));
        QCOMPARE(bc.opens, 0); // HTTP answered; Baichuan is never touched
    }
    void nvrListsOnlyOnlineChannelsWithTheirCodecs()
    {
        ScriptedHttp http;
        http.values["GetDevInfo"] = devInfo("NVR", 4);
        http.values["GetEnc"] = enc(1, "h264", 1920, 1080);
        http.values["GetChannelstatus"] = Json{{"status", Json::array({
            Json{{"channel", 0}, {"name", "Gone"}, {"online", 0}},
            Json{{"channel", 1}, {"name", "Yard"}, {"online", 1}, {"uid", "U1"}}})}};
        BaichuanScript bc;
        const Validation v = validateReolinkHost(http, rec(), QStringLiteral("pw"), false,
                                                 scriptedOpener(bc));
        QVERIFY(v.online);
        QCOMPARE(v.channels.size(), 1);
        QCOMPARE(v.channels[0].channel, 1);
        QCOMPARE(v.channels[0].name, QStringLiteral("Yard"));
        QCOMPARE(v.channels[0].uid, QStringLiteral("U1"));
        QCOMPARE(v.channels[0].codec, QStringLiteral("h264"));
    }
    void nvrWithNoOnlineCamerasKeepsAPlaceholder()
    {
        ScriptedHttp http;
        http.values["GetDevInfo"] = devInfo("NVR", 4);
        http.values["GetChannelstatus"] = Json{{"status", Json::array()}};
        BaichuanScript bc;
        const Validation v = validateReolinkHost(http, rec(), QStringLiteral("pw"), false,
                                                 scriptedOpener(bc));
        QVERIFY(v.online);
        QCOMPARE(v.channels.size(), 1);
        QVERIFY(!v.channels[0].online);
    }
    void failureKindDecidesTheProblem_data()
    {
        QTest::addColumn<int>("fail");
        QTest::addColumn<int>("problem");
        QTest::newRow("auth") << int(ReolinkTransport::FailKind::Auth) << int(Problem::Auth);
        QTest::newRow("locked") << int(ReolinkTransport::FailKind::Locked) << int(Problem::Locked);
        QTest::newRow("transport") << int(ReolinkTransport::FailKind::Transport)
                                   << int(Problem::Unreachable);
    }
    void failureKindDecidesTheProblem()
    {
        QFETCH(int, fail);
        QFETCH(int, problem);
        ScriptedHttp http;
        http.transportOk = false;
        http.transportError = QStringLiteral("login refused");
        http.failKind = ReolinkTransport::FailKind(fail);
        BaichuanScript bc;
        bc.canOpen = false;
        const Validation v = validateReolinkHost(http, rec(), QStringLiteral("pw"), false,
                                                 scriptedOpener(bc));
        QVERIFY(!v.online);
        QCOMPARE(int(v.problem), problem);
        QCOMPARE(v.status, QStringLiteral("login refused"));
        // Whatever the problem, only an unreachable host may be retried (see HostSession).
        HostSession s;
        s.validationFailed(v.problem, t0);
        QCOMPARE(s.nextRetryAt().has_value(), v.problem == Problem::Unreachable);
    }
    void rejectedLoginNeverTouchesBaichuan()
    {
        // A second login attempt on port 9000 would count against the lockout too.
        ScriptedHttp http;
        http.transportOk = false;
        http.failKind = ReolinkTransport::FailKind::Auth;
        BaichuanScript bc;
        validateReolinkHost(http, rec(), QStringLiteral("pw"), false, scriptedOpener(bc));
        QCOMPARE(bc.opens, 0);
    }
    void deadHttpApiFallsBackToBaichuanCameraList()
    {
        ScriptedHttp http;
        http.transportOk = false;
        http.failKind = ReolinkTransport::FailKind::Transport;
        BaichuanScript bc;
        // cmd 146 StreamInfoList names the channels; the test only needs the opener used.
        const Validation v = validateReolinkHost(http, rec(), QStringLiteral("pw"), false,
                                                 scriptedOpener(bc));
        QVERIFY(!v.online);
        QVERIFY(bc.opens >= 1);
        QCOMPARE(bc.lastParams.host, QStringLiteral("10.0.0.5"));
        QCOMPARE(bc.lastParams.port, 9000);

        BaichuanScript bc2;
        validateReolinkHost(http, rec(), QStringLiteral("pw"), /*alreadyDegraded*/ true,
                            scriptedOpener(bc2));
        QCOMPARE(bc2.opens, 0); // a degraded host is not re-discovered
    }

    // ---- camera commands, through the Baichuan seam ----
    void alertsReadsThreeEnablesInOneSession()
    {
        BaichuanScript bc;
        bc.enables[219] = 1;
        bc.enables[217] = 0;
        CameraCommands cmds(scriptedOpener(bc));
        const QVariantMap m = cmds.fetchAlerts({QStringLiteral("h"), 9000, "u", "p"}, 2);
        QCOMPARE(m.value("ok").toBool(), true);
        QCOMPARE(m.value("push").toInt(), 1);
        QCOMPARE(m.value("email").toInt(), 0);
        QCOMPARE(m.value("ftp").toInt(), -1);
        QCOMPARE(bc.opens, 1);
        QCOMPARE(bc.closes, 1);
    }
    void alertsUnreachableIsNotOk()
    {
        BaichuanScript bc;
        bc.canOpen = false;
        CameraCommands cmds(scriptedOpener(bc));
        const QVariantMap m = cmds.fetchAlerts({}, 0);
        QCOMPARE(m.value("ok").toBool(), false);
        QVERIFY(!m.contains("push"));
    }
    void alertKindsMapToCommandIds()
    {
        quint32 g = 0, s = 0;
        QVERIFY(CameraCommands::alertCommandIds("push", g, s));
        QCOMPARE(std::make_pair(g, s), std::make_pair(219u, 218u));
        QVERIFY(CameraCommands::alertCommandIds("email", g, s));
        QCOMPARE(std::make_pair(g, s), std::make_pair(217u, 216u));
        QVERIFY(CameraCommands::alertCommandIds("ftp", g, s));
        QCOMPARE(std::make_pair(g, s), std::make_pair(70u, 71u));
        QVERIFY(!CameraCommands::alertCommandIds("sms", g, s));
    }
    void setAlertWritesTheRightPair()
    {
        BaichuanScript bc;
        CameraCommands cmds(scriptedOpener(bc));
        const CommandResult r = cmds.setAlert({}, 3, "email", true);
        QVERIFY(r.ok);
        QCOMPARE(bc.enableWrites.size(), 1);
        QCOMPARE(std::get<0>(bc.enableWrites[0]), 217u);
        QCOMPARE(std::get<1>(bc.enableWrites[0]), 216u);
        QCOMPARE(std::get<2>(bc.enableWrites[0]), 3);
        QCOMPARE(std::get<3>(bc.enableWrites[0]), true);
        QCOMPARE(cmds.setAlert({}, 3, "sms", true).failure, CommandFailure::UnknownKind);
        bc.canOpen = false;
        QCOMPARE(cmds.setAlert({}, 3, "push", true).failure, CommandFailure::Unreachable);
    }
    void recScheduleParses()
    {
        const QByteArray xml =
            "<body><enable>1</enable><list>"
            "<item><type>Normal</type><valueTable>1111</valueTable></item>"
            "<item><type>MD</type><valueTable>0000</valueTable></item></list></body>";
        const QVariantMap m = CameraCommands::parseRecSchedule(xml);
        QCOMPARE(m.value("Normal").toString(), QStringLiteral("1111"));
        QCOMPARE(m.value("MD").toString(), QStringLiteral("0000"));
        QCOMPARE(m.value("enable").toBool(), true);
        QVERIFY(CameraCommands::parseRecSchedule("<body/>").isEmpty());
        // A truncated item stops the parse without losing what came before it.
        const QVariantMap part = CameraCommands::parseRecSchedule(
            "<item><type>A</type><valueTable>01</valueTable></item><item><type>B</type>");
        QCOMPARE(part.value("A").toString(), QStringLiteral("01"));
        QVERIFY(!part.contains("B"));
    }
    void writeRecScheduleReplacesOnlyOneTable()
    {
        BaichuanScript bc;
        bc.replies[81] = "<a><item><type>Normal</type><valueTable>1111</valueTable></item>"
                         "<item><type>MD</type><valueTable>0000</valueTable></item></a>";
        CameraCommands cmds(scriptedOpener(bc));
        const CommandResult r = cmds.writeRecSchedule({}, 1, "MD", "1010");
        QVERIFY(r.ok);
        QCOMPARE(bc.transacts.size(), 1);
        QCOMPARE(bc.transacts[0].cmd, 82u);
        QCOMPARE(bc.transacts[0].channel, 1);
        QCOMPARE(bc.transacts[0].body,
                 QByteArray("<a><item><type>Normal</type><valueTable>1111</valueTable></item>"
                            "<item><type>MD</type><valueTable>1010</valueTable></item></a>"));
    }
    void writeRecScheduleFailureModes()
    {
        BaichuanScript bc;
        bc.replies[81] = "<item><type>Normal</type><valueTable>1</valueTable></item>";
        CameraCommands cmds(scriptedOpener(bc));
        QCOMPARE(cmds.writeRecSchedule({}, 0, "dog_cat", "1").failure, CommandFailure::NotFound);
        QVERIFY(bc.transacts.isEmpty());
        bc.transactStatus = 400;
        const CommandResult rej = cmds.writeRecSchedule({}, 0, "Normal", "0");
        QCOMPARE(rej.failure, CommandFailure::Rejected);
        QCOMPARE(rej.status, 400);
        for (int ok : {201, 300}) {
            bc.transactStatus = quint16(ok);
            QVERIFY(cmds.writeRecSchedule({}, 0, "Normal", "0").ok);
        }
        bc.canOpen = false;
        QCOMPARE(cmds.writeRecSchedule({}, 0, "Normal", "0").failure, CommandFailure::Unreachable);
    }
    void configReadAndWritePassThrough()
    {
        BaichuanScript bc;
        bc.flatConfigs[26] = QVariantMap{{"x", "1"}};
        CameraCommands cmds(scriptedOpener(bc));
        QCOMPARE(cmds.fetchConfig({}, 0, 26, {}).value("x").toString(), QStringLiteral("1"));
        bc.canOpen = false;
        QVERIFY(cmds.fetchConfig({}, 0, 26, {}).isEmpty());
        bc.canOpen = true;
        QVERIFY(cmds.writeConfig({}, 0, 26, 27, QVariantMap{{"x", "2"}}, {}).ok);
        bc.writeFieldsOk = false;
        QCOMPARE(cmds.writeConfig({}, 0, 26, 27, {}, {}).failure, CommandFailure::Rejected);
    }
    void floodlightToggleFlipsOnlyState()
    {
        ScriptedHttp http;
        http.values["GetWhiteLed"] = Json{{"WhiteLed",
            {{"state", 0}, {"mode", 1}, {"bright", 70}, {"channel", 0}, {"extra", 5}}}};
        http.values["SetWhiteLed"] = Json::object();
        const CommandResult r = CameraCommands::toggleFloodlight(http, 0);
        QVERIFY(r.ok);
        QCOMPARE(http.sent, (QStringList{"GetWhiteLed", "SetWhiteLed"}));
        const Json set = http.sentParams.last()["WhiteLed"];
        QCOMPARE(set["state"].get<int>(), 1);
        QCOMPARE(set["mode"].get<int>(), 1);
        QCOMPARE(set["bright"].get<int>(), 70);
        QVERIFY(!set.contains("extra")); // only the settable fields are written back
    }
    void floodlightToggleReportsUnsupported()
    {
        ScriptedHttp http;
        const CommandResult r = CameraCommands::toggleFloodlight(http, 0);
        QVERIFY(!r.ok);
        QCOMPARE(r.failure, CommandFailure::Unsupported);
        QCOMPARE(r.detail, QStringLiteral("not supported"));
    }
};

QTEST_MAIN(TestHostSession)
#include "test_hostsession.moc"
