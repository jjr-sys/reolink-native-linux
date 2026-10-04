#include "BoschTourController.h"

#include "core/CredentialStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace rl {
namespace {

const QString kCredentialKey = QStringLiteral("homeassistant/bosch-tour");
const QString kCredentialLabel = QStringLiteral("Reolink Client Home Assistant token");
const int kPollIntervalMs = 1500;
const int kPresetPollIntervalMs = 10000;
const int kIrisHeartbeatIntervalMs = 700;

QJsonObject serviceResponse(const QByteArray &body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return {};
    return doc.object().value(QStringLiteral("service_response")).toObject();
}

QString responseMessage(const QByteArray &body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return {};
    const QJsonObject object = doc.object();
    return object.value(QStringLiteral("message")).toString(
        object.value(QStringLiteral("error")).toString());
}

} // namespace

BoschTourController::BoschTourController(CredentialStore *credentials, QObject *parent)
    : QObject(parent), m_credentials(credentials),
      m_network(new QNetworkAccessManager(this)), m_pollTimer(new QTimer(this)),
      m_presetTimer(new QTimer(this)), m_irisHeartbeatTimer(new QTimer(this))
{
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &BoschTourController::refreshPatrol);
    m_presetTimer->setInterval(kPresetPollIntervalMs);
    connect(m_presetTimer, &QTimer::timeout, this, &BoschTourController::refreshPresets);
    m_irisHeartbeatTimer->setInterval(kIrisHeartbeatIntervalMs);
    connect(m_irisHeartbeatTimer, &QTimer::timeout, this,
            [this] { sendIrisStart(true); });

    QSettings settings;
    const QString savedEndpoint = settings.value(QStringLiteral("boschTour/homeAssistantUrl")).toString();
    m_cameraEntity = settings.value(QStringLiteral("boschTour/cameraEntity"),
                                    QStringLiteral("camera.lascelles_driveway_h26x_l1s1")).toString();
    bool found = false;
    const QString savedToken = m_credentials
                                   ? m_credentials->lookupSecret(kCredentialKey, &found)
                                   : QString();
    if (!savedEndpoint.isEmpty() && found && !savedToken.isEmpty()) {
        m_endpoint = savedEndpoint;
        m_token = savedToken;
        setConnectionStatus(QStringLiteral("Connecting to Home Assistant…"), false);
        m_pollTimer->start();
        QTimer::singleShot(0, this, [this] {
            request(QUrl(m_endpoint + QStringLiteral("/api/")), m_token, {},
                    [this](int status, const QByteArray &body, const QString &error) {
                if (status == 200) {
                    setConnectionStatus(QStringLiteral("Connected"), true);
                    refresh();
                    refreshPresets();
                    m_presetTimer->start();
                } else {
                    operationFailed(QStringLiteral("Connect to Home Assistant"), status, body, error);
                }
            });
        });
    }
}

QString BoschTourController::normalizeEndpoint(const QString &raw, QString *error)
{
    QString value = raw.trimmed();
    if (value.isEmpty()) {
        if (error)
            *error = QStringLiteral("Enter the Home Assistant URL.");
        return {};
    }
    if (!value.contains(QStringLiteral("://")))
        value.prepend(QStringLiteral("https://"));

    QUrl url(value);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
        if (error)
            *error = QStringLiteral("Use a valid http:// or https:// Home Assistant URL.");
        return {};
    }
    if (!url.userInfo().isEmpty()) {
        if (error)
            *error = QStringLiteral("Do not put a username or password in the Home Assistant URL; use the bearer token field.");
        return {};
    }

    // Users often paste the HA dashboard URL. The API is rooted at the same
    // origin, so discard the Lovelace path, query, and fragment.
    url.setPath(QString());
    url.setQuery(QString());
    url.setFragment(QString());
    QString normalized = url.toString();
    if (normalized.endsWith(QLatin1Char('/')))
        normalized.chop(1);
    return normalized;
}

void BoschTourController::connectHomeAssistant(const QString &url, const QString &token,
                                               const QString &cameraEntity)
{
    if (m_selectionTourSession) {
        setMessage(QStringLiteral("Turn off tour selection and wait for Home Assistant to restore its saved route before changing connections."));
        return;
    }
    const bool resumePreviousConnection = m_connected;
    QString validationError;
    const QString endpoint = normalizeEndpoint(url, &validationError);
    const QString candidateToken = token.trimmed();
    const QString candidateEntity = cameraEntity.trimmed();
    if (endpoint.isEmpty()) {
        setMessage(validationError);
        return;
    }
    if (candidateToken.isEmpty()) {
        setMessage(QStringLiteral("Enter a Home Assistant bearer token."));
        return;
    }
    if (!candidateEntity.startsWith(QStringLiteral("camera."))) {
        setMessage(QStringLiteral("Enter the Bosch ONVIF camera entity ID, such as camera.lascelles_driveway_h26x_l1s1."));
        return;
    }

    stopIris();
    ++m_generation;
    m_sceneModeReadPending = false;
    m_sceneModePending = false;
    m_sceneModeAvailable = false;
    m_sceneModeStale = m_sceneModeId >= 0;
    m_sceneModeError.clear();
    emit sceneModeChanged();
    m_irisReadPending = false;
    m_irisPressId.clear();
    m_irisDirection.clear();
    m_irisPressed = false;
    m_irisStartPending = false;
    m_irisStopAfterStart = false;
    m_irisStopPending = false;
    m_irisHeartbeatPending = false;
    m_pollTimer->stop();
    m_presetTimer->stop();
    m_patrolRequestPending = false;
    m_presetsRequestPending = false;
    m_ptzRequestPending = false;
    setBusy(false);
    setMessage({});
    setConnectionStatus(QStringLiteral("Checking Home Assistant access…"), false);
    setBusy(true);
    // The Lascelles reverse proxy blocks Python's default user-agent but accepts
    // curl's. Keep this known-working request identity for the shared API client.
    request(QUrl(endpoint + QStringLiteral("/api/")), candidateToken, {},
            [this, endpoint, candidateToken, candidateEntity, resumePreviousConnection](
                int status, const QByteArray &body, const QString &networkError) {
        setBusy(false);
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (status != 200 || !doc.isObject()
            || doc.object().value(QStringLiteral("message")).toString()
                   != QLatin1String("API running.")) {
            operationFailed(QStringLiteral("Connect to Home Assistant"), status, body, networkError);
            if (resumePreviousConnection && configured()) {
                m_pollTimer->start();
                m_presetTimer->start();
                refresh();
            }
            return;
        }
        if (!m_credentials
            || !m_credentials->storeSecret(kCredentialKey, candidateToken, kCredentialLabel)) {
            setConnectionStatus(QStringLiteral("Could not save the token to the system keyring"), false);
            setMessage(m_credentials ? m_credentials->lastError()
                                     : QStringLiteral("The system keyring is unavailable."));
            if (resumePreviousConnection && configured()) {
                m_pollTimer->start();
                m_presetTimer->start();
                refresh();
            }
            return;
        }

        m_endpoint = endpoint;
        m_cameraEntity = candidateEntity;
        m_token = candidateToken;
        QSettings settings;
        settings.setValue(QStringLiteral("boschTour/homeAssistantUrl"), m_endpoint);
        settings.setValue(QStringLiteral("boschTour/cameraEntity"), m_cameraEntity);
        setConnectionStatus(QStringLiteral("Connected"), true);
        setMessage(QStringLiteral("Home Assistant connection saved."));
        m_pollTimer->start();
        m_presetTimer->start();
        refresh();
        refreshPresets();
    });
}

