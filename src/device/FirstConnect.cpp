#include "device/FirstConnect.h"

namespace rl::device {

QVector<ConnectFact> FirstConnect::failed(qint64 hostId, const QString &hostName,
                                          Problem problem, const QVector<CameraRef> &known)
{
    if (m_seen.contains(hostId))
        return {};
    m_seen.insert(hostId);
    if (problem != Problem::Unreachable)
        return {};
    m_downAtConnect.insert(hostId);
    return {{-1, hostName, false, known}};
}

QVector<ConnectFact> FirstConnect::listed(qint64 hostId, const QString &hostName,
                                          const QVector<CameraRef> &offline)
{
    QVector<ConnectFact> facts;
    m_seen.insert(hostId);
    const bool firstListing = !m_listedOnce.contains(hostId);   // also after a failed start
    m_listedOnce.insert(hostId);
    if (m_downAtConnect.remove(hostId))
        facts.append({-1, hostName, true, {}});
    if (firstListing)
        for (const CameraRef &cam : offline)
            facts.append({cam.channel, cam.name, false, {}});
    return facts;
}

} // namespace rl::device
