#pragma once

#include "core/Database.h"
#include "core/Notifier.h"
#include "device/EventHistory.h"
#include "device/FrigateApi.h"

#include <QAbstractListModel>
#include <QFutureSynchronizer>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QTimer>

namespace rl {

class DeviceManager;

// The event inbox: persists detection events (from DeviceManager's poller) and
// exposes the newest of them to QML, with an unread counter. The store keeps its own history (motion 24 h; everything else 30 days under a
// per-recorder cap, see EventHistory.h); the list and the per-day lookups query it.
class EventManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int unread READ unread NOTIFY unreadChanged)
    // App-level master switch for desktop pop-ups (independent of the device Push flag).
    Q_PROPERTY(bool desktopNotifications READ desktopNotifications WRITE setDesktopNotifications NOTIFY desktopNotificationsChanged)
    // How many of the newest events the list shows.
    Q_PROPERTY(int listLimit READ listLimit CONSTANT)

public:
    enum Role {
        TypeRole = Qt::UserRole + 1,
        CameraRole,
        TimestampRole,
        TimeTextRole,
        ThumbnailRole,
        HostIdRole,
        ChannelRole,
        PropertyRole,
    };

    EventManager(Database *db, DeviceManager *devices, QObject *parent = nullptr);
    ~EventManager() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int unread() const { return m_unread; }
    // The store, for the Events page's columns (EventColumn), which query it per day.
    Database *database() const { return m_db; }
    static constexpr int kListLimit = 500;
    int listLimit() const { return kListLimit; }

    bool desktopNotifications() const { return m_desktopNotifications; }
    void setDesktopNotifications(bool on);

    Q_INVOKABLE void markAllRead();
    // Seconds-into-day of each detection event for this camera on the given day
    // (dayStart = local midnight epoch). Feeds the playback timeline's markers.
    Q_INVOKABLE QVariantList eventTimesFor(qint64 hostId, int channel, qint64 dayStart) const;
    // The spans of the local day starting at dayStart when the app was not running (and
    // so stored no events), as {from, to} epoch maps.
    Q_INVOKABLE QVariantList notRunningSpans(qint64 dayStart) const;
    // Each recorder's events on the local day starting at dayStart, by stored type:
    // { "hostId": { type: count } }.
    Q_INVOKABLE QVariantMap dayCountsByHost(qint64 dayStart) const;
    Q_INVOKABLE void clear();
    // Event switches. What switching a camera's event type off would delete:
    // { count, bytes } (stored events, snapshot bytes on disk).
    Q_INVOKABLE QVariantMap switchStats(qint64 hostId, int channel, const QString &type) const;
    // Switches the pair off, then deletes its stored events and their snapshots. False
    // when Status (nothing changes), when the switch can't be saved (nothing changes),
    // or when the delete fails (the pair is off, its stored events remain).
    Q_INVOKABLE bool switchOff(qint64 hostId, int channel, const QString &type);
    // Switches the pair back on: it logs afresh from now (what was deleted stays gone).
    Q_INVOKABLE bool switchOn(qint64 hostId, int channel, const QString &type);
    // Backfills the inbox from a Frigate server's own event history (unlike
    // onDetection, which only ever files what was seen live since the app was last
    // connected). Filed with each event's real historical timestamp, deduplicated
    // against what's already stored, and still subject to the retention window.
    // No-op for a non-Frigate host or one the manager doesn't know about.
    Q_INVOKABLE void syncFrigateHistory(qint64 hostId);
    // Convenience: runs syncFrigateHistory() for every known Frigate host.
    Q_INVOKABLE void syncAllFrigateHistory();

    // Runs on the GUI thread once syncFrigateHistory()'s worker thread has the raw
    // events back: applies the same burst-gate as live polling, skips switched-off
    // pairs and anything already covering that moment, and files the rest with their
    // real timestamps. Public for tests; the app reaches it only through the sync.
    void applyFrigateHistory(qint64 hostId, const QHash<QString, int> &channelOf,
                             const QVector<frigate::Event> &events);

signals:
    void desktopNotificationsChanged();
    void countChanged();
    void unreadChanged();
    // Stored events changed: one recorder's (an event filed, a thumbnail landed), or
    // anyone's (hostId -1: a purge, a trim, a clear, a history sync).
    void storedChanged(qint64 hostId);
    // The user clicked a desktop notification — jump to this event's playback.
    void eventActivated(qint64 hostId, int channel, qint64 timestamp);

private slots:
    void onDetection(qint64 hostId, int channel, const QString &type, const QString &camera);
    void onThumbnailReady(qint64 eventId, const QString &path);
    void onConnectivity(qint64 hostId, int channel, const QString &name, bool online,
                        const QVector<rl::device::CameraRef> &changed);

private:
    void reload();
    // Files one event: stores it, puts it at the top of the list, and keeps its recorder
    // under the cap.
    void fileEvent(EventRecord &rec);
    // The per-recorder cap: counts capped (non-motion) events in memory and trims the
    // recorder only once it goes over.
    void enforceCap(qint64 hostId, const QString &type);
    void dropThumbnails(const QStringList &paths);
    // One-time cleanup: events stored under a Frigate camera's raw config key
    // ("bosch_camera") before the display-name fix shipped are rewritten in
    // place in the database to the pretty form ("Bosch Camera")
    // so they line up with the filter dropdown and the property/camera shown
    // on the row. No-op once everything is already in the pretty form.
    void normalizeStoredCameraNames();
    // Age-based purge of the whole store (motion 24 h, the rest 30 days). Deletes the
    // removed rows' thumbnail files and reloads the list if anything went.
    void enforceRetention();
    // Backfilled events have no live thumbnail to grab (the camera's not showing that
    // moment any more), so this pulls Frigate's own saved frame for each one instead —
    // (local db event id, Frigate's own event id) pairs, fetched on a worker thread.
    void fetchHistoryThumbnails(qint64 hostId, const QVector<QPair<qint64, QString>> &ids);
    // A freshly-added Frigate host's cameras are discovered asynchronously (a worker
    // thread fetches /api/config after the constructor returns), so the one-time
    // catch-up can't run at construction time — channelsForHost() would still see
    // only the generic placeholder entry, not real camera keys. Called again on every
    // DeviceManager::countChanged, it re-checks any host not yet looked at and, once
    // Frigate reports it online (cameras known), runs the catch-up if it has no events.
    void checkFrigateCatchup();

    Database *m_db;
    DeviceManager *m_devices;
    bool m_desktopNotifications = true;
    Notifier m_notifier;             // desktop notifications for new detections
    QTimer m_retentionTimer;         // hourly age-based purge
    QTimer m_runTimer;               // stamps this app run's last_seen
    qint64 m_runId = -1;             // this launch's app_runs row
    QFutureSynchronizer<void> m_pending; // drains in-flight history syncs at teardown
    QVector<EventRecord> m_rows;     // the list: newest first
    QHash<qint64, int> m_cappedCount; // recorder -> capped events, counted lazily
    QSet<qint64> m_frigateHistoryChecked; // hosts already considered for the one-time catch-up
    int m_unread = 0;
};

} // namespace rl
