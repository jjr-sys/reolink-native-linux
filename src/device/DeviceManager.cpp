#include <QElapsedTimer>
#include "DeviceManager.h"
#include "media/BufferReplay.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "device/EventSnapshot.h"
#include "device/FisheyeDetect.h"
#include "device/FrigateApi.h"
#include "device/FrigateLive.h"
#include "device/QuickReply.h"
#include "media/StreamPlayer.h"
#include "protocol/BaichuanChannels.h"
#include "protocol/BaichuanClient.h"
#include "protocol/BcLiveFramePacer.h"
#include "protocol/BaichuanControl.h"
#include "protocol/BaichuanEventListener.h"
#include "protocol/BcAlarmEvents.h"
#include "protocol/ReolinkHttpClient.h"

#include <QDateTime>
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QUrl>
#include <QVariant>
#include <QThread>
#include <QTimeZone>
#include <QtConcurrent/QtConcurrent>

namespace rl {

namespace {
// Per-camera manual view-rotation fix (issue #3), keyed by identity rather
// than row because rows shift when devices come and go.
QString rotationKey(qint64 hostId, int channel)
{
    return QStringLiteral("rotation/%1:%2").arg(hostId).arg(channel);
}
int loadRotation(qint64 hostId, int channel)
{
    return QSettings().value(rotationKey(hostId, channel), 0).toInt();
}
QString lensKey(qint64 hostId, int channel)
{
    return QStringLiteral("lens/%1:%2").arg(hostId).arg(channel);
}
QString viewModeKey(qint64 hostId, int channel)
{
    return QStringLiteral("viewMode/%1:%2").arg(hostId).arg(channel);
}
// Frigate names its cameras like config keys ("bosch_camera"); show those as a person
// would write them. The raw key stays in the entry, since Frigate's URLs need it.
// Adds the kind's capabilities (direct, multiChannel, frigateApi, ...) to a QML map.
void mergeCaps(QVariantMap &m, const QString &kind)
{
    const QVariantMap caps = device::capabilityMap(device::kindFromString(kind));
    for (auto it = caps.constBegin(); it != caps.constEnd(); ++it)
        m.insert(it.key(), it.value());
}
QString displayName(const QString &name, const QString &kind)
{
    return device::capabilities(device::kindFromString(kind)).frigateApi ? frigate::displayCameraName(name) : name;
}
// The lens remembered for a channel (-1 none, 0 flat, 1 fisheye), from Database::channelFisheye.
int rememberedLens(const QHash<int, bool> &lenses, int channel)
{
    return lenses.contains(channel) ? int(lenses.value(channel)) : -1;
}
const QStringList kViewModes{"fisheye", "ptz", "panorama", "dual", "quad", "fisheye_ptz"};
// Load a camera's saved fisheye settings into a device entry.
template <class Entry>
void loadLensSettings(Entry &e, qint64 hostId, int channel)
{
    QSettings s;
    if (s.contains(lensKey(hostId, channel)))
        e.lensOverride = s.value(lensKey(hostId, channel)).toBool() ? 1 : 0;
    const QString mode = s.value(viewModeKey(hostId, channel)).toString();
    if (kViewModes.contains(mode))
        e.viewMode = mode;
}
} // namespace



DeviceManager::DeviceManager(Database *db, CredentialStore *credentials, QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_credentials(credentials),
      m_eventSwitches(new EventSwitches(db, this))
{
    // A camera's day of recordings: Frigate's own API, else its Reolink recorder or camera
    // over HTTP; none while the device isn't ready.
    m_days = new RecorderDays([this](const QString &key) -> DaySearch {
        const int row = rowOfCameraKey(key);
        if (row < 0)
            return {};
        const Entry &e = m_entries.at(row);
        if (e.kindCaps().frigateApi)   // not before its login is loaded (ticket 07)
            return e.frigateReady() ? frigateDaySearch(e.rec.addr, e.rec.port, e.serverName())
                                    : DaySearch{};
        const auto client = clientFor(row);
        return client ? reolinkDaySearch(client, e.channel) : DaySearch{};
    }, this);
    // Pages show search results only on screen; log them so an empty page can be told apart
    // from a failed search afterwards.
    auto searchLabel = [this](const QString &key) {
        const int row = rowOfCameraKey(key);
        if (row < 0)
            return key;
        const Entry &e = m_entries.at(row);
        return QStringLiteral("%1 / %2 (ch %3)").arg(e.rec.name, e.chanName).arg(e.channel);
    };
    connect(m_days, &RecorderDays::dayFound, this,
            [searchLabel](const QString &key, int y, int m, int d, const QVariantList &segments) {
                qCInfo(lcCore).noquote() << "recording search" << searchLabel(key)
                                         << QDate(y, m, d).toString(Qt::ISODate) << ":"
                                         << segments.size() << "recordings";
            });
    connect(m_days, &RecorderDays::dayNotReady, this,
            [searchLabel](const QString &key, int y, int m, int d) {
                qCInfo(lcCore).noquote() << "recording search" << searchLabel(key)
                                         << QDate(y, m, d).toString(Qt::ISODate)
                                         << ": not ready yet";
            });
    connect(m_days, &RecorderDays::dayFailed, this,
            [searchLabel](const QString &key, int y, int m, int d, const QString &error) {
                qCWarning(lcCore).noquote() << "recording search" << searchLabel(key)
                                            << QDate(y, m, d).toString(Qt::ISODate)
                                            << "failed:" << error;
            });
    const QVector<HostRecord> stored = m_db->hosts();
    m_entries.reserve(stored.size());
    for (const HostRecord &rec : stored) {
        Entry e;
        e.rec = rec;
        e.chanName = rec.name;
        e.rotationOverride = loadRotation(rec.id, 0);
        loadLensSettings(e, rec.id, 0);
        e.lensRemembered = rememberedLens(m_db->channelFisheye(rec.id), 0);
        e.online = device::capabilities(device::kindFromString(rec.kind)).alwaysOnline;
        e.status = e.online ? tr("ready") : tr("connecting…");
        m_entries.append(e);
    }
    // Prime credentials + refresh status for stored camera/NVR devices on startup
    // (also enumerates NVR channels and loads passwords into memory).
    QSet<qint64> validated;
    for (const Entry &e : m_entries)
        if (!validated.contains(e.rec.id)) {
            validated.insert(e.rec.id);
            validateAsync(e.rec.id);
        }

    // 10s interval also delays the first poll to ~10s, so it doesn't compete with
    // the startup validation burst for the NVR's limited connection slots.
    m_pollTimer.setInterval(10000);
    connect(&m_pollTimer, &QTimer::timeout, this, &DeviceManager::pollDetections);
    m_pollTimer.start();
}

DeviceManager::~DeviceManager()
{
    delete m_days;   // its searches stop first; nothing below waits on them
    m_days = nullptr;
    for (qint64 hostId : m_pushListeners.keys())
        stopPushListener(hostId);
    qDeleteAll(m_frigateLive);
    m_frigateLive.clear();
    // A worker mid-read notices within 0.3 s; one stuck in a login is left to the OS.
    BaichuanEventListener::waitForWorkers(1500);
    FrigateLive::waitForWorkers(500);
    m_pending.waitForFinished();
}

void DeviceManager::applyConnectivity(qint64 hostId, bool transportOk,
                                      const QHash<int, bool> &chanOnline)
{
    // Host-level: three consecutive failed poll cycles (~30s) = unreachable;
    // one success after that = recovered. Fires exactly once per transition.
    QString hostName;
    for (const Entry &e : m_entries)
        if (e.rec.id == hostId) { hostName = e.rec.name; break; }

    using PollAction = device::HostSession::PollAction;
    const PollAction action = m_sessions.of(hostId).onPoll(transportOk);
    if (!transportOk) {
        // At the third failure, and every ~minute while the host is in API-only state (in
        // case the whole device has since gone), ask the Baichuan port whether it is alive.
        if (action == PollAction::ProbeAlive)
            probeHostAlive(hostId, hostName);
        return;
    }
    if (action == PollAction::ApiBack) {
        // The API is back; the cameras never went offline, so just clear the label.
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.rec.id == hostId && e.online) {
                e.status = tr("online");
                emit dataChanged(index(i), index(i));
            }
        }
    } else if (action == PollAction::Recovered) {
        // Cameras this same poll reports down stay down (the channel loop below would
        // only flip them back, filing a false online/offline pair).
        QVector<device::CameraRef> changed;
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.rec.id != hostId || e.online || !chanOnline.value(e.channel, true))
                continue;
            e.online = true;
            e.status = tr("online");
            changed.append({e.channel, displayName(e.chanName, e.rec.kind)});
            emit dataChanged(index(i), index(i));
        }
        emit connectivityChanged(hostId, -1, hostName, true, changed);
    }

    // Channel-level (NVRs): flip individual cameras that dropped or returned.
    for (auto it = chanOnline.constBegin(); it != chanOnline.constEnd(); ++it) {
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.rec.id != hostId || e.channel != it.key())
                continue;
            if (e.online != it.value()) {
                e.online = it.value();
                e.status = it.value() ? tr("online") : tr("offline");
                emit dataChanged(index(i), index(i));
                emit connectivityChanged(hostId, e.channel, displayName(e.chanName, e.rec.kind),
                                         it.value(), {});
            }
            break;
        }
    }
}

void DeviceManager::markHostUnreachable(qint64 hostId, const QString &hostName)
{
    QVector<device::CameraRef> changed;
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.rec.id != hostId || !e.online)
            continue;
        e.online = false;
        e.status = tr("unreachable");
        changed.append({e.channel, displayName(e.chanName, e.rec.kind)});
        emit dataChanged(index(i), index(i));
    }
    emit connectivityChanged(hostId, -1, hostName, false, changed);
}