void BoschTourController::disconnectHomeAssistant()
{
    if (m_selectionTourSession) {
        setMessage(QStringLiteral("Turn off tour selection and wait for Home Assistant to restore its saved route before disconnecting."));
        return;
    }
    stopIris();
    ++m_generation;
    m_pollTimer->stop();
    m_presetTimer->stop();
    m_patrolRequestPending = false;
    m_presetsRequestPending = false;
    m_ptzRequestPending = false;
    setBusy(false);
    if (m_credentials)
        m_credentials->removeSecret(kCredentialKey);
    QSettings settings;
    settings.remove(QStringLiteral("boschTour/homeAssistantUrl"));
    m_endpoint.clear();
    m_token.clear();
    m_connected = false;
    m_connectionStatus = QStringLiteral("Not configured");
    m_tourState = QStringLiteral("unknown");
    m_currentPosition.clear();
    m_tourError.clear();
    m_route.clear();
    m_presets.clear();
    m_sceneModeReadPending = false;
    m_sceneModePending = false;
    m_sceneModeAvailable = false;
    m_sceneModeStale = m_sceneModeId >= 0;
    m_sceneModeError = m_sceneModeStale
                           ? QStringLiteral("Home Assistant is disconnected.")
                           : QString();
    m_irisAvailable = false;
    m_irisReadPending = false;
    m_irisStatus = QStringLiteral("Not read");
    m_irisPressId.clear();
    m_irisDirection.clear();
    m_irisPressed = false;
    m_irisStartPending = false;
    m_irisStopAfterStart = false;
    m_irisStopPending = false;
    m_irisHeartbeatPending = false;
    m_message.clear();
    emit connectionChanged();
    emit patrolChanged();
    emit routeChanged();
    emit presetsChanged();
    emit sceneModeChanged();
    emit irisChanged();
    emit messageChanged();
}

void BoschTourController::request(const QUrl &url, const QString &token,
                                  const QByteArray &body, ReplyHandler handler)
{
    const quint64 generation = m_generation;
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    req.setRawHeader("User-Agent", "curl/8.5.0");
    req.setRawHeader("Accept", "application/json");
    if (!body.isNull())
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = body.isNull() ? m_network->get(req) : m_network->post(req, body);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, handler = std::move(handler)]() mutable {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError
                                  ? QString()
                                  : reply->errorString();
        reply->deleteLater();
        if (generation == m_generation && handler)
            handler(status, data, error);
    });
}

void BoschTourController::callService(const QString &service, const QJsonObject &data,
                                      bool returnResponse, bool mutation,
                                      ReplyHandler handler)
{
    if (!configured()) {
        if (handler)
            handler(0, {}, QStringLiteral("Home Assistant is not configured."));
        return;
    }
    QUrl url(m_endpoint + QStringLiteral("/api/services/pyscript/") + service);
    if (returnResponse)
        url.setQuery(QStringLiteral("return_response"));
    if (mutation)
        setBusy(true);
    const QByteArray body = QJsonDocument(data).toJson(QJsonDocument::Compact);
    request(url, m_token, body, [this, mutation, handler = std::move(handler)](
                                    int status, const QByteArray &response,
                                    const QString &networkError) mutable {
        if (mutation)
            setBusy(false);
        if (handler)
            handler(status, response, networkError);
    });
}

void BoschTourController::refresh()
{
    refreshPatrol();
    refreshPresets();
    refreshSceneMode();
}

void BoschTourController::refreshSceneMode()
{
    if (!configured() || m_sceneModeReadPending || m_sceneModePending)
        return;
    m_sceneModeReadPending = true;
    callService(QStringLiteral("bosch_camera_get_scene_mode"), {}, true, false,
                [this](int status, const QByteArray &body, const QString &networkError) {
        m_sceneModeReadPending = false;
        const QJsonObject response = serviceResponse(body);
        if (status != 200 || !response.value(QStringLiteral("ok")).toBool(false)
            || !applySceneModeResponse(response)) {
            QString error = response.value(QStringLiteral("error")).toString();
            if (error.isEmpty())
                error = responseMessage(body);
            if (error.isEmpty())
                error = networkError;
            if (error.isEmpty())
                error = QStringLiteral("Home Assistant did not return the camera's Marked-range mode.");
            failSceneModeRequest(error, true);
            return;
        }
        setConnectionStatus(QStringLiteral("Connected"), true);
        setMessage({});
    });
}

