#pragma once

#include <QString>
#include <QVariantMap>

namespace rl::device {

// What kind of host a device row hangs off. The persisted form is the Database kind
// column's free-text value ("camera", "nvr", "stream", "frigate"); that column is
// unchanged. Everything else in the app asks capabilities() what a host can do rather
// than comparing those strings.
enum class Kind { Camera, Nvr, Stream, Frigate };

// Unknown or blank text is a Reolink camera, the default a row starts as.
Kind kindFromString(const QString &text);
QString kindToString(Kind kind);

// Where a host's detections (motion, person, ...) come from.
enum class DetectionSource {
    None,                // a bare stream URL raises nothing
    ReolinkPollAndPush,  // HTTP poll, taken over by Baichuan push while it is alive
    FrigateServer,       // Frigate's events API plus its websocket
};

struct Capabilities
{
    // Login, Baichuan, PTZ, talk, settings, playback search, HD playback. False for the
    // "direct" hosts, which are only ever played from a URL (and, for Frigate, its API).
    bool reolinkProtocol = true;
    // One host fans out into one row per channel (NVR, Frigate server); a channel is
    // removed together with its host.
    bool multiChannel = false;
    // Needs no validation round trip: the row is playable as soon as it is added.
    bool alwaysOnline = false;
    // Snapshots, recordings, clips, history and validation go through Frigate's API.
    bool frigateApi = false;
    DetectionSource detections = DetectionSource::ReolinkPollAndPush;

    bool direct() const { return !reolinkProtocol; }
};

Capabilities capabilities(Kind kind);
// The same table as booleans for QML (keys: reolinkProtocol, direct, multiChannel,
// alwaysOnline, frigateApi, hasDetections).
QVariantMap capabilityMap(Kind kind);

// ---- how a camera is addressed -------------------------------------------------

// Stream vendors whose URL layout lets us pick a quality. Recognised from the saved URL.
enum class StreamVendor { Generic, Hikvision };
StreamVendor streamVendor(const QString &savedUrl);

// The Bosch camera's feed arrives through Frigate, so it is recognised by its name; its
// controls come from the Home Assistant Bosch integration, not from the host's kind.
bool looksLikeBoschFeed(const QString &label);


// The URL a direct-URL ("stream") host plays at the asked quality. The saved URL is
// its main stream. Hikvision cameras serve a channel's main stream at
// /Streaming/Channels/<N>01 and its sub stream at <N>02, so a Fluent tile gets the
// small stream there as it does for every other camera; any other URL plays as saved.
QString directLiveUrl(const QString &savedUrl, bool mainStream);

// Everything liveUrl() needs about one camera row.
struct LiveSource
{
    Kind kind = Kind::Camera;
    QString addr;      // host address, or the saved URL for a stream
    int port = 0;
    QString username;
    QString password;  // from the keyring
    QString chanName;  // Frigate camera key
    int channel = 0;
    QString mainCodec; // GetEnc-declared main stream codec; empty if unknown
    bool primed = false; // credentials loaded and the host validated
};

// Playable URL at the asked quality; empty while a Reolink host is not yet primed.
QString liveUrl(const LiveSource &source, bool mainStream);

}

namespace rl {
using device::directLiveUrl;
}