// The HTTP API failed several polls in a row. NVRs can wedge that API while still serving
// video and Baichuan, so check the Baichuan port before calling the cameras unreachable.
void DeviceManager::probeHostAlive(qint64 hostId, const QString &hostName)
{
    if (!m_sessions.of(hostId).beginProbe())
        return;
    const int row = rowForHostId(hostId);
    if (row < 0) {
        m_sessions.of(hostId).probeFinished(true); // nothing to probe; just release the flag
        return;
    }
    const HostRecord rec = m_entries.at(row).rec;
    const QString password = m_entries.at(row).password;
    const BaichuanOpener opener = m_baichuan;

    m_pending.addFuture(QtConcurrent::run([this, hostId, hostName, rec, password, opener] {
        bool alive = false;
        if (!password.isEmpty()) {
            BaichuanParams p;
            p.host = rec.addr;
            p.port = 9000;
            p.username = rec.username;
            p.password = password;
            const std::unique_ptr<BaichuanLink> link = opener(p);
            alive = link->open();
            link->close();
        }
        QMetaObject::invokeMethod(
            this,
            [this, hostId, hostName, alive] {
                using ProbeAction = device::HostSession::ProbeAction;
                switch (m_sessions.of(hostId).probeFinished(alive)) {
                case ProbeAction::Ignore:
                    return;
                case ProbeAction::MarkUnreachable:
                    markHostUnreachable(hostId, hostName);
                    return;
                case ProbeAction::EnterApiOnly:
                    break;
                }
                qCInfo(lcCore) << hostName << "HTTP API not responding but device is alive"
                               << "(Baichuan); keeping cameras online";
                for (int i = 0; i < m_entries.size(); ++i) {
                    Entry &e = m_entries[i];
                    if (e.rec.id == hostId && e.online) {
                        e.status = tr("limited: device API not responding");
                        emit dataChanged(index(i), index(i));
                    }
                }
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::pollDetections()
{
    if (!m_pushWarmed)
        warmPushCache();

    // Recovery probes ride the poll tick: hosts that failed with a TRANSPORT
    // problem revalidate when their backoff expires, so an NVR that was off at
    // app launch appears by itself once it's back — previously a host with no
    // client was skipped by everything and stayed "connecting…" until restart.
    for (qint64 hostId : m_sessions.takeDueRetries(QDateTime::currentDateTimeUtc()))
        validateAsync(hostId);

    pollFrigate();

    // Poll each HOST with a SINGLE batched request covering all its online
    // channels — Reolink NVRs are connection-limited, so opening one connection
    // per channel (5+ at once) starves live/playback streams. One connection per
    // host per cycle keeps capacity free.
    struct HostPoll {
        std::shared_ptr<ReolinkHttpClient> client;
        QVector<int> channels;
        QVector<bool> wantAi;
        QVector<QString> names;
        bool pushLive = false;     // detections come by push; the poll reports none
        bool queryStates = true;   // ask for motion/AI state (not when push covers an NVR)
    };
    QHash<qint64, HostPoll> byHost;
    QVector<qint64> order;
    for (const Entry &e : m_entries) {
        // Offline entries stay in the poll: that's how we notice recovery.
        if (e.kindCaps().direct() || !e.client)
            continue;
        if (!byHost.contains(e.rec.id)) {
            byHost[e.rec.id] = HostPoll{e.client, {}, {}, {}};
            order.append(e.rec.id);
        }
        HostPoll &hp = byHost[e.rec.id];
        hp.channels.append(e.channel);
        hp.wantAi.append(e.caps.ai);
        hp.names.append(e.chanName);
    }

    for (const Entry &e : m_entries)
        if (byHost.contains(e.rec.id))
            ensurePushListener(e);

    for (qint64 hostId : order) {
        if (!m_sessions.of(hostId).beginPoll())
            continue;
        device::HostDetections &det = m_detections[hostId];
        byHost[hostId].pushLive = det.pushLive();
        byHost[hostId].queryStates = det.pollQueriesStates(byHost[hostId].channels.size());
        const HostPoll hp = byHost.value(hostId);

        m_pending.addFuture(QtConcurrent::run([this, hostId, hp] {
            // Build one ordered batch; remember what each entry maps to.
            struct Item { int channel; bool isAi; };
            QVector<Item> items;
            Json cmds = Json::array();
            // Pushed detections replace the state queries; a multi-channel host still
            // answers GetChannelstatus below, which keeps connectivity current.
            const bool statesByPush = !hp.queryStates;
            for (int i = 0; i < hp.channels.size() && !statesByPush; ++i) {
                const int ch = hp.channels[i];
                items.append({ch, false});
                cmds.push_back(api::command(QStringLiteral("GetMdState"), Json{{"channel", ch}}));
                if (hp.wantAi[i]) {
                    items.append({ch, true});
                    cmds.push_back(
                        api::command(QStringLiteral("GetAiState"), Json{{"channel", ch}}));
                }
            }
            // One channel-status query per NVR cycle: notices cameras that
            // drop or return while the app is running.
            const bool wantStatus = hp.channels.size() > 1;
            if (wantStatus)
                cmds.push_back(api::command(QStringLiteral("GetChannelstatus"), Json::object()));
            const api::BatchResult batch = hp.client->call(cmds);

            QHash<int, bool> chanOnline; // parsed channel -> online (NVRs only)
            if (batch.transportOk && wantStatus && batch.results.size() == items.size() + 1) {
                const api::CommandResult &cs = batch.results.last();
                if (cs.ok && cs.value.contains("status") && cs.value["status"].is_array())
                    for (const auto &c : cs.value["status"]) {
                        if (c.contains("channel"))
                            chanOnline[c["channel"].get<int>()] =
                                c.value("online", 1) != 0;
                    }
            }

            // Combine per-channel md/ai (results preserve request order).
            QHash<int, api::DetectionState> states;
            if (batch.transportOk) {
                for (int i = 0; i < batch.results.size() && i < items.size(); ++i) {
                    const api::CommandResult &r = batch.results[i];
                    if (!r.ok)
                        continue;
                    api::DetectionState &st = states[items[i].channel];
                    if (items[i].isAi) {
                        const api::DetectionState ai = api::parseAiState(r.value);
                        st.person = ai.person;
                        st.vehicle = ai.vehicle;
                        st.pet = ai.pet;
                    } else {
                        st.motion = api::parseMdState(r.value);
                    }
                }
            }

            const bool transportOk = batch.transportOk;
            QMetaObject::invokeMethod(
                this,
                [this, hostId, hp, states, transportOk, chanOnline] {
                    m_sessions.of(hostId).endPoll();
                    applyConnectivity(hostId, transportOk, chanOnline);
                    if (!transportOk)
                        return;
                    // The module drops the poll's report once push holds the host.
                    device::HostDetections &det = m_detections[hostId];
                    for (int i = 0; i < hp.channels.size(); ++i)
                        emitDetections(hostId,
                                       det.onPoll(hp.channels[i], states.value(hp.channels[i]),
                                                  hp.names[i], hp.pushLive),
                                       false);
                },
                Qt::QueuedConnection);
        }));
    }
}

void DeviceManager::emitDetections(qint64 hostId, const QVector<device::Detection> &found,
                                   bool frigateHost)
{
    for (const device::Detection &d : found)
        reportDetection(hostId, d.channel, d.type,
                        frigateHost ? displayName(d.camera, device::kindToString(
                                                                device::Kind::Frigate))
                                    : d.camera,
                        d.at);
}

void DeviceManager::reportDetection(qint64 hostId, int channel, const QString &type,
                                    const QString &camera, qint64 at)
{
    if (m_eventSwitches->isOff(hostId, channel, type))
        return;
    emit detectionEvent(hostId, channel, type, camera, at);
}

void DeviceManager::ensurePushListener(const Entry &e)
{
    // RL_NO_PUSH=1 keeps to the poll (scratch runs that shouldn't hold NVR sessions).
    static const bool disabled = qEnvironmentVariableIsSet("RL_NO_PUSH");
    if (disabled || !e.reolinkReady() || e.password.isEmpty())
        return;
    if (BaichuanEventListener *l = m_pushListeners.value(e.rec.id)) {
        const BaichuanControl::Params &p = l->params();
        if (p.host == e.rec.addr && p.username == e.rec.username && p.password == e.password)
            return;
        stopPushListener(e.rec.id); // the host's address or credentials changed
    }
    BaichuanControl::Params p;
    p.host = e.rec.addr;
    p.port = 9000;
    p.username = e.rec.username;
    p.password = e.password;
    const qint64 hostId = e.rec.id;
    auto *l = new BaichuanEventListener(p, this);
    connect(l, &BaichuanEventListener::subscribedChanged, this, [this, hostId](bool on) {
        qCInfo(lcProto) << "push detections" << (on ? "on" : "off") << "for host" << hostId;
        m_detections[hostId].setPushLive(on);
    });
    connect(l, &BaichuanEventListener::alarmEvents, this,
            [this, hostId](const QByteArray &xml) { onAlarmEvents(hostId, xml); });
    connect(l, &BaichuanEventListener::refused, this, [hostId] {
        qCWarning(lcProto) << "push listener login refused for host" << hostId
                            << "- retrying in 10 minutes";
    });
    m_pushListeners.insert(hostId, l);
}

void DeviceManager::stopPushListener(qint64 hostId)
{
    delete m_pushListeners.take(hostId);
    m_detections[hostId].setPushLive(false);
}

void DeviceManager::onAlarmEvents(qint64 hostId, const QByteArray &xml)
{
    for (const bc::ChannelAlarm &a : bc::parseAlarmEvents(xml))
        for (const Entry &e : std::as_const(m_entries))
            if (e.rec.id == hostId && e.channel == a.channel) {
                emitDetections(hostId, m_detections[hostId].onPush(a.channel, a.state, e.chanName),
                               false);
                break;
            }
}

int DeviceManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant DeviceManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole:
        return displayName(e.chanName.isEmpty() ? e.rec.name : e.chanName, e.rec.kind);
    case AddrRole:
        return e.rec.addr;
    case KindRole:
        return e.rec.kind;
    case MultiChannelRole:
        return e.kindCaps().multiChannel;
    case ModelRole:
        return e.rec.model;
    case OnlineRole:
        return e.online;
    case StatusRole:
        return e.status;
    case ProblemRole:
        switch (e.problem) {
        case Problem::Connecting: return QStringLiteral("connecting");
        case Problem::Unreachable: return QStringLiteral("unreachable");
        case Problem::Auth: return QStringLiteral("auth");
        case Problem::Locked: return QStringLiteral("locked");
        case Problem::None: break;
        }
        return QString();
    case HostIdRole:
        return e.rec.id;
    case ChannelRole:
        return e.channel;
    case RotationRole:
        return e.rotationOverride;
    case IsFisheyeRole:
        return device::isFisheye(e.lensOverride, e.mainSize, e.lensRemembered);
    case ViewModeRole:
        return e.viewMode;
    case HasPtzRole:
        return e.caps.ptz;
    case HasPtzPresetRole:
        return e.caps.ptzPreset;
    case HasZoomRole:
        return e.caps.zoom;
    case HasAudioRole:
        return e.caps.audio;
    case HasSirenRole:
        return e.caps.siren;
    case HasFloodlightRole:
        return e.caps.floodlight;
    case HasBatteryRole:
        return e.caps.battery;
    case HasTalkRole:
        return e.talk;
    case IsAdminRole:
        return e.isAdmin;
    case BatteryPercentRole:
        return e.battery.present ? e.battery.percent : -1;
    case BatteryChargingRole:
        return e.battery.charging;
    case HostNameRole:
        return e.rec.name;
    }
    return {};
}

QHash<int, QByteArray> DeviceManager::roleNames() const
{
    return {
        {NameRole, "name"},
        {AddrRole, "addr"},
        {KindRole, "kind"},
        {MultiChannelRole, "multiChannel"},
        {ModelRole, "model"},
        {OnlineRole, "online"},
        {StatusRole, "status"},
        {HostIdRole, "hostId"},
        {ChannelRole, "channel"},
        {HasPtzRole, "hasPtz"},
        {HasPtzPresetRole, "hasPtzPreset"},
        {HasZoomRole, "hasZoom"},
        {HasAudioRole, "hasAudio"},
        {HasSirenRole, "hasSiren"},
        {HasFloodlightRole, "hasFloodlight"},
        {HasBatteryRole, "hasBattery"},
        {HasTalkRole, "hasTalk"},
        {IsAdminRole, "isAdmin"},
        {BatteryPercentRole, "batteryPercent"},
        {BatteryChargingRole, "batteryCharging"},
        {ProblemRole, "problem"},
        {RotationRole, "rotationOverride"},
        {IsFisheyeRole, "isFisheye"},
        {ViewModeRole, "viewMode"},
        {HostNameRole, "hostName"},
    };
}

void DeviceManager::testDevice(const QString &addr, const QString &username,
                               const QString &password, bool https, int port)
{
    const int portv = port > 0 ? port : (https ? 443 : 80);
    m_pending.addFuture(QtConcurrent::run([this, addr, username, password, https, portv] {
        ReolinkHttpClient client(addr, portv, https, username, password);
        const api::BatchResult b =
            client.call(Json::array({api::command(QStringLiteral("GetDevInfo"))}));

        bool ok = false;
        QString name, model, message;
        if (b.transportOk && !b.results.isEmpty() && b.results.first().ok) {
            const Json info = jsonObj(b.results.first().value, "DevInfo");
            name = QString::fromStdString(jsonStr(info, "name"));
            model = QString::fromStdString(jsonStr(info, "model"));
            ok = true;
        } else if (!b.error.isEmpty()) {
            message = b.error;
        } else if (b.transportOk && !b.results.isEmpty()) {
            message = b.results.first().detail;
        }
        if (!ok && message.isEmpty())
            message = tr("the device did not answer like a Reolink camera or NVR");

        QString problem;
        switch (client.lastFailKind()) {
        case ReolinkHttpClient::FailKind::Transport: problem = QStringLiteral("transport"); break;
        case ReolinkHttpClient::FailKind::Auth: problem = QStringLiteral("auth"); break;
        case ReolinkHttpClient::FailKind::Locked: problem = QStringLiteral("locked"); break;
        case ReolinkHttpClient::FailKind::Protocol: problem = QStringLiteral("protocol"); break;
        case ReolinkHttpClient::FailKind::None: break;
        }
        if (!ok && problem.isEmpty())
            problem = QStringLiteral("protocol");

        QMetaObject::invokeMethod(
            this,
            [this, ok, message, name, model, problem] {
                emit testDeviceResult(ok, message, name, model, problem);
            },
            Qt::QueuedConnection);
    }));
}

namespace {
// The restream login is kept in the keyring as "user\npassword" (a user name has no newline).
QString restreamKey(qint64 hostId) { return QStringLiteral("restream-login/%1").arg(hostId); }
QString packRestream(const QString &user, const QString &password) { return user + QLatin1Char('\n') + password; }
void unpackRestream(const QString &packed, QString *user, QString *password)
{
    const int nl = packed.indexOf(QLatin1Char('\n'));
    if (nl <= 0) {
        user->clear();
        password->clear();
        return;
    }
    *user = packed.left(nl);
    *password = packed.mid(nl + 1);
}
} // namespace

void DeviceManager::reconnect(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const qint64 hostId = m_entries.at(row).rec.id;
    // Reset the backoff — this is the user saying "try NOW".
    m_sessions.of(hostId).resetBackoff();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].rec.id != hostId)
            continue;
        m_entries[i].status = tr("connecting…");
        m_entries[i].problem = Problem::Connecting;
        emit dataChanged(index(i), index(i));
    }
    validateAsync(hostId);
}

void DeviceManager::updateCredentials(int row, const QString &username, const QString &password)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const qint64 hostId = m_entries.at(row).rec.id;
    if (!username.trimmed().isEmpty()) {
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].rec.id == hostId)
                m_entries[i].rec.username = username.trimmed();
        }
        const int first = rowForHostId(hostId);
        if (first >= 0)
            m_db->updateHost(m_entries.at(first).rec);
    }
    m_sessions.of(hostId).resetBackoff();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].rec.id != hostId)
            continue;
        m_entries[i].status = tr("connecting…");
        m_entries[i].problem = Problem::Connecting;
        emit dataChanged(index(i), index(i));
    }
    // A non-empty password is stored to the keyring and used; empty keeps the
    // stored one (username-only fix).
    validateAsync(hostId, password, /*storeNew=*/!password.isEmpty());
}

