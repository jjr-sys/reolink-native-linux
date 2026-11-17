#pragma once

#include "device/EventHistory.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace rl::device {

// One desktop offline alert to show (wayfinder/urgent-offline-alerts ticket 05/08).
struct OfflineAlert
{
    enum class Kind {
        Offline,      // went offline (critical, sound, stays until dismissed)
        Unstable,     // dropped 3 times in 10 min (critical, sound); quiet after
        BackOnline,   // returned after an Offline alert (ordinary; replaces it)
        Stable,       // up 10 min after Unstable (ordinary; replaces it)
    };
    Kind kind = Kind::Offline;
    QString key;          // one bubble per key: a later alert for it replaces the earlier
    qint64 hostId = -1;   // what a click opens: the camera, or channel -1 = the recorder
    int channel = -1;
    QString title;
    QString body;
    bool critical = false;
};

// The rules for desktop offline alerts: a camera waits out a 30 s grace (a whole recorder's
// own detection already waited ~35 s, so it alerts at once); one "back online" after an
// alert; 3 drops within 10 min make one "unstable" alert, then quiet until up 10 min; a
// recorder going down swallows its cameras still in their grace. Pure: the clock is passed
// in, and the caller shows what comes back and calls tick() at nextDue().
class OfflineAlerts
{
public:
    struct Timing {
        qint64 graceSecs = 30;
        qint64 flapWindowSecs = 600;
        int flapDrops = 3;
        qint64 stableSecs = 600;
    };
    // A connectivity change as DeviceManager reports it. channel -1 = the whole host, with
    // `cameras` the ones that flipped with it; `recorder` = a multi-camera device (worded
    // "… offline · N cameras"), otherwise a lone camera worded by its name.
    struct Change {
        qint64 hostId = -1;
        int channel = -1;
        QString name;
        bool online = false;
        QVector<CameraRef> cameras;
        bool recorder = false;
        // Found this way when the app first connected: nothing has waited yet, so a whole
        // host waits out the grace too.
        bool atStart = false;
    };

    OfflineAlerts() = default;
    explicit OfflineAlerts(Timing timing) : m_timing(timing) {}

    QVector<OfflineAlert> change(const Change &c, qint64 now);
    QVector<OfflineAlert> tick(qint64 now);
    // The next time tick() has something to decide (epoch seconds), or -1.
    qint64 nextDue() const;

    // The camera key ("hostId:channel", as device::cameraKey); channel -1 = the whole host.
    static QString keyOf(qint64 hostId, int channel);

private:
    enum class State { Up, Waiting, Alerted, Unstable };
    struct Track {
        State state = State::Up;
        bool down = false;
        qint64 since = 0;          // Waiting/Alerted: went down; Unstable: last came up
        QVector<qint64> drops;     // offline times within the window
        QString name;
        qint64 hostId = -1;
        int channel = -1;          // click target
        bool recorder = false;     // a multi-camera host (worded with its camera count)
        int cameraCount = 0;       // cameras that went down with it
    };

    // The end of an outage for one key: "back online" after an alert, silent otherwise.
    QVector<OfflineAlert> cameUp(const QString &key, Track &t, qint64 now);
    // When tick() next has something to decide for this track, or -1.
    qint64 dueAt(const Track &t) const;
    static QString offlineTitle(const Track &t);
    static QString offlineBody(const Track &t);

    Timing m_timing;
    QHash<QString, Track> m_tracks;
};

} // namespace rl::device
