#include "DeviceKind.h"

#include "FrigateApi.h"
#include "protocol/ReolinkApi.h"

#include <QRegularExpression>
#include <QUrl>

namespace rl::device {

Kind kindFromString(const QString &text)
{
    if (text == QLatin1String("nvr"))
        return Kind::Nvr;
    if (text == QLatin1String("stream"))
        return Kind::Stream;
    if (text == QLatin1String("frigate"))
        return Kind::Frigate;
    return Kind::Camera;
}

double streamCostMbps(Kind kind, bool mainStream, int kbps)
{
    const double fallback = mainStream ? 6.0 : 0.5;
    switch (kind) {
    case Kind::Stream: return 1.0;
    case Kind::Frigate: return fallback;
    case Kind::Camera:
    case Kind::Nvr: break;
    }
    return kbps > 0 ? kbps / 1000.0 : fallback;
}

QString kindToString(Kind kind)
{
    switch (kind) {
    case Kind::Camera: return QStringLiteral("camera");
    case Kind::Nvr: return QStringLiteral("nvr");
    case Kind::Stream: return QStringLiteral("stream");
    case Kind::Frigate: return QStringLiteral("frigate");
    }
    return QStringLiteral("camera");
}

Capabilities capabilities(Kind kind)
{
    Capabilities c;
    switch (kind) {
    case Kind::Camera:
        break;
    case Kind::Nvr:
        c.multiChannel = true;
        break;
    case Kind::Stream:
        c.reolinkProtocol = false;
        c.alwaysOnline = true;
        c.hasRecordings = false;
        c.detections = DetectionSource::None;
        break;
    case Kind::Frigate:
        c.reolinkProtocol = false;
        c.multiChannel = true;
        c.frigateApi = true;
        c.detections = DetectionSource::FrigateServer;
        break;
    }
    return c;
}

QVariantMap capabilityMap(Kind kind)
{
    const Capabilities c = capabilities(kind);
    QVariantMap m;
    m[QStringLiteral("reolinkProtocol")] = c.reolinkProtocol;
    m[QStringLiteral("direct")] = c.direct();
    m[QStringLiteral("multiChannel")] = c.multiChannel;
    m[QStringLiteral("alwaysOnline")] = c.alwaysOnline;
    m[QStringLiteral("frigateApi")] = c.frigateApi;
    m[QStringLiteral("hasRecordings")] = c.hasRecordings;
    m[QStringLiteral("hasDetections")] = c.detections != DetectionSource::None;
    return m;
}

namespace {
// Hikvision: ".../Streaming/Channels/<N>01" is the main stream, "<N>02" the sub stream.
const QRegularExpression &hikvisionMain()
{
    static const QRegularExpression re(
        QStringLiteral("(/Streaming/Channels/\\d+?)01(?=/?(?:\\?|$))"));
    return re;
}
} // namespace

bool looksLikeBoschFeed(const QString &label)
{
    return label.contains(QLatin1String("bosch"), Qt::CaseInsensitive);
}

QString directLiveUrl(const QString &savedUrl, bool mainStream)
{
    if (mainStream)
        return savedUrl;
    // Only the path segment changes; credentials and the query stay as saved.
    const QRegularExpressionMatch m = hikvisionMain().match(savedUrl);
    if (!m.hasMatch())
        return savedUrl;
    QString url = savedUrl;
    url.replace(m.capturedEnd(1), 2, QStringLiteral("02"));
    return url;
}

QString liveUrl(const LiveSource &s, bool mainStream)
{
    switch (s.kind) {
    case Kind::Frigate:
        return frigate::liveUrl(s.addr, s.port,
                                mainStream && !s.mainRestream.isEmpty() ? s.mainRestream
                                                                        : s.chanName,
                                s.restreamUser, s.restreamPassword);
    case Kind::Stream: {
        const QString saved = directLiveUrl(s.addr, mainStream);
        if (s.username.isEmpty() && s.password.isEmpty())
            return saved;
        QUrl u(saved);
        u.setUserName(s.username);
        u.setPassword(s.password);
        return u.toString(QUrl::FullyEncoded);
    }
    case Kind::Camera:
    case Kind::Nvr:
        break;
    }
    if (!s.primed)
        return {};
    // Sub stream ("Fluent") is h264 on all models; main ("Clear") may be h265.
    const QString codec = (mainStream && !s.mainCodec.isEmpty()) ? s.mainCodec
                                                                 : QStringLiteral("h264");
    return api::rtspUrl(s.addr, s.username, s.password, s.channel, mainStream, codec);
}

} // namespace rl::device