void BoschTourController::setSceneMode(int modeId)
{
    if (!configured() || !m_sceneModeAvailable || m_sceneModePending)
        return;
    const bool modeExists = std::any_of(m_sceneModes.cbegin(), m_sceneModes.cend(),
                                        [modeId](const QVariant &value) {
        return value.toMap().value(QStringLiteral("id")).toInt() == modeId;
    });
    if (!modeExists)
        return;

    m_sceneModePending = true;
    m_sceneModeError.clear();
    emit sceneModeChanged();
    callService(QStringLiteral("bosch_camera_set_scene_mode"),
                {{QStringLiteral("mode_id"), modeId}}, true, false,
                [this, modeId](int status, const QByteArray &body,
                               const QString &networkError) {
        m_sceneModePending = false;
        const QJsonObject response = serviceResponse(body);
        if (status != 200 || !response.value(QStringLiteral("ok")).toBool(false)
            || response.value(QStringLiteral("mode_id")).toInt(-1) != modeId
            || !applySceneModeResponse(response)) {
            QString error = response.value(QStringLiteral("error")).toString();
            if (error.isEmpty())
                error = responseMessage(body);
            if (error.isEmpty())
                error = networkError;
            if (error.isEmpty())
                error = QStringLiteral("The camera did not confirm the selected Marked-range mode.");
            failSceneModeRequest(error, true);
            return;
        }
        setMessage({});
    });
}

bool BoschTourController::applySceneModeResponse(const QJsonObject &response)
{
    const QJsonArray choices = response.value(QStringLiteral("modes")).toArray();
    QVariantList modes;
    QString selectedName;
    const int selectedId = response.value(QStringLiteral("mode_id")).toInt(-1);
    for (const QJsonValue &value : choices) {
        const QJsonObject choice = value.toObject();
        const int id = choice.value(QStringLiteral("id")).toInt(-1);
        const QString name = choice.value(QStringLiteral("name")).toString().trimmed();
        if (id < 1 || name.isEmpty())
            continue;
        modes.append(QVariantMap{{QStringLiteral("id"), id},
                                 {QStringLiteral("name"), name}});
        if (id == selectedId)
            selectedName = name;
    }
    if (selectedName.isEmpty())
        return false;

    m_sceneModes = modes;
    m_sceneModeId = selectedId;
    m_sceneModeName = selectedName;
    m_sceneModeError.clear();
    m_sceneModeAvailable = true;
    m_sceneModeStale = false;
    emit sceneModeChanged();
    return true;
}

void BoschTourController::failSceneModeRequest(const QString &error, bool stale)
{
    m_sceneModeAvailable = false;
    m_sceneModeStale = stale && m_sceneModeId >= 0;
    m_sceneModeError = error;
    emit sceneModeChanged();
    setMessage(QStringLiteral("Marked range: ") + error);
}

void BoschTourController::refreshIris()
{
    if (!configured() || m_irisReadPending)
        return;
    m_irisReadPending = true;
    callService(QStringLiteral("bosch_camera_get_iris"), {}, true, false,
                [this](int status, const QByteArray &body, const QString &networkError) {
        m_irisReadPending = false;
        if (status != 200) {
            m_irisAvailable = false;
            m_irisStatus = QStringLiteral("Unavailable");
            emit irisChanged();
            if (m_connected)
                operationFailed(QStringLiteral("Read Bosch iris state"), status, body, networkError);
            return;
        }
        const QJsonObject response = serviceResponse(body);
        applyIrisResponse(response);
    });
}

void BoschTourController::applyIrisResponse(const QJsonObject &response)
{
    m_irisAvailable = response.value(QStringLiteral("supported")).toBool(false);
    m_irisError = response.value(QStringLiteral("error")).toString();
    if (m_irisAvailable) {
        const QString mode = response.value(QStringLiteral("mode")).toString();
        const QJsonValue value = response.value(QStringLiteral("iris"));
        m_irisStatus = mode;
        if (value.isDouble())
            m_irisStatus += QStringLiteral(" · %1").arg(value.toDouble(), 0, 'f', 1);
        if (mode == QLatin1String("MANUAL")) {
            const QJsonValue gain = response.value(QStringLiteral("gain"));
            const QJsonValue exposureTime = response.value(QStringLiteral("exposure_time_us"));
            if (gain.isDouble())
                m_irisStatus += QStringLiteral(" · G %1 dB").arg(gain.toDouble(), 0, 'f', 1);
            if (exposureTime.isDouble())
                m_irisStatus += QStringLiteral(" · T %1 ms")
                                    .arg(exposureTime.toDouble() / 1000.0, 0, 'f', 2);
        }
    } else {
        m_irisStatus = m_irisError.isEmpty() ? QStringLiteral("Unavailable") : m_irisError;
    }
    emit irisChanged();
}

bool BoschTourController::startIris(const QString &direction)
{
    const QString normalized = direction.trimmed().toLower();
    if (!configured() || !m_connected) {
        setMessage(QStringLiteral("Connect Home Assistant before controlling the Bosch iris."));
        return false;
    }
    if (!m_irisAvailable) {
        setMessage(QStringLiteral("Bosch iris control is unavailable. Refresh Home Assistant iris state first."));
        refreshIris();
        return false;
    }
    if (normalized != QLatin1String("open") && normalized != QLatin1String("close"))
        return false;
    if (m_irisPressed || !m_irisPressId.isEmpty())
        return false;

    m_irisDirection = normalized;
    m_irisPressId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_irisPressed = true;
    m_irisStartPending = true;
    m_irisStopAfterStart = false;
    m_irisHeartbeatPending = false;
    m_irisHeartbeatTimer->start();
    sendIrisStart(false);
    return true;
}

