#pragma once

#include "core/CredentialStore.h"
#include "core/Database.h"
#include "CameraCommands.h"
#include "DeviceKind.h"
#include "Detections.h"
#include "EventHistory.h"
#include "EventSwitches.h"
#include "HostSession.h"
#include "DownloadSource.h"
#include "protocol/ReolinkApi.h"
// Not forward-declared on purpose — see the note by the other forward
// declarations below.
#include "media/StreamPlayer.h"
#include "media/LiveBuffer.h"
#include "media/TalkSession.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QFutureSynchronizer>
#include <QHash>
#include <QSet>
#include <QSize>
#include <QTimer>

#include <memory>

namespace rl {
// These two are only ever held behind pointers in private members and never
// appear in a signal/slot/Q_INVOKABLE signature, so moc needs nothing from
// them and a forward declaration is right.
//
// StreamPlayer is different and is INCLUDED above: it appears as a pointer
// parameter of Q_INVOKABLE methods, so moc instantiates Qt's metatype traits
// for rl::StreamPlayer* in its own translation unit. Qt decides
// IsPointerToTypeDerivedFromQObject by overload resolution against QObject*,
// which silently answers "false" for an incomplete type and "true" once the
// class is visible — so the same template specialisation had different values
// in the moc TU than in the TUs that include StreamPlayer.h. That is an ODR
// violation (GCC 16 reports it as -Wsfinae-incomplete) and it left the
// registered metatype without its PointerToQObject flag.
class BaichuanClient;
class BaichuanEventListener;
class FrigateLive;
class ReolinkHttpClient;
}

