// BoschTourController state machines driven through the Home Assistant seam:
// a scripted transport records every request and the test decides when and how
// each one is answered. No network, no keyring, no timers firing (the event
// loop is never spun).
#include "core/CredentialStore.h"
#include "device/BoschTourController.h"
#include "device/HomeAssistantTransport.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QtTest>

#include <deque>
#include <map>

using namespace rl;

namespace {

class ScriptedTransport : public HomeAssistantTransport
{
public:
    struct Pending {
        QUrl url;
        QString token;
        QByteArray body;
        Reply reply;
    };

    void send(const QUrl &url, const QString &token, const QByteArray &body, Reply reply) override
    {
        pending.push_back({url, token, body, std::move(reply)});
        ++totalSent;
    }

    static QString nameOf(const QUrl &url) { return url.path().section(QLatin1Char('/'), -1); }

    int count(const QString &name) const
    {
        int n = 0;
        for (const auto &p : pending)
            n += nameOf(p.url) == name;
        return n;
    }

    // Answer the oldest pending request for this service name.
    QJsonObject answer(const QString &name, int status, const QByteArray &body = "{}",
                       const QString &error = QString())
    {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (nameOf(it->url) != name)
                continue;
            Pending p = std::move(*it);
            pending.erase(it);
            const QJsonObject sent = QJsonDocument::fromJson(p.body).object();
            p.reply(status, body, error);
            return sent;
        }
        QTest::qFail(qPrintable(QStringLiteral("no pending request for ") + name), __FILE__, __LINE__);
        return {};
    }

    // Body the oldest pending request for this name carried.
    QJsonObject peekBody(const QString &name) const
    {
        for (const auto &p : pending)
            if (nameOf(p.url) == name)
                return QJsonDocument::fromJson(p.body).object();
        return {};
    }

    std::deque<Pending> pending;
    int totalSent = 0;
};

class FakeCredentials : public CredentialStore
{
public:
    bool storeSecret(const QString &key, const QString &secret, const QString &) override
    {
        secrets[key] = secret;
        return true;
    }
    QString lookupSecret(const QString &key, bool *ok = nullptr) override
    {
        if (ok)
            *ok = secrets.count(key) > 0;
        return secrets.count(key) ? secrets[key] : QString();
    }
    bool removeSecret(const QString &key) override { return secrets.erase(key) > 0; }
    std::map<QString, QString> secrets;
};

QByteArray svc(const QByteArray &inner) { return "{\"service_response\":" + inner + "}"; }

const QByteArray kPatrolStopped = svc(
    R"({"status":"stopped","current_position":"","config":{"route":[{"token":"1","label":"Driveway"},{"token":"2","label":"Road"}],"travel_speed":40,"dwell_seconds":2}})");
const QByteArray kPresets = svc(
    R"({"presets":[{"name":"Position 1","token":"1"},{"name":"Position 2","token":"2"},{"name":"Position 3","token":"3"}]})");
const QByteArray kSlots = svc(
    R"({"slots":[{"token":"1","label":"Driveway"},{"token":"2","label":"Road"},{"token":"3","label":"Gate"}]})");
const QByteArray kScene = svc(
    R"({"ok":true,"mode_id":1,"modes":[{"id":1,"name":"Standard"},{"id":2,"name":"Fast Movement"}]})");
const QByteArray kIrisAuto = svc(R"({"supported":true,"mode":"AUTO","iris":5.0})");
const QString kWiperState = QStringLiteral("pyscript.bosch_camera_wiper");
const QString kWiper = QStringLiteral("bosch_camera_wiper");
const QString kSteer = QStringLiteral("bosch_camera_steer");
const QByteArray kSteerOk = svc(R"({"ok":true})");
const qreal kReach = 192;   // the drag's full-speed distance, px
bool isStop(const QJsonObject &steer)
{
    return steer.value("pan").toDouble() == 0 && steer.value("tilt").toDouble() == 0
           && steer.value("zoom").toDouble() == 0;
}
QByteArray wiperState(const char *state)
{
    return QByteArray(R"({"entity_id":"pyscript.bosch_camera_wiper","state":")") + state + "\"}";
}

} // namespace

class TestBoschTour : public QObject
{
    Q_OBJECT

    ScriptedTransport *ht = nullptr;
    FakeCredentials creds;
    BoschTourController *c = nullptr;
    QTemporaryDir settingsDir;

    // Answer every pending read (get_*) with the canned Home Assistant state,
    // including reads that those answers trigger. Mutations are left pending.
    void settle(const QByteArray &patrol = kPatrolStopped, const QByteArray &iris = kIrisAuto,
                const QByteArray &wiper = wiperState("off"))
    {
        const std::map<QString, QByteArray> reads = {
            {QStringLiteral("bosch_camera_get_patrol"), patrol},
            {QStringLiteral("bosch_camera_get_presets"), kPresets},
            {QStringLiteral("bosch_camera_get_slots"), kSlots},
            {QStringLiteral("bosch_camera_get_scene_mode"), kScene},
            {QStringLiteral("bosch_camera_get_iris"), iris},
            {kWiperState, wiper},
        };
        for (bool again = true; again;) {
            again = false;
            for (const auto &[name, body] : reads) {
                if (ht->count(name)) {
                    ht->answer(name, 200, body);
                    again = true;
                }
            }
        }
    }

    // A steer that would move the camera is waiting.
    bool anyMoveSent() const
    {
        for (const auto &p : ht->pending)
            if (ScriptedTransport::nameOf(p.url) == kSteer
                && !isStop(QJsonDocument::fromJson(p.body).object()))
                return true;
        return false;
    }

    void holdTick() { QVERIFY(QMetaObject::invokeMethod(c, "resendSteer")); }