void BoschTourController::sendIrisStart(bool heartbeat)
{
    if (m_irisPressId.isEmpty() || (heartbeat && (!m_irisPressed || m_irisHeartbeatPending)))
        return;
    if (heartbeat)
        m_irisHeartbeatPending = true;

    const QString pressId = m_irisPressId;
    const QJsonObject data{{QStringLiteral("direction"), m_irisDirection},
                           {QStringLiteral("press_id"), pressId}};
    callService(QStringLiteral("bosch_camera_iris_start"), data, true, false,
                [this, pressId, heartbeat](int status, const QByteArray &body,
                                           const QString &networkError) {
        if (heartbeat)
            m_irisHeartbeatPending = false;
        else
            m_irisStartPending = false;
        if (pressId != m_irisPressId)
            return;

        if (status != 200) {
            if (m_irisPressed) {
                m_irisPressed = false;
                m_irisHeartbeatTimer->stop();
                setMessage(QStringLiteral("Start Bosch iris adjustment failed: %1")
                               .arg(networkError.isEmpty() ? responseMessage(body) : networkError));
                sendIrisStop(pressId);
            }
        } else {
            const QJsonObject result = serviceResponse(body);
            const QJsonObject state = result.value(QStringLiteral("status")).toObject();
            if (!state.isEmpty())
                applyIrisResponse(state);
            if (!result.value(QStringLiteral("ok")).toBool(false)) {
                m_irisPressed = false;
                m_irisHeartbeatTimer->stop();
                const QString error = result.value(QStringLiteral("error")).toString();
                if (!error.isEmpty())
                    setMessage(QStringLiteral("Bosch iris adjustment failed: %1").arg(error));
                sendIrisStop(pressId);
            } else if (!result.value(QStringLiteral("active")).toBool(false)) {
                m_irisPressed = false;
                m_irisHeartbeatTimer->stop();
                if (!m_irisStopAfterStart)
                    setMessage(QStringLiteral("The iris is already at that limit."));
            }
        }

        if (m_irisStopAfterStart) {
            m_irisStopAfterStart = false;
            sendIrisStop(pressId);
        }
    });
}

void BoschTourController::stopIris()
{
    if (m_irisPressId.isEmpty()) {
        m_irisHeartbeatTimer->stop();
        m_irisPressed = false;
        return;
    }
    const QString pressId = m_irisPressId;
    m_irisPressed = false;
    m_irisHeartbeatTimer->stop();
    if (m_irisStartPending) {
        m_irisStopAfterStart = true;
        return;
    }
    sendIrisStop(pressId);
}

void BoschTourController::sendIrisStop(const QString &pressId)
{
    if (pressId.isEmpty() || (m_irisStopPending && pressId == m_irisPressId))
        return;
    m_irisStopPending = true;
    const QJsonObject data{{QStringLiteral("press_id"), pressId}};
    callService(QStringLiteral("bosch_camera_iris_stop"), data, true, false,
                [this, pressId](int status, const QByteArray &body,
                               const QString &networkError) {
        m_irisStopPending = false;
        if (pressId != m_irisPressId)
            return;
        if (status == 200) {
            const QJsonObject result = serviceResponse(body);
            const QJsonObject state = result.value(QStringLiteral("status")).toObject();
            if (!state.isEmpty())
                applyIrisResponse(state);
            if (!result.value(QStringLiteral("ok")).toBool(false)) {
                const QString error = result.value(QStringLiteral("error")).toString();
                if (!error.isEmpty())
                    setMessage(QStringLiteral("Stop Bosch iris adjustment failed: %1").arg(error));
            }
        } else {
            setMessage(QStringLiteral("Stop Bosch iris adjustment failed: %1")
                           .arg(networkError.isEmpty() ? responseMessage(body) : networkError));
        }
        m_irisPressId.clear();
        m_irisDirection.clear();
        m_irisStartPending = false;
        m_irisStopAfterStart = false;
        m_irisStopPending = false;
        m_irisHeartbeatPending = false;
        refreshIris();
    });
}

void BoschTourController::setIrisAuto()
{
    if (!configured() || !m_connected) {
        setMessage(QStringLiteral("Connect Home Assistant before changing Bosch exposure mode."));
        return;
    }
    if (m_irisPressed || !m_irisPressId.isEmpty()) {
        setMessage(QStringLiteral("Release the iris button before switching to Auto."));
        return;
    }
    callService(QStringLiteral("bosch_camera_set_iris_mode"),
                {{QStringLiteral("mode"), QStringLiteral("AUTO")}}, true, false,
                [this](int status, const QByteArray &body, const QString &networkError) {
        if (status != 200) {
            setMessage(QStringLiteral("Set Bosch Auto exposure failed: %1")
                           .arg(networkError.isEmpty() ? responseMessage(body) : networkError));
            return;
        }
        const QJsonObject response = serviceResponse(body);
        if (!response.value(QStringLiteral("ok")).toBool(false)) {
            const QString error = response.value(QStringLiteral("error")).toString();
            setMessage(error.isEmpty() ? QStringLiteral("Set Bosch Auto exposure failed.") : error);
            refreshIris();
            return;
        }
        applyIrisResponse(response);
        setMessage(QStringLiteral("Bosch automatic exposure restored."));
    });
}

void BoschTourController::refreshPatrol()
{
    if (!configured() || m_patrolRequestPending)
        return;
    m_patrolRequestPending = true;
    const quint64 requestRevision = m_patrolMutationRevision;
    callService(QStringLiteral("bosch_camera_get_patrol"), {}, true, false,
                [this, requestRevision](int status, const QByteArray &body,
                                        const QString &networkError) {
        m_patrolRequestPending = false;
        if (requestRevision != m_patrolMutationRevision) {
            refreshPatrol();
            return;
        }
        if (status != 200) {
            operationFailed(QStringLiteral("Read Bosch tour state"), status, body, networkError);
            return;
        }
        const QJsonObject response = serviceResponse(body);
        if (response.isEmpty()) {
            operationFailed(QStringLiteral("Read Bosch tour state"), status, body,
                            QStringLiteral("Home Assistant returned no patrol data."));
            return;
        }
        setConnectionStatus(QStringLiteral("Connected"), true);
        applyPatrolResponse(response);
    });
}

void BoschTourController::notePatrolMutation()
{
    ++m_patrolMutationRevision;
}

void BoschTourController::setLocalTourState(const QString &state)
{
    notePatrolMutation();
    if (m_tourState == state)
        return;
    m_tourState = state;
    emit patrolChanged();
}