void DeviceManager::addDevice(const QString &addr, const QString &username,
                              const QString &password, bool https, int port)
{
    HostRecord rec;
    rec.kind = device::kindToString(device::Kind::Camera); // refined to "nvr" after GetDevInfo
    rec.name = addr;
    rec.addr = addr;
    rec.https = https;
    rec.port = port > 0 ? port : (https ? 443 : 80);
    rec.username = username;
    rec.id = m_db->addHost(rec);
    if (rec.id < 0) {
        emit deviceError(addr, m_db->lastError());
        return;
    }

    beginInsertRows({}, m_entries.size(), m_entries.size());
    Entry e;
    e.rec = rec;
    e.chanName = addr;
    e.status = tr("connecting…");
    e.rotationOverride = loadRotation(rec.id, 0);
    loadLensSettings(e, rec.id, 0);
    m_entries.append(e);
    endInsertRows();
    emit countChanged();

    validateAsync(rec.id, password, /*storeNew=*/true);
}

void DeviceManager::addStreamUrl(const QString &name, const QString &url)
{
    QUrl u(url);
    const QString user = u.userName();
    const QString pass = u.password();
    if (!user.isEmpty() || !pass.isEmpty()) {
        u.setUserName({});
        u.setPassword({});
    }
    const QString cleanUrl =
        u.isValid() && !u.scheme().isEmpty() ? u.toString(QUrl::FullyEncoded) : url;

    HostRecord rec;
    rec.kind = device::kindToString(device::Kind::Stream);
    rec.name = name.isEmpty() ? cleanUrl : name;
    rec.addr = cleanUrl;
    rec.https = false;
    rec.port = 0;
    rec.username = user;
    rec.id = m_db->addHost(rec);
    if (rec.id < 0) {
        emit deviceError(url, m_db->lastError());
        return;
    }
    if (!pass.isEmpty() && !m_credentials->store(rec.id, pass))
        qCWarning(lcCore) << "Keyring unavailable; stream credentials not saved";

    beginInsertRows({}, m_entries.size(), m_entries.size());
    Entry e;
    e.rec = rec;
    e.chanName = rec.name;
    e.online = true;
    e.status = tr("ready");
    e.password = pass;
    e.primed = true;
    m_entries.append(e);
    endInsertRows();
    emit countChanged();
}

void DeviceManager::setFisheye(int row, bool on)
{
    if (row < 0 || row >= m_entries.size())
        return;
    Entry &e = m_entries[row];
    e.lensOverride = on ? 1 : 0;
    QSettings().setValue(lensKey(e.rec.id, e.channel), on);
    emit dataChanged(index(row), index(row), {IsFisheyeRole});
}

void DeviceManager::setViewMode(int row, const QString &mode)
{
    if (row < 0 || row >= m_entries.size() || !kViewModes.contains(mode))
        return;
    Entry &e = m_entries[row];
    if (e.viewMode == mode)
        return;
    e.viewMode = mode;
    QSettings().setValue(viewModeKey(e.rec.id, e.channel), mode);
    emit dataChanged(index(row), index(row), {ViewModeRole});
}

void DeviceManager::renameChannel(int row, const QString &name)
{
    if (row < 0 || row >= m_entries.size())
        return;
    Entry &e = m_entries[row];
    if (!m_db->setChannelNameOverride(e.rec.id, e.channel, name))
        return;
    if (!name.trimmed().isEmpty()) {
        e.chanName = name.trimmed();
        emit dataChanged(index(row), index(row));
    }
}

void DeviceManager::removeDevice(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const qint64 id = m_entries.at(row).rec.id;
    // Remove every channel entry belonging to this host.
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        if (m_entries.at(i).rec.id == id) {
            beginRemoveRows({}, i, i);
            m_entries.removeAt(i);
            endRemoveRows();
        }
    }
    stopPushListener(id);
    m_detections.remove(id);
    delete m_frigateLive.take(id);
    m_frigateListed.remove(id);
    m_frigateListCheckedAt.remove(id);
    m_db->removeHost(id);
    m_credentials->remove(id);
    m_credentials->removeSecret(restreamKey(id));
    emit countChanged();
}

int DeviceManager::rowForHostId(qint64 hostId) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).rec.id == hostId)
            return i;
    return -1;
}

void DeviceManager::applyValidation(qint64 hostId, const Validation &vIn)
{
    // Locate the contiguous block of rows for this host.
    int first = rowForHostId(hostId);
    if (first < 0)
        return;
    int oldCount = 0;
    for (int i = first; i < m_entries.size() && m_entries.at(i).rec.id == hostId; ++i)
        ++oldCount;

    HostRecord rec = m_entries.at(first).rec;

    device::HostSession &session = m_sessions.of(hostId);
    session.endValidation();
    const QDateTime now = QDateTime::currentDateTimeUtc();

    Validation v = vIn;
    if (v.degraded && !v.channels.isEmpty()) {
        // A camera list read over Baichuan only fills in for a host whose list has never
        // come from the HTTP API. If it has, that list (real names, caps) stays.
        bool haveFull = false;
        bool same = oldCount == v.channels.size();
        for (int i = 0; i < oldCount; ++i) {
            const Entry &old = m_entries.at(first + i);
            haveFull = haveFull || old.validated;
            same = same && old.degraded && i < v.channels.size()
                   && old.channel == v.channels.at(i).channel;
        }
        if (haveFull) {
            v.degraded = false;
            v.channels.clear(); // handled as a plain failure below
        } else {
            // Keep trying HTTP with the usual backoff so names and the rest arrive later.
            session.keepRetrying(now);
            if (same)
                return; // nothing changed; don't rebuild rows under running streams
        }
    }

    if (!v.degraded && v.problem == Problem::Unreachable && m_entries.at(first).degraded) {
        // Still showing the Baichuan-listed cameras; the HTTP retry failed again. Leave the
        // rows (and their streams) alone and try again later.
        session.keepRetrying(now);
        return;
    }

    if ((!v.online && !v.degraded) || v.channels.isEmpty()) {
        // Only announce a CHANGE — the retry loop re-lands the same failure
        // every backoff interval, and repeating it would spam any listener.
        const bool changed = m_entries.at(first).status != v.status
                             || m_entries.at(first).problem != v.problem;
        // Failed/offline: keep the existing rows, just mark them.
        for (int i = first; i < first + oldCount; ++i) {
            m_entries[i].online = false;
            m_entries[i].status = v.status;
            m_entries[i].problem = v.problem;
            m_entries[i].primed = true;
            m_entries[i].password = v.password;
            m_entries[i].restreamUser = v.restreamUser;
            m_entries[i].restreamPassword = v.restreamPassword;
        }
        emit dataChanged(index(first), index(first + oldCount - 1));
        if (changed)
            emit deviceError(rec.addr, v.status);
        // Schedule the recovery probe. Unreachable retries with doubling
        // backoff (15s..300s); Auth/Locked never auto-retry — every rejected
        // login burns the firmware's 10-attempt lockout counter, so recovery
        // there is Reconnect / Update credentials in the sidebar.
        session.validationFailed(v.problem, now);
        return;
    }
    if (!v.degraded)
        session.validationSucceeded();

    // Frigate cameras keep their number by name (wayfinder ticket 24): set each one's channel
    // before anything is stored or shown per channel.
    if (device::capabilities(device::kindFromString(rec.kind)).frigateApi) {
        QStringList listed;
        for (const ChannelResult &ch : std::as_const(v.channels))
            listed.append(ch.name);
        numberFrigateCameras(rec.id, v.channels);
        m_frigateListed.insert(rec.id, listed);
        m_frigateListCheckedAt.insert(rec.id, QDateTime::currentSecsSinceEpoch()); // just read
    }

    // Update the host record (name/model/kind), persist once.
    if (!v.hostName.isEmpty())
        rec.name = v.hostName;
    rec.model = v.model;
    if (v.channelNum > 1)
        rec.kind = device::kindToString(device::Kind::Nvr);
    m_db->updateHost(rec);

    // Build the new per-channel entries.
    QVector<Entry> fresh;
    fresh.reserve(v.channels.size());
    const QHash<int, QString> chosenNames = m_db->channelNameOverrides(rec.id);
    const QHash<int, bool> lenses = m_db->channelFisheye(rec.id);
    // Supported event types: kept when this validation read them, so a camera that is
    // offline at a later launch still knows them.
    if (!v.degraded)
        for (const ChannelResult &ch : v.channels)
            if (ch.online && !ch.eventTypes.isEmpty())
                m_db->setChannelEventTypes(rec.id, ch.channel, ch.eventTypes);
    for (const ChannelResult &ch : v.channels) {
        Entry e;
        e.rec = rec;
        e.channel = ch.channel;
        e.chanName = chosenNames.contains(ch.channel) ? chosenNames.value(ch.channel)
                     : ch.name.isEmpty()              ? rec.name
                                                      : ch.name;
        if (!ch.name.isEmpty())
            e.sourceName = ch.name;
        e.online = ch.online;
        e.status = v.degraded ? v.status : (ch.online ? tr("online") : tr("offline"));
        e.validated = !v.degraded;
        e.degraded = v.degraded;
        e.mainCodec = ch.codec; // may be empty = unknown -> probe
        e.mainSize = ch.mainSize;
        e.subSize = ch.subSize;
        e.mainKbps = ch.mainKbps;
        e.subKbps = ch.subKbps;
        e.uid = ch.uid;
        e.mainRestream = ch.mainRestream;
        e.caps = ch.caps;
        e.talk = ch.caps.talk;
        e.eventTypes = ch.eventTypes;
        e.isAdmin = v.isAdmin;
        e.password = v.password;
        e.restreamUser = v.restreamUser;
        e.restreamPassword = v.restreamPassword;
        e.primed = true;
        e.problem = Problem::None;
        e.rotationOverride = loadRotation(rec.id, ch.channel);
        loadLensSettings(e, rec.id, ch.channel);
        // Remember the lens this read saw; keep the earlier one when the size wasn't read.
        const int seen = device::lensSeen(ch.mainSize);
        const int before = rememberedLens(lenses, ch.channel);
        if (seen >= 0 && seen != before && !m_db->setChannelFisheye(rec.id, ch.channel, seen == 1))
            qCWarning(lcCore) << "could not remember the lens of" << rec.name << ch.channel << ":"
                              << m_db->lastError();
        e.lensRemembered = seen >= 0 ? seen : before;
        e.client = v.client;
        if (ch.channel == 0)
            e.battery = v.battery;
        fresh.append(e);
    }

    // Replace the old block with the new channel entries.
    beginRemoveRows({}, first, first + oldCount - 1);
    m_entries.remove(first, oldCount);
    endRemoveRows();
    beginInsertRows({}, first, first + fresh.size() - 1);
    for (int j = 0; j < fresh.size(); ++j)
        m_entries.insert(first + j, fresh.at(j));
    endInsertRows();
    emit countChanged();
}

