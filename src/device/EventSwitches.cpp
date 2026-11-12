#include "EventSwitches.h"

namespace rl {

EventSwitches::EventSwitches(Database *db, QObject *parent) : QObject(parent), m_db(db)
{
    for (const EventSwitchKey &k : m_db->switchedOffEvents())
        m_off.insert(k);
}

QString EventSwitches::storedType(const QString &type)
{
    return type == QLatin1String("dog_cat") ? QStringLiteral("pet") : type;
}

bool EventSwitches::isStatus(const QString &type)
{
    return type == QLatin1String("online") || type == QLatin1String("offline");
}

bool EventSwitches::isOff(qint64 hostId, int channel, const QString &type) const
{
    return !m_off.isEmpty() && m_off.contains({hostId, channel, storedType(type)});
}

bool EventSwitches::setOff(qint64 hostId, int channel, const QString &type, bool off)
{
    if (isStatus(type))
        return false;
    const EventSwitchKey key{hostId, channel, storedType(type)};
    if (m_off.contains(key) == off)
        return true;
    if (!m_db->setEventSwitchedOff(key, off))
        return false;
    if (off)
        m_off.insert(key);
    else
        m_off.remove(key);
    emit changed();
    return true;
}

} // namespace rl