    void connectAndSettle(const QByteArray &patrol = kPatrolStopped)
    {
        c->connectHomeAssistant(QStringLiteral("https://ha.test/lovelace/0"),
                                QStringLiteral("tok"), QStringLiteral("camera.bosch"));
        QCOMPARE(ht->pending.size(), size_t(1));
        QCOMPARE(ht->pending.front().url.toString(), QStringLiteral("https://ha.test/api/"));
        QCOMPARE(ht->pending.front().token, QStringLiteral("tok"));
        ht->answer(QStringLiteral(""), 200, R"({"message":"API running."})");
        QVERIFY(c->connected());
        c->refreshIris(); // the iris panel reads its own state on open
        settle(patrol);
        QVERIFY(ht->pending.empty());
    }

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("rl-test"));
        QCoreApplication::setApplicationName(QStringLiteral("test_boschtour"));
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    }

    void init()
    {
        QSettings().clear();
        creds.secrets.clear();
        ht = new ScriptedTransport;
        c = new BoschTourController(&creds, ht);
        ht->setParent(c);
    }

    void cleanup()
    {
        delete c;
        c = nullptr;
    }

    // ---- connection -----------------------------------------------------

    void unconfiguredByDefault()
    {
        QVERIFY(!c->configured());
        QVERIFY(!c->connected());
        QCOMPARE(c->connectionStatus(), QStringLiteral("Not configured"));
        QVERIFY(ht->pending.empty());
    }

    void connectNormalizesUrlAndPersistsToken()
    {
        connectAndSettle();
        QCOMPARE(c->endpoint(), QStringLiteral("https://ha.test"));
        QCOMPARE(creds.secrets.at(QStringLiteral("homeassistant/bosch-tour")), QStringLiteral("tok"));
        QCOMPARE(c->tourState(), QStringLiteral("stopped"));
    }

    void connectRejectsBadTokenResponse()
    {
        c->connectHomeAssistant(QStringLiteral("ha.test"), QStringLiteral("bad"),
                                QStringLiteral("camera.bosch"));
        ht->answer(QStringLiteral(""), 401, R"({"message":"401: Unauthorized"})");
        QVERIFY(!c->connected());
        QVERIFY(!c->configured());
        QVERIFY(creds.secrets.empty());
        QVERIFY(c->message().contains(QStringLiteral("rejected")));
    }

    void staleReplyAfterDisconnectIsIgnored()
    {
        connectAndSettle();
        c->refresh();
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_get_patrol")), 1);
        c->disconnectHomeAssistant();
        ht->answer(QStringLiteral("bosch_camera_get_patrol"), 200,
                   svc(R"({"status":"travelling","config":{}})"));
        QCOMPARE(c->tourState(), QStringLiteral("unknown"));
        QVERIFY(!c->configured());
    }

    // ---- patrol ---------------------------------------------------------

    void patrolReadFillsRouteStateAndStep()
    {
        connectAndSettle(svc(R"({"status":"dwelling","current_position":"Road","config":{"route":[{"token":"1","label":"x"},{"token":"2","label":"y"}],"travel_speed":500,"dwell_seconds":-3}})"));
        QVERIFY(c->touring());
        QCOMPARE(c->currentPosition(), QStringLiteral("Road"));
        QCOMPARE(c->currentStep(), QStringLiteral("2 / 2"));
        // Labels follow the HA slot names; speed and dwell are clamped.
        QCOMPARE(c->route().at(0).toMap().value(QStringLiteral("label")).toString(),
                 QStringLiteral("Driveway"));
        QCOMPARE(c->travelSpeed(), 100);
        QCOMPARE(c->dwellSeconds(), 0);
    }

    void startTourRefusedWhileRunningOrEmpty()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{"route":[{"token":"1","label":"Driveway"}]}})"));
        c->startTour();
        QVERIFY(ht->pending.empty());
        QVERIFY(c->message().contains(QStringLiteral("must be stopped")));
    }

    void startTourThenStopTour()
    {
        connectAndSettle();
        c->startTour();
        QVERIFY(c->busy());
        ht->answer(QStringLiteral("bosch_camera_start_patrol"), 200,
                   svc(R"({"status":"starting"})"));
        QVERIFY(!c->busy());
        QCOMPARE(c->tourState(), QStringLiteral("starting"));
        QVERIFY(c->touring());
        ht->answer(QStringLiteral("bosch_camera_get_patrol"), 200,
                   svc(R"({"status":"travelling","config":{}})"));
        QCOMPARE(c->tourState(), QStringLiteral("travelling"));

        c->stopTour();
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QCOMPARE(c->tourState(), QStringLiteral("stopped"));
        QVERIFY(!c->touring());
    }

    void patrolReadOverlappingAMutationIsReread()
    {
        connectAndSettle();
        c->refresh();
        c->startTour();
        // The read was issued before the start; its answer predates the change.
        ht->answer(QStringLiteral("bosch_camera_get_patrol"), 200, kPatrolStopped);
        ht->answer(QStringLiteral("bosch_camera_start_patrol"), 200, svc(R"({"status":"starting"})"));
        QCOMPARE(c->tourState(), QStringLiteral("starting"));
        QVERIFY(ht->count(QStringLiteral("bosch_camera_get_patrol")) >= 1);
    }

    void saveTourValidatesAndSends()
    {
        connectAndSettle();
        QSignalSpy saved(c, &BoschTourController::tourSaved);
        c->saveTour({QVariantMap{{"token", "9"}, {"label", "Nowhere"}}}, 40, 1);
        QCOMPARE(saved.takeFirst().at(0).toBool(), false);
        c->saveTour({QVariantMap{{"token", "1"}, {"label", "A"}},
                     QVariantMap{{"token", "2"}, {"label", "a"}}}, 40, 1);
        QCOMPARE(saved.takeFirst().at(0).toBool(), false);
        QVERIFY(ht->pending.empty());

        c->saveTour({QVariantMap{{"token", "3"}, {"label", "Gate"}}}, 5, 9999);
        const QJsonObject sent = ht->peekBody(QStringLiteral("bosch_camera_set_patrol"));
        QCOMPARE(sent.value("travel_speed").toInt(), 10);
        QCOMPARE(sent.value("dwell_seconds").toInt(), 3600);
        ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        QCOMPARE(saved.takeFirst().at(0).toBool(), true);
    }

    // ---- a manual move during a tour stops it (lascelles-bosch-tour ticket 03) ----

    void gotoPresetDuringTourStopsItThenMoves()
    {
        connectAndSettle(svc(R"({"status":"dwelling","config":{}})"));
        c->gotoPreset(QStringLiteral("1"));
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_stop_patrol")), 1);
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_goto_preset")), 0);   // stop first
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QCOMPARE(ht->peekBody(QStringLiteral("bosch_camera_goto_preset")).value("token").toString(),
                 QStringLiteral("1"));
        QVERIFY(c->manualStopped());
        QCOMPARE(c->tourStatus(), QStringLiteral("Manual · tour stopped"));
    }

    // ---- Home Assistant says why the tour stopped (lascelles-bosch-tour ticket 10) ----

    void aManualStopTellsHomeAssistantWhy()
    {
        connectAndSettle(svc(R"({"status":"dwelling","config":{}})"));
        c->gotoPreset(QStringLiteral("1"));
        const QJsonObject stop = ht->peekBody(QStringLiteral("bosch_camera_stop_patrol"));
        QCOMPARE(stop.value("reason").toString(), QStringLiteral("manual"));
    }

    void homeAssistantsStopReasonDecidesManual()
    {
        // A move from Home Assistant's own dashboard: the fork made none, but HA says "manual".
        connectAndSettle(svc(R"({"status":"stopped","stop_reason":"manual","config":{}})"));
        QVERIFY(c->manualStopped());
        QCOMPARE(c->tourStatus(), QStringLiteral("Manual · tour stopped"));
    }

    void homeAssistantsPlainStopClearsManual()
    {
        connectAndSettle(svc(R"({"status":"dwelling","config":{}})"));
        c->gotoPreset(QStringLiteral("1"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QVERIFY(c->manualStopped());
        // Later someone pressed Stop in Home Assistant after starting it again: its word wins.
        ht->answer(QStringLiteral("bosch_camera_get_patrol"), 200,
                   svc(R"({"status":"stopped","stop_reason":"stopped","config":{}})"));
        QVERIFY(!c->manualStopped());
        QCOMPARE(c->tourStatus(), QString());
    }

    void gotoPresetWithUnknownStateIsStillRefused()
    {
        connectAndSettle(svc(R"({"status":"unknown","config":{}})"));
        c->gotoPreset(QStringLiteral("1"));
        QVERIFY(ht->pending.empty());
    }

    void moveDuringTourStopsItThenMovesWhileHeld()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        QCOMPARE(ht->count(kSteer), 0);
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        const QJsonObject move = ht->peekBody(kSteer);
        QCOMPARE(move.value("pan").toDouble(), -0.35);
        QCOMPARE(move.value("tilt").toDouble(), 0.0);
        QCOMPARE(move.value("zoom").toDouble(), 0.0);
        QVERIFY(c->manualStopped());
    }

    void releasedBeforeTheStopLandsOnlyStops()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        c->stopCamera();   // released
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QVERIFY(!anyMoveSent());
        QVERIFY(c->manualStopped());
    }

    void failedStopNeitherMovesNorMarksManual()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->gotoPreset(QStringLiteral("1"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 500, "{}");
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_goto_preset")), 0);
        QVERIFY(!c->manualStopped());
        QCOMPARE(c->tourStatus(), QStringLiteral("Touring"));
    }

    void manualStaysThroughStoppedReadsAndClearsWhenATourStarts()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->gotoPreset(QStringLiteral("1"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_goto_preset"), 200, "");
        settle();   // HA reads "stopped"
        QVERIFY(c->manualStopped());
        // HA starts the tour again (its own dashboard): Manual clears.
        c->refresh();
        settle(svc(R"({"status":"travelling","config":{}})"));
        QVERIFY(!c->manualStopped());
        QCOMPARE(c->tourStatus(), QStringLiteral("Touring"));
    }

    void startTourClearsManual()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{"route":[{"token":"1","label":"Driveway"}]}})"));
        c->gotoPreset(QStringLiteral("1"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_goto_preset"), 200, "");
        settle();
        c->startTour();
        ht->answer(QStringLiteral("bosch_camera_start_patrol"), 200, svc(R"({"status":"starting"})"));
        QVERIFY(!c->manualStopped());
    }

    void editsStayBlockedDuringATour()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        QSignalSpy saved(c, &BoschTourController::tourSaved);
        c->saveTour({QVariantMap{{"token", "1"}, {"label", "Driveway"}}}, 40, 1);
        QCOMPARE(saved.takeFirst().at(0).toBool(), false);
        QVERIFY(ht->pending.empty());
    }

    void reconnectDuringTheStopDoesNotWedgeManualMoves()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->gotoPreset(QStringLiteral("1"));
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_stop_patrol")), 1);
        // Reconnect: the old stop's answer is dropped (new generation).
        c->connectHomeAssistant(QStringLiteral("https://ha.test"), QStringLiteral("tok"),
                                QStringLiteral("camera.bosch"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral(""), 200, R"({"message":"API running."})");
        settle(svc(R"({"status":"travelling","config":{}})"));
        QVERIFY(ht->pending.empty());
        c->gotoPreset(QStringLiteral("2"));   // works again
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_stop_patrol")), 1);
    }

    void disconnectClearsManual()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->gotoPreset(QStringLiteral("1"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QVERIFY(c->manualStopped());
        c->disconnectHomeAssistant();
        QVERIFY(!c->manualStopped());
        QVERIFY(c->tourStatus().isEmpty());
    }

    void latestPressWinsWhileTheStopIsPending()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        c->gotoPreset(QStringLiteral("2"));   // the tap replaces the held direction
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_goto_preset")), 1);
        QVERIFY(!anyMoveSent());
    }

    void aMoveThatEndsTsTourTellsHomeAssistantItWasManual()
    {
        // Ticket 10: the takeover's stop carries the reason, so HA's own word keeps "Manual".
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("1"));
        c->toggleSelectionTourPosition(QStringLiteral("3"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        ht->answer(QStringLiteral("bosch_camera_start_patrol"), 200,
                   svc(R"({"status":"starting"})"));
        c->gotoPreset(QStringLiteral("2"));
        const QJsonObject stop = ht->peekBody(QStringLiteral("bosch_camera_stop_patrol"));
        QCOMPARE(stop.value("reason").toString(), QStringLiteral("manual"));
    }

    void positionTapForTsTourEndsTThenMoves()
    {
        // The controller side of answer 4: a goto while T is on leaves T first.
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("1"));
        c->toggleSelectionTourPosition(QStringLiteral("3"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        ht->answer(QStringLiteral("bosch_camera_start_patrol"), 200, svc(R"({"status":"starting"})"));
        c->gotoPreset(QStringLiteral("2"));
        QVERIFY(!c->selectionTourMode());
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_goto_preset")), 0);
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        QCOMPARE(ht->peekBody(QStringLiteral("bosch_camera_goto_preset")).value("token").toString(),
                 QStringLiteral("2"));
        QVERIFY(c->manualStopped());
    }

    void failedRestoreDropsTheHeldMove()
    {
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("1"));
        c->moveCamera(QStringLiteral("RIGHT"), QString(), QString());
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 500, "{}");
        c->toggleSelectionTourPosition(QStringLiteral("2"));   // no effect; T is off
        c->endSelectionTour();                                // retry the restore
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QVERIFY(!anyMoveSent());   // only the retry's own traffic: no stale move
    }

    void padPressInSelectionModeEndsItThenMoves()
    {
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("1"));   // one stop: nothing runs
        c->moveCamera(QStringLiteral("RIGHT"), QString(), QString());
        QVERIFY(!c->selectionTourMode());
        QCOMPARE(ht->count(kSteer), 0);   // after T's usual stop lands
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QVERIFY(!c->selectionTourRestoring());
        QCOMPARE(ht->count(kSteer), 1);   // the move, no Stop racing it
        QCOMPARE(ht->peekBody(kSteer).value("pan").toDouble(), 0.35);
        QVERIFY(!c->manualStopped());   // no tour was running
    }

    void padPressDuringASelectionTourStopsRestoresThenMoves()
    {
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("1"));
        c->toggleSelectionTourPosition(QStringLiteral("3"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        ht->answer(QStringLiteral("bosch_camera_start_patrol"), 200, svc(R"({"status":"starting"})"));
        QVERIFY(c->touring());

        c->moveCamera(QStringLiteral("RIGHT"), QString(), QString());
        QVERIFY(!c->selectionTourMode());
        QCOMPARE(ht->count(kSteer), 0);   // not before the route is restored
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        QVERIFY(!c->selectionTourRestoring());
        QCOMPARE(ht->peekBody(kSteer).value("pan").toDouble(), 0.35);
        QVERIFY(c->manualStopped());
    }

    // ---- tour selection -------------------------------------------------

    void selectionTourRunsAndRestoresSavedRoute()
    {
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        QVERIFY(c->selectionTourMode());

        c->toggleSelectionTourPosition(QStringLiteral("1"));
        QVERIFY(ht->pending.empty()); // one position never starts a tour
        c->toggleSelectionTourPosition(QStringLiteral("3"));
        QCOMPARE(c->selectionTourRoute().size(), 2);

        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        // Route differs from the saved one: HA is reconfigured, then started.
        const QJsonObject route = ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        QCOMPARE(route.value("route").toArray().size(), 2);
        QCOMPARE(route.value("route").toArray().at(1).toObject().value("token").toString(),
                 QStringLiteral("3"));
        QCOMPARE(route.value("travel_speed").toInt(), 40);
        QCOMPARE(route.value("dwell_seconds").toInt(), 2);
        ht->answer(QStringLiteral("bosch_camera_start_patrol"), 200, svc(R"({"status":"starting"})"));
        QVERIFY(c->touring());

        QSignalSpy restoring(c, &BoschTourController::selectionTourRestoringChanged);
        c->endSelectionTour();
        QVERIFY(!c->selectionTourMode());
        QVERIFY(c->selectionTourRestoring());
        QCOMPARE(ht->count(kSteer), 1); // camera movement stopped too
        QVERIFY(isStop(ht->peekBody(kSteer)));
        ht->answer(kSteer, 200, kSteerOk);
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        const QJsonObject restored = ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        QCOMPARE(restored.value("route").toArray().size(), 2);
        QCOMPARE(restored.value("route").toArray().at(1).toObject().value("token").toString(),
                 QStringLiteral("2"));
        QVERIFY(!c->selectionTourRestoring());
        QVERIFY(!restoring.isEmpty());
    }

    void selectionTourNotAllowedWhileTouring()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        QVERIFY(!c->beginSelectionTour());
        QVERIFY(!c->selectionTourMode());
    }

    void selectionTourRejectsDuplicateNamesAndUnknownPositions()
    {
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("99"));
        QVERIFY(c->selectionTourRoute().isEmpty());
        c->toggleSelectionTourPosition(QStringLiteral("1"));
        c->toggleSelectionTourPosition(QStringLiteral("1")); // toggles off
        QVERIFY(c->selectionTourRoute().isEmpty());
    }

    void selectionTourFailureDoesNotWedgeTheSession()
    {
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("1"));
        c->toggleSelectionTourPosition(QStringLiteral("3"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 500, "{}");
        QVERIFY(c->message().contains(QStringLiteral("Stop selected Bosch tour")));
        // Another toggle retries the reconcile instead of hanging.
        c->toggleSelectionTourPosition(QStringLiteral("2"));
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_stop_patrol")), 1);
    }

    // ---- scene mode -----------------------------------------------------

    void sceneModeReadPublishesModes()
    {
        connectAndSettle();
        QVERIFY(c->sceneModeAvailable());
        QCOMPARE(c->sceneModeId(), 1);
        QCOMPARE(c->sceneModeName(), QStringLiteral("Standard"));
        QCOMPARE(c->sceneModes().size(), 2);
        QVERIFY(!c->sceneModeStale());
    }

    void setSceneModeConfirmedByCamera()
    {
        connectAndSettle();
        c->setSceneMode(2);
        QVERIFY(c->sceneModePending());
        const QJsonObject sent = ht->answer(QStringLiteral("bosch_camera_set_scene_mode"), 200,
            svc(R"({"ok":true,"mode_id":2,"modes":[{"id":1,"name":"Standard"},{"id":2,"name":"Fast Movement"}]})"));
        QCOMPARE(sent.value("mode_id").toInt(), 2);
        QVERIFY(!c->sceneModePending());
        QCOMPARE(c->sceneModeName(), QStringLiteral("Fast Movement"));
    }

    void setSceneModeIgnoresUnknownIdAndDoubleSubmit()
    {
        connectAndSettle();
        c->setSceneMode(42);
        QVERIFY(ht->pending.empty());
        c->setSceneMode(2);
        c->setSceneMode(1);
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_set_scene_mode")), 1);
    }

    void setSceneModeUnconfirmedMarksStale()
    {
        connectAndSettle();
        c->setSceneMode(2);
        // Camera answers ok but reports a different mode than requested.
        ht->answer(QStringLiteral("bosch_camera_set_scene_mode"), 200, kScene);
        QVERIFY(!c->sceneModePending());
        QVERIFY(!c->sceneModeAvailable());
        QVERIFY(c->sceneModeStale());
        QVERIFY(!c->sceneModeError().isEmpty());
        QVERIFY(c->message().startsWith(QStringLiteral("Marked range:")));
    }

    void sceneModeSurvivesAsStaleAfterDisconnect()
    {
        connectAndSettle();
        c->disconnectHomeAssistant();
        QVERIFY(c->sceneModeStale());
        QVERIFY(!c->sceneModeAvailable());
        QCOMPARE(c->sceneModeError(), QStringLiteral("Home Assistant is disconnected."));
    }

    // ---- iris -----------------------------------------------------------

    void irisStatusFormatting()
    {
        connectAndSettle();
        QVERIFY(c->irisAvailable());
        QCOMPARE(c->irisStatus(), QStringLiteral("AUTO · 5.0"));
        c->refreshIris();
        ht->answer(QStringLiteral("bosch_camera_get_iris"), 200,
                   svc(R"({"supported":true,"mode":"MANUAL","iris":3.5,"gain":6.0,"exposure_time_us":8000})"));
        QCOMPARE(c->irisStatus(), QStringLiteral("MANUAL · 3.5 · G 6.0 dB · T 8.00 ms"));
    }

    void irisUnsupportedShowsReason()
    {
        connectAndSettle(kPatrolStopped);
        c->refreshIris();
        ht->answer(QStringLiteral("bosch_camera_get_iris"), 200,
                   svc(R"({"supported":false,"error":"no iris"})"));
        QVERIFY(!c->irisAvailable());
        QCOMPARE(c->irisStatus(), QStringLiteral("no iris"));
        QVERIFY(!c->startIris(QStringLiteral("open")));
        QVERIFY(c->message().contains(QStringLiteral("unavailable")));
    }

    // bosch-ptz-port ticket 02: manual moves go through bosch_camera_steer.
    void padDirectionsBecomeSteerNumbers()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("RIGHT"), QStringLiteral("UP"), QString());
        QJsonObject move = ht->answer(kSteer, 200, kSteerOk);
        QCOMPARE(move.value("pan").toDouble(), 0.35);
        QCOMPARE(move.value("tilt").toDouble(), 0.35);
        QCOMPARE(move.value("zoom").toDouble(), 0.0);
        c->stopCamera();
        ht->answer(kSteer, 200, kSteerOk);
        c->moveCamera(QStringLiteral("LEFT"), QStringLiteral("DOWN"), QString());
        move = ht->answer(kSteer, 200, kSteerOk);
        QCOMPARE(move.value("pan").toDouble(), -0.35);
        QCOMPARE(move.value("tilt").toDouble(), -0.35);
        c->stopCamera();
        ht->answer(kSteer, 200, kSteerOk);
        c->moveCamera(QString(), QString(), QStringLiteral("ZOOM_OUT"));
        QCOMPARE(ht->answer(kSteer, 200, kSteerOk).value("zoom").toDouble(), -0.35);
        c->stopCamera();
        ht->answer(kSteer, 200, kSteerOk);
        c->moveCamera(QString(), QString(), QStringLiteral("ZOOM_IN"));
        QCOMPARE(ht->answer(kSteer, 200, kSteerOk).value("zoom").toDouble(), 0.35);
        QCOMPARE(ht->count(QStringLiteral("ptz")), 0);   // never onvif.ptz
    }

    void aHeldMoveIsResentEachTickWithoutStacking()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        QCOMPARE(ht->count(kSteer), 1);
        holdTick();   // the first is unanswered (up to ~24 s while tracking turns off)
        QCOMPARE(ht->count(kSteer), 1);
        ht->answer(kSteer, 200, kSteerOk);
        QCOMPARE(ht->count(kSteer), 0);   // an answer alone sends nothing new
        holdTick();
        QCOMPARE(ht->count(kSteer), 1);
        QCOMPARE(ht->answer(kSteer, 200, kSteerOk).value("pan").toDouble(), -0.35);
        c->stopCamera();
        QVERIFY(isStop(ht->answer(kSteer, 200, kSteerOk)));
        holdTick();   // released: nothing more
        QVERIFY(ht->pending.empty());
    }

    void theLatestPressWinsOnceTheMoveIsAnswered()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        c->moveCamera(QString(), QStringLiteral("UP"), QString());   // e.g. a second key
        QCOMPARE(ht->count(kSteer), 1);
        ht->answer(kSteer, 200, kSteerOk);
        const QJsonObject next = ht->peekBody(kSteer);
        QCOMPARE(next.value("pan").toDouble(), 0.0);
        QCOMPARE(next.value("tilt").toDouble(), 0.35);
    }

    void releaseStopsAtOnceAndAgainAfterAnUnansweredMove()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        c->stopCamera();   // the move is still unanswered: the stop doesn't wait
        QCOMPARE(ht->count(kSteer), 2);
        QVERIFY(isStop(QJsonDocument::fromJson(ht->pending.back().body).object()));
        ht->answer(kSteer, 200, kSteerOk);   // the move lands after the stop was sent...
        QCOMPARE(ht->count(kSteer), 2);       // ...so one more stop follows it
        for (const auto &p : ht->pending)
            QVERIFY(isStop(QJsonDocument::fromJson(p.body).object()));
        ht->answer(kSteer, 200, kSteerOk);
        ht->answer(kSteer, 200, kSteerOk);
        QVERIFY(ht->pending.empty());
        QCOMPARE(c->message(), QStringLiteral("Camera movement stopped."));
    }

    void aRefusedSteerSaysWhyAndEndsTheHold()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        ht->answer(kSteer, 200,
                   svc(R"({"ok":false,"error":"The camera could not confirm this move"})"));
        QVERIFY(c->message().contains(QStringLiteral("The camera could not confirm this move")));
        holdTick();
        QVERIFY(ht->pending.empty());   // no retry storm while the button is still down
        c->stopCamera();                // release still stops
        QVERIFY(isStop(ht->peekBody(kSteer)));
    }

    void aSteerThatFailsOnTheWireSaysSoAndEndsTheHold()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        ht->answer(kSteer, 0, "", QStringLiteral("No answer from Home Assistant in 40 s"));
        QVERIFY(c->message().contains(QStringLiteral("No answer from Home Assistant")));
        holdTick();
        QVERIFY(ht->pending.empty());
    }

    void reconnectDropsAHoldInFlight()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        c->disconnectHomeAssistant();
        ht->pending.clear();
        holdTick();
        QVERIFY(ht->pending.empty());
    }

    void aSlowMoveSaysSoWithoutMarkingHomeAssistantDown()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        ht->answer(kSteer, 0, "", QStringLiteral("No answer from Home Assistant in 40 s"));
        QVERIFY(c->connected());
        QCOMPARE(c->message(),
                 QStringLiteral("Move Bosch camera: No answer from Home Assistant in 40 s"));
    }

    void aRejectedTokenDuringAHoldEndsIt()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        ht->answer(kSteer, 401, "{}");
        QVERIFY(c->message().contains(QStringLiteral("rejected the saved token")));
        holdTick();
        QVERIFY(ht->pending.empty());
    }

    void theSameDirectionPressedAgainGoesOnTheAnswer()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        c->stopCamera();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());   // first LEFT unanswered
        ht->answer(kSteer, 200, kSteerOk);   // the first LEFT
        QVERIFY(anyMoveSent());              // the new press goes now, not on the next tick
    }

    void aPressThatWaitsForATourStopEndsTheOlderHold()
    {
        connectAndSettle();
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        ht->answer(kSteer, 200, kSteerOk);
        // HA reports a tour started elsewhere; the next press waits for its stop.
        c->refresh();
        settle(svc(R"({"status":"travelling","config":{}})"));
        c->moveCamera(QString(), QStringLiteral("UP"), QString());
        holdTick();   // the old LEFT is not resent while the tour stops
        QVERIFY(!anyMoveSent());
    }

    void turningTOffKeepsTheRestoreMessage()
    {
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("1"));
        c->toggleSelectionTourPosition(QStringLiteral("3"));
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        ht->answer(QStringLiteral("bosch_camera_start_patrol"), 200, svc(R"({"status":"starting"})"));
        c->endSelectionTour();
        ht->answer(kSteer, 200, kSteerOk);
        QVERIFY(c->message().startsWith(QStringLiteral("Stopping the tour and restoring")));
    }

    // bosch-ptz-port ticket 04: steering by dragging the picture, as the Outpost app.
    void aDragSteersInProportionUpToFullSpeed()
    {
        connectAndSettle();
        c->steerDrag(96, 0, kReach);   // half the full-speed distance, to the right
        QJsonObject move = ht->answer(kSteer, 200, kSteerOk);
        QCOMPARE(move.value("pan").toDouble(), 0.5);
        QCOMPARE(move.value("tilt").toDouble(), 0.0);
        c->steerDrag(0, -96, kReach);   // up the screen tilts up
        move = ht->answer(kSteer, 200, kSteerOk);
        QCOMPARE(move.value("pan").toDouble(), 0.0);
        QCOMPARE(move.value("tilt").toDouble(), 0.5);
        c->steerDrag(-400, 400, kReach);   // past full speed: capped, direction kept
        move = ht->answer(kSteer, 200, kSteerOk);
        QCOMPARE(move.value("pan").toDouble(), -0.71);
        QCOMPARE(move.value("tilt").toDouble(), -0.71);
        QCOMPARE(move.value("zoom").toDouble(), 0.0);
    }

    void aDragWithinThreePixelsDoesNotMove()
    {
        connectAndSettle();
        c->steerDrag(2, -2, kReach);
        QVERIFY(ht->pending.empty());
    }

    void backIntoTheDeadZoneStopsAndOutAgainMoves()
    {
        connectAndSettle();
        c->steerDrag(96, 0, kReach);
        ht->answer(kSteer, 200, kSteerOk);
        c->steerDrag(1, 0, kReach);
        QVERIFY(isStop(ht->answer(kSteer, 200, kSteerOk)));
        holdTick();
        QVERIFY(ht->pending.empty());
        c->steerDrag(0, 48, kReach);
        QCOMPARE(ht->answer(kSteer, 200, kSteerOk).value("tilt").toDouble(), -0.25);
    }

    void theZoomStripFollowsThePhonesCurve()
    {
        connectAndSettle();
        c->zoomDrag(-10, 100);   // inside the still middle (16 %)
        QVERIFY(ht->pending.empty());
        c->zoomDrag(-50, 100);   // halfway up: 0.04 + 0.96 * ((0.5 - 0.16) / 0.84)^2 = 0.197
        QJsonObject move = ht->answer(kSteer, 200, kSteerOk);
        QCOMPARE(move.value("zoom").toDouble(), 0.2);
        QCOMPARE(move.value("pan").toDouble(), 0.0);
        c->zoomDrag(150, 100);   // past the bottom: full zoom out
        QCOMPARE(ht->answer(kSteer, 200, kSteerOk).value("zoom").toDouble(), -1.0);
    }

    void aDragDuringATourStopsItOnceThenSteersTheLatest()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->steerDrag(1, 0, kReach);   // dead zone: the tour keeps going
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_stop_patrol")), 0);
        c->steerDrag(40, 0, kReach);
        c->steerDrag(80, 0, kReach);
        c->steerDrag(96, 0, kReach);
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_stop_patrol")), 1);
        QCOMPARE(ht->count(kSteer), 0);
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QCOMPARE(ht->answer(kSteer, 200, kSteerOk).value("pan").toDouble(), 0.5);
        QVERIFY(c->manualStopped());
    }

    void aClickOnATouringPictureNeitherStopsTheTourNorTheCamera()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->steerDrag(2, 1, kReach);   // jitter inside the dead zone
        c->stopCamera();             // released
        QVERIFY(ht->pending.empty());
    }

    void aReleaseAfterAZoomInsideTheStillMiddleSendsNothing()
    {
        connectAndSettle();
        c->zoomDrag(-10, 100);
        c->stopCamera();
        QVERIFY(ht->pending.empty());
    }

    void irisPressHeartbeatAndRelease()
    {
        connectAndSettle();
        QVERIFY(!c->startIris(QStringLiteral("sideways")));
        QVERIFY(c->startIris(QStringLiteral("Open")));
        QVERIFY(!c->startIris(QStringLiteral("close"))); // already pressed
        const QJsonObject start = ht->peekBody(QStringLiteral("bosch_camera_iris_start"));
        QCOMPARE(start.value("direction").toString(), QStringLiteral("open"));
        const QString pressId = start.value("press_id").toString();
        QVERIFY(!pressId.isEmpty());
        ht->answer(QStringLiteral("bosch_camera_iris_start"), 200,
                   svc(R"({"ok":true,"active":true})"));

        c->stopIris();
        const QJsonObject stop = ht->answer(QStringLiteral("bosch_camera_iris_stop"), 200,
                                            svc(R"({"ok":true})"));
        QCOMPARE(stop.value("press_id").toString(), pressId);
        // Release re-reads the iris state and frees the press for the next one.
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_get_iris")), 1);
        ht->answer(QStringLiteral("bosch_camera_get_iris"), 200, kIrisAuto);
        QVERIFY(c->startIris(QStringLiteral("close")));
    }

    void irisReleaseBeforeStartReplyStopsAfterStart()
    {
        connectAndSettle();
        QVERIFY(c->startIris(QStringLiteral("open")));
        c->stopIris(); // start not yet answered: stop must wait
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_iris_stop")), 0);
        ht->answer(QStringLiteral("bosch_camera_iris_start"), 200,
                   svc(R"({"ok":true,"active":true})"));
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_iris_stop")), 1);
    }

    void irisStartFailureSendsStop()
    {
        connectAndSettle();
        QVERIFY(c->startIris(QStringLiteral("open")));
        ht->answer(QStringLiteral("bosch_camera_iris_start"), 500, "{}", QStringLiteral("boom"));
        QVERIFY(c->message().contains(QStringLiteral("boom")));
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_iris_stop")), 1);
    }

    void irisAtLimitReportsAndReleases()
    {
        connectAndSettle();
        QVERIFY(c->startIris(QStringLiteral("open")));
        ht->answer(QStringLiteral("bosch_camera_iris_start"), 200,
                   svc(R"({"ok":true,"active":false})"));
        QVERIFY(c->message().contains(QStringLiteral("limit")));
    }

    void irisAutoRefusedWhilePressed()
    {
        connectAndSettle();
        QVERIFY(c->startIris(QStringLiteral("open")));
        c->setIrisAuto();
        QCOMPARE(ht->count(QStringLiteral("bosch_camera_set_iris_mode")), 0);
        QVERIFY(c->message().contains(QStringLiteral("Release")));
    }
    // ---- wiper (bosch-wiper-controls ticket 03) ---------------------------

    void wiperFollowsHomeAssistantsState()
    {
        QCOMPARE(c->wiperState(), QStringLiteral("unknown"));
        connectAndSettle();
        QCOMPARE(c->wiperState(), QStringLiteral("off"));
        QVERIFY(!c->wiperOn());
        // Home Assistant's own 60 s auto-off (or another app) shows on the next read.
        c->refresh();
        settle(kPatrolStopped, kIrisAuto, wiperState("on"));
        QVERIFY(c->wiperOn());
        QCOMPARE(c->wiperState(), QStringLiteral("on"));
    }

    void wiperReadIsAGetOfTheState()
    {
        connectAndSettle();
        c->refresh();
        QCOMPARE(ht->count(kWiperState), 1);
        for (const auto &p : ht->pending) {
            if (ScriptedTransport::nameOf(p.url) == kWiperState) {
                QCOMPARE(p.url.toString(),
                         QStringLiteral("https://ha.test/api/states/pyscript.bosch_camera_wiper"));
                QVERIFY(p.body.isNull());
            }
        }
        settle();
    }

    void wiperMissingInHomeAssistantIsUnknown()
    {
        connectAndSettle();
        c->refresh();
        ht->answer(kWiperState, 404, R"({"message":"Entity not found."})");
        QCOMPARE(c->wiperState(), QStringLiteral("unknown"));
        QVERIFY(c->connected());
        settle();
    }

    void wiperToggleWaitsForTheCameraThenShowsIt()
    {
        connectAndSettle();
        QSignalSpy changed(c, &BoschTourController::wiperChanged);
        QVERIFY(c->toggleWiper());
        QVERIFY(c->wiperPending());
        QVERIFY(!c->wiperOn());   // not until Home Assistant says the camera took it
        QVERIFY(changed.count() >= 1);
        QCOMPARE(ht->count(kWiper), 1);
        QVERIFY(ht->pending.front().url.query().contains(QStringLiteral("return_response")));
        const QJsonObject sent = ht->answer(kWiper, 200, svc(R"({"ok":true,"state":"on"})"));
        QCOMPARE(sent.value(QStringLiteral("state")).toString(), QStringLiteral("on"));
        QVERIFY(!c->wiperPending());
        QVERIFY(c->wiperOn());
        QVERIFY(c->wiperError().isEmpty());
    }

    void wiperToggleOffWhenOn()
    {
        connectAndSettle();
        c->refresh();
        settle(kPatrolStopped, kIrisAuto, wiperState("on"));
        QVERIFY(c->toggleWiper());
        const QJsonObject sent = ht->answer(kWiper, 200, svc(R"({"ok":true,"state":"off"})"));
        QCOMPARE(sent.value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        QVERIFY(!c->wiperOn());
    }

    void wiperSecondPressWhilePendingSendsNothing()
    {
        connectAndSettle();
        QVERIFY(c->toggleWiper());
        QVERIFY(!c->toggleWiper());
        QCOMPARE(ht->count(kWiper), 1);
    }

    void wiperReadDuringAPressDoesNotFlipIt()
    {
        connectAndSettle();
        c->refresh();
        QVERIFY(c->toggleWiper());
        // A read sent before the press answers "off" while the order is out: the toggle waits.
        ht->answer(kWiperState, 200, wiperState("off"));
        QVERIFY(c->wiperPending());
        ht->answer(kWiper, 200, svc(R"({"ok":true,"state":"on"})"));
        QVERIFY(c->wiperOn());
        // An older read landing after the answer is dropped too.
        settle();
        QVERIFY(c->wiperOn());
    }

    void wiperCameraDidNotTakeItFlipsBackAndSaysSo()
    {
        connectAndSettle();
        QVERIFY(c->toggleWiper());
        ht->answer(kWiper, 200, svc(R"({"ok":false,"state":"off","error":"timed out"})"));
        QVERIFY(!c->wiperPending());
        QVERIFY(!c->wiperOn());
        QCOMPARE(c->wiperError(), QStringLiteral("Wiper didn't respond"));
        QVERIFY(c->message().contains(QStringLiteral("timed out")));
        QVERIFY(c->connected());
        // The next press starts clean.
        QVERIFY(c->toggleWiper());
        QVERIFY(c->wiperError().isEmpty());
    }

    void wiperErrorClearsWhenTheStateChanges()
    {
        connectAndSettle();
        QVERIFY(c->toggleWiper());
        ht->answer(kWiper, 200, svc(R"({"ok":false,"state":"off","error":"timed out"})"));
        c->refresh();
        settle();   // still off: the error stands
        QCOMPARE(c->wiperError(), QStringLiteral("Wiper didn't respond"));
        c->refresh();
        settle(kPatrolStopped, kIrisAuto, wiperState("on"));   // a phone turned it on
        QVERIFY(c->wiperOn());
        QVERIFY(c->wiperError().isEmpty());
    }

    void wiperServiceErrorFlipsBackAndSaysSo()
    {
        connectAndSettle();
        QVERIFY(c->toggleWiper());
        ht->answer(kWiper, 500, R"({"message":"Service call failed"})");
        QVERIFY(!c->wiperOn());
        QCOMPARE(c->wiperError(), QStringLiteral("Wiper didn't respond"));
    }

    void wiperRefusedWithoutHomeAssistant()
    {
        QVERIFY(!c->toggleWiper());
        QVERIFY(ht->pending.empty());
    }

    void wiperUnknownAfterDisconnect()
    {
        connectAndSettle();
        c->refresh();
        settle(kPatrolStopped, kIrisAuto, wiperState("on"));
        c->disconnectHomeAssistant();
        QCOMPARE(c->wiperState(), QStringLiteral("unknown"));
        QVERIFY(!c->wiperOn());
        QVERIFY(!c->wiperPending());
    }
};

QTEST_GUILESS_MAIN(TestBoschTour)
#include "test_boschtour.moc"
