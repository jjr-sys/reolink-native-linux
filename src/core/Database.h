#pragma once

#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <utility>

namespace rl {

struct HostRecord {
    qint64 id = -1;
    QString kind;     // "camera" | "nvr" | "stream" (direct URL, for testing/generic RTSP)
    QString name;
    QString addr;     // IP/hostname, or full URL when kind == "stream"
    int port = 443;
    bool https = true;
    QString username;
    QString model;
};

struct EventRecord {
    qint64 id = -1;
    qint64 hostId = -1;
    int channel = 0;
    qint64 timestamp = 0; // Unix seconds (UTC)
    QString type;         // "motion" | "person" | "vehicle" | "pet" | "visitor" |
                          // "online" | "offline" (Status)
    QString camera;       // device name at event time
    QString thumbnail;    // optional JPEG path
};

// One event switch: a recorder's camera and a stored event type.
struct EventSwitchKey {
    qint64 hostId = -1;
    int channel = 0;
    QString type;
    bool operator==(const EventSwitchKey &o) const
    {
        return hostId == o.hostId && channel == o.channel && type == o.type;
    }
};

inline size_t qHash(const EventSwitchKey &k, size_t seed = 0)
{
    return qHashMulti(seed, k.hostId, k.channel, k.type);
}

// SQLite-backed application store: devices, channels, layouts.
// Schema is versioned; open() runs pending migrations.
class Database
{
public:
    // connectionName lets tests open isolated databases.
    explicit Database(const QString &filePath,
                      const QString &connectionName = QStringLiteral("main"));
    ~Database();

    bool open();
    QString lastError() const { return m_lastError; }
    int schemaVersion() const;

    qint64 addHost(const HostRecord &rec);
    bool removeHost(qint64 id);
    bool updateHost(const HostRecord &rec);
    QVector<HostRecord> hosts() const;

    // A camera name the user chose for an NVR channel. It wins over the name the NVR
    // reports (or the "Channel N" placeholder when the NVR cannot say). Empty clears it.
    QHash<int, QString> channelNameOverrides(qint64 hostId) const;
    bool setChannelNameOverride(qint64 hostId, int channel, const QString &name);

    // Supported event types per channel ("person", ..., "motion", "status"), kept so a
    // camera offline at launch still knows what it can report. Separate from the name
    // override: clearing one keeps the other.
    QHash<int, QStringList> channelEventTypes(qint64 hostId) const;
    bool setChannelEventTypes(qint64 hostId, int channel, const QStringList &types);

    // Event switches: the pairs switched off, and switching one off or on.
    QVector<EventSwitchKey> switchedOffEvents() const;
    bool setEventSwitchedOff(const EventSwitchKey &key, bool off);

    // Events (motion / AI detections / Status), newest first.
    qint64 addEvent(const EventRecord &rec);
    // The newest `limit` events, optionally narrowed to one type, some hosts (empty = all)
    // and one camera name.
    QVector<EventRecord> recentEvents(int limit = 500, const QString &type = {},
                                      const QList<qint64> &hostIds = {},
                                      const QString &camera = {}) const;
    // One camera's events in [from, to), newest first; `types` empty = every type.
    QVector<EventRecord> eventsBetween(qint64 hostId, int channel, qint64 from, qint64 to,
                                       const QStringList &types = {}) const;
    // Counts by stored type: one camera's in [from, to), and every host's in [from, to).
    QHash<QString, int> eventCountsBetween(qint64 hostId, int channel, qint64 from, qint64 to) const;
    QHash<qint64, QHash<QString, int>> eventCountsByHost(qint64 from, qint64 to) const;
    bool hostHasEvents(qint64 hostId) const;
    // The event on this camera nearest `ts`, if one is less than `windowSecs` away.
    std::optional<EventRecord> eventNear(qint64 hostId, int channel, qint64 ts,
                                         qint64 windowSecs) const;
    // The camera names a host's events are filed under, and renaming them in place.
    QStringList eventCameras(qint64 hostId) const;
    bool renameEventCamera(qint64 hostId, const QString &from, const QString &to);
    bool clearEvents();
    // Age-based retention at `now`: motion older than `motionKeepSecs`, everything else
    // older than `otherKeepSecs`. Returns the (non-empty) thumbnail paths of the removed
    // rows so the caller can delete those files too.
    QStringList purgeExpiredEvents(qint64 now, qint64 motionKeepSecs, qint64 otherKeepSecs);
    // The per-recorder cap counts every event except motion.
    int countCappedEvents(qint64 hostId) const;
    // Deletes all but the newest `keep` capped (non-motion) events of one host, returning
    // the thumbnail paths of the removed rows. Motion is left to the age purge.
    QStringList trimHostEvents(qint64 hostId, int keep);
    // One camera's stored events of one type: how many, with their snapshot paths
    // appended to `thumbnails`; and deleting them, returning those paths.
    int countEvents(qint64 hostId, int channel, const QString &type,
                    QStringList *thumbnails = nullptr) const;
    QStringList deleteEvents(qint64 hostId, int channel, const QString &type);
    // False when the event no longer exists (trimmed while its thumbnail was on the way).
    bool setEventThumbnail(qint64 id, const QString &path);
    bool setEventCamera(qint64 id, const QString &camera);

    // App runs: one row per launch, its last_seen stamped by a heartbeat, so the time the
    // app was not running (and so stored no events) can be shown.
    qint64 beginAppRun(qint64 now);
    bool touchAppRun(qint64 id, qint64 now);
    // (started, last seen) of the runs overlapping [from, to), oldest first.
    QVector<std::pair<qint64, qint64>> appRuns(qint64 from, qint64 to) const;
    // When the first recorded run started; 0 when none is recorded.
    qint64 firstAppRunStart() const;

private:
    bool migrate();
    QSqlDatabase db() const;

    QString m_filePath;
    QString m_connectionName;
    QString m_lastError;
};

} // namespace rl
