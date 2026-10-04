#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QVariantList>

#include <functional>

class QNetworkAccessManager;
class QTimer;

namespace rl {

class CredentialStore;

// Home Assistant is the shared authority for the Lascelles Bosch positions and
// patrol. This client calls its existing pyscript services and HA's ONVIF PTZ
// action; it never sends camera protocol commands directly from the fork.
class BoschTourController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool configured READ configured NOTIFY connectionChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString endpoint READ endpoint NOTIFY connectionChanged)
    Q_PROPERTY(QString cameraEntity READ cameraEntity NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionStatus READ connectionStatus NOTIFY connectionChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QString tourState READ tourState NOTIFY patrolChanged)
    Q_PROPERTY(QString currentPosition READ currentPosition NOTIFY patrolChanged)
    Q_PROPERTY(QString tourError READ tourError NOTIFY patrolChanged)
    Q_PROPERTY(QString currentStep READ currentStep NOTIFY patrolChanged)
    Q_PROPERTY(int travelSpeed READ travelSpeed NOTIFY patrolChanged)
    Q_PROPERTY(int dwellSeconds READ dwellSeconds NOTIFY patrolChanged)
    Q_PROPERTY(QVariantList route READ route NOTIFY routeChanged)
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    Q_PROPERTY(QVariantList sceneModes READ sceneModes NOTIFY sceneModeChanged)
    Q_PROPERTY(int sceneModeId READ sceneModeId NOTIFY sceneModeChanged)
    Q_PROPERTY(QString sceneModeName READ sceneModeName NOTIFY sceneModeChanged)
    Q_PROPERTY(QString sceneModeError READ sceneModeError NOTIFY sceneModeChanged)
    Q_PROPERTY(bool sceneModeAvailable READ sceneModeAvailable NOTIFY sceneModeChanged)
    Q_PROPERTY(bool sceneModePending READ sceneModePending NOTIFY sceneModeChanged)
    Q_PROPERTY(bool sceneModeStale READ sceneModeStale NOTIFY sceneModeChanged)
    Q_PROPERTY(QString irisStatus READ irisStatus NOTIFY irisChanged)
    Q_PROPERTY(bool irisAvailable READ irisAvailable NOTIFY irisChanged)
    Q_PROPERTY(QVariantList selectionTourRoute READ selectionTourRoute NOTIFY selectionTourRouteChanged)
    Q_PROPERTY(bool touring READ touring NOTIFY patrolChanged)
    Q_PROPERTY(bool selectionTourMode READ selectionTourMode NOTIFY selectionTourModeChanged)
    Q_PROPERTY(bool selectionTourRestoring READ selectionTourRestoring NOTIFY selectionTourRestoringChanged)

public:
    explicit BoschTourController(CredentialStore *credentials, QObject *parent = nullptr);

    bool configured() const { return !m_endpoint.isEmpty() && !m_token.isEmpty(); }
    bool connected() const { return m_connected; }
    bool busy() const { return m_busy; }
    QString endpoint() const { return m_endpoint; }
    QString cameraEntity() const { return m_cameraEntity; }
    QString connectionStatus() const { return m_connectionStatus; }
    QString message() const { return m_message; }
    QString tourState() const { return m_tourState; }
    QString currentPosition() const { return m_currentPosition; }
    QString tourError() const { return m_tourError; }
    QString currentStep() const;
    int travelSpeed() const { return m_travelSpeed; }
    int dwellSeconds() const { return m_dwellSeconds; }
    QVariantList route() const { return m_route; }
    QVariantList presets() const { return m_presets; }
    QVariantList sceneModes() const { return m_sceneModes; }
    int sceneModeId() const { return m_sceneModeId; }
    QString sceneModeName() const { return m_sceneModeName; }
    QString sceneModeError() const { return m_sceneModeError; }
    bool sceneModeAvailable() const { return m_sceneModeAvailable; }
    bool sceneModePending() const { return m_sceneModePending; }
    bool sceneModeStale() const { return m_sceneModeStale; }
    QString irisStatus() const { return m_irisStatus; }
    bool irisAvailable() const { return m_irisAvailable; }
    QVariantList selectionTourRoute() const { return m_selectionTourRequestedRoute; }
    bool touring() const;
    bool selectionTourMode() const { return m_selectionTourMode; }
    bool selectionTourRestoring() const { return m_selectionTourSession && !m_selectionTourMode; }

    Q_INVOKABLE void connectHomeAssistant(const QString &url, const QString &token,
                                          const QString &cameraEntity);
    Q_INVOKABLE void disconnectHomeAssistant();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void refreshPresets();
    Q_INVOKABLE void refreshSceneMode();
    Q_INVOKABLE void setSceneMode(int modeId);
    Q_INVOKABLE void refreshIris();
    Q_INVOKABLE void savePreset(const QString &name, const QString &token = QString());
    Q_INVOKABLE void removePreset(const QString &token);
    Q_INVOKABLE void gotoPreset(const QString &token);
    Q_INVOKABLE void moveCamera(const QString &pan, const QString &tilt, const QString &zoom);
    Q_INVOKABLE void stopCamera();
    Q_INVOKABLE bool startIris(const QString &direction);
    Q_INVOKABLE void stopIris();
    Q_INVOKABLE void setIrisAuto();
    Q_INVOKABLE void saveTour(const QVariantList &route, int travelSpeed, int dwellSeconds);
    Q_INVOKABLE void startTour();
    Q_INVOKABLE void stopTour();
    Q_INVOKABLE bool beginSelectionTour();
    Q_INVOKABLE void toggleSelectionTourPosition(const QString &token);
    Q_INVOKABLE void endSelectionTour();