namespace rl {

// The device tree behind the sidebar: hosts from the database, exposed to QML.
// Adding a camera/NVR validates it over the HTTP-CGI API on a worker thread
// (Login + GetDevInfo + GetEnc) and fills in name/model/codec. Credentials live
// in the keyring; the in-memory copy (loaded on a worker thread) lets liveUrl()
// build RTSP URLs without blocking the GUI thread on DBus.
class DeviceManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        AddrRole,
        KindRole,
        MultiChannelRole,
        ModelRole,
        OnlineRole,
        StatusRole,
        HostIdRole,
        HasPtzRole,
        HasPtzPresetRole,
        HasZoomRole,
        HasAudioRole,
        HasSirenRole,
        HasFloodlightRole,
        HasBatteryRole,
        HasTalkRole,
        IsAdminRole,
        BatteryPercentRole,
        BatteryChargingRole,
        ChannelRole,
        ProblemRole,
        RotationRole,
        IsFisheyeRole,
        ViewModeRole,
        HostNameRole,
    };

    DeviceManager(Database *db, CredentialStore *credentials, QObject *parent = nullptr);
    ~DeviceManager() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The event switches: which (recorder, camera, type) pairs are not logged at all.
    EventSwitches *eventSwitches() const { return m_eventSwitches; }
    // The one door every detection goes through on its way to the inbox, the
    // notifications and the Live highlights: a switched-off pair stops here.
    // `at`: when the source saw it (epoch seconds; 0 = now).
    void reportDetection(qint64 hostId, int channel, const QString &type, const QString &camera,
                         qint64 at = 0);

    // Camera/NVR by IP or hostname. Validates asynchronously; the row appears
    // immediately with status "connecting…".
    // Probe a device WITHOUT persisting anything: the Add Device dialog calls
    // this first so a typo'd address or rejected password becomes an inline
    // error instead of a phantom "connecting…" row. Result via testDeviceResult.
    Q_INVOKABLE void testDevice(const QString &addr, const QString &username,
                                const QString &password, bool https, int port);
    // User-driven revalidation of a host (sidebar "Reconnect").
    Q_INVOKABLE void reconnect(int row);
    // Fix a host's credentials in place (sidebar "Update credentials…"): stores
    // the new username/password and revalidates. Empty password keeps the stored
    // one (username-only fix).
    Q_INVOKABLE void updateCredentials(int row, const QString &username,
                                       const QString &password);
    // Manual view-rotation correction for a camera (degrees, multiples of 90),
    // persisted per hostId:channel. For streams whose rotation our heuristics
    // can't know (hub-attached doorbells and the like) — issue #3.
    Q_INVOKABLE void setRotationOverride(int row, int degrees);
    // Fisheye (ceiling-mounted 180° lens) cameras: whether a camera is one, and how its
    // picture is shown ("fisheye" = the raw circle, "ptz", "panorama", "dual", "quad",
    // "fisheye_ptz"). Both are persisted per hostId:channel. Until the user says
    // otherwise a camera counts as fisheye when its main stream is square.
    Q_INVOKABLE void setFisheye(int row, bool on);
    Q_INVOKABLE void setViewMode(int row, const QString &mode);

    Q_INVOKABLE void addDevice(const QString &addr, const QString &username,
                               const QString &password, bool https = true, int port = 0);
    // A Frigate server (http, no login): each enabled camera becomes a row.
    Q_INVOKABLE void addFrigate(const QString &name, const QString &host, int port = 8971,
                                const QString &username = {}, const QString &password = {},
                                const QString &restreamUser = {},
                                const QString &restreamPassword = {});
    // A Frigate server's go2rtc restream can require its own login (Basic), which is not the
    // Frigate login. Stores it in the keyring (empty user clears it) and reconnects the host
    // so its cameras reopen with it.
    Q_INVOKABLE void setRestreamLogin(int row, const QString &user, const QString &password);
    // The saved restream user of a Frigate host's row, for the edit dialog (never the password).
    Q_INVOKABLE QString restreamUserAt(int row) const;
    // Checks the address answers like Frigate; reports through testDeviceResult.
    Q_INVOKABLE void testFrigate(const QString &host, int port = 8971,
                                 const QString &username = {}, const QString &password = {});
    // Direct stream URL (rtsp://…, file …) — testing and generic-RTSP escape hatch.
    // Any embedded credentials are stripped to the keyring, not persisted in the DB.
    Q_INVOKABLE void addStreamUrl(const QString &name, const QString &url);
    Q_INVOKABLE void removeDevice(int row);
    // Give an NVR channel a name of the user's own, kept across restarts. Empty clears it
    // (the NVR's own name returns at the next refresh).
    Q_INVOKABLE void renameChannel(int row, const QString &name);

    // The recorded clip for a detection on a Frigate camera (empty for anything else,
    // whose recordings play through the Playback page).
    Q_INVOKABLE QString eventClipUrl(qint64 hostId, int channel, qint64 timestamp) const;
    // The same over a chosen window (the Events clip player, step 22).
    QString eventClipUrl(qint64 hostId, int channel, qint64 timestamp, int preSecs,
                         int postSecs) const;

    // Playable URL for a device row; empty when credentials aren't loaded yet.
    Q_INVOKABLE QString liveUrl(int row, bool mainStream = true);

    // GetEnc-declared display size for a channel's main/sub stream (e.g. main
    // 7680x2160). Lets StreamPlayer detect a transmitted-rotated stream by
    // comparing this against the decoded frame size. Invalid if not yet primed.
    Q_INVOKABLE QSize declaredSize(int row, bool mainStream) const;
    Q_INVOKABLE QString nameAt(int row) const;
    // What each camera's live pane costs against its Site budget, in Mbit/s:
    // { "hostId:channel": { sub: Mbit/s, main: Mbit/s, known: bool } } for every row.
    // `known` is false while the figure is a default (no GetEnc bitrate yet).
    Q_INVOKABLE QVariantMap streamCosts() const;
    // file: URL of a camera's last-seen live frame, "" when none has been kept.
    Q_INVOKABLE QString lastFrameUrl(qint64 hostId, int channel) const;
    Q_INVOKABLE int rowOfHost(qint64 hostId) const { return rowForHostId(hostId); }
    // The exact camera row for a host+channel (an NVR shares a host across cameras).
    Q_INVOKABLE int rowOfHostChannel(qint64 hostId, int channel) const
    {
        for (int i = 0; i < m_entries.size(); ++i)
            if (m_entries.at(i).rec.id == hostId && m_entries.at(i).channel == channel)
                return i;
        return rowForHostId(hostId);
    }
    // A URL stream or Frigate camera: plays from liveUrl() only (no Baichuan HD path).
    Q_INVOKABLE bool isDirectAt(int row) const
    {
        if (row < 0 || row >= m_entries.size())
            return false;
        return m_entries.at(row).kindCaps().direct();
    }
    // The Bosch camera's feed (recognised by its name); its controls come from Home Assistant.
    Q_INVOKABLE bool isBoschFeed(const QString &label) const { return device::looksLikeBoschFeed(label); }
    // The same for a row, by the camera's name on its server, so a rename in the app keeps
    // the Bosch controls (wayfinder ticket 25).
    Q_INVOKABLE bool isBoschFeedRow(int row) const;
    Q_INVOKABLE bool isAdminAt(int row) const
    {
        return row >= 0 && row < m_entries.size() && m_entries.at(row).isAdmin;
    }
    // This camera's channel index on its host (0 for a standalone camera; the NVR
    // channel for a fanned-out one). Lets QML target a live action (siren, etc.) at
    // the correct camera instead of a hardcoded channel 0.
    Q_INVOKABLE int channelOf(int row) const
    {
        return (row >= 0 && row < m_entries.size()) ? m_entries.at(row).channel : 0;
    }

    // Host-level summary (an NVR or a standalone camera), keyed by hostId — feeds
    // the sidebar's NVR header and the NVR properties dialog. Keys: name, kind,
    // model, addr, port, https, username, online, channelCount, onlineCount,
    // isAdmin, firstRow.
    Q_INVOKABLE QVariantMap hostInfo(qint64 hostId) const;
    // Per-camera summary for the camera properties dialog. Keys: name, hostName,
    // hostId, channel, kind, model, codec, mainSize, subSize, uid, online, isAdmin,
    // and cap* booleans (ptz/zoom/audio/siren/floodlight/battery/talk).
    Q_INVOKABLE QVariantMap cameraInfo(int row) const;
    // Distinct host ids in list order (NVRs and standalone cameras), for grouping.
    Q_INVOKABLE QVariantList hostIds() const;
    // Channels of each host whose camera list is definitive (a validated Frigate server: its
    // configured cameras), keyed by host id as text, for SiteSelection::pruneCameras. NVRs
    // are left out: they list only online channels.
    Q_INVOKABLE QVariantMap listedCameras() const;
    // The property/site name a host was added under (e.g. "Lascelles"), or empty
    // if hostId is unknown. Distinct from a camera's own display name.
    Q_INVOKABLE QString hostName(qint64 hostId) const;
    // The event types this camera can report, in display order ("person", "vehicle",
    // "pet", "visitor", "motion", "status"): as read at this launch's validation, else as
    // kept from an earlier one. Empty for a bare stream, which raises no events.
    Q_INVOKABLE QStringList eventTypesFor(qint64 hostId, int channel) const;
    // Distinct property/site names in list order, for a property filter.
    Q_INVOKABLE QStringList propertyNames() const;
    // Channel -> raw config-key camera name for a host (e.g. Frigate's "bosch_camera",
    // not the pretty "Bosch Camera" NameRole shows). Frigate's own APIs need the raw
    // key; each entry is {"channel": int, "camera": QString}.
    Q_INVOKABLE QVariantList channelsForHost(qint64 hostId) const;

    // Live controls (channel 0). Fire-and-forget on the worker pool.
    Q_INVOKABLE void ptzMove(int row, const QString &op, int speed = 32);
    Q_INVOKABLE void ptzStop(int row);
    Q_INVOKABLE void ptzPreset(int row, int presetId);
    // Capture a JPEG snapshot; emits snapshotSaved/snapshotFailed.
    Q_INVOKABLE void snapshot(int row);
    // Quiet snapshot for an event thumbnail: fetches a Snap JPEG, downscales it,
    // writes it under Paths::thumbnailsDir(), and emits eventThumbnailReady with
    // the caller's cookie (no snapshotSaved toast). Silent no-op on failure.
    void captureEventThumbnail(qint64 hostId, int channel, qint64 eventId);

    // Playback: search a day's recordings (emits recordingsFound with a list of
    // {start,end,type,name} where start/end are seconds into the day), and build
    // a playable URL for a recording file handle.
    Q_INVOKABLE void searchRecordings(int row, int year, int month, int day);
    // The same search for Live replay (wayfinder/live-replay ticket 07): answered with
    // replayDayFound / replayDayFailed, leaving Playback's results alone.
    Q_INVOKABLE void searchReplayDay(int row, int year, int month, int day);
    // A direct (Frigate) camera's playable URL from startEpoch (Unix seconds); "" otherwise.
    Q_INVOKABLE QString playbackUrl(int row, qint64 startEpoch);

    // What the downloads queue needs to fetch this camera's recordings (invalid if the
    // device is not ready or has no recordings).
    DownloadSource downloadSource(int row);

    // Native (Baichuan/TCP 9000) recorded playback: stream the recording from
    // startEpoch straight into `player` — realtime, frame-accurate, full HEVC main.
    // `reverse`: backwards from startEpoch, in windows (wayfinder/playback-controls ticket 12).
    // `forwardWindows`: forwards in windows the player paces (HD fast forward, live-replay 09).
    Q_INVOKABLE void startBaichuanPlayback(int row, qint64 startEpoch, rl::StreamPlayer *player,
                                           bool mainStream = true, bool reverse = false,
                                           bool forwardWindows = false);
    // Native LIVE view over Baichuan (cmd 3 Preview), for the maximized pane's HD
    // stream. The NVR's RTSP output for 8K duo main streams is itself corrupt
    // (verified: raw ffmpeg -c copy captures carry invalid NALUs), so HD live must
    // go over Baichuan — the transport the official clients use. The NVR only
    // allows ~1 concurrent Baichuan session; callers fall back to RTSP on error.
    Q_INVOKABLE void startBaichuanLive(int row, rl::StreamPlayer *player,
                                       bool mainStream = true);
    // The camera's live buffer (wayfinder/live-replay ticket 08b): its last 5 minutes of live
    // frames, kept while its Live pane streams; null for a camera with no recorder behind it
    // (Frigate, bare streams). All buffers share LiveBufferPool's cap.
    std::shared_ptr<LiveBuffer> liveBuffer(int row);
    std::shared_ptr<LiveBuffer> findLiveBuffer(int row) const;
    // Live replay from the buffer: whether it holds the moment (ms since the epoch); its
    // footage as [{start, end}] epoch seconds (for Live replay's strip and segments); and
    // playing it from the moment into `player`, backwards or forwards (media/BufferReplay.h).
    bool liveBufferHas(int row, qint64 epochMs, int direction) const;
    Q_INVOKABLE QVariantList liveBufferSpans(int row) const;
    void startLiveBufferPlayback(int row, qint64 epochMs, int direction, StreamPlayer *player);

    // Two-way talk: open a Baichuan talk session to this camera's speaker and start
    // streaming the microphone into it. `session` reports progress (Connecting ->
    // Talking, or Error with a reason). Stop with session.stop(). No-op for a device
    // that isn't ready or has no talk capability.
    Q_INVOKABLE void startTalk(int row, rl::TalkSession *session);

    // Settings: fetch a batch of Get* commands (emits settingsLoaded with a map
    // of cmd -> value) and apply one Set* command (emits settingApplied).
    Q_INVOKABLE void fetchSettings(int row, const QStringList &getCommands);
    Q_INVOKABLE void applySetting(int row, const QString &setCommand, const QVariantMap &param);

    // Doorbell quick replies. fetchQuickReplies asks the device for its clips (HTTP
    // first, then Baichuan cmd 347 — an NVR may answer only one) and emits
    // quickRepliesLoaded. playQuickReply plays one clip on this camera's channel and
    // emits quickReplyPlayed. Both target the row's own NVR channel.
    Q_INVOKABLE void fetchQuickReplies(int row);
    Q_INVOKABLE void playQuickReply(int row, int clipId);
    // Doorbell auto-reply (play a clip by itself when nobody answers): read the setting
    // and its clip choices; change it (needs an administrator account).
    Q_INVOKABLE void fetchAutoReply(int row);
    Q_INVOKABLE void setAutoReply(int row, bool enable, int fileId, int timeout);
    // Toggle the camera's white-LED / floodlight on<->off. Reads GetWhiteLed first
    // so only the on/off state flips, leaving brightness and the auto/schedule mode
    // untouched. Emits settingApplied("SetWhiteLed", ...) for UI feedback.
    Q_INVOKABLE void toggleFloodlight(int row);
    Q_INVOKABLE void reboot(int row);

    // Alert-action config (push / email / ftp enable) over the native Baichuan
    // protocol (TCP 9000). The NVR's HTTP-CGI 502s on these commands under load,
    // but the app-native BC transport the official clients use is reliable.
    // fetchAlerts reads the current enables (emits alertsLoaded with keys
    // ok/push/email/ftp, each 0/1 or -1); setAlertEnable toggles one over BC and
    // emits settingApplied(kind, ...). kind = "push" | "email" | "ftp".
    Q_INVOKABLE void fetchAlerts(int row);
    Q_INVOKABLE void setAlertEnable(int row, const QString &kind, bool enable);

    // Whether this camera's Push Notifications action is enabled, from a cache
    // warmed shortly after startup and updated whenever alerts are read/toggled.
    // Gates desktop notifications for new detections; unknown => treated enabled.
    bool pushEnabledFor(qint64 hostId, int channel) const;

    // Motion-detection zone (grid mask) helpers. The device stores the zone as a
    // base64 <valueTable> in the MdAlarm config: one bit per cell (1 = detect),
    // row-major over columns x rows. mdZoneBits decodes it to a '0'/'1' string of
    // `cells` chars for the editor; mdZoneTable re-encodes a bit string to the
    // base64 the device expects (write it back via writeBcConfig(row, 46, 47, ...)).
    Q_INVOKABLE QString mdZoneBits(const QString &valueTable, int cells) const;
    Q_INVOKABLE QString mdZoneTable(const QString &bits) const;

    // Generic settings over Baichuan: read a command's config into a flat
    // { tag: value } map (emits bcConfigLoaded), and read-modify-write leaf tags
    // (emits settingApplied("Set<setCmd>", ...)). reqBody is the optional GET
    // request XML some commands need (e.g. AI detection selects an ai_type).
    // These bypass the 502-prone HTTP-CGI, like the official apps.
    Q_INVOKABLE void fetchBcConfig(int row, int cmdId, const QString &reqBody = QString());
    // Weekly recording schedule (BC cmd 81/82): per-type 168-char '0'/'1'
    // tables (7 days x 24 hours). fetchRecSchedule emits recScheduleLoaded with
    // { enable, <type>: table } (types e.g. Normal/MD/people/vehicle/dog_cat);
    // writeRecSchedule RMWs one type's table (emits settingApplied).
    Q_INVOKABLE void fetchRecSchedule(int row);
    Q_INVOKABLE void writeRecSchedule(int row, const QString &type, const QString &table);
    Q_INVOKABLE void writeBcConfig(int row, int getCmd, int setCmd, const QVariantMap &changes,
                                   const QString &reqBody = QString());

