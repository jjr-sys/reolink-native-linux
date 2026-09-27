#pragma once

#include "core/Database.h"
#include "core/Notifier.h"
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
// exposes them newest-first to QML, with a type filter and an unread counter.
class EventManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int unread READ unread NOTIFY unreadChanged)
    // App-level master switch for desktop pop-ups (independent of the device Push flag).
    Q_PROPERTY(bool desktopNotifications READ desktopNotifications WRITE setDesktopNotifications NOTIFY desktopNotificationsChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString cameraFilter READ cameraFilter WRITE setCameraFilter NOTIFY cameraFilterChanged)
    Q_PROPERTY(QString propertyFilter READ propertyFilter WRITE setPropertyFilter NOTIFY propertyFilterChanged)
    // Oldest event's Unix timestamp within the current filter (0 = view is empty).
    Q_PROPERTY(qint64 oldestVisibleTimestamp READ oldestVisibleTimestamp NOTIFY countChanged)
    // True once the retention window/ceiling has trimmed something at least once,
    // meaning some history further back existed but aged out or got capped.
    Q_PROPERTY(bool historyTrimmed READ historyTrimmed NOTIFY countChanged)

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
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    QString cameraFilter() const { return m_cameraFilter; }
    void setCameraFilter(const QString &camera);
    QString propertyFilter() const { return m_propertyFilter; }
    void setPropertyFilter(const QString &property);
    qint64 oldestVisibleTimestamp() const;
    bool historyTrimmed() const { return m_historyTrimmed; }

    bool desktopNotifications() const { return m_desktopNotifications; }
    void setDesktopNotifications(bool on);

    Q_INVOKABLE void markAllRead();
    // Seconds-into-day of each detection event for this camera on the given day
    // (dayStart = local midnight epoch). Feeds the playback timeline's markers.
    Q_INVOKABLE QVariantList eventTimesFor(qint64 hostId, int channel, qint64 dayStart) const;
    Q_INVOKABLE void clear();
    // Backfills the inbox from a Frigate server's own event history (unlike
    // onDetection, which only ever files what was seen live since the app was last
    // connected). Filed with each event's real historical timestamp, deduplicated
    // against what's already stored, and still subject to the retention window.
    // No-op for a non-Frigate host or one the manager doesn't know about.
    Q_INVOKABLE void syncFrigateHistory(qint64 hostId);
    // Convenience: runs syncFrigateHistory() for every known Frigate host.
    Q_INVOKABLE void syncAllFrigateHistory();

signals:
    void desktopNotificationsChanged();
    void countChanged();
    void unreadChanged();
    void filterChanged();
    void cameraFilterChanged();
    void propertyFilterChanged();
    // The user clicked a desktop notification — jump to this event's playback.
    void eventActivated(qint64 hostId, int channel, qint64 timestamp);

private slots:
    void onDetection(qint64 hostId, int channel, const QString &type, const QString &camera);
    void onThumbnailReady(qint64 eventId, const QString &path);
    void onConnectivity(qint64 hostId, int channel, const QString &name, bool online);

private:
    void reload();
    bool matches(const EventRecord &e) const;
    // One-time cleanup: events stored under a Frigate camera's raw config key
    // ("bosch_camera") before the display-name fix shipped are rewritten in
    // place, in memory and in the database, to the pretty form ("Bosch Camera")
    // so they line up with the filter dropdown and the property/camera shown
    // on the row. No-op once everything is already in the pretty form.
    void normalizeStoredCameraNames();
    // Age-based purge (every property, whole store) plus a per-host safety-valve
    // trim for `hostId` (0 = skip). Deletes orphaned thumbnail files and reloads
    // the model if anything was actually removed.
    void enforceRetention(qint64 hostId = 0);
    // Runs on the GUI thread once syncFrigateHistory()'s worker thread has the raw
    // events back: applies the same burst-gate as live polling, skips anything
    // already covering that moment, and files the rest with their real timestamps.
    void applyFrigateHistory(qint64 hostId, const QHash<QString, int> &channelOf,
                             const QVector<frigate::Event> &events);
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
    QTimer m_retentionTimer;         // periodic age-based purge, in case nothing new arrives
    QFutureSynchronizer<void> m_pending; // drains in-flight history syncs at teardown
    QVector<EventRecord> m_all;      // newest first
    QVector<int> m_view;             // indices into m_all passing the filter
    QString m_filter;                // "" = all, else a type
    QString m_cameraFilter;          // "" = all cameras, else a camera name
    QString m_propertyFilter;        // "" = all properties, else a host/site name
    QSet<qint64> m_frigateHistoryChecked; // hosts already considered for the one-time catch-up
    int m_unread = 0;
    bool m_historyTrimmed = false;   // true once the retention window/ceiling has ever trimmed something
};

} // namespace rl
