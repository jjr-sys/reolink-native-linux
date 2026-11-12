#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

namespace rl {

class Database;
class EventManager;
class EventSwitches;

// The switch controls at the top of a camera's Events feed (wayfinder/events-feed step 21).
//
//  - `cards`: a noise card for each type logging kNoisyPerDay or more a day, counted per day
//    of app running over the last 24 h (and only once the app ran an hour of it), unless
//    the person chose Keep logging for this camera and type (remembered in the database).
//    Each card: { type, perDay, everySecs, count, thumbnails: the four newest snapshots }.
//  - `offTypes`: the types switched off on this camera, in display order, for the dashed
//    "switched off here" line with Turn on.
//  - Set `events` (QML), or `setDatabase`/`setSwitches` (tests); with `events` it re-reads a
//    moment after events are stored or a switch changes.
class CameraNoise : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::EventManager *events READ events WRITE setEvents NOTIFY eventsChanged)
    Q_PROPERTY(QString cameraKey READ cameraKey WRITE setCameraKey NOTIFY cameraKeyChanged)
    Q_PROPERTY(QVariantList cards READ cards NOTIFY refreshed)
    Q_PROPERTY(QStringList offTypes READ offTypes NOTIFY refreshed)
    Q_PROPERTY(int cardSnapshots READ cardSnapshots CONSTANT)

public:
    static constexpr int kNoisyPerDay = 1000;
    static constexpr qint64 kMinRunningSecs = 3600;
    static constexpr qint64 kDaySecs = 86400;
    static constexpr int kCardSnapshots = 4;

    explicit CameraNoise(QObject *parent = nullptr);

    EventManager *events() const;
    void setEvents(EventManager *events);
    void setDatabase(Database *db);
    void setSwitches(EventSwitches *switches);
    // The clock, for tests; 0 means the real time.
    void setNow(qint64 now) { m_now = now; }

    QString cameraKey() const { return m_key; }
    void setCameraKey(const QString &key);
    QVariantList cards() const { return m_cards; }
    QStringList offTypes() const { return m_off; }
    int cardSnapshots() const { return kCardSnapshots; }

    // How many a day `type` logs here (per day of app running over the last 24 h), for the
    // switch-off confirmation; -1 before the app has run an hour of it.
    Q_INVOKABLE int perDay(const QString &type) const;
    // Keep logging: this camera's card for `type` doesn't come back.
    Q_INVOKABLE void keepLogging(const QString &type);
    Q_INVOKABLE void refresh();

signals:
    void eventsChanged();
    void cameraKeyChanged();
    void refreshed();

private:
    qint64 now() const;
    // Seconds the app ran in the last 24 h, and that window's counts by stored type.
    qint64 runningSecs() const;
    QHash<QString, int> dayCounts() const;
    // `count` over `running` seconds as a daily rate; -1 under kMinRunningSecs.
    static int rate(int count, qint64 running);

    QPointer<EventManager> m_events;
    Database *m_db = nullptr;
    QPointer<EventSwitches> m_switches;
    QTimer m_refreshSoon;
    qint64 m_now = 0;
    QString m_key;
    qint64 m_host = -1;
    int m_channel = -1;
    QVariantList m_cards;
    QStringList m_off;
};

} // namespace rl