signals:
    void countChanged();
    void deviceError(const QString &addr, const QString &message);
    // Outcome of testDevice. problem is "", "transport", "auth", "locked" or
    // "protocol" so the dialog can phrase the failure usefully.
    void testDeviceResult(bool ok, const QString &message, const QString &name,
                          const QString &model, const QString &problem);
    void snapshotSaved(int row, const QString &path);
    void snapshotFailed(int row, const QString &error);
    void recordingsFound(int row, const QVariantList &segments);
    void recordingDaysFound(int row, int year, int month, const QVariantList &days);
    void recordingsFailed(int row, const QString &error);
    void replayDayFound(int row, int year, int month, int day, const QVariantList &segments,
                        const QVariantList &monthDays);
    void replayDayFailed(int row, int year, int month, int day);
    void settingsLoaded(int row, const QVariantMap &values);
    void settingsFailed(int row, const QString &error);
    void settingApplied(int row, const QString &command, bool ok, const QString &error);
    // clips: [{id, name}]. error empty + no clips = the device has none; error set =
    // the list could not be read at all (offline, refused, unsupported).
    void quickRepliesLoaded(int row, const QVariantList &clips, int maxFiles, const QString &error);
    void quickReplyPlayed(int row, int clipId, bool ok, const QString &error);
    // setting: { enable, fileId (-1 = none), timeout }; empty with `error` set = unavailable.
    void autoReplyLoaded(int row, const QVariantMap &setting, const QVariantList &clips, const QString &error);
    void autoReplySaved(int row, bool ok, const QString &error);
    void alertsLoaded(int row, const QVariantMap &values);
    void bcConfigLoaded(int row, int cmdId, const QVariantMap &values);
    void recScheduleLoaded(int row, const QVariantMap &values);
    // Emitted on each detection 0->1 transition (feeds the event inbox).
    void detectionEvent(qint64 hostId, int channel, const QString &type, const QString &camera,
                        qint64 at);
    // A quiet event-thumbnail capture finished (path is a local JPEG).
    void eventThumbnailReady(qint64 eventId, const QString &path);
    // A camera (channel >= 0) or a whole host (channel == -1) changed
    // connectivity. name is the camera/host display name. For a host, `changed` lists
    // the cameras that actually flipped with it (empty for a single camera).
    void connectivityChanged(qint64 hostId, int channel, const QString &name, bool online,
                             const QVector<rl::device::CameraRef> &changed);

