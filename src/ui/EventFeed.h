#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

namespace rl {

class Database;
class EventManager;

// The Events page's feed (wayfinder/events-feed step 18): one camera's events over
// everything kept (30 days), newest first, filtered by its event type chips, with an
// "app not running" band wherever the app was not running. Each row carries its local
// day ("yyyy-MM-dd") for the sticky day headers.
//
//  - The camera is a selection key, "hostId:channel".
//  - `supportedTypes` are the camera's types in display order ("person", "vehicle", "pet",
//    "visitor", "motion", "status"); none means the camera reports nothing. They start on
//    except Motion and Status (`defaultTypes`; Status when it is all the camera has, Motion
//    aside), or as remembered (`restoreTypes`).
//    "status" stands for the stored "online"/"offline" rows.
//  - `counts` has the kept window's count for every supported type, on or off.
//  - Set `events` (QML) or `setDatabase` (tests); with `events` the feed re-queries a
//    moment after its recorder's events change. New rows slide in at the top and rows
//    leaving the window go from the bottom, so a scrolled feed keeps its place.
class EventFeed : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(rl::EventManager *events READ events WRITE setEvents NOTIFY eventsChanged)
    Q_PROPERTY(QString cameraKey READ cameraKey WRITE setCameraKey NOTIFY cameraKeyChanged)
    Q_PROPERTY(QStringList supportedTypes READ supportedTypes WRITE setSupportedTypes NOTIFY supportedTypesChanged)
    Q_PROPERTY(QStringList enabledTypes READ enabledTypes NOTIFY enabledTypesChanged)
    Q_PROPERTY(bool hasTypes READ hasTypes NOTIFY supportedTypesChanged)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY refreshed)
    Q_PROPERTY(int count READ count NOTIFY refreshed)

public:
    enum Role {
        KindRole = Qt::UserRole + 1, // "event" or "gap"
        TypeRole,                    // the stored type ("online"/"offline" for Status)
        TimestampRole,
        ThumbnailRole,
        FromRole,                    // a gap's span, [from, to)
        ToRole,
        DayRole,                     // local day, "yyyy-MM-dd" (a gap: the day it ends on)
        IdRole,                      // the stored event's id (-1 for a gap)
    };

    explicit EventFeed(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    EventManager *events() const;
    void setEvents(EventManager *events);
    void setDatabase(Database *db);
    // The clock, for tests; 0 means the real time.
    void setNow(qint64 now) { m_now = now; }

    QString cameraKey() const { return m_key; }
    void setCameraKey(const QString &key);
    QStringList supportedTypes() const { return m_supported; }
    void setSupportedTypes(const QStringList &types);
    QStringList enabledTypes() const { return m_enabled; }
    bool hasTypes() const { return !m_supported.isEmpty(); }
    QVariantMap counts() const { return m_counts; }
    int count() const { return int(m_rows.size()); }

    Q_INVOKABLE static QStringList defaultTypes(const QStringList &supported);
    // Turn a type on or off; emits typesToggled so the page can remember the set.
    Q_INVOKABLE void toggle(const QString &type);
    // A remembered set, kept to the supported types.
    Q_INVOKABLE void restoreTypes(const QStringList &types);
    // Nothing remembered: the defaults, following the supported types as they change.
    Q_INVOKABLE void useDefaultTypes();
    Q_INVOKABLE void refresh();
    // A row's time (an event's, or a gap's end); 0 for a row that isn't there.
    Q_INVOKABLE qint64 timestampAt(int row) const;

signals:
    void eventsChanged();
    void cameraKeyChanged();
    void supportedTypesChanged();
    void enabledTypesChanged();
    void typesToggled(const QStringList &types);
    void refreshed();

private:
    struct Row
    {
        bool gap = false;
        QString type, thumbnail, day;
        qint64 ts = 0, from = 0, to = 0;
        qint64 id = -1;
    };
    void setEnabled(const QStringList &types);
    void applyRows(const QVector<Row> &rows);
    qint64 now() const;

    QPointer<EventManager> m_events;
    Database *m_db = nullptr;
    QTimer m_refreshSoon;       // coalesces bursts of stored events into one query
    qint64 m_now = 0;
    QString m_key;
    qint64 m_host = -1;
    int m_channel = -1;
    QStringList m_supported, m_enabled;
    bool m_chosen = false;      // the enabled set was toggled or restored, not the default
    QStringList m_wanted;       // the chosen set, before it is kept to the supported types
    QVariantMap m_counts;
    QVector<Row> m_rows;
};

} // namespace rl