void DeviceManager::numberFrigateCameras(qint64 hostId, QVector<ChannelResult> &channels)
{
    QStringList names;
    for (const ChannelResult &ch : std::as_const(channels))
        names.append(ch.name);
    const QHash<QString, int> known = m_db->channelSourceNames(hostId);
    const QHash<QString, int> number =
        frigate::assignChannels(names, known, m_db->channelNumbersInUse(hostId));
    for (ChannelResult &ch : channels) {
        ch.channel = number.value(ch.name);
        if (known.value(ch.name, -1) != ch.channel
            && !m_db->setChannelSourceName(hostId, ch.channel, ch.name))
            qCWarning(lcCore) << "could not keep the number of Frigate camera" << ch.name << ":"
                              << m_db->lastError();
    }
    // Rows follow the numbers, which is Frigate's order until a camera is removed.
    std::sort(channels.begin(), channels.end(),
              [](const ChannelResult &a, const ChannelResult &b) { return a.channel < b.channel; });
}

void DeviceManager::postValidation(qint64 hostId, const Validation &v)
{
    QMetaObject::invokeMethod(
        this, [this, hostId, v] { applyValidation(hostId, v); }, Qt::QueuedConnection);
}

DeviceManager::Validation DeviceManager::validateFrigate(const HostRecord &rec,
                                                          const QString &password)
{
    Validation v;
    frigate::setCredentials(rec.addr, rec.port, rec.username, password);
    QString error;
    const QByteArray body = frigate::httpGet(frigate::configUrl(rec.addr, rec.port), 10000, &error);
    const Json cfg = Json::parse(body.toStdString(), nullptr, false);
    const QStringList names = cfg.is_discarded() ? QStringList() : frigate::parseCameraNames(cfg);
    if (names.isEmpty()) {
        v.problem = error.contains(QStringLiteral("sign-in"), Qt::CaseInsensitive)
                        ? Problem::Auth : Problem::Unreachable;
        v.status = !error.isEmpty() ? error
                   : cfg.is_discarded() ? tr("not a Frigate server")
                                        : tr("Frigate has no enabled cameras");
        return v;
    }
    v.online = true;
    v.problem = Problem::None;
    v.status = tr("online");
    v.model = QStringLiteral("Frigate");
    v.channelNum = 0; // stay a "frigate" host (channelNum > 1 would turn it into an NVR)
    for (int i = 0; i < names.size(); ++i) {
        ChannelResult cr;
        cr.channel = i;
        cr.name = names.at(i);
        cr.online = true;
        cr.codec = QStringLiteral("h264");
        cr.mainRestream = frigate::mainRestream(cfg, names.at(i));
        cr.eventTypes = frigate::cameraEventTypes(cfg, names.at(i));
        v.channels.append(cr);
    }
    return v;
}

void DeviceManager::addFrigate(const QString &name, const QString &host, int port,
                               const QString &username, const QString &password,
                               const QString &restreamUser, const QString &restreamPassword)
{
    HostRecord rec;
    rec.kind = device::kindToString(device::Kind::Frigate);
    rec.name = name.trimmed().isEmpty() ? host : name.trimmed();
    rec.addr = host;
    rec.port = port > 0 ? port : 8971;
    rec.https = rec.port == 8971;
    rec.username = username.trimmed();
    rec.id = m_db->addHost(rec);
    if (rec.id < 0) {
        emit deviceError(host, m_db->lastError());
        return;
    }
    if (!restreamUser.trimmed().isEmpty()
        && !m_credentials->storeSecret(restreamKey(rec.id),
                                       packRestream(restreamUser.trimmed(), restreamPassword),
                                       QStringLiteral("Frigate restream login")))
        qCWarning(lcCore) << "Keyring unavailable; restream login not saved";
    beginInsertRows({}, m_entries.size(), m_entries.size());
    Entry e;
    e.rec = rec;
    e.chanName = rec.name;
    e.status = tr("connecting…");
    m_entries.append(e);
    endInsertRows();
    emit countChanged();
    validateAsync(rec.id, password, !password.isEmpty());
}

void DeviceManager::setRestreamLogin(int row, const QString &user, const QString &password)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const Entry &at = m_entries.at(row);
    if (!at.kindCaps().frigateApi)
        return;
    const qint64 hostId = at.rec.id;
    const QString u = user.trimmed();
    // An empty password keeps the saved one (user-only fix), as updateCredentials does.
    QString pw = password;
    if (pw.isEmpty() && !u.isEmpty()) {
        for (const Entry &e : std::as_const(m_entries))
            if (e.rec.id == hostId) {
                pw = e.restreamPassword;
                break;
            }
    }
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].rec.id != hostId)
            continue;
        m_entries[i].restreamUser = u;
        m_entries[i].restreamPassword = u.isEmpty() ? QString() : pw;
    }
    // Stored before the host revalidates (as addStreamUrl stores), so the validation reads it back
    // rather than racing a background write; a keyring call is a few milliseconds.
    if (u.isEmpty())
        m_credentials->removeSecret(restreamKey(hostId));
    else if (!m_credentials->storeSecret(restreamKey(hostId), packRestream(u, pw),
                                         QStringLiteral("Frigate restream login")))
        qCWarning(lcCore) << "Keyring unavailable; restream login not saved";
    reconnect(row);
}

QString DeviceManager::restreamUserAt(int row) const
{
    return row >= 0 && row < m_entries.size() ? m_entries.at(row).restreamUser : QString();
}

void DeviceManager::testFrigate(const QString &host, int port,
                                const QString &username, const QString &password)
{
    const int portv = port > 0 ? port : 8971;
    m_pending.addFuture(QtConcurrent::run([this, host, portv, username, password] {
        frigate::setCredentials(host, portv, username.trimmed(), password);
        QString error;
        const QByteArray body = frigate::httpGet(frigate::configUrl(host, portv), 10000, &error);
        const Json cfg = Json::parse(body.toStdString(), nullptr, false);
        const QStringList names = cfg.is_discarded() ? QStringList() : frigate::parseCameraNames(cfg);
        const bool ok = !names.isEmpty();
        QString problem, message;
        if (!ok) {
            problem = error.contains(QStringLiteral("sign-in"), Qt::CaseInsensitive)
                          ? QStringLiteral("auth")
                          : error.isEmpty() ? QStringLiteral("protocol") : QStringLiteral("transport");
            message = !error.isEmpty() ? error
                      : cfg.is_discarded() ? tr("the address did not answer like a Frigate server")
                                           : tr("Frigate has no enabled cameras");
        }
        const QString model = ok ? tr("%n camera(s)", "", names.size()) : QString();
        QMetaObject::invokeMethod(
            this,
            [this, ok, message, model, problem] { emit testDeviceResult(ok, message, QString(), model, problem); },
            Qt::QueuedConnection);
    }));
}

// Frigate has no per-channel state to poll: one events request per server per cycle
// raises the same detectionEvent the Reolink poller does, and its success is the
// server's connectivity. The first cycle only records where "now" is on the server's
// clock, so old history is never replayed as fresh activity.
void DeviceManager::pollFrigate()
{
    QSet<qint64> done;
    for (const Entry &e : m_entries) {
        if (!e.kindCaps().frigateApi || !e.primed || done.contains(e.rec.id))
            continue;
        done.insert(e.rec.id);
        const qint64 hostId = e.rec.id;
        if (!m_sessions.of(hostId).beginPoll())
            continue;
        const QString host = e.rec.addr;
        const int port = e.rec.port;
        ensureFrigateLive(hostId, host, port);
        const QHash<QString, int> channelOf = frigateChannels(hostId);
        QHash<int, bool> online;
        for (auto it = channelOf.constBegin(); it != channelOf.constEnd(); ++it)
            online.insert(it.value(), true);
        const device::FrigatePoll plan = m_detections[hostId].frigatePlan();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const bool readList = frigate::cameraListDue(m_frigateListCheckedAt.value(hostId), now);
        if (readList)
            m_frigateListCheckedAt.insert(hostId, now);
        m_pending.addFuture(QtConcurrent::run([this, hostId, host, port, channelOf, online,
                                               plan, readList] {
            QString error;
            const QByteArray body = frigate::httpGet(
                frigate::eventsUrl(host, port, plan.after, plan.initialized ? 50 : 1), 8000, &error);
            const Json parsed = Json::parse(body.toStdString(), nullptr, false);
            const bool ok = error.isEmpty() && !parsed.is_discarded() && parsed.is_array();
            const QVector<frigate::Event> events = ok ? frigate::parseEvents(parsed) : QVector<frigate::Event>();
            QStringList listed;
            if (ok && readList) {
                QString cfgError;
                const Json cfg = Json::parse(
                    frigate::httpGet(frigate::configUrl(host, port), 10000, &cfgError).toStdString(),
                    nullptr, false);
                if (cfgError.isEmpty() && !cfg.is_discarded())
                    listed = frigate::parseCameraNames(cfg);
                else
                    qCDebug(lcCore) << "Frigate camera list not read:" << cfgError;
            }
            QMetaObject::invokeMethod(
                this,
                [this, hostId, ok, events, channelOf, online, plan, listed] {
                    m_sessions.of(hostId).endPoll();
                    applyConnectivity(hostId, ok, online);
                    if (!ok)
                        return;
                    if (!listed.isEmpty())
                        qCDebug(lcCore) << "Frigate host" << hostId << "lists" << listed;
                    if (frigate::cameraListChanged(m_frigateListed.value(hostId), listed)) {
                        qCInfo(lcCore) << "Frigate host" << hostId << "now lists" << listed
                                       << "; re-reading it";
                        validateAsync(hostId);
                    }
                    // RL_FRIGATE_BACKFILL=<seconds>: treat the last N seconds of real events
                    // as new, so the event path can be tested without waiting.
                    const int backfill = qEnvironmentVariableIntValue("RL_FRIGATE_BACKFILL");
                    emitDetections(hostId,
                                   m_detections[hostId].onFrigatePoll(
                                       plan, events, channelOf, QDateTime::currentSecsSinceEpoch(),
                                       backfill),
                                   true);
                },
                Qt::QueuedConnection);
        }));
    }
}