void BoschTourController::applyPatrolResponse(const QJsonObject &response)
{
    const QJsonObject config = response.value(QStringLiteral("config")).toObject();
    const QJsonArray routeArray = config.value(QStringLiteral("route")).toArray();
    QVariantList newRoute;
    for (const QJsonValue &value : routeArray) {
        const QJsonObject item = value.toObject();
        QVariantMap row;
        const QVariant token = item.value(QStringLiteral("token")).toVariant();
        const QString storedLabel = item.value(QStringLiteral("label")).toString();
        const QString positionName = presetNameForToken(token.toString());
        row.insert(QStringLiteral("token"), token);
        row.insert(QStringLiteral("label"), positionName.isEmpty() ? storedLabel : positionName);
        if (!row.value(QStringLiteral("label")).toString().isEmpty())
            newRoute.append(row);
    }

    const int speed = config.value(QStringLiteral("travel_speed")).toInt(m_travelSpeed);
    const int dwell = config.value(QStringLiteral("dwell_seconds")).toInt(m_dwellSeconds);
    const QString state = response.value(QStringLiteral("status")).toString(QStringLiteral("unknown"));
    QString position = response.value(QStringLiteral("current_position")).toString();
    for (const QVariant &value : m_presets) {
        const QVariantMap preset = value.toMap();
        if (preset.value(QStringLiteral("nativeName")).toString() == position) {
            position = preset.value(QStringLiteral("name")).toString();
            break;
        }
    }
    const QString error = response.value(QStringLiteral("error")).toString();
    const bool routeWasChanged = newRoute != m_route;
    const bool patrolWasChanged = state != m_tourState || position != m_currentPosition
                                  || error != m_tourError || speed != m_travelSpeed
                                  || dwell != m_dwellSeconds;
    m_route = newRoute;
    m_travelSpeed = std::clamp(speed, 10, 100);
    m_dwellSeconds = std::clamp(dwell, 0, 3600);
    m_tourState = state;
    m_currentPosition = position;
    m_tourError = error;
    if (routeWasChanged)
        emit routeChanged();
    if (patrolWasChanged || routeWasChanged)
        emit patrolChanged();
}

void BoschTourController::refreshPresets()
{
    if (!configured() || m_presetsRequestPending)
        return;
    m_presetsRequestPending = true;
    callService(QStringLiteral("bosch_camera_get_presets"), {}, true, false,
                [this](int status, const QByteArray &body, const QString &networkError) {
        if (status != 200) {
            m_presetsRequestPending = false;
            operationFailed(QStringLiteral("Read Bosch presets"), status, body, networkError);
            return;
        }
        const QJsonObject response = serviceResponse(body);
        const QJsonArray list = response.value(QStringLiteral("presets")).toArray();
        QVariantList cameraPresets;
        QHash<QString, QVariantMap> presetsByToken;
        for (const QJsonValue &value : list) {
            const QJsonObject preset = value.toObject();
            QVariantMap row;
            const QString name = preset.value(QStringLiteral("name")).toString();
            row.insert(QStringLiteral("name"), name);
            row.insert(QStringLiteral("nativeName"), name);
            row.insert(QStringLiteral("token"), preset.value(QStringLiteral("token")).toVariant());
            row.insert(QStringLiteral("pan"), preset.value(QStringLiteral("pan")).toDouble());
            row.insert(QStringLiteral("tilt"), preset.value(QStringLiteral("tilt")).toDouble());
            row.insert(QStringLiteral("zoom"), preset.value(QStringLiteral("zoom")).toDouble());
            cameraPresets.append(row);
            presetsByToken.insert(row.value(QStringLiteral("token")).toString(), row);
        }

        // The HA dashboard names its buttons through saved slots. The ONVIF
        // preset names are often only "Position 1", "Position 2", etc.; join
        // the slot labels to those presets by token and keep HA's slot order.
        callService(QStringLiteral("bosch_camera_get_slots"), {}, true, false,
                    [this, cameraPresets, presetsByToken](int slotsStatus,
                                                           const QByteArray &slotsBody,
                                                           const QString &slotsError) {
            m_presetsRequestPending = false;
            QVariantList namedPositions;
            QString slotWarning;
            if (slotsStatus != 200) {
                namedPositions = cameraPresets;
                QString detail = responseMessage(slotsBody);
                if (detail.isEmpty())
                    detail = slotsError.isEmpty()
                                 ? QStringLiteral("HTTP %1").arg(slotsStatus)
                                 : slotsError;
                slotWarning = QStringLiteral("Could not read HA position names: %1").arg(detail);
            } else {
                const QJsonArray slotList = serviceResponse(slotsBody)
                                                .value(QStringLiteral("slots")).toArray();
                QSet<QString> includedTokens;
                for (const QJsonValue &value : slotList) {
                    if (!value.isObject())
                        continue;
                    const QJsonObject slot = value.toObject();
                    const QString token = slot.value(QStringLiteral("token")).toVariant().toString();
                    const QString label = slot.value(QStringLiteral("label")).toString().trimmed();
                    if (token.isEmpty() || label.isEmpty() || includedTokens.contains(token))
                        continue;

                    QVariantMap position = presetsByToken.value(token);
                    if (position.isEmpty())
                        position.insert(QStringLiteral("nativeName"), QString());
                    position.insert(QStringLiteral("token"), slot.value(QStringLiteral("token")).toVariant());
                    position.insert(QStringLiteral("name"), label);
                    namedPositions.append(position);
                    includedTokens.insert(token);
                }
                if (namedPositions.isEmpty() && !cameraPresets.isEmpty())
                    slotWarning = QStringLiteral("Home Assistant returned no named camera positions.");
            }

            const bool presetsWereChanged = namedPositions != m_presets;
            m_presets = namedPositions;
            const bool routeWasChanged = applyPresetNamesToRoute();
            if (presetsWereChanged)
                emit presetsChanged();
            if (routeWasChanged)
                emit routeChanged();
            setConnectionStatus(QStringLiteral("Connected"), true);
            if (!slotWarning.isEmpty())
                setMessage(slotWarning);
        });
    });
}

QString BoschTourController::presetNameForToken(const QString &token) const
{
    for (const QVariant &value : m_presets) {
        const QVariantMap preset = value.toMap();
        if (preset.value(QStringLiteral("token")).toString() == token)
            return preset.value(QStringLiteral("name")).toString();
    }
    return {};
}

bool BoschTourController::applyPresetNamesToRoute()
{
    bool changed = false;
    QVariantList namedRoute = m_route;
    for (QVariant &value : namedRoute) {
        QVariantMap row = value.toMap();
        const QString name = presetNameForToken(row.value(QStringLiteral("token")).toString());
        if (name.isEmpty() || row.value(QStringLiteral("label")).toString() == name)
            continue;
        row.insert(QStringLiteral("label"), name);
        value = row;
        changed = true;
    }
    if (changed)
        m_route = namedRoute;
    return changed;
}