private slots:
    void pollDetections();

private:
    // One model row = one camera. A standalone camera is a host with a single
    // channel (0); an NVR fans out into one Entry per online channel, all sharing
    // the same host connection (hostId, addr, credentials, client).
    using Problem = device::Problem;
    using ChannelResult = device::ChannelResult;
    using Validation = device::Validation;

    struct Entry {
        HostRecord rec;    // host connection (shared across an NVR's channels)
        device::Capabilities kindCaps() const
        {
            return device::capabilities(device::kindFromString(rec.kind));
        }
        // A Reolink host whose credentials are loaded: HTTP/Baichuan commands may be sent.
        bool reolinkReady() const { return kindCaps().reolinkProtocol && primed; }
        int channel = 0;   // channel index on the host
        QString chanName;  // camera name for this channel
        QString sourceName; // the camera's own name on its server (Frigate key), kept through a rename
        QString serverName() const { return device::serverCameraName(sourceName, chanName); }
        bool online = false;
        QString status;
        Problem problem = Problem::Connecting; // until first validation lands
        int rotationOverride = 0;              // user view-rotation fix (degrees CW)
        int lensOverride = -1;                 // user's fisheye flag: -1 unset (auto), 0 no, 1 yes
        int lensRemembered = -1;               // lens seen on an earlier read: -1 none, 0 flat, 1 fisheye
        QString viewMode = QStringLiteral("fisheye"); // how a fisheye camera is shown
        QString mainCodec = QStringLiteral("h264"); // sub stream is always h264
        QSize mainSize;                             // GetEnc-declared main resolution
        QSize subSize;                              // GetEnc-declared sub resolution
        int mainKbps = 0;                           // GetEnc-configured bitrates, 0 = unknown
        int subKbps = 0;
        QString uid;                                // per-camera UID (Baichuan playback)
        QString mainRestream;                       // Frigate: go2rtc stream for HD ("" = own)
        QString password;                           // in-memory only (keyring at rest)
        QString restreamUser;                       // Frigate go2rtc login (keyring at rest)
        QString restreamPassword;
        bool primed = false;                        // password loaded?
        bool validated = false;                     // channel list came from the HTTP API
        bool degraded = false;                      // channel list came from Baichuan only
        api::ChannelCaps caps;                      // this channel's capabilities
        QStringList eventTypes;                     // event types it reports (validation)
        bool talk = false;                          // channel supports two-way audio
        bool isAdmin = false;                       // logged-in user may edit settings
        api::BatteryInfo battery;                   // battery/solar state (if any)
        // Persistent authenticated client for the host, shared by its channels.
        std::shared_ptr<ReolinkHttpClient> client;
    };

    // Runs on a worker: loads the password from the keyring (or stores newPassword
    // first), logs in, fetches device info + encoding + abilities, updates the row
    // on the GUI thread. storeNew persists newPassword to the keyring first.
    // Give each Frigate camera its kept number (wayfinder ticket 24) and keep new ones.
    void numberFrigateCameras(qint64 hostId, QVector<ChannelResult> &channels);
    void validateAsync(qint64 hostId, const QString &newPassword = QString(),
                       bool storeNew = false);
    Validation validateFrigate(const HostRecord &rec, const QString &password);
    void searchFrigateRecordings(int row, int year, int month, int day);
    void pollFrigate();
    int rowForHostId(qint64 hostId) const;
    void warmPushCache();   // fetch each camera's push state once, staggered
    void applyConnectivity(qint64 hostId, bool transportOk, const QHash<int, bool> &chanOnline);
    // Shared body of startBaichuanPlayback/startBaichuanLive (startEpoch<=0 = live).
    void searchRecorderDay(int row, int year, int month, int day, bool forReplay);
    void startBaichuan(int row, qint64 startEpoch, rl::StreamPlayer *player, bool mainStream,
                       bool reverse = false, bool forwardWindows = false);
    void applyValidation(qint64 hostId, const Validation &v);
    void postValidation(qint64 hostId, const Validation &v); // marshals to GUI thread
    std::shared_ptr<ReolinkHttpClient> clientFor(int row);

    Database *m_db;
    CredentialStore *m_credentials;
    EventSwitches *m_eventSwitches;
    QVector<Entry> m_entries;
    QFutureSynchronizer<void> m_pending; // drains in-flight validations at teardown

    QTimer m_pollTimer;
    std::shared_ptr<LiveBufferPool> m_liveBuffers = std::make_shared<LiveBufferPool>();
    QTimer m_liveBufferLog;   // a line a minute: what each live buffer holds
    // Frigate hosts: the cameras the last validation listed, and when the poll last re-read
    // the list (epoch secs), so a camera added or removed on the server shows up by itself.
    QHash<qint64, QStringList> m_frigateListed;
    QHash<qint64, qint64> m_frigateListCheckedAt;
    // Everything about what a host has detected: edges, push-over-poll takeover, Frigate's
    // watermark and burst gate (see Detections.h). The sockets below only feed it.
    QHash<qint64, device::HostDetections> m_detections;
    // Baichuan push (wayfinder/detection-highlights ticket 06): one listener per Reolink
    // host. While a host's is subscribed, the poll stops asking it for motion/AI state.
    QHash<qint64, BaichuanEventListener *> m_pushListeners;
    void ensurePushListener(const Entry &e);
    void stopPushListener(qint64 hostId);
    void onAlarmEvents(qint64 hostId, const QByteArray &xml);
    // Report each normalised detection (Frigate names are prettified).
    void emitDetections(qint64 hostId, const QVector<device::Detection> &found, bool frigateHost);
    // Frigate's /ws websocket per Frigate host (ticket 07): new events at once; the poll
    // still runs as catch-up and connectivity check.
    QHash<qint64, FrigateLive *> m_frigateLive;
    void ensureFrigateLive(qint64 hostId, const QString &host, int port);
    void onFrigateLiveEvent(qint64 hostId, const QString &camera, const QString &label, double start);
    QHash<QString, int> frigateChannels(qint64 hostId) const; // camera key -> channel
    QHash<int, int> m_pushEnabled;  // row -> push enable (1/0/-1) for notif gating
    // Per-host login, retry-backoff and poll-health rules (see HostSession.h).
    device::HostSessions m_sessions;
    BaichuanOpener m_baichuan = realBaichuanOpener();
    device::CameraCommands m_commands{m_baichuan};
    void probeHostAlive(qint64 hostId, const QString &hostName);
    void markHostUnreachable(qint64 hostId, const QString &hostName);
    bool m_pushWarmed = false;      // one-time warm of the push cache after priming

};

} // namespace rl
