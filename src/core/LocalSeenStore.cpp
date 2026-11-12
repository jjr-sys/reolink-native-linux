#include "core/LocalSeenStore.h"

#include "core/Database.h"

namespace rl {

LocalSeenStore::LocalSeenStore(Database *db, QObject *parent)
    : SeenStore(parent), m_db(db), m_seen(db ? db->cameraSeenTimes() : decltype(m_seen)())
{
}

qint64 LocalSeenStore::seenAt(qint64 hostId, int channel) const
{
    return m_seen.value({hostId, channel});
}

bool LocalSeenStore::markSeen(qint64 hostId, int channel, qint64 at)
{
    if (m_seen.value({hostId, channel}) == at)
        return true;
    if (!m_db || !m_db->setCameraSeen(hostId, channel, at))
        return false;
    m_seen.insert({hostId, channel}, at);
    emit changed();
    return true;
}

} // namespace rl
