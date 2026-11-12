#pragma once

#include "core/Database.h"

#include <QObject>
#include <QSet>

namespace rl {

// Event switches: the (recorder, camera, type) pairs the user has switched off. A
// switched-off pair is not stored, pictured, notified or highlighted; switching it on
// logs afresh. Kept in the app's own store and never written to a recorder. Status
// (online/offline) can't be switched off. Separate from the Event type row's view filter.
class EventSwitches : public QObject
{
    Q_OBJECT

public:
    explicit EventSwitches(Database *db, QObject *parent = nullptr);

    // The stored type a detection type is filed under ("dog_cat" is "pet").
    static QString storedType(const QString &type);
    static bool isStatus(const QString &type);

    bool isOff(qint64 hostId, int channel, const QString &type) const;
    // False (and nothing changes) for Status or when the store can't be written.
    bool setOff(qint64 hostId, int channel, const QString &type, bool off);
    QVector<EventSwitchKey> offPairs() const { return m_off.values().toVector(); }

signals:
    void changed();

private:
    Database *m_db;
    QSet<EventSwitchKey> m_off;
};

} // namespace rl
