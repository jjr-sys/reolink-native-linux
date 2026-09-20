#pragma once

#include "protocol/Json.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// A Frigate NVR server (http, port 5000, no login), used by DeviceManager as a third
// kind of site. Only the parts the app needs: the camera list, live video, and
// detection events. Live video goes through Frigate's own go2rtc proxy over the
// normal port as an HTTP-FLV stream — go2rtc's RTSP port is often not published.
namespace rl::frigate {

struct Event {
    QString id;
    QString camera;
    QString label;
    double start = 0; // epoch seconds, server clock
};

// Enabled cameras in /api/config. go2rtc-only streams are not cameras and are ignored.
QStringList parseCameraNames(const Json &config);
// /api/events reply. Entries without id, camera and start_time are dropped.
QVector<Event> parseEvents(const Json &events);
// The app's detection type for a Frigate object label ("person", "vehicle", "pet"),
// or empty for labels the app does not model.
QString typeForLabel(const QString &label);
// Where to poll from next: the newest start seen, never going backwards.
double newWatermark(double current, const QVector<Event> &events);

// Frigate opens a new event for each object it tracks, so one person crossing the yard
// can be several events. Raise one detection per camera and type per burst: a new
// event only counts when it starts at least this long after the last one raised.
inline constexpr double kBurstGapSecs = 30.0;
inline bool startsNewBurst(double lastRaisedStart, double start)
{
    return start - lastRaisedStart >= kBurstGapSecs;
}

QString configUrl(const QString &host, int port);
QString eventsUrl(const QString &host, int port, double after, int limit);
QString liveUrl(const QString &host, int port, const QString &camera);
QString latestFrameUrl(const QString &host, int port, const QString &camera);

// Blocking GET for worker threads. Empty result with `error` set on failure.
QByteArray httpGet(const QString &url, int timeoutMs, QString *error = nullptr);

} // namespace rl::frigate
