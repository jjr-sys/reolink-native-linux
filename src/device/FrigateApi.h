#pragma once

#include "protocol/Json.h"

#include <QByteArray>
#include <QString>
#include <QList>
#include <QStringList>
#include <QVector>

// A Frigate server, used by DeviceManager as a third kind of site. Port 8971 uses
// Frigate's authenticated HTTPS API; port 5000 remains supported for legacy servers.
// Live video uses Frigate's go2rtc RTSP restream on port 8554.
namespace rl::frigate {

struct Event {
    QString id;
    QString camera;
    QString label;
    double start = 0; // epoch seconds, server clock
};

// Enabled cameras in /api/config. go2rtc-only streams are not cameras and are ignored.
QStringList parseCameraNames(const Json &config);
// Frigate names its cameras like config keys ("bosch_camera"). Show that the way a
// person would write it; a name that already has a space (e.g. previously stored
// under an older, already-pretty form) is returned unchanged.
QString displayCameraName(const QString &name);
// /api/events reply. Entries without id, camera and start_time are dropped.
QVector<Event> parseEvents(const Json &events);
// A message from Frigate's /ws websocket, {"topic","payload"}: for topic "events" with
// payload type "new" (an object stopped being a false positive), the event it opened.
// The payload is itself a JSON string. False for anything else.
bool parseLiveEvent(const QByteArray &message, Event *out);
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

// The clip to play for a detection raised at `timestamp` (epoch s). The app sees a
// detection up to one poll (10 s) after it began, so the window opens well before it.
inline constexpr int kClipPreSecs = 25;
inline constexpr int kClipPostSecs = 20;
QString clipUrl(const QString &host, int port, const QString &camera, qint64 timestamp);
// The frame Frigate saved for a specific event (its own event id, e.g.
// "1790171530.03951-kul3vc") — unlike a live snapshot, this is the actual frame from
// when the event happened, so it's what backfilled history has to use for a thumbnail.
QString snapshotUrl(const QString &host, int port, const QString &eventId);

// ---- Recordings (Playback page) -------------------------------------------------------
// Frigate stores ~10 s segments. The timeline wants continuous ranges, and the calendar
// wants the days that have any footage.
struct Recording {
    double start = 0; // epoch seconds
    double end = 0;
};
// /api/<camera>/recordings reply. Entries without start_time and end_time are dropped.
QVector<Recording> parseRecordings(const Json &recordings);
// Join segments that touch (or are within `gapSecs`); input need not be sorted.
QVector<Recording> mergeRecordings(QVector<Recording> segments, double gapSecs = 2.0);
// Days of `year`/`month` with recorded footage, from /api/<camera>/recordings/summary
// (requested with the local timezone so the day strings are local days).
QList<int> parseRecordingDays(const Json &summary, int year, int month);

QString recordingsUrl(const QString &host, int port, const QString &camera, qint64 after, qint64 before);
QString summaryUrl(const QString &host, int port, const QString &camera, const QString &timeZone);
// HLS playlist of the recordings between two instants; plays from `start`.
QString vodUrl(const QString &host, int port, const QString &camera, qint64 start, qint64 end);

QString configUrl(const QString &host, int port);
QString eventsUrl(const QString &host, int port, double after, int limit);
QString liveUrl(const QString &host, int port, const QString &camera);
QString latestFrameUrl(const QString &host, int port, const QString &camera);

// Register the Frigate login for this host. The HTTP helper obtains and caches the
// session cookie; credentials are kept by the OS keyring, not in this module.
void setCredentials(const QString &host, int port, const QString &username,
                    const QString &password);

// Blocking GET for worker threads. Empty result with `error` set on failure.
QByteArray httpGet(const QString &url, int timeoutMs, QString *error = nullptr);
// The session cookie ("frigate_token=…") for a host whose login was registered with
// setCredentials, signing in if needed; empty when it has no login (port 5000) or on
// failure. Blocking, for worker threads with an event loop (QThread).
QByteArray sessionCookie(const QString &host, int port, int timeoutMs, QString *error = nullptr);
// Forget a host's cached session cookie (the server refused it); the next call signs in again.
void forgetSession(const QString &host, int port);

} // namespace rl::frigate
