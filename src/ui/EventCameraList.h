#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVector>

namespace rl {

class Database;
class EventManager;
class EventSwitches;

// The Events page's camera list (wayfinder/events-feed step 18): the selected Site's
// cameras, the most recently active first (the newest stored event that isn't Status),
// then those with nothing stored yet, in the order given. A camera whose types other than
// Status are all switched off can only log Status now: it goes last, marked so. A camera
// whose types aren't known yet (its recorder hasn't answered) is not marked.
//
//  - `cameras` comes from the page: [{ key: "hostId:channel", name, types }].
//  - Set `events` (QML), or `setDatabase` and `setSwitches` (tests); with `events` the
//    list re-orders a moment after events are stored or a switch changes.
class EventCameraList : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(rl::EventManager *events READ events WRITE setEvents NOTIFY eventsChanged)
    Q_PROPERTY(QVariantList cameras READ cameras WRITE setCameras NOTIFY camerasChanged)
    Q_PROPERTY(int count READ count NOTIFY refreshed)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        NameRole,
        LastRole,       // newest stored event that isn't Status (epoch), 0 when none
        StatusOnlyRole, // it can only log Status now
    };

    explicit EventCameraList(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    EventManager *events() const;
    void setEvents(EventManager *events);
    void setDatabase(Database *db);
    void setSwitches(EventSwitches *switches);
    QVariantList cameras() const { return m_cameras; }
    void setCameras(const QVariantList &cameras);
    int count() const { return int(m_rows.size()); }

    // The camera at the top, "" when there is none; whether a camera is listed.
    Q_INVOKABLE QString first() const;
    Q_INVOKABLE bool contains(const QString &key) const;
    Q_INVOKABLE void refresh();

signals:
    void eventsChanged();
    void camerasChanged();
    void refreshed();

private:
    struct Row
    {
        QString key, name;
        qint64 last = 0;
        bool statusOnly = false;
    };

    QPointer<EventManager> m_events;
    Database *m_db = nullptr;
    QPointer<EventSwitches> m_switches;
    QTimer m_refreshSoon; // coalesces bursts of stored events into one query
    QVariantList m_cameras;
    QVector<Row> m_rows;
};

} // namespace rl
