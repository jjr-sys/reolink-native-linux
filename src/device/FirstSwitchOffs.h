#pragma once

#include "core/Database.h"

#include <QStringList>
#include <QVector>

namespace rl::device {

// A camera the app knows of, for matching the first switch-offs by name.
struct KnownCamera {
    qint64 hostId = -1;
    QString hostName;  // the recorder's name
    int channel = 0;
    QString camera;    // the camera's name
    QStringList types; // the event types it can report; empty = not known yet
};

// The first switch-offs (wayfinder/events-feed): a one-time list kept in settings, each
// entry "Recorder/Camera/type" with "*" for any recorder or camera. Recorder names match
// exactly, camera names ignoring case. A camera whose known types lack the type, Status,
// and entries that match nothing or don't parse are skipped. Each pair appears once.
QVector<EventSwitchKey> resolveFirstSwitchOffs(const QStringList &entries,
                                               const QVector<KnownCamera> &cameras);

} // namespace rl::device