void BoschTourController::savePreset(const QString &name, const QString &token)
{
    if (m_tourState != QLatin1String("stopped")) {
        setMessage(QStringLiteral("Stop the tour before saving a camera position."));
        return;
    }
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty()) {
        setMessage(QStringLiteral("Enter a preset name."));
        return;
    }
    QJsonObject data{{QStringLiteral("name"), trimmedName}};
    if (!token.trimmed().isEmpty())
        data.insert(QStringLiteral("token"), token.trimmed());
    callService(QStringLiteral("bosch_camera_set_preset"), data, true, true,
                [this](int status, const QByteArray &body, const QString &networkError) {
        if (status != 200) {
            operationFailed(QStringLiteral("Save Bosch preset"), status, body, networkError);
            return;
        }
        setMessage(QStringLiteral("Preset saved."));
        refreshPresets();
    });
}

void BoschTourController::removePreset(const QString &token)
{
    if (m_tourState != QLatin1String("stopped")) {
        setMessage(QStringLiteral("Stop the tour before deleting a camera position."));
        return;
    }
    const QString key = token.trimmed();
    if (key.isEmpty())
        return;
    for (const QVariant &value : m_route) {
        if (value.toMap().value(QStringLiteral("token")).toString() == key) {
            setMessage(QStringLiteral("Remove this position from the tour before deleting it."));
            return;
        }
    }
    callService(QStringLiteral("bosch_camera_remove_preset"),
                QJsonObject{{QStringLiteral("token"), key}}, false, true,
                [this](int status, const QByteArray &body, const QString &networkError) {
        if (status < 200 || status >= 300) {
            operationFailed(QStringLiteral("Delete Bosch preset"), status, body, networkError);
            return;
        }
        setMessage(QStringLiteral("Preset deleted."));
        refreshPresets();
    });
}

void BoschTourController::gotoPreset(const QString &token)
{
    const QString key = token.trimmed();
    if (key.isEmpty())
        return;
    if (m_selectionTourSession && !m_selectionTourMode) {
        m_pendingSelectionTourPreset = key;
        setMessage(QStringLiteral("Position queued until the tour stops and its saved route is restored."));
        return;
    }
    if (m_tourState != QLatin1String("stopped")) {
        setMessage(QStringLiteral("Stop the tour before moving the camera to a preset."));
        return;
    }
    const QString positionName = presetNameForToken(key);
    setMessage(positionName.isEmpty()
                   ? QStringLiteral("Sending camera to saved position…")
                   : QStringLiteral("Moving camera to %1…").arg(positionName));
    callService(QStringLiteral("bosch_camera_goto_preset"),
                QJsonObject{{QStringLiteral("token"), key}}, false, true,
                [this](int status, const QByteArray &body, const QString &networkError) {
        if (status < 200 || status >= 300) {
            operationFailed(QStringLiteral("Move to Bosch preset"), status, body, networkError);
            return;
        }
        setMessage(QStringLiteral("Camera moving to preset."));
        refreshPatrol();
    });
}

void BoschTourController::moveCamera(const QString &pan, const QString &tilt,
                                     const QString &zoom)
{
    if (m_tourState != QLatin1String("stopped")) {
        setMessage(QStringLiteral("Stop the tour before moving the camera."));
        return;
    }
    if (m_ptzRequestPending || (pan.isEmpty() && tilt.isEmpty() && zoom.isEmpty()))
        return;

    QJsonObject data{
        {QStringLiteral("entity_id"), m_cameraEntity},
        {QStringLiteral("move_mode"), QStringLiteral("ContinuousMove")},
        {QStringLiteral("speed"), 0.35},
        // Home Assistant treats duration 0 as a held move; the stream's PTZ
        // pad sends Stop as soon as the pointer is released.
        {QStringLiteral("continuous_duration"), 0.0},
    };
    if (!pan.isEmpty())
        data.insert(QStringLiteral("pan"), pan);
    if (!tilt.isEmpty())
        data.insert(QStringLiteral("tilt"), tilt);
    if (!zoom.isEmpty())
        data.insert(QStringLiteral("zoom"), zoom);
    m_ptzRequestPending = true;
    callOnvif(data, QStringLiteral("Move Bosch camera"), true);
}

void BoschTourController::stopCamera()
{
    callOnvif(QJsonObject{{QStringLiteral("entity_id"), m_cameraEntity},
                          {QStringLiteral("move_mode"), QStringLiteral("Stop")}},
             QStringLiteral("Stop Bosch camera movement"), false);
}

void BoschTourController::callOnvif(const QJsonObject &data, const QString &action,
                                    bool movement)
{
    if (!configured() || m_cameraEntity.isEmpty()) {
        m_ptzRequestPending = false;
        setMessage(QStringLiteral("Configure the Home Assistant camera entity first."));
        return;
    }
    const QUrl url(m_endpoint + QStringLiteral("/api/services/onvif/ptz"));
    const QByteArray body = QJsonDocument(data).toJson(QJsonDocument::Compact);
    request(url, m_token, body, [this, action, movement](int status, const QByteArray &response,
                                                         const QString &networkError) {
        if (movement)
            m_ptzRequestPending = false;
        if (status < 200 || status >= 300) {
            operationFailed(action, status, response, networkError);
            return;
        }
        if (!movement)
            setMessage(QStringLiteral("Camera movement stopped."));
    });
}

