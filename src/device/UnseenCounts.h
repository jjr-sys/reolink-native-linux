#pragma once

#include <QHash>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace rl {

class Database;
class EventManager;
class EventSwitches;
class SeenStore;

// Unseen counts (wayfinder/events-feed step 20): for each camera, its logged Person,
// Vehicle, Pet and Visitor events after it was last opened (SeenStore), less switched-off
// pairs. Motion and Status never count.
//
//  - `openCamera` is the camera shown on screen ("hostId:channel", "" for none). Opening
//    marks it seen; while it stays open, what arrives is seen too, and leaving marks it
//    seen up to that moment. `seenLine` keeps the time it had been seen up to when it was
//    opened, for the feed's line and blue dots: 0 when never opened, -1 with none open.
//  - Set `setEvents` (app) or `setDatabase`/`setSwitches` (tests); with events the counts
//    re-read a moment after events are stored or a switch changes.
class UnseenCounts : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(QString openCamera READ openCamera WRITE setOpenCamera NOTIFY openCameraChanged)
    Q_PROPERTY(qint64 seenLine READ seenLine NOTIFY openCameraChanged)

public:
    explicit UnseenCounts(QObject *parent = nullptr);

    // The types counted, in bubble order.
    Q_INVOKABLE static QStringList countedTypes();

    void setEvents(EventManager *events);
    void setDatabase(Database *db);
    void setSwitches(EventSwitches *switches);
    void setStore(SeenStore *store);
    // The clock, for tests; 0 means the real time.
    void setNow(qint64 now) { m_now = now; }

    int total() const { return m_total; }
    int revision() const { return m_revision; }
    QString openCamera() const { return m_openCamera; }
    void setOpenCamera(const QString &key);
    qint64 seenLine() const { return m_seenLine; }

    // { type: count } for one camera, only the types with any.
    Q_INVOKABLE QVariantMap countsFor(const QString &key) const;
    // The sum over the given recorders' cameras.
    Q_INVOKABLE int totalForHosts(const QVariantList &hostIds) const;
    Q_INVOKABLE void refresh();

signals:
    void changed();
    void openCameraChanged();

private:
    qint64 now() const;
    void markOpenSeen(const QHash<QPair<qint64, int>, qint64> &lastEventTimes);

    QPointer<EventManager> m_events;
    Database *m_db = nullptr;
    QPointer<EventSwitches> m_switches;
    QPointer<SeenStore> m_store;
    QTimer m_refreshSoon;
    qint64 m_now = 0;
    QString m_openCamera;
    qint64 m_seenLine = -1;
    bool m_marking = false; // marking the open camera seen ourselves
    int m_total = 0;
    int m_revision = 0;
    QHash<QPair<qint64, int>, QVariantMap> m_counts;
};

} // namespace rl
