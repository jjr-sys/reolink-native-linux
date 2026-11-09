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

// One column of the Events page (wayfinder/site-tabs-redesign, step 6): one camera's events
// for one local day, newest first, filtered by the column's own event type row, with an
// "app not running" band wherever the app was not running that day.
//
//  - The camera is a selection key, "hostId:channel". `dayStart` is local midnight (epoch).
//  - `supportedTypes` are the camera's types in display order ("person", "vehicle", "pet",
//    "visitor", "motion", "status"); none means the camera reports nothing. They start on
//    except Motion and Status (`defaultTypes`), or as remembered (`restoreTypes`).
//    "status" stands for the stored "online"/"offline" rows.
//  - `counts` has the day's count for every supported type, on or off.
//  - Set `events` (QML) or `setDatabase` (tests); with `events` the column re-queries a
//    moment after its recorder's events change.
class EventColumn : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(rl::EventManager *events READ events WRITE setEvents NOTIFY eventsChanged)
    Q_PROPERTY(QString cameraKey READ cameraKey WRITE setCameraKey NOTIFY cameraKeyChanged)
    Q_PROPERTY(qint64 dayStart READ dayStart WRITE setDayStart NOTIFY dayStartChanged)
    Q_PROPERTY(QStringList supportedTypes READ supportedTypes WRITE setSupportedTypes NOTIFY supportedTypesChanged)
    Q_PROPERTY(QStringList enabledTypes READ enabledTypes NOTIFY enabledTypesChanged)
    Q_PROPERTY(bool hasTypes READ hasTypes NOTIFY supportedTypesChanged)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY refreshed)
    Q_PROPERTY(QString keptNote READ keptNote NOTIFY refreshed)
    Q_PROPERTY(int count READ count NOTIFY refreshed)

public:
    enum Role {
        KindRole = Qt::UserRole + 1, // "event" or "gap"
        TypeRole,                    // the stored type ("online"/"offline" for Status)
        TimestampRole,
        ThumbnailRole,
        FromRole,                    // a gap's span, [from, to)
        ToRole,
    };

    explicit EventColumn(QObject *parent = nullptr);

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
    qint64 dayStart() const { return m_dayStart; }
    void setDayStart(qint64 dayStart);
    QStringList supportedTypes() const { return m_supported; }
    void setSupportedTypes(const QStringList &types);
    QStringList enabledTypes() const { return m_enabled; }
    bool hasTypes() const { return !m_supported.isEmpty(); }
    QVariantMap counts() const { return m_counts; }
    QString keptNote() const { return m_keptNote; }
    int count() const { return int(m_rows.size()); }

    Q_INVOKABLE static QStringList defaultTypes(const QStringList &supported);
    // Turn a type on or off; emits typesToggled so the page can remember the set.
    Q_INVOKABLE void toggle(const QString &type);
    // A remembered set, kept to the supported types.
    Q_INVOKABLE void restoreTypes(const QStringList &types);
    Q_INVOKABLE void refresh();

signals:
    void eventsChanged();
    void cameraKeyChanged();
    void dayStartChanged();
    void supportedTypesChanged();
    void enabledTypesChanged();
    void typesToggled(const QStringList &types);
    void refreshed();

private:
    struct Row
    {
        bool gap = false;
        QString type, thumbnail;
        qint64 ts = 0, from = 0, to = 0;
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
    qint64 m_dayStart = 0;
    QStringList m_supported, m_enabled;
    bool m_chosen = false;      // the enabled set was toggled or restored, not the default
    QStringList m_wanted;       // the chosen set, before it is kept to the supported types
    QVariantMap m_counts;
    QString m_keptNote;
    QVector<Row> m_rows;
};

} // namespace rl
