#pragma once

#include "core/Database.h"
#include "core/Notifier.h"
#include "device/EventHistory.h"
#include "device/FirstSwitchOffs.h"
#include "device/FrigateApi.h"
#include "device/Inbox.h"
#include "device/OfflineAlerts.h"

#include <QAbstractListModel>
#include <QFutureSynchronizer>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QTimer>

namespace rl {

class DeviceManager;
class EventSwitches;

// The event inbox's list: files detection, history and status events through the Inbox
// (which decides what is stored) and exposes the newest of them to QML, with an unread
// counter and desktop notices. The store keeps its own history (motion 24 h; everything else 30 days under a
// per-recorder cap, see EventHistory.h); the list and the per-day lookups query it.
class EventManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int unread READ unread NOTIFY unreadChanged)
    // App-level master switch for desktop pop-ups (independent of the device Push flag).
    Q_PROPERTY(bool desktopNotifications READ desktopNotifications WRITE setDesktopNotifications NOTIFY desktopNotificationsChanged)
    // Desktop offline alerts (wayfinder/urgent-offline-alerts 05): its own switch, on by
    // default, independent of desktopNotifications.
    Q_PROPERTY(bool offlineAlerts READ offlineAlerts WRITE setOfflineAlerts NOTIFY offlineAlertsChanged)
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
    // The store, for the Events page's columns (EventFeed), which query it.
    Database *database() const { return m_db; }
    // The event switches (DeviceManager's), for the Events page's models.
    EventSwitches *switches() const;
    static constexpr int kListLimit = 500;
    int listLimit() const { return kListLimit; }

    bool desktopNotifications() const { return m_desktopNotifications; }
    void setDesktopNotifications(bool on);
    bool offlineAlerts() const { return m_offlineAlerts; }
    void setOfflineAlerts(bool on);

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
    // Whether this camera's event type is switched off (not logged at all).
    Q_INVOKABLE bool isSwitchedOff(qint64 hostId, int channel, const QString &type) const;
    // Copies an event's snapshot to ~/Pictures/Reolink as "<camera> <date time>.jpg" (never
    // over an existing file). The saved path, or "" when it couldn't be saved.
    Q_INVOKABLE QString saveSnapshot(const QString &thumbnail, const QString &camera,
                                     qint64 timestamp) const;
    // Event switches. What switching a camera's event type off would delete:
    // { count, bytes } (stored events, snapshot bytes on disk).
    Q_INVOKABLE QVariantMap switchStats(qint64 hostId, int channel, const QString &type) const;
    // Switches the pair off, then deletes its stored events and their snapshots. False
    // when Status (nothing changes), when the switch can't be saved (nothing changes),
    // or when the delete fails (the pair is off, its stored events remain).
    Q_INVOKABLE bool switchOff(qint64 hostId, int channel, const QString &type);
    // Switches the pair back on: it logs afresh from now (what was deleted stays gone).
    Q_INVOKABLE bool switchOn(qint64 hostId, int channel, const QString &type);
    // The first switch-offs: a one-time list in settings (events/firstSwitchOffs, see
    // FirstSwitchOffs.h), offered once with one confirmation. The offer: { pairs: [{ hostId,
    // channel, type, recorder, camera, count }], count, bytes }, or empty once asked or with
    // no list. A list that matches no camera counts as asked.
    Q_INVOKABLE QVariantMap offerFirstSwitchOffs();
    // Switch off exactly the pairs last offered (deleting their stored events), or leave
    // everything on. Either way it isn't offered again. Confirm is false when nothing was
    // offered or a pair failed.
    Q_INVOKABLE bool confirmFirstSwitchOffs();
    Q_INVOKABLE void declineFirstSwitchOffs();
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
    void offlineAlertsChanged();
    void countChanged();
    void unreadChanged();
    // Stored events changed: one recorder's (an event filed, a thumbnail landed), or
    // anyone's (hostId -1: a purge, a trim, a clear, a history sync).
    void storedChanged(qint64 hostId);
    // A camera's event type was switched off or on.
    void switchesChanged();
    // The user clicked a desktop notification — jump to this event's playback.
    void eventActivated(qint64 hostId, int channel, qint64 timestamp);
    // The user clicked an offline alert — show this camera live (channel -1: a recorder).
    void offlineAlertActivated(qint64 hostId, int channel);