void BoschTourController::saveTour(const QVariantList &route, int speed, int dwell)
{
    if (m_tourState != QLatin1String("stopped")) {
        setMessage(QStringLiteral("Stop the tour before changing its sequence or settings."));
        emit tourSaved(false);
        return;
    }
    QJsonArray sequence;
    QSet<QString> labels;
    QSet<QString> availablePresets;
    for (const QVariant &preset : m_presets)
        availablePresets.insert(preset.toMap().value(QStringLiteral("token")).toString());
    for (const QVariant &value : route) {
        const QVariantMap row = value.toMap();
        const QString token = row.value(QStringLiteral("token")).toString().trimmed();
        const QString label = row.value(QStringLiteral("label")).toString().trimmed();
        if (token.isEmpty() || label.isEmpty()) {
            setMessage(QStringLiteral("Every tour step needs a saved preset and a name."));
            emit tourSaved(false);
            return;
        }
        if (!availablePresets.contains(token)) {
            setMessage(QStringLiteral("Refresh positions and replace any tour step whose preset no longer exists."));
            emit tourSaved(false);
            return;
        }
        const QString labelKey = label.toCaseFolded();
        if (labels.contains(labelKey)) {
            setMessage(QStringLiteral("Tour position names must be unique so HA can report the current step."));
            emit tourSaved(false);
            return;
        }
        labels.insert(labelKey);
        sequence.append(QJsonObject{{QStringLiteral("token"), token},
                                    {QStringLiteral("label"), label}});
    }
    const QJsonObject data{
        {QStringLiteral("route"), sequence},
        {QStringLiteral("travel_speed"), std::clamp(speed, 10, 100)},
        {QStringLiteral("dwell_seconds"), std::clamp(dwell, 0, 3600)},
    };
    callService(QStringLiteral("bosch_camera_set_patrol"), data, true, true,
                [this](int status, const QByteArray &body, const QString &networkError) {
        if (status != 200) {
            operationFailed(QStringLiteral("Save Bosch tour"), status, body, networkError);
            emit tourSaved(false);
            return;
        }
        notePatrolMutation();
        setMessage(QStringLiteral("Tour settings saved in Home Assistant."));
        emit tourSaved(true);
        refreshPatrol();
    });
}

void BoschTourController::startTour()
{
    if (m_tourState != QLatin1String("stopped")) {
        setMessage(QStringLiteral("The tour must be stopped before starting."));
        return;
    }
    if (m_route.isEmpty()) {
        setMessage(QStringLiteral("Add at least one position to the tour first."));
        return;
    }
    callService(QStringLiteral("bosch_camera_start_patrol"), {}, true, true,
                [this](int status, const QByteArray &body, const QString &networkError) {
        if (status != 200) {
            operationFailed(QStringLiteral("Start Bosch tour"), status, body, networkError);
            return;
        }
        const QJsonObject result = serviceResponse(body);
        setLocalTourState(result.value(QStringLiteral("status")).toString(QStringLiteral("starting")));
        setMessage(QStringLiteral("Tour started."));
        refreshPatrol();
    });
}

void BoschTourController::stopTour()
{
    callService(QStringLiteral("bosch_camera_stop_patrol"), {}, false, true,
                [this](int status, const QByteArray &body, const QString &networkError) {
        if (status < 200 || status >= 300) {
            operationFailed(QStringLiteral("Stop Bosch tour"), status, body, networkError);
            return;
        }
        setLocalTourState(QStringLiteral("stopped"));
        setMessage(QStringLiteral("Tour stopped."));
        refreshPatrol();
    });
}

bool BoschTourController::beginSelectionTour()
{
    if (m_selectionTourMode)
        return true;
    if (m_selectionTourSession) {
        reconcileSelectionTour();
        setMessage(QStringLiteral("Finishing the previous tour stop and restoring its saved Home Assistant route."));
        return false;
    }
    if (!connected() || !configured()) {
        setMessage(QStringLiteral("Connect to Home Assistant before selecting a tour."));
        return false;
    }
    if (busy() || touring() || m_tourState != QLatin1String("stopped")) {
        setMessage(QStringLiteral("Stop the Home Assistant tour and wait for it to finish before selecting a route."));
        return false;
    }

    m_selectionTourSession = true;
    m_selectionTourMode = true;
    m_selectionTourStarted = false;
    m_selectionTourConfigOverridden = false;
    m_selectionTourOperationPending = false;
    m_selectionTourOriginalRoute = m_route;
    m_selectionTourConfiguredRoute = m_route;
    m_selectionTourRequestedRoute.clear();
    m_selectionTourOriginalSpeed = m_travelSpeed;
    m_selectionTourOriginalDwell = m_dwellSeconds;
    emit selectionTourModeChanged();
    emit selectionTourRouteChanged();
    setMessage(QStringLiteral("Tour selection is on. Select at least two positions to start."));
    return true;
}

void BoschTourController::toggleSelectionTourPosition(const QString &rawToken)
{
    if (!m_selectionTourMode || !m_selectionTourSession)
        return;

    const QString token = rawToken.trimmed();
    const QString label = presetNameForToken(token);
    if (token.isEmpty() || label.isEmpty()) {
        setMessage(QStringLiteral("This Home Assistant position is no longer available. Refresh positions and select again."));
        return;
    }

    QVariantList nextRoute = m_selectionTourRequestedRoute;
    qsizetype selectedIndex = -1;
    for (qsizetype i = 0; i < nextRoute.size(); ++i) {
        if (nextRoute.at(i).toMap().value(QStringLiteral("token")).toString() == token) {
            selectedIndex = i;
            break;
        }
    }
    if (selectedIndex >= 0) {
        nextRoute.removeAt(selectedIndex);
    } else {
        for (const QVariant &value : nextRoute) {
            if (value.toMap().value(QStringLiteral("label")).toString().compare(
                    label, Qt::CaseInsensitive) == 0) {
                setMessage(QStringLiteral("Selected position names must be unique for Home Assistant to report the current tour step."));
                return;
            }
        }
        nextRoute.append(QVariantMap{{QStringLiteral("token"), token},
                                     {QStringLiteral("label"), label}});
    }

    if (nextRoute == m_selectionTourRequestedRoute)
        return;
    m_selectionTourRequestedRoute = nextRoute;
    emit selectionTourRouteChanged();

    // Reconcile the last accepted route after the active patrol has stopped.
    // This includes tours started by Home Assistant while selection mode is on.
    if (nextRoute.size() >= 2 || m_selectionTourStarted || touring()
        || m_selectionTourConfigOverridden)
        m_selectionTourStopRequested = true;
    reconcileSelectionTour();
}

void BoschTourController::endSelectionTour()
{
    if (m_selectionTourMode) {
        m_selectionTourMode = false;
        m_selectionTourRequestedRoute.clear();
        m_selectionTourStopRequested = true;
        emit selectionTourModeChanged();
        emit selectionTourRouteChanged();
        emit selectionTourRestoringChanged();
        setMessage(QStringLiteral("Stopping the tour and restoring the Home Assistant route…"));
        // Stop a manual goto as well as an HA patrol so turning T off always
        // stops camera motion immediately from the user's point of view.
        stopCamera();
    }
    reconcileSelectionTour();
}

