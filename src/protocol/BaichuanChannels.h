#pragma once

#include <QByteArray>
#include <QRegularExpression>
#include <QString>
#include <QVector>

namespace rl::bc {

// Channels an NVR really has, read from its Baichuan StreamInfoList reply (cmd 146).
// Each <StreamInfo> carries a <channelBits> mask of the channels it applies to; the union
// of the masks is the set of attached cameras. Works without the HTTP API, which some
// NVRs stop answering while their Baichuan port (9000) keeps serving.
inline QVector<int> channelsFromStreamInfo(const QByteArray &xml)
{
    quint64 mask = 0;
    static const QRegularExpression re(QStringLiteral("<channelBits>\\s*(\\d+)\\s*</channelBits>"));
    auto it = re.globalMatch(QString::fromUtf8(xml));
    while (it.hasNext()) {
        bool ok = false;
        const quint64 bits = it.next().captured(1).toULongLong(&ok);
        if (ok)
            mask |= bits;
    }
    QVector<int> channels;
    for (int ch = 0; ch < 64; ++ch)
        if (mask & (quint64(1) << ch))
            channels.append(ch);
    return channels;
}

// A camera's name from its Baichuan OSD reply (cmd 44, one channel): the <name> inside
// <OsdChannelName>, the label the NVR burns into the picture. This is what the official
// apps show, and it needs no HTTP API. Empty when the reply has none (a third-party
// camera has no OSD to read, and the NVR answers such a channel with status 400).
inline QString osdChannelName(const QByteArray &xml)
{
    static const QRegularExpression re(
        QStringLiteral("<OsdChannelName\\b[^>]*>(?:(?!</OsdChannelName>).)*?<name>([^<]*)</name>"),
        QRegularExpression::DotMatchesEverythingOption);
    QString name = re.match(QString::fromUtf8(xml)).captured(1).trimmed();
    name.replace(QLatin1String("&amp;"), QLatin1String("&"))
        .replace(QLatin1String("&lt;"), QLatin1String("<"))
        .replace(QLatin1String("&gt;"), QLatin1String(">"))
        .replace(QLatin1String("&quot;"), QLatin1String("\""))
        .replace(QLatin1String("&apos;"), QLatin1String("'"));
    return name;
}

} // namespace rl::bc