signals:
    void connectionChanged();
    void busyChanged();
    void messageChanged();
    void patrolChanged();
    void routeChanged();
    void presetsChanged();
    void sceneModeChanged();
    void irisChanged();
    void tourSaved(bool ok);
    void selectionTourModeChanged();
    void selectionTourRouteChanged();
    void selectionTourRestoringChanged();

private:
    using ReplyHandler = std::function<void(int, const QByteArray &, const QString &)>;

    static QString normalizeEndpoint(const QString &url, QString *error);
    void request(const QUrl &url, const QString &token, const QByteArray &body,
                 ReplyHandler handler);
    void callService(const QString &service, const QJsonObject &data, bool returnResponse,
                     bool mutation, ReplyHandler handler);
    void callOnvif(const QJsonObject &data, const QString &action, bool movement);
    void refreshPatrol();
    void notePatrolMutation();
    void setLocalTourState(const QString &state);
    void sendIrisStart(bool heartbeat);
    void sendIrisStop(const QString &pressId);
    void applyIrisResponse(const QJsonObject &response);
    bool applySceneModeResponse(const QJsonObject &response);
    void failSceneModeRequest(const QString &error, bool stale);
    void applyPatrolResponse(const QJsonObject &response);
    QString presetNameForToken(const QString &token) const;
    bool applyPresetNamesToRoute();
    void reconcileSelectionTour();
    void finishSelectionTourSession();
    void setMessage(const QString &message);
    void setConnectionStatus(const QString &status, bool connected);
    void setBusy(bool busy);
    void operationFailed(const QString &action, int status, const QByteArray &body,
                         const QString &networkError);

    CredentialStore *m_credentials = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_presetTimer = nullptr;
    QTimer *m_irisHeartbeatTimer = nullptr;
    QString m_endpoint;
    QString m_cameraEntity;
    QString m_token;
    QString m_connectionStatus = QStringLiteral("Not configured");
    QString m_message;
    QString m_tourState = QStringLiteral("unknown");
    QString m_currentPosition;
    QString m_tourError;
    int m_travelSpeed = 40;
    int m_dwellSeconds = 1;
    QVariantList m_route;
    QVariantList m_presets;
    QVariantList m_sceneModes;
    int m_sceneModeId = -1;
    QString m_sceneModeName;
    QString m_sceneModeError;
    bool m_sceneModeAvailable = false;
    bool m_sceneModePending = false;
    bool m_sceneModeStale = false;
    bool m_sceneModeReadPending = false;
    QString m_irisStatus = QStringLiteral("Not read");
    QString m_irisPressId;
    QString m_irisDirection;
    QString m_irisError;
    bool m_connected = false;
    bool m_busy = false;
    bool m_patrolRequestPending = false;
    bool m_presetsRequestPending = false;
    bool m_irisAvailable = false;
    bool m_irisReadPending = false;
    bool m_irisPressed = false;
    bool m_irisStartPending = false;
    bool m_irisStopAfterStart = false;
    bool m_irisStopPending = false;
    bool m_irisHeartbeatPending = false;
    bool m_ptzRequestPending = false;
    bool m_selectionTourMode = false;
    bool m_selectionTourSession = false;
    bool m_selectionTourStarted = false;
    bool m_selectionTourConfigOverridden = false;
    bool m_selectionTourOperationPending = false;
    bool m_selectionTourStopRequested = false;
    quint64 m_generation = 0;
    quint64 m_patrolMutationRevision = 0;
    QVariantList m_selectionTourOriginalRoute;
    QVariantList m_selectionTourConfiguredRoute;
    QVariantList m_selectionTourRequestedRoute;
    int m_selectionTourOriginalSpeed = 40;
    int m_selectionTourOriginalDwell = 1;
    QString m_pendingSelectionTourPreset;
};

} // namespace rl
