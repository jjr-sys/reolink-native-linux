#pragma once

#include <QString>
#include <QtGlobal>

namespace rl {

// How a native playback stream is carried: recorder-paced replay (cmd 381), or windows the
// app fetches (cmd 143) and plays backwards (wayfinder/playback-controls ticket 12) or
// forwards at its own pace (HD fast forward, wayfinder/live-replay ticket 09).
enum class NativeWindows { None, Backward, Forward };

// Seam of PlaybackSession: what the device can do for one playback view. The
// production adapter (in PlaybackSession.cpp) wraps DeviceManager + a StreamPlayer;
// tests script it. The session decides; the transport only carries out.
class PlaybackTransport
{
public:
    virtual ~PlaybackTransport() = default;
    virtual bool streaming() const = 0; // a picture is arriving
    virtual bool busy() const = 0;      // connecting (opening) but no picture yet
    virtual bool direct(int row) const = 0; // single HTTP stream, no native protocol (Frigate)
    // HTTP-FLV sub-stream URL for the moment; empty when no recording covers it.
    virtual QString httpUrl(int row, qint64 epoch) = 0;
    // Move the running native session to `epoch` without reconnecting; false if none.
    virtual bool seekInPlace(int row, qint64 epoch) = 0;
    virtual void openHttp(int row, qint64 epoch, const QString &url) = 0;
    virtual void openBaichuan(int row, qint64 epoch, bool main, NativeWindows windows) = 0;
    virtual void stop() = 0;
};

} // namespace rl
