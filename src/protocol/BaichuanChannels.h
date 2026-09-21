#pragma once

#include <QByteArray>
#include <QRegularExpression>
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

} // namespace rl::bc