void BoschTourController::reconcileSelectionTour()
{
    if (!m_selectionTourSession || m_selectionTourOperationPending || !configured())
        return;

    const bool shouldRun = m_selectionTourMode && m_selectionTourRequestedRoute.size() >= 2;
    const QVariantList targetRoute = shouldRun ? m_selectionTourRequestedRoute
                                                : m_selectionTourOriginalRoute;
    const bool routeChanged = targetRoute != m_selectionTourConfiguredRoute;

    if (m_selectionTourStarted && (!shouldRun || routeChanged))
        m_selectionTourStopRequested = true;

    if (m_selectionTourStopRequested) {
        m_selectionTourOperationPending = true;
        // stop_patrol does not return response data. Asking Home Assistant for
        // return_response produces HTTP 400 and leaves the running patrol alone.
        callService(QStringLiteral("bosch_camera_stop_patrol"), {}, false, true,
                    [this](int status, const QByteArray &body, const QString &networkError) {
            m_selectionTourOperationPending = false;
            if (status < 200 || status >= 300) {
                operationFailed(QStringLiteral("Stop selected Bosch tour"), status, body, networkError);
                return;
            }
            m_selectionTourStarted = false;
            m_selectionTourStopRequested = false;
            setLocalTourState(QStringLiteral("stopped"));
            refreshPatrol();
            reconcileSelectionTour();
        });
        return;
    }

    if (routeChanged) {
        QJsonArray sequence;
        for (const QVariant &value : targetRoute) {
            const QVariantMap row = value.toMap();
            sequence.append(QJsonObject{{QStringLiteral("token"), row.value(QStringLiteral("token")).toString()},
                                        {QStringLiteral("label"), row.value(QStringLiteral("label")).toString()}});
        }
        const QJsonObject data{
            {QStringLiteral("route"), sequence},
            {QStringLiteral("travel_speed"), m_selectionTourOriginalSpeed},
            {QStringLiteral("dwell_seconds"), m_selectionTourOriginalDwell},
        };
        m_selectionTourOperationPending = true;
        callService(QStringLiteral("bosch_camera_set_patrol"), data, true, true,
                    [this, targetRoute](int status, const QByteArray &body,
                                        const QString &networkError) {
            m_selectionTourOperationPending = false;
            if (status != 200) {
                operationFailed(QStringLiteral("Update selected Bosch tour"), status, body, networkError);
                return;
            }
            notePatrolMutation();
            m_selectionTourConfiguredRoute = targetRoute;
            m_selectionTourConfigOverridden = m_selectionTourConfiguredRoute
                                              != m_selectionTourOriginalRoute;
            refreshPatrol();
            reconcileSelectionTour();
        });
        return;
    }

    if (shouldRun && !m_selectionTourStarted) {
        m_selectionTourOperationPending = true;
        callService(QStringLiteral("bosch_camera_start_patrol"), {}, true, true,
                    [this](int status, const QByteArray &body, const QString &networkError) {
            m_selectionTourOperationPending = false;
            if (status != 200) {
                operationFailed(QStringLiteral("Start selected Bosch tour"), status, body, networkError);
                return;
            }
            const QJsonObject result = serviceResponse(body);
            setLocalTourState(result.value(QStringLiteral("status")).toString(QStringLiteral("starting")));
            m_selectionTourStarted = true;
            setMessage(QStringLiteral("Selected Bosch tour started."));
            refreshPatrol();
            reconcileSelectionTour();
        });
        return;
    }

    if (!m_selectionTourMode && !m_selectionTourStarted && !m_selectionTourConfigOverridden)
        finishSelectionTourSession();
}

void BoschTourController::finishSelectionTourSession()
{
    const QString pendingPreset = m_pendingSelectionTourPreset;
    m_pendingSelectionTourPreset.clear();
    m_selectionTourSession = false;
    m_selectionTourStarted = false;
    m_selectionTourConfigOverridden = false;
    m_selectionTourOperationPending = false;
    m_selectionTourOriginalRoute.clear();
    m_selectionTourConfiguredRoute.clear();
    m_selectionTourRequestedRoute.clear();
    m_selectionTourStopRequested = false;
    emit selectionTourRouteChanged();
    emit selectionTourRestoringChanged();
    if (!pendingPreset.isEmpty())
        gotoPreset(pendingPreset);
}

QString BoschTourController::currentStep() const
{
    if (m_currentPosition.isEmpty())
        return {};
    for (qsizetype i = 0; i < m_route.size(); ++i) {
        if (m_route.at(i).toMap().value(QStringLiteral("label")).toString()
            == m_currentPosition)
            return QStringLiteral("%1 / %2").arg(i + 1).arg(m_route.size());
    }
    return {};
}

bool BoschTourController::touring() const
{
    return m_tourState == QLatin1String("starting")
           || m_tourState == QLatin1String("travelling")
           || m_tourState == QLatin1String("dwelling");
}

void BoschTourController::setMessage(const QString &message)
{
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

void BoschTourController::setConnectionStatus(const QString &status, bool connected)
{
    if (m_connectionStatus == status && m_connected == connected)
        return;
    m_connectionStatus = status;
    m_connected = connected;
    emit connectionChanged();
}

void BoschTourController::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void BoschTourController::operationFailed(const QString &action, int status,
                                          const QByteArray &body,
                                          const QString &networkError)
{
    QString detail = responseMessage(body);
    if (status == 401 || status == 403) {
        m_pollTimer->stop();
        m_presetTimer->stop();
        m_irisHeartbeatTimer->stop();
        m_irisHeartbeatPending = false;
        detail = QStringLiteral("Home Assistant rejected the saved token (HTTP %1). Update the token in Bosch tour settings.")
                     .arg(status);
    }
    if (detail.isEmpty())
        detail = networkError;
    if (detail.isEmpty())
        detail = status > 0 ? QStringLiteral("HTTP %1").arg(status)
                            : QStringLiteral("request failed");
    setConnectionStatus(detail, false);
    setMessage(action + QStringLiteral(": ") + detail);
}

} // namespace rl