void DeviceManager::ensureFrigateLive(qint64 hostId, const QString &host, int port)
{
    static const bool disabled = qEnvironmentVariableIsSet("RL_NO_PUSH");
    if (disabled)
        return;
    if (FrigateLive *l = m_frigateLive.value(hostId)) {
        if (l->host() == host && l->port() == port)
            return;
        delete m_frigateLive.take(hostId);
    }
    auto *l = new FrigateLive(host, port, this);
    connect(l, &FrigateLive::connectedChanged, this, [hostId](bool on) {
        qCInfo(lcProto) << "Frigate live events" << (on ? "on" : "off") << "for host" << hostId;
    });
    connect(l, &FrigateLive::liveEvent, this,
            [this, hostId](const QString &camera, const QString &label, double start) {
                onFrigateLiveEvent(hostId, camera, label, start);
            });
    m_frigateLive.insert(hostId, l);
}

QHash<QString, int> DeviceManager::frigateChannels(qint64 hostId) const
{
    QHash<QString, int> channelOf;
    for (const Entry &c : m_entries)
        if (c.rec.id == hostId)
            channelOf.insert(c.serverName(), c.channel);
    return channelOf;
}

void DeviceManager::onFrigateLiveEvent(qint64 hostId, const QString &camera, const QString &label,
                                       double start)
{
    emitDetections(hostId,
                   m_detections[hostId].onFrigateLive(camera, label, start,
                                                      frigateChannels(hostId)),
                   true);
}

void DeviceManager::validateAsync(qint64 hostId, const QString &newPassword, bool storeNew)
{
    const int row = rowForHostId(hostId);
    if (row < 0)
        return;
    // One validation per host at a time — the retry tick and a user-driven
    // Reconnect must not stack logins on a struggling device.
    if (!m_sessions.of(hostId).beginValidation())
        return;
    const HostRecord rec = m_entries.at(row).rec;
    const bool isStream = device::capabilities(device::kindFromString(rec.kind)).alwaysOnline;
    // Already showing the Baichuan-discovered list: just retry HTTP, don't reopen Baichuan
    // (the NVR has few connection slots).
    // Same when the host's list already came from HTTP: a fallback list would be discarded.
    bool alreadyDegraded = false;
    for (int i = row; i < m_entries.size() && m_entries.at(i).rec.id == hostId; ++i)
        alreadyDegraded = alreadyDegraded || m_entries.at(i).degraded || m_entries.at(i).validated;

    const BaichuanOpener opener = m_baichuan;
    m_pending.addFuture(QtConcurrent::run([this, rec, newPassword, storeNew, isStream, hostId,
                                           alreadyDegraded, opener] {
        if (storeNew && !newPassword.isEmpty())
            m_credentials->store(hostId, newPassword);

        bool havePassword = false;
        QString password = newPassword;
        if (password.isEmpty())
            password = m_credentials->lookup(hostId, &havePassword);
        else
            havePassword = true;

        if (isStream) {
            const QString pw = password;
            QMetaObject::invokeMethod(
                this,
                [this, hostId, pw] {
                    const int r = rowForHostId(hostId);
                    if (r >= 0) {
                        m_entries[r].password = pw;
                        m_entries[r].primed = true;
                    }
                    m_sessions.of(hostId).endValidation();
                },
                Qt::QueuedConnection);
            return;
        }

        if (device::capabilities(device::kindFromString(rec.kind)).frigateApi) {
            Validation v = validateFrigate(rec, password);
            unpackRestream(m_credentials->lookupSecret(restreamKey(hostId)), &v.restreamUser,
                           &v.restreamPassword);
            postValidation(hostId, v);
            return;
        }

        if (!havePassword && storeNew) {
            Validation v;
            v.status = tr("keyring unavailable — password not saved");
            v.problem = Problem::Auth; // needs the user, not a retry loop
            postValidation(hostId, v);
            return;
        }

        auto client = std::make_shared<ReolinkHttpClient>(rec.addr, rec.port, rec.https,
                                                          rec.username, password);
        Validation v = device::validateReolinkHost(*client, rec, password, alreadyDegraded,
                                                   opener);
        if (v.online)
            v.client = client;
        postValidation(hostId, v);
    }));
}

std::shared_ptr<ReolinkHttpClient> DeviceManager::clientFor(int row)
{
    if (row < 0 || row >= m_entries.size())
        return {};
    Entry &e = m_entries[row];
    if (!e.reolinkReady())
        return {};
    if (!e.client)
        e.client = std::make_shared<ReolinkHttpClient>(e.rec.addr, e.rec.port, e.rec.https,
                                                       e.rec.username, e.password);
    return e.client;
}

void DeviceManager::ptzMove(int row, const QString &op, int speed)
{
    auto client = clientFor(row);
    if (!client)
        return;
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run(
        [client, ch, op, speed] { client->call(Json::array({api::ptzCtrl(ch, op, speed)})); }));
}

void DeviceManager::ptzStop(int row)
{
    auto client = clientFor(row);
    if (!client)
        return;
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run(
        [client, ch] { client->call(Json::array({api::ptzCtrl(ch, QStringLiteral("Stop"))})); }));
}

void DeviceManager::ptzPreset(int row, int presetId)
{
    auto client = clientFor(row);
    if (!client)
        return;
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run([client, ch, presetId] {
        client->call(Json::array({api::ptzCtrl(ch, QStringLiteral("ToPos"), 32, presetId)}));
    }));
}

