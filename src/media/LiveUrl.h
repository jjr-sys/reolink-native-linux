#pragma once

#include <QString>

namespace rl {

// Sources that never end on their own, so a drop means "reconnect", not "finished":
// camera streams, and a Frigate server's live proxy (go2rtc over HTTP-FLV). An ordinary
// http(s) URL is a file or a clip: it plays to the end and stops.
inline bool isLiveStreamUrl(const QString &source)
{
    return source.startsWith(QLatin1String("rtsp://")) ||
           source.startsWith(QLatin1String("rtmp://")) ||
           source.startsWith(QLatin1String("tcp://")) ||
           source.startsWith(QLatin1String("udp://")) ||
           ((source.startsWith(QLatin1String("http://")) || source.startsWith(QLatin1String("https://")))
            && source.contains(QLatin1String("/api/go2rtc/api/stream.")));
}

} // namespace rl
