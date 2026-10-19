#pragma once

#include "protocol/ReolinkApi.h"

#include <QByteArray>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

namespace rl::bc {

// Detections a device pushes over Baichuan, unasked, once a connection has subscribed:
// header-only cmd 31 to the host asks for them, cmd 33 carries them, cmd 93 is a no-op
// that keeps an idle connection open. Message numbers and body shape follow the MIT
// reolink_aio (reolink_aio/baichuan/baichuan.py); seen on the household's RLN36 NVRs,
// where every cmd 33 lists all channels (wayfinder/detection-highlights/research/02).
constexpr quint32 kCmdSubscribeEvents = 31;
constexpr quint32 kCmdAlarmEvents = 33;
constexpr quint32 kCmdKeepalive = 93;

// What one channel reports in a cmd 33 <AlarmEventList>.
struct ChannelAlarm {
    int channel = -1;              // the same numbering as the HTTP API
    api::DetectionState state;     // motion from status "MD", AI types from AItype, visitor
};

// Each <AlarmEvent> has a <channelId>, a <status> listing "MD" and "visitor" while active,
// and <AItype> listing "people", "vehicle", "dog_cat" (and others we ignore); "none" means
// nothing. Events without a channel, and anything else in the body, are skipped.
inline QVector<ChannelAlarm> parseAlarmEvents(const QByteArray &xml)
{
    static const QRegularExpression eventRe(QStringLiteral("<AlarmEvent[ >](.*?)</AlarmEvent>"),
                                            QRegularExpression::DotMatchesEverythingOption);
    auto words = [](const QString &event, const QString &tag) {
        const QRegularExpression re(QStringLiteral("<%1>\\s*([^<]*?)\\s*</%1>").arg(tag));
        QStringList out = re.match(event).captured(1).split(QRegularExpression(QStringLiteral("[, ]")),
                                                            Qt::SkipEmptyParts);
        out.removeAll(QStringLiteral("none"));
        return out;
    };
    QVector<ChannelAlarm> out;
    auto it = eventRe.globalMatch(QString::fromUtf8(xml));
    while (it.hasNext()) {
        const QString event = it.next().captured(1);
        bool ok = false;
        const int channel = words(event, QStringLiteral("channelId")).value(0).toInt(&ok);
        if (!ok)
            continue;
        const QStringList status = words(event, QStringLiteral("status"));
        const QStringList ai = words(event, QStringLiteral("AItype"));
        ChannelAlarm a;
        a.channel = channel;
        a.state.motion = status.contains(QStringLiteral("MD"));
        a.state.visitor = status.contains(QStringLiteral("visitor"));
        a.state.person = ai.contains(QStringLiteral("people"));
        a.state.vehicle = ai.contains(QStringLiteral("vehicle"));
        a.state.pet = ai.contains(QStringLiteral("dog_cat"));
        out.append(a);
    }
    return out;
}

} // namespace rl::bc