void DeviceManager::snapshot(int row)
{
    auto client = clientFor(row);
    if (!client) {
        emit snapshotFailed(row, tr("device not ready"));
        return;
    }
    const int ch = m_entries.at(row).channel;
    const QString name = m_entries.at(row).chanName;
    m_pending.addFuture(QtConcurrent::run([this, client, ch, row, name] {
        QString error;
        const QByteArray jpeg = client->fetchSnapshot(ch, &error);
        QString path;
        if (!jpeg.isEmpty()) {
            const QString safe =
                QString(name).replace(QRegularExpression(QStringLiteral("[^\\w-]")),
                                      QStringLiteral("_"));
            const QString stamp =
                QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
            path = Paths::recordingsDir() + u'/' + safe + u'_' + stamp + QStringLiteral(".jpg");
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(jpeg) != jpeg.size()) {
                error = f.errorString();
                path.clear();
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, row, path, error] {
                if (!path.isEmpty())
                    emit snapshotSaved(row, path);
                else
                    emit snapshotFailed(row, error.isEmpty() ? tr("snapshot failed") : error);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::captureEventThumbnail(qint64 hostId, int channel, qint64 eventId)
{
    const int row = rowOfHostChannel(hostId, channel);
    if (row < 0 || row >= m_entries.size())
        return;
    const QString path = Paths::thumbnailsDir() + QStringLiteral("/event_%1.jpg").arg(eventId);
    auto finish = [this, eventId, path](const QByteArray &jpeg) {
        // Kept in HD, at most 2560 px wide (fisheye-and-framing ticket 11); silent on
        // failure: an event without one keeps its placeholder.
        if (jpeg.isEmpty() || !device::storeEventSnapshot(jpeg, path))
            return;
        QMetaObject::invokeMethod(
            this, [this, eventId, path] { emit eventThumbnailReady(eventId, path); },
            Qt::QueuedConnection);
    };
    const Entry &e = m_entries.at(row);
    if (e.kindCaps().frigateApi) {
        const QString url = frigate::latestFrameUrl(e.rec.addr, e.rec.port, e.serverName());
        m_pending.addFuture(QtConcurrent::run([finish, url] { finish(frigate::httpGet(url, 8000)); }));
        return;
    }
    auto client = clientFor(row);
    if (!client)
        return;
    const int ch = channel;
    m_pending.addFuture(QtConcurrent::run([client, ch, finish] {
        QString error;
        finish(client->fetchSnapshot(ch, &error));
    }));
}

int DeviceManager::rowOfCameraKey(const QString &key) const
{
    const QStringList hc = key.split(QLatin1Char(':'));
    bool okH = false, okC = false;
    const qint64 hostId = hc.value(0).toLongLong(&okH);
    const int channel = hc.value(1).toInt(&okC);
    if (hc.size() != 2 || !okH || !okC)
        return -1;
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).rec.id == hostId && m_entries.at(i).channel == channel)
            return i;
    return -1;
}

QSize DeviceManager::declaredSize(int row, bool mainStream) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    return mainStream ? e.mainSize : e.subSize;
}

QString DeviceManager::playbackUrl(int row, qint64 startEpoch)
{
    if (row < 0 || row >= m_entries.size() || startEpoch <= 0)
        return {};
    const Entry &e = m_entries.at(row);
    // Only a direct camera plays by URL (PlaybackSession::chooseRoute); a Reolink recorder's
    // footage always comes over Baichuan. Frigate plays from `startEpoch` for an hour;
    // Playback reopens past the end, a seek opens a new list.
    if (!e.kindCaps().frigateApi)
        return {};
    return frigate::playbackVodUrl(e.rec.addr, e.rec.port, e.serverName(), startEpoch);
}

DownloadSource DeviceManager::downloadSource(int row)
{
    DownloadSource s;
    auto client = clientFor(row);
    if (!client)
        return s;
    const Entry &e = m_entries.at(row);
    if (e.kindCaps().direct())
        return s;
    s.hostId = e.rec.id;
    s.site = e.rec.name;
    s.camera = e.chanName.isEmpty() ? e.rec.name : e.chanName;
    s.channel = e.channel;
    s.host = e.rec.addr;
    s.port = e.rec.port;
    s.https = e.rec.https;
    s.client = std::move(client);
    s.baichuan = e.kindCaps().baichuanDownload;
    s.username = e.rec.username;
    s.password = e.password;
    s.uid = e.uid;
    return s;
}

void DeviceManager::startBaichuan(int row, qint64 startEpoch, StreamPlayer *player,
                                  bool mainStream, bool reverse, bool forwardWindows)
{
    if (row < 0 || row >= m_entries.size() || !player)
        return;
    const Entry &e = m_entries.at(row);
    if (!e.reolinkReady()) {
        if (startEpoch > 0)
            qCWarning(lcCore).noquote() << "native playback not started:" << e.rec.name
                                        << "is not ready";
        return;
    }
    if (startEpoch > 0)
        qCInfo(lcCore).noquote()
            << "native playback" << e.rec.name << "/" << e.chanName << "ch" << e.channel
            << (mainStream ? "main" : "sub")
            << (reverse ? "backwards from" : forwardWindows ? "in windows from" : "from")
            << QDateTime::fromSecsSinceEpoch(startEpoch).toString(Qt::ISODate);

    BaichuanClient::Params p;
    p.host = e.rec.addr;
    p.port = 9000; // Baichuan is a separate port from the HTTP API
    p.username = e.rec.username;
    p.password = e.password;
    p.channel = e.channel;
    p.uid = e.uid;
    p.mainStream = mainStream;
    p.startEpoch = startEpoch; // <= 0 = live (cmd 3 Preview), else by-time playback
    p.reverse = reverse && startEpoch > 0;
    p.forwardWindows = forwardWindows && !p.reverse && startEpoch > 0;
    const bool smoothLive = startEpoch <= 0 && player->smoothing();
    p.smoothLive = smoothLive;

    auto client = std::make_shared<BaichuanClient>(p);
    client->setAudioHandler(player->audioFeed()); // before start(): read by the worker
    client->setSpeedCell(player->speedCell());
    if (p.reverse || p.forwardWindows) {
        // Tagged with the session it belongs to: a late report from an older one is ignored.
        QPointer<StreamPlayer> to(player);
        const quint64 sessionId = player->nextSessionId();
        client->setLinkSlowHandler([to, sessionId] {
            QMetaObject::invokeMethod(to, [to, sessionId] { if (to) to->reportLinkTooSlow(sessionId); },
                                      Qt::QueuedConnection);
        });
    }
    client->start();
    // The sub stream is always H.264; the main stream's codec is per-channel.
    // An UNKNOWN main codec (firmware omitted/mangled GetEnc vType — issue #4)
    // passes an empty format so avformat probes the stream instead: forcing
    // h264 onto an HEVC bitstream yields "Could not find codec parameters".
    QString format = QStringLiteral("h264");
    if (mainStream)
        format = e.mainCodec.isEmpty()
                     ? QString()
                     : (e.mainCodec == QLatin1String("h265") ? QStringLiteral("hevc")
                                                             : QStringLiteral("h264"));
    player->setExpectedSize(mainStream ? e.mainSize : e.subSize);
    player->setReversePlayback(p.reverse);
    player->setForwardWindows(p.forwardWindows);
    player->setPacketSource(
        [client](unsigned char *buf, int size) { return client->read(buf, size); },
        format,
        [client] { client->stop(); },
        smoothLive ? int(BcLiveFramePacer::kDelayUs / 1000) : 0,
        // Recorded footage moves in place on this pane's own stream (StreamPlayer::seekSource).
        startEpoch > 0 ? std::function<bool(qint64)>([client](qint64 epoch) { return client->seek(epoch); })
                       : std::function<bool(qint64)>());
    player->start();
}

void DeviceManager::startTalk(int row, TalkSession *session)
{
    if (row < 0 || row >= m_entries.size() || !session)
        return;
    const Entry &e = m_entries.at(row);
    if (!e.reolinkReady() || !e.talk)
        return;

    BaichuanTalk::Params p;
    p.host = e.rec.addr;
    p.port = 9000; // Baichuan port, separate from the HTTP API
    p.username = e.rec.username;
    p.password = e.password;
    p.channel = e.channel;
    session->start(p);
}

void DeviceManager::startBaichuanPlayback(int row, qint64 startEpoch, StreamPlayer *player,
                                          bool mainStream, bool reverse, bool forwardWindows)
{
    if (startEpoch <= 0)
        return;
    startBaichuan(row, startEpoch, player, mainStream, reverse, forwardWindows);
}

void DeviceManager::startBaichuanLive(int row, StreamPlayer *player, bool mainStream)
{
    startBaichuan(row, /*startEpoch=*/0, player, mainStream);
}

namespace {
QString liveBufferKey(qint64 hostId, int channel) { return QStringLiteral("%1:%2").arg(hostId).arg(channel); }
} // namespace

std::shared_ptr<LiveBuffer> DeviceManager::liveBuffer(int row)
{
    if (row < 0 || row >= m_entries.size() || isDirectAt(row))
        return {};
    const Entry &e = m_entries.at(row);
    if (!m_liveBufferLog.isActive()) {   // the first buffer starts the minutely log
        m_liveBufferLog.setInterval(60000);
        connect(&m_liveBufferLog, &QTimer::timeout, this, [this] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (int r = 0; r < m_entries.size(); ++r) {
                const auto b = findLiveBuffer(r);
                if (!b)
                    continue;
                b->trimBefore(now - LiveBuffer::kSpanMs);
                if (b->empty())
                    continue;
                qCInfo(lcMedia).noquote() << "live buffer" << m_entries.at(r).rec.name << "/"
                    << m_entries.at(r).chanName << ":" << (b->newestMs() - b->oldestMs()) / 1000
                    << "s in" << b->runs() << "run(s)," << b->bytes() / 1000000 << "MB; all"
                    << m_liveBuffers->bytes() / 1000000 << "MB";
            }
        });
        m_liveBufferLog.start();
    }
    return m_liveBuffers->buffer(liveBufferKey(e.rec.id, e.channel));
}

bool DeviceManager::liveBufferHas(int row, qint64 epochMs, int direction) const
{
    const auto b = findLiveBuffer(row);
    return b && b->hasRunAt(epochMs, direction);
}

QVariantList DeviceManager::liveBufferSpans(int row) const
{
    QVariantList out;
    const auto b = findLiveBuffer(row);
    if (!b)
        return out;
    // A camera that stopped streaming keeps only its last 5 minutes too.
    b->trimBefore(QDateTime::currentMSecsSinceEpoch() - LiveBuffer::kSpanMs);
    for (const LiveBuffer::Span &s : b->spans())
        out << QVariantMap{{QStringLiteral("start"), s.startMs / 1000.0},
                           {QStringLiteral("end"), s.endMs / 1000.0}};
    return out;
}

void DeviceManager::startLiveBufferPlayback(int row, qint64 epochMs, int direction, StreamPlayer *player)
{
    const auto b = findLiveBuffer(row);
    if (!b || !player)
        return;
    const Entry &e = m_entries.at(row);
    auto source = std::make_shared<BufferReplay>(b, epochMs, direction, player->speedCell());
    qCInfo(lcMedia).noquote() << "live buffer playback" << e.rec.name << "/" << e.chanName
        << (source->main() ? "main" : "sub") << (direction < 0 ? "backwards from" : "from")
        << QDateTime::fromMSecsSinceEpoch(epochMs).toString(Qt::ISODateWithMs);
    player->setExpectedSize(source->main() ? e.mainSize : e.subSize);
    player->setReversePlayback(direction < 0);
    player->setForwardWindows(direction > 0);   // paced by the frames' times (ForwardPacer)
    player->setPacketSource(
        [source](unsigned char *buf, int size) { return source->read(buf, size); },
        source->codec() == BcMediaParser::Codec::H265 ? QStringLiteral("hevc") : QStringLiteral("h264"),
        [source] { source->stop(); });
    player->start();
}

std::shared_ptr<LiveBuffer> DeviceManager::findLiveBuffer(int row) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    return m_liveBuffers->find(liveBufferKey(e.rec.id, e.channel));
}

