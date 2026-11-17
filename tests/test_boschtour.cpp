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
    void settle(const QByteArray &patrol = kPatrolStopped, const QByteArray &iris = kIrisAuto)
    {
        const std::map<QString, QByteArray> reads = {
            {QStringLiteral("bosch_camera_get_patrol"), patrol},
            {QStringLiteral("bosch_camera_get_presets"), kPresets},
            {QStringLiteral("bosch_camera_get_slots"), kSlots},
            {QStringLiteral("bosch_camera_get_scene_mode"), kScene},
            {QStringLiteral("bosch_camera_get_iris"), iris},
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
        QCOMPARE(ht->count(QStringLiteral("ptz")), 0);
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        const QJsonObject move = ht->peekBody(QStringLiteral("ptz"));
        QCOMPARE(move.value("move_mode").toString(), QStringLiteral("ContinuousMove"));
        QCOMPARE(move.value("pan").toString(), QStringLiteral("LEFT"));
        QVERIFY(c->manualStopped());
    }

    void releasedBeforeTheStopLandsOnlyStops()
    {
        connectAndSettle(svc(R"({"status":"travelling","config":{}})"));
        c->moveCamera(QStringLiteral("LEFT"), QString(), QString());
        c->stopCamera();   // released
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        for (const auto &p : ht->pending)
            QVERIFY(QJsonDocument::fromJson(p.body).object().value("move_mode").toString()
                    != QStringLiteral("ContinuousMove"));
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

    void padPressInSelectionModeEndsItThenMoves()
    {
        connectAndSettle();
        QVERIFY(c->beginSelectionTour());
        c->toggleSelectionTourPosition(QStringLiteral("1"));   // one stop: nothing runs
        c->moveCamera(QStringLiteral("RIGHT"), QString(), QString());
        QVERIFY(!c->selectionTourMode());
        QCOMPARE(ht->count(QStringLiteral("ptz")), 0);   // after T's usual stop lands
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        QVERIFY(!c->selectionTourRestoring());
        QCOMPARE(ht->count(QStringLiteral("ptz")), 1);   // the move, no Stop racing it
        QCOMPARE(ht->peekBody(QStringLiteral("ptz")).value("pan").toString(), QStringLiteral("RIGHT"));
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
        QCOMPARE(ht->count(QStringLiteral("ptz")), 0);   // not before the route is restored
        ht->answer(QStringLiteral("bosch_camera_stop_patrol"), 200, "");
        ht->answer(QStringLiteral("bosch_camera_set_patrol"), 200, "{}");
        QVERIFY(!c->selectionTourRestoring());
        QCOMPARE(ht->peekBody(QStringLiteral("ptz")).value("pan").toString(), QStringLiteral("RIGHT"));
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
        QCOMPARE(ht->count(QStringLiteral("ptz")), 1); // camera movement stopped too
        ht->answer(QStringLiteral("ptz"), 200, "{}");
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
};

QTEST_GUILESS_MAIN(TestBoschTour)
#include "test_boschtour.moc"
