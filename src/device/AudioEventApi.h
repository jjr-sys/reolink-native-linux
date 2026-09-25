#pragma once

#include "protocol/Json.h"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// The Woorabinda audio-monitor service (see wayfinder/hostile-audio): a small HTTP API on
// the server's Tailscale address, Bearer-token auth. It flags raised voices and threat
// speech in a camera's audio. These are an *audio event*, not a Frigate event; they have
// their own poller and their own storage on the server (ticket 13), so nothing here is
// filed into the detection inbox or its database.
namespace rl::audio {

inline constexpr const char *kRaisedVoice = "raised_voice";
inline constexpr const char *kThreatSpeech = "threat_speech";
inline constexpr const char *kSpeech = "speech"; // plain transcribed speech: kept, not an alert

struct Event {
    QString id;          // "<epoch>-<hex>", also the clip's name on the server
    double time = 0;     // epoch seconds, server clock
    QString camera;      // the service's camera key, e.g. "kitchen"
    QStringList triggers;
    QString scoreText;   // human-readable score for the trigger(s)
    QString transcript;  // empty when speech-to-text did not run (raised-voice events)
    bool hasClip = false;
    // An alert (raised voice or threat speech), as opposed to plain speech. Only these
    // light the red border or notify.
    bool flagged() const
    {
        return triggers.contains(QLatin1String(kRaisedVoice)) || triggers.contains(QLatin1String(kThreatSpeech));
    }
};

struct Status {
    bool ok = false; // false when the reply was not a status object
    bool transcriptionPaused = false;
    QString pausedReason;
};

// GET /events: newest first. Entries without id, time and camera are dropped.
QVector<Event> parseEvents(const Json &events);
// GET /status.
Status parseStatus(const Json &status);

// "Loud voice" / "Threatening words" / both, joined. Deliberately not "hostile": the
// classifier hears volume and phrases, not intent.
QString triggerText(const QStringList &triggers);

// limit is capped by the server at 500; `before` (epoch s, 0 = none) pages back.
QString eventsUrl(const QString &host, int port, int limit = 100, double before = 0, bool flaggedOnly = false);
QString statusUrl(const QString &host, int port);
QString resumeUrl(const QString &host, int port);
QString clipUrl(const QString &host, int port, const QString &eventId);

struct Reply {
    int status = 0;      // HTTP status, 0 when no reply
    QByteArray body;
    QString error;       // set when there was no usable reply
};
// Blocking, for worker threads. `token` is sent as a Bearer header and never logged.
Reply httpGet(const QString &url, const QString &token, int timeoutMs);
Reply httpPost(const QString &url, const QString &token, int timeoutMs);

// ---- Alert window ---------------------------------------------------------------------
// A camera shows the red border from a detection until it has been quiet this long; a
// further detection from the same camera extends it (ticket 10: no second visual state).
inline constexpr double kQuietSecs = 300.0;

// What to do with each poll's events. Pure, so the timing rules are testable.
class AlertTracker
{
public:
    // Newest-first events from one poll. Returns the events not seen before, oldest first.
    // The first poll is a backlog: it lights the border for anything still inside the
    // quiet window but returns nothing to notify about. Later polls treat an event's
    // arrival as its detection time, so a skewed server clock cannot shorten or lengthen
    // the window.
    QVector<Event> update(const QVector<Event> &events, double now);

    bool active(const QString &camera, double now) const;
    // Cameras still inside the window, and the moment the soonest one clears (0 = none).
    QStringList activeCameras(double now) const;
    double nextExpiry(double now) const;
    bool primed() const { return m_primed; }

private:
    QHash<QString, double> m_until; // camera -> border clears at
    QSet<QString> m_seen;
    bool m_primed = false;
};

} // namespace rl::audio