void DeviceManager::fetchSettings(int row, const QStringList &getCommands)
{
    auto client = clientFor(row);
    if (!client || getCommands.isEmpty()) {
        emit settingsFailed(row, tr("device not ready"));
        return;
    }
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run([this, client, row, ch, getCommands] {
        QVariantMap values;
        QStringList failed;
        QString lastError;
        // Fetch one command, retrying only transient transport failures: NVRs (e.g.
        // RLN8-410) intermittently return HTTP 502 when proxying a camera command
        // under load. Retry a few times with a short backoff (the lighter action=0
        // on the 2nd try); a DEFINITIVE device error (rspCode set — e.g. -9 "not
        // supported") stops immediately so we don't hammer the NVR for nothing.
        auto fetchOne = [&](const QString &cmd) {
            api::CommandResult r;
            for (int attempt = 0; attempt < 3; ++attempt) {
                r = client->callOne(cmd, Json{{"channel", ch}}, attempt == 1 ? 0 : 1);
                if (r.ok || r.rspCode != 0)
                    break;
                QThread::msleep(400);
            }
            return r;
        };
        // Newer firmware renamed several commands with a "V20" suffix (a different
        // schedule shape). If the device says the legacy name isn't supported, try
        // the V20 variant and store it under the legacy key so panels bind uniformly.
        static const QHash<QString, QString> v20Alt = {
            {QStringLiteral("GetPush"), QStringLiteral("GetPushV20")},
            {QStringLiteral("GetEmail"), QStringLiteral("GetEmailV20")},
            {QStringLiteral("GetFtp"), QStringLiteral("GetFtpV20")},
            {QStringLiteral("GetRec"), QStringLiteral("GetRecV20")}};
        for (const QString &c : getCommands) {
            api::CommandResult r = fetchOne(c);
            if (!r.ok && r.rspCode != 0 && v20Alt.contains(c))
                r = fetchOne(v20Alt.value(c));
            if (r.ok) {
                values.insert(c, api::toVariant(r.value));
            } else {
                failed.append(c);
                if (!r.detail.isEmpty())
                    lastError = r.detail;
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, row, values, failed, lastError] {
                if (values.isEmpty()) {
                    emit settingsFailed(row, lastError.isEmpty() ? tr("settings fetch failed")
                                                                 : lastError);
                    return;
                }
                // Report which commands the device refused so panels can show an
                // honest "unavailable" state instead of misleading defaults.
                QVariantMap v = values;
                if (!failed.isEmpty()) {
                    QVariantList f;
                    for (const QString &c : failed)
                        f.append(c);
                    v.insert(QStringLiteral("_failed"), f);
                }
                emit settingsLoaded(row, v);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::applySetting(int row, const QString &setCommand, const QVariantMap &param)
{
    auto client = clientFor(row);
    if (!client) {
        emit settingApplied(row, setCommand, false, tr("device not ready"));
        return;
    }
    if (row < m_entries.size() && !m_entries.at(row).isAdmin) {
        emit settingApplied(row, setCommand, false, tr("requires an administrator account"));
        return;
    }
    const Json p = api::toJson(param);
    m_pending.addFuture(QtConcurrent::run([this, client, row, setCommand, p] {
        const api::CommandResult r = client->callOne(setCommand, p, /*action=*/0);
        const bool ok = r.ok;
        const QString error = ok ? QString() : (r.detail.isEmpty() ? tr("failed") : r.detail);
        QMetaObject::invokeMethod(
            this,
            [this, row, setCommand, ok, error] {
                emit settingApplied(row, setCommand, ok, error);
            },
            Qt::QueuedConnection);
    }));
}

namespace {
// The doorbell's clips: HTTP first; an empty or refused HTTP list is cross-checked over
// Baichuan, since an NVR can answer the HTTP list with nothing while the device has clips.
// `error` is set only when neither path could be read at all.
quickreply::ClipList fetchClipList(ReolinkHttpClient &client, const BaichuanControl::Params &p,
                                   int ch, QString *error)
{
    namespace qr = quickreply;
    qr::ClipList list;
    const api::CommandResult r =
        client.callOne(QStringLiteral("GetAudioFileList"), Json{{"channel", ch}}, 0);
    if (r.ok)
        list = qr::parseHttpList(r.value);
    if (!list.clips.isEmpty())
        return list;
    BaichuanControl bc(p);
    if (bc.open()) {
        quint16 st = 0;
        const QByteArray xml = bc.get(qr::kBcListCmd, ch, &st);
        bc.close();
        if (qr::baichuanStatusOk(st))
            list = qr::parseBaichuanList(xml);
        else if (!r.ok && error)
            *error = QObject::tr("the device does not support quick replies (status %1)").arg(st);
    } else if (!r.ok && error) {
        *error = r.detail.isEmpty() ? QObject::tr("could not reach the device") : r.detail;
    }
    return list;
}

QVariantList clipsToVariant(const quickreply::ClipList &list)
{
    QVariantList clips;
    for (const quickreply::Clip &c : list.clips)
        clips.append(QVariantMap{{QStringLiteral("id"), c.id}, {QStringLiteral("name"), c.name}});
    return clips;
}
} // namespace

void DeviceManager::fetchQuickReplies(int row)
{
    auto client = clientFor(row);
    if (!client || row < 0 || row >= m_entries.size()) {
        emit quickRepliesLoaded(row, {}, 0, tr("device not ready"));
        return;
    }
    const Entry &e = m_entries.at(row);
    BaichuanControl::Params p{e.rec.addr, 9000, e.rec.username, e.password};
    const int ch = e.channel;
    m_pending.addFuture(QtConcurrent::run([this, client, row, p, ch] {
        QString error;
        const quickreply::ClipList list = fetchClipList(*client, p, ch, &error);
        const QVariantList clips = clipsToVariant(list);
        const int maxFiles = list.maxFiles;
        QMetaObject::invokeMethod(
            this,
            [this, row, clips, maxFiles, error] { emit quickRepliesLoaded(row, clips, maxFiles, error); },
            Qt::QueuedConnection);
    }));
}

// Auto-reply: the current setting plus the clips it can pick from. The setting is read
// over HTTP, then Baichuan cmd 427 if HTTP does not answer.
void DeviceManager::fetchAutoReply(int row)
{
    auto client = clientFor(row);
    if (!client || row < 0 || row >= m_entries.size()) {
        emit autoReplyLoaded(row, {}, {}, tr("device not ready"));
        return;
    }
    const Entry &e = m_entries.at(row);
    BaichuanControl::Params p{e.rec.addr, 9000, e.rec.username, e.password};
    const int ch = e.channel;
    m_pending.addFuture(QtConcurrent::run([this, client, row, p, ch] {
        namespace qr = quickreply;
        QString error;
        qr::AutoReply a;
        const api::CommandResult r =
            client->callOne(QStringLiteral("GetAutoReply"), Json{{"channel", ch}}, 0);
        if (r.ok)
            a = qr::parseHttpAutoReply(r.value);
        if (!a.valid) {
            BaichuanControl bc(p);
            if (bc.open()) {
                quint16 st = 0;
                const QByteArray xml = bc.get(qr::kBcAutoReplyGetCmd, ch, &st);
                bc.close();
                if (qr::baichuanStatusOk(st))
                    a = qr::parseBaichuanAutoReply(xml);
            }
        }
        QVariantList clips;
        if (a.valid) {
            QString clipError;
            clips = clipsToVariant(fetchClipList(*client, p, ch, &clipError));
        } else {
            error = tr("this device does not offer auto-reply");
        }
        const QVariantMap setting{{QStringLiteral("enable"), a.enable},
                                  {QStringLiteral("fileId"), a.fileId},
                                  {QStringLiteral("timeout"), a.timeout}};
        QMetaObject::invokeMethod(
            this,
            [this, row, setting, clips, error] { emit autoReplyLoaded(row, setting, clips, error); },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::setAutoReply(int row, bool enable, int fileId, int timeout)
{
    auto client = clientFor(row);
    if (!client || row < 0 || row >= m_entries.size()) {
        emit autoReplySaved(row, false, tr("device not ready"));
        return;
    }
    if (!m_entries.at(row).isAdmin) {
        emit autoReplySaved(row, false, tr("requires an administrator account"));
        return;
    }
    const Entry &e = m_entries.at(row);
    BaichuanControl::Params p{e.rec.addr, 9000, e.rec.username, e.password};
    const int ch = e.channel;
    m_pending.addFuture(QtConcurrent::run([this, client, row, enable, fileId, timeout, p, ch] {
        namespace qr = quickreply;
        qr::AutoReply a;
        a.valid = true;
        a.enable = enable;
        a.fileId = fileId;
        a.timeout = timeout;
        const api::CommandResult r =
            client->callOne(QStringLiteral("SetAutoReply"), qr::httpAutoReplyParam(ch, a), 0);
        bool ok = r.ok && (!r.value.is_object() || !r.value.contains("rspCode") ||
                           r.value.value("rspCode", 200) == 200);
        QString error;
        if (!ok) {
            // Same fallback as the clip list: try the native protocol.
            BaichuanControl bc(p);
            if (bc.open()) {
                ok = bc.writeFields(qr::kBcAutoReplyGetCmd, qr::kBcAutoReplySetCmd, ch,
                                    qr::baichuanAutoReplyChanges(a));
                bc.close();
            }
            if (!ok)
                error = r.detail.isEmpty() ? tr("the device refused the change") : r.detail;
        }
        QMetaObject::invokeMethod(
            this, [this, row, ok, error] { emit autoReplySaved(row, ok, error); },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::playQuickReply(int row, int clipId)
{
    auto client = clientFor(row);
    if (!client || row < 0 || row >= m_entries.size()) {
        emit quickReplyPlayed(row, clipId, false, tr("device not ready"));
        return;
    }
    const Entry &e = m_entries.at(row);
    BaichuanControl::Params p{e.rec.addr, 9000, e.rec.username, e.password};
    const int ch = e.channel;
    m_pending.addFuture(QtConcurrent::run([this, client, row, clipId, p, ch] {
        namespace qr = quickreply;
        bool ok = false;
        QString error;
        const api::CommandResult r = client->callOne(
            QStringLiteral("QuickReplyPlay"), Json{{"id", clipId}, {"channel", ch}}, 0);
        // Success is code 0 and value.rspCode 200 (reolink_aio).
        ok = r.ok && (!r.value.is_object() || !r.value.contains("rspCode") ||
                      r.value.value("rspCode", 200) == 200);
        if (!ok && (r.ok || qr::httpShouldFallBack(r.rspCode) || r.rspCode == 0)) {
            BaichuanControl bc(p);
            if (bc.open()) {
                quint16 st = 0;
                bc.transact(qr::kBcPlayCmd, ch, qr::baichuanPlayBody(ch, clipId), &st);
                bc.close();
                ok = qr::baichuanStatusOk(st);
                if (!ok)
                    error = tr("the doorbell refused the clip (status %1)").arg(st);
            } else {
                error = tr("could not reach the device");
            }
        } else if (!ok) {
            error = r.detail.isEmpty() ? tr("failed") : r.detail;
        }
        QMetaObject::invokeMethod(
            this, [this, row, clipId, ok, error] { emit quickReplyPlayed(row, clipId, ok, error); },
            Qt::QueuedConnection);
    }));
}

QVariantMap DeviceManager::streamCosts() const
{
    QVariantMap out;
    for (const Entry &e : m_entries) {
        const device::Kind kind = device::kindFromString(e.rec.kind);
        const bool reolink = kind == device::Kind::Camera || kind == device::Kind::Nvr;
        out.insert(QStringLiteral("%1:%2").arg(e.rec.id).arg(e.channel),
                   QVariantMap{{QStringLiteral("sub"), device::streamCostMbps(kind, false, e.subKbps)},
                               {QStringLiteral("main"), device::streamCostMbps(kind, true, e.mainKbps)},
                               {QStringLiteral("known"), !reolink || (e.subKbps > 0 && e.mainKbps > 0)}});
    }
    return out;
}

QString DeviceManager::lastFrameUrl(qint64 hostId, int channel) const
{
    const QString path = Paths::lastFrameFile(hostId, channel);
    return QFileInfo::exists(path) ? QUrl::fromLocalFile(path).toString() : QString();
}

QVariantMap DeviceManager::hostInfo(qint64 hostId) const
{
    QVariantMap m;
    int channelCount = 0, onlineCount = 0, firstRow = -1;
    const Entry *host = nullptr;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        if (e.rec.id != hostId)
            continue;
        if (!host) { host = &e; firstRow = i; }
        ++channelCount;
        if (e.online)
            ++onlineCount;
    }
    if (!host)
        return m;
    m["name"] = host->rec.name;
    m["kind"] = host->rec.kind;
    mergeCaps(m, host->rec.kind);
    m["model"] = host->rec.model;
    m["addr"] = host->rec.addr;
    m["port"] = host->rec.port;
    m["https"] = host->rec.https;
    m["username"] = host->rec.username;
    m["online"] = onlineCount > 0;
    m["channelCount"] = channelCount;
    m["onlineCount"] = onlineCount;
    m["isAdmin"] = host->isAdmin;
    m["firstRow"] = firstRow;
    m["status"] = host->status;
    switch (host->problem) {
    case Problem::Connecting: m["problem"] = QStringLiteral("connecting"); break;
    case Problem::Unreachable: m["problem"] = QStringLiteral("unreachable"); break;
    case Problem::Auth: m["problem"] = QStringLiteral("auth"); break;
    case Problem::Locked: m["problem"] = QStringLiteral("locked"); break;
    case Problem::None: m["problem"] = QString(); break;
    }
    return m;
}

QVariantMap DeviceManager::cameraInfo(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_entries.size())
        return m;
    const Entry &e = m_entries.at(row);
    const auto dim = [](const QSize &s) {
        return s.isValid() ? QStringLiteral("%1×%2").arg(s.width()).arg(s.height()) : QString();
    };
    m["name"] = e.chanName.isEmpty() ? e.rec.name : e.chanName;
    m["hostName"] = e.rec.name;
    m["hostId"] = e.rec.id;
    m["username"] = e.rec.username;
    m["addr"] = e.rec.addr;
    m["channel"] = e.channel;
    m["kind"] = e.rec.kind;
    mergeCaps(m, e.rec.kind);
    m["model"] = e.rec.model;
    m["codec"] = e.mainCodec;
    m["mainSize"] = dim(e.mainSize);
    m["subSize"] = dim(e.subSize);
    m["uid"] = e.uid;
    m["online"] = e.online;
    m["isAdmin"] = e.isAdmin;
    m["capPtz"] = e.caps.ptz;
    m["capZoom"] = e.caps.zoom;
    m["capAudio"] = e.caps.audio;
    m["capSiren"] = e.caps.siren;
    m["capFloodlight"] = e.caps.floodlight;
    m["capBattery"] = e.caps.battery;
    m["capTalk"] = e.talk;
    m["capAutoReply"] = e.caps.autoReply;
    m["rotationOverride"] = e.rotationOverride;
    m["isFisheye"] = data(index(row), IsFisheyeRole);
    m["viewMode"] = e.viewMode;
    return m;
}

void DeviceManager::setRotationOverride(int row, int degrees)
{
    if (row < 0 || row >= m_entries.size())
        return;
    Entry &e = m_entries[row];
    const int deg = ((degrees % 360) + 360) % 360;
    if (e.rotationOverride == deg)
        return;
    e.rotationOverride = deg;
    QSettings().setValue(rotationKey(e.rec.id, e.channel), deg);
    emit dataChanged(index(row), index(row), {RotationRole});
}

QVariantList DeviceManager::hostIds() const
{
    QVariantList ids;
    for (const Entry &e : m_entries)
        if (!ids.contains(e.rec.id))
            ids.append(e.rec.id);
    return ids;
}

bool DeviceManager::isBoschFeedRow(int row) const
{
    return row >= 0 && row < m_entries.size()
           && device::looksLikeBoschFeed(m_entries.at(row).serverName());
}

QVariantMap DeviceManager::listedCameras() const
{
    QVariantMap out;
    for (const Entry &e : m_entries) {
        if (!e.kindCaps().frigateApi || !e.validated)
            continue;
        const QString host = QString::number(e.rec.id);
        QVariantList channels = out.value(host).toList();
        channels.append(e.channel);
        out.insert(host, channels);
    }
    return out;
}

QString DeviceManager::hostName(qint64 hostId) const
{
    for (const Entry &e : m_entries)
        if (e.rec.id == hostId)
            return e.rec.name;
    return {};
}

QStringList DeviceManager::eventTypesFor(qint64 hostId, int channel) const
{
    for (const Entry &e : m_entries) {
        if (e.rec.id != hostId)
            continue;
        if (e.kindCaps().detections == device::DetectionSource::None)
            return {}; // a bare stream raises nothing
        if (e.channel == channel && !e.eventTypes.isEmpty())
            return e.eventTypes;
    }
    return m_db->channelEventTypes(hostId).value(channel);
}

QStringList DeviceManager::propertyNames() const
{
    QStringList names;
    for (const Entry &e : m_entries)
        if (!names.contains(e.rec.name))
            names.append(e.rec.name);
    return names;
}

QVariantList DeviceManager::channelsForHost(qint64 hostId) const
{
    QVariantList out;
    for (const Entry &e : m_entries) {
        if (e.rec.id != hostId)
            continue;
        QVariantMap m;
        m["channel"] = e.channel;
        m["camera"] = e.serverName();   // matched against the server's camera keys
        out.append(m);
    }
    return out;
}

void DeviceManager::toggleFloodlight(int row)
{
    const QString cmd = QStringLiteral("SetWhiteLed");
    auto client = clientFor(row);
    if (!client) {
        emit settingApplied(row, cmd, false, tr("device not ready"));
        return;
    }
    if (row < m_entries.size() && !m_entries.at(row).isAdmin) {
        emit settingApplied(row, cmd, false, tr("requires an administrator account"));
        return;
    }
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run([this, client, row, ch, cmd] {
        const device::CommandResult r = device::CameraCommands::toggleFloodlight(*client, ch);
        QString error;
        if (!r.ok) {
            const bool unsupported = r.failure == device::CommandFailure::Unsupported;
            error = !r.detail.isEmpty() ? r.detail
                    : unsupported       ? tr("floodlight not supported")
                                        : tr("failed");
        }
        const bool ok = r.ok;
        QMetaObject::invokeMethod(
            this, [this, row, cmd, ok, error] { emit settingApplied(row, cmd, ok, error); },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::reboot(int row)
{
    applySetting(row, QStringLiteral("Reboot"), {});
}

// The Baichuan session parameters for a row (a Reolink host with its password loaded).
static BaichuanParams bcParams(const QString &addr, const QString &user, const QString &password)
{
    BaichuanParams p;
    p.host = addr;
    p.username = user;
    p.password = password;
    return p;
}

void DeviceManager::fetchAlerts(int row)
{
    if (row < 0 || row >= m_entries.size()) {
        emit alertsLoaded(row, {});
        return;
    }
    const Entry &e = m_entries.at(row);
    if (!e.reolinkReady()) {
        emit alertsLoaded(row, {});
        return;
    }
    const BaichuanParams p = bcParams(e.rec.addr, e.rec.username, e.password);
    const int ch = e.channel;
    const device::CameraCommands commands = m_commands;
    m_pending.addFuture(QtConcurrent::run([this, row, p, ch, commands] {
        const QVariantMap m = commands.fetchAlerts(p, ch);
        QMetaObject::invokeMethod(
            this, [this, row, m] {
                if (m.contains(QStringLiteral("push")))
                    m_pushEnabled[row] = m.value(QStringLiteral("push")).toInt();
                emit alertsLoaded(row, m);
            }, Qt::QueuedConnection);
    }));
}

bool DeviceManager::pushEnabledFor(qint64 hostId, int channel) const
{
    const int row = rowOfHostChannel(hostId, channel);
    // Unknown (cache cold, or push not reported by the device) => allow the
    // notification; only an explicit disabled (0) suppresses it.
    return m_pushEnabled.value(row, 1) != 0;
}

QString DeviceManager::mdZoneBits(const QString &valueTable, int cells) const
{
    const QByteArray raw = QByteArray::fromBase64(valueTable.toLatin1());
    QString out;
    out.reserve(cells);
    for (int i = 0; i < cells; ++i) {
        const int byte = i / 8;
        const bool on = byte < raw.size()
                        && ((static_cast<quint8>(raw[byte]) >> (7 - (i % 8))) & 1);
        out += on ? QLatin1Char('1') : QLatin1Char('0');
    }
    return out;
}

QString DeviceManager::mdZoneTable(const QString &bits) const
{
    const int cells = bits.size();
    QByteArray raw((cells + 7) / 8, '\0');
    for (int i = 0; i < cells; ++i)
        if (bits.at(i) == QLatin1Char('1'))
            raw[i / 8] = static_cast<char>(static_cast<quint8>(raw[i / 8])
                                           | (1 << (7 - (i % 8))));
    return QString::fromLatin1(raw.toBase64());
}

void DeviceManager::warmPushCache()
{
    int i = 0;
    for (int row = 0; row < m_entries.size(); ++row) {
        const Entry &e = m_entries.at(row);
        if (!e.reolinkReady())
            continue;
        m_pushWarmed = true;   // a camera is ready; don't re-warm every poll cycle
        // Stagger so we never open several Baichuan settings sessions at once.
        QTimer::singleShot(i++ * 1200, this, [this, row] { fetchAlerts(row); });
    }
}

void DeviceManager::setAlertEnable(int row, const QString &kind, bool enable)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const Entry &e = m_entries.at(row);
    if (!e.isAdmin) {
        emit settingApplied(row, kind, false, tr("requires an administrator account"));
        return;
    }
    quint32 getCmd = 0, setCmd = 0;
    if (!device::CameraCommands::alertCommandIds(kind, getCmd, setCmd))
        return;
    const BaichuanParams p = bcParams(e.rec.addr, e.rec.username, e.password);
    const int ch = e.channel;
    const device::CameraCommands commands = m_commands;
    m_pending.addFuture(QtConcurrent::run([this, row, p, ch, kind, enable, commands] {
        const bool ok = commands.setAlert(p, ch, kind, enable).ok;
        QMetaObject::invokeMethod(
            this,
            [this, row, kind, ok, enable] {
                if (ok && kind == QLatin1String("push"))
                    m_pushEnabled[row] = enable ? 1 : 0;
                emit settingApplied(row, kind, ok,
                                    ok ? QString() : tr("couldn't reach the device"));
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::fetchRecSchedule(int row)
{
    if (row < 0 || row >= m_entries.size()) {
        emit recScheduleLoaded(row, {});
        return;
    }
    const Entry &e = m_entries.at(row);
    if (!e.reolinkReady()) {
        emit recScheduleLoaded(row, {});
        return;
    }
    const BaichuanParams p = bcParams(e.rec.addr, e.rec.username, e.password);
    const int ch = e.channel;
    const device::CameraCommands commands = m_commands;
    m_pending.addFuture(QtConcurrent::run([this, row, p, ch, commands] {
        const QVariantMap m = commands.fetchRecSchedule(p, ch);
        QMetaObject::invokeMethod(
            this, [this, row, m] { emit recScheduleLoaded(row, m); }, Qt::QueuedConnection);
    }));
}

void DeviceManager::writeRecSchedule(int row, const QString &type, const QString &table)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const Entry &e = m_entries.at(row);
    if (!e.isAdmin) {
        emit settingApplied(row, QStringLiteral("SetRecSchedule"), false,
                            tr("requires an administrator account"));
        return;
    }
    const BaichuanParams p = bcParams(e.rec.addr, e.rec.username, e.password);
    const int ch = e.channel;
    const device::CameraCommands commands = m_commands;
    m_pending.addFuture(QtConcurrent::run([this, row, p, ch, type, table, commands] {
        const device::CommandResult r = commands.writeRecSchedule(p, ch, type, table);
        QString err;
        switch (r.failure) {
        case device::CommandFailure::None: break;
        case device::CommandFailure::Unreachable: err = tr("couldn't reach the device"); break;
        case device::CommandFailure::NotFound: err = tr("schedule type not found on this device"); break;
        default: err = tr("device rejected the schedule (status %1)").arg(r.status); break;
        }
        const bool ok = r.ok;
        QMetaObject::invokeMethod(
            this,
            [this, row, ok, err] {
                emit settingApplied(row, QStringLiteral("SetRecSchedule"), ok, err);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::fetchBcConfig(int row, int cmdId, const QString &reqBody)
{
    if (row < 0 || row >= m_entries.size()) {
        emit bcConfigLoaded(row, cmdId, {});
        return;
    }
    const Entry &e = m_entries.at(row);
    if (!e.reolinkReady()) {
        emit bcConfigLoaded(row, cmdId, {});
        return;
    }
    const BaichuanParams p = bcParams(e.rec.addr, e.rec.username, e.password);
    const int ch = e.channel;
    const QByteArray body = reqBody.toUtf8();
    const device::CameraCommands commands = m_commands;
    m_pending.addFuture(QtConcurrent::run([this, row, cmdId, p, ch, body, commands] {
        const QVariantMap m = commands.fetchConfig(p, ch, static_cast<quint32>(cmdId), body);
        QMetaObject::invokeMethod(
            this, [this, row, cmdId, m] { emit bcConfigLoaded(row, cmdId, m); },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::writeBcConfig(int row, int getCmd, int setCmd, const QVariantMap &changes,
                                  const QString &reqBody)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const Entry &e = m_entries.at(row);
    const QString label = QStringLiteral("Set%1").arg(setCmd);
    if (!e.isAdmin) {
        emit settingApplied(row, label, false, tr("requires an administrator account"));
        return;
    }
    const BaichuanParams p = bcParams(e.rec.addr, e.rec.username, e.password);
    const int ch = e.channel;
    const QByteArray body = reqBody.toUtf8();
    const device::CameraCommands commands = m_commands;
    m_pending.addFuture(QtConcurrent::run(
        [this, row, getCmd, setCmd, p, ch, changes, body, label, commands] {
            const bool ok = commands
                                .writeConfig(p, ch, static_cast<quint32>(getCmd),
                                             static_cast<quint32>(setCmd), changes, body)
                                .ok;
            QMetaObject::invokeMethod(
                this,
                [this, row, label, ok] {
                    emit settingApplied(row, label, ok,
                                        ok ? QString() : tr("couldn't reach the device"));
                },
                Qt::QueuedConnection);
        }));
}

QString DeviceManager::nameAt(int row) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    return e.chanName.isEmpty() ? e.rec.name : e.chanName;
}

QString DeviceManager::eventClipUrl(qint64 hostId, int channel, qint64 timestamp) const
{
    return eventClipUrl(hostId, channel, timestamp, frigate::kClipPreSecs, frigate::kClipPostSecs);
}

QString DeviceManager::eventClipUrl(qint64 hostId, int channel, qint64 timestamp, int preSecs,
                                    int postSecs) const
{
    for (const Entry &e : m_entries)
        if (e.rec.id == hostId && e.channel == channel && e.kindCaps().frigateApi)
            return frigate::clipUrl(e.rec.addr, e.rec.port, e.serverName(), timestamp, preSecs, postSecs);
    return {};
}

QString DeviceManager::liveUrl(int row, bool mainStream)
{
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    device::LiveSource src;
    src.kind = device::kindFromString(e.rec.kind);
    src.addr = e.rec.addr;
    src.port = e.rec.port;
    src.username = e.rec.username;
    src.password = e.password;
    src.restreamUser = e.restreamUser;
    src.restreamPassword = e.restreamPassword;
    src.chanName = e.serverName();
    src.channel = e.channel;
    src.mainCodec = e.mainCodec;
    src.mainRestream = e.mainRestream;
    src.primed = e.primed;
    return device::liveUrl(src, mainStream);
}

} // namespace rl
