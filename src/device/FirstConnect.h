#pragma once

#include "device/EventHistory.h"
#include "device/HostSession.h"

#include <QSet>
#include <QString>
#include <QVector>

namespace rl::device {

// One thing the first connect found, for the offline alerts only (nothing is filed): the
// whole host unreachable (channel -1, `cameras` its known ones) or connecting after all
// (channel -1, online), or a camera offline at the host's first listing.
struct ConnectFact
{
    int channel = -1;
    QString name;
    bool online = false;
    QVector<CameraRef> cameras;
};

// What this run's first connect of each host found (urgent-offline-alerts 05, 08): a host
// unreachable at its first validation, a camera offline at its host's first listing, and a
// host found unreachable that is listed later after all. Every later failure or listing is
// a change, which the poll reports; this says nothing about it. Pure: DeviceManager hands
// it each validation's outcome and emits what comes back as connectivityKnown, in order.
class FirstConnect
{
public:
    // A validation failed (the rows are kept). `known`: the host's listed cameras, empty
    // while it is still the start-up placeholder. Only the host's first outcome counts,
    // and only Unreachable is reported: a refused login (Auth, Locked) is not "down".
    QVector<ConnectFact> failed(qint64 hostId, const QString &hostName, Problem problem,
                                const QVector<CameraRef> &known);
    // The host's cameras were listed (from the API, or over Baichuan when degraded).
    // `offline`: the listed cameras that are offline, in row order. A host found down
    // first is reported back; the offline cameras only at the host's first listing.
    QVector<ConnectFact> listed(qint64 hostId, const QString &hostName,
                                const QVector<CameraRef> &offline);

private:
    QSet<qint64> m_seen;        // hosts whose first connect of this run has landed
    QSet<qint64> m_downAtStart; // ... and found unreachable, until one is listed
    QSet<qint64> m_listedOnce;  // hosts whose cameras this run has listed
};

} // namespace rl::device
