#pragma once

#include "core/CredentialStore.h"
#include "core/Notifier.h"
#include "device/AudioEventApi.h"

#include <QAbstractListModel>
#include <QFutureSynchronizer>
#include <QTemporaryDir>
#include <QTimer>

namespace rl {

// Client for the Woorabinda audio-monitor service. Polls its authenticated endpoint,
// keeps the recent audio events as a model (for the Events page's Audio view), tells the
// Live view which cameras are inside an alert window (the red border), and surfaces the
// "transcription paused" state. The server owns storage and retention (60 days for
// transcripts, 7 for clips), so nothing about an event is written to the app's database;
// clips are fetched into a temporary directory only to be played and deleted.
//
// Enabled only when a token is in the keyring under kTokenHostId; the address comes from
// QSettings (audio/host, audio/port).
class AudioMonitor : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY statusChanged)
    Q_PROPERTY(bool reachable READ reachable NOTIFY statusChanged)
    Q_PROPERTY(bool transcriptionPaused READ transcriptionPaused NOTIFY statusChanged)
    Q_PROPERTY(QString pausedReason READ pausedReason NOTIFY statusChanged)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)
    // Bumps whenever any camera's alert state may have changed; bind isAlerting() to it.
    Q_PROPERTY(int alertRevision READ alertRevision NOTIFY alertsChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    // False: the model lists alerts only. True: every kept transcript (server keeps 60 days),
    // newest first, `loaded` at a time; loadMore() pages further back (server max 500).
    Q_PROPERTY(bool showAll READ showAll WRITE setShowAll NOTIFY showAllChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY countChanged)

public:
    // Device host ids are SQLite rowids (positive), so this can never collide with one.
    static constexpr qint64 kTokenHostId = -1000;
    static constexpr int kPollMs = 10000;
    static constexpr int kPageSize = 200;
    static constexpr int kMaxLimit = 500;

    enum Role { IdRole = Qt::UserRole + 1, TimestampRole, TimeTextRole, CameraRole, TriggerTextRole,
                TriggersRole, ScoreTextRole, TranscriptRole, HasClipRole, FlaggedRole, StartRole, EndRole, RawCameraRole };

    explicit AudioMonitor(CredentialStore *credentials, QObject *parent = nullptr);
    ~AudioMonitor() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool enabled() const { return !m_token.isEmpty(); }
    bool reachable() const { return m_reachable; }
    bool transcriptionPaused() const { return m_status.transcriptionPaused; }
    QString pausedReason() const { return m_status.pausedReason; }
    bool notificationsEnabled() const { return m_notify; }
    void setNotificationsEnabled(bool on);
    int alertRevision() const { return m_revision; }

    // True while `cameraName` (a tile's name, e.g. "Kitchen") is inside an alert window.
    // The service's camera key is matched case-insensitively against the tile's name.
    Q_INVOKABLE bool isAlerting(const QString &cameraName) const;
    // POST /resume after an automatic overload pause.
    Q_INVOKABLE void resumeTranscription();
    // Fetches the event's clip into a temporary file; clipReady() gives its path, or an
    // empty path and a message (an expired clip is a 404, reported as such).
    Q_INVOKABLE void fetchClip(const QString &eventId);
    // Deletes every fetched clip; call when playback stops.
    Q_INVOKABLE void discardClips();
    Q_INVOKABLE void refresh() { poll(); }
    Q_INVOKABLE void loadMore();
    bool showAll() const { return m_showAll; }
    void setShowAll(bool on);
    bool canLoadMore() const { return m_showAll && m_limit < kMaxLimit && m_events.size() >= m_limit; }

signals:
    void statusChanged();
    void notificationsEnabledChanged();
    void alertsChanged();
    void countChanged();
    void showAllChanged();
    void clipReady(const QString &eventId, const QString &path, const QString &error);

private slots:
    void poll();
    void onPolled(bool reachable, const QVector<audio::Event> &alerts, bool haveAlerts,
                  const QVector<audio::Event> &listing, bool haveListing, const audio::Status &status);
    void onExpiry();

private:
    void notifyFor(const audio::Event &e);
    static QString keyOf(const QString &s) { return s.trimmed().toLower(); }

    CredentialStore *m_credentials;
    QString m_host;
    int m_port = 8090;
    QString m_token; // kept in memory only; never logged
    Notifier m_notifier;
    QTimer m_pollTimer;
    QTimer m_expiryTimer;
    QFutureSynchronizer<void> m_pending;
    QTemporaryDir m_clipDir;
    audio::AlertTracker m_tracker;
    audio::Status m_status;
    QVector<audio::Event> m_events; // newest first
    bool m_reachable = false;
    bool m_polling = false;
    bool m_showAll = false;
    bool m_primedBorders = false;
    int m_limit = kPageSize;
    bool m_notify = true;
    bool m_pauseNotified = false;
    int m_revision = 0;
};

} // namespace rl

Q_DECLARE_METATYPE(rl::audio::Event)
Q_DECLARE_METATYPE(rl::audio::Status)
Q_DECLARE_METATYPE(QVector<rl::audio::Event>)