private slots:
    void onDetection(qint64 hostId, int channel, const QString &type, const QString &camera,
                     qint64 at);
    void onThumbnailReady(qint64 eventId, const QString &path);
    void onConnectivity(qint64 hostId, int channel, const QString &name, bool online,
                        const QVector<rl::device::CameraRef> &changed);

private:
    void reload();
    // Switches these pairs off, deletes their stored events and snapshots, then reloads
    // the list once. False when any pair's switch or delete failed.
    bool switchOffAll(const QVector<EventSwitchKey> &pairs);
    // The cameras the first switch-offs can name: stored event names, and the DB's channel
    // list (with each channel's event types). Bare streams raise nothing and are left out.
    QVector<device::KnownCamera> knownCameras() const;
    // A door's filings into the list: stored rows go in at their place by time, the
    // recorder's columns hear once, and a trim during the filing reloads instead. True when
    // any row was stored.
    bool take(qint64 hostId, const QVector<Inbox::Filing> &filed);
    // Puts a stored row in the list at its place by time (newest first).
    void insertSorted(const EventRecord &row);
    // The Inbox trimmed the store (cap, retention): reload once the filing is done.
    void reloadIfTrimmed();
    // One-time cleanup: events stored under a Frigate camera's raw config key
    // ("bosch_camera") before the display-name fix shipped are rewritten in
    // place in the database to the pretty form ("Bosch Camera")
    // so they line up with the filter dropdown and the property/camera shown
    // on the row. No-op once everything is already in the pretty form.
    void normalizeStoredCameraNames();
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
    // Hands a connectivity change to the offline-alert rules, shows what they decide and
    // re-arms the timer.
    void alertConnectivity(qint64 hostId, int channel, const QString &name, bool online,
                           const QVector<device::CameraRef> &changed, bool atStart = false);
    void showOfflineAlerts(const QVector<device::OfflineAlert> &alerts);
    void armOfflineTimer();
    // Test hook RL_FAKE_OFFLINE="<hostId>:<channel>:<downSecs>" (channel -1 = the whole
    // host): 5 s after start the alert rules see it go down, then back after downSecs.
    // Only the alerts: nothing is filed.
    void startFakeOffline(const QString &spec);

    Database *m_db;
    DeviceManager *m_devices;
    bool m_desktopNotifications = true;
    bool m_offlineAlerts = true;
    Notifier m_notifier;             // detection notices and offline alerts
    device::OfflineAlerts m_offline; // when a connectivity change becomes an alert
    QTimer m_offlineTimer;           // wakes at m_offline.nextDue()
    QTimer m_retentionTimer;         // hourly age-based purge
    QTimer m_runTimer;               // stamps this app run's last_seen
    qint64 m_runId = -1;             // this launch's app_runs row
    QFutureSynchronizer<void> m_pending; // drains in-flight history syncs at teardown
    QVector<EventRecord> m_rows;     // the list: newest first
    Inbox m_inbox;                   // what is stored (wayfinder/architecture ticket 03)
    bool m_trimmed = false;          // the Inbox trimmed during the current filing
    QSet<qint64> m_snapshotsDue;     // rows a Frigate snapshot is on its way to
    QSet<qint64> m_frigateHistoryChecked;
    QVector<EventSwitchKey> m_offeredSwitchOffs; // what the first switch-offs dialog shows // hosts already considered for the one-time catch-up
    int m_unread = 0;
};

} // namespace rl
