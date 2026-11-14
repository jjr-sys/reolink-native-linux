#include "protocol/BcReplay.h"

#include <QtGlobal>

namespace rl::bc {

namespace {

QByteArray num(int v) { return QByteArray::number(v); }

} // namespace

QByteArray timeXml(const char *tag, const QDateTime &t)
{
    const QDate d = t.date();
    const QTime h = t.time();
    return QByteArray("<") + tag + "><year>" + num(d.year()) + "</year><month>" + num(d.month())
           + "</month><day>" + num(d.day()) + "</day><hour>" + num(h.hour()) + "</hour><minute>"
           + num(h.minute()) + "</minute><second>" + num(h.second()) + "</second></" + tag + ">";
}

namespace {

QByteArray streamName(bool mainStream) { return mainStream ? "mainStream" : "subStream"; }

// The recorder's window for the replay: from `from` to the end of its day, in seconds of the day.
QByteArray durationList(int channel, bool mainStream, const QDateTime &from)
{
    const QDate d = from.date();
    return "<durationList><year>" + num(d.year()) + "</year><month>" + num(d.month()) + "</month><day>"
           + num(d.day()) + "</day><duration><channelId>" + num(channel) + "</channelId><streamType>"
           + streamName(mainStream) + "</streamType><times><i><s>"
           + num(QTime(0, 0).secsTo(from.time())) + "</s><e>86399</e></i></times></duration></durationList>";
}

QDateTime endOfDay(const QDateTime &t) { return QDateTime(t.date(), QTime(23, 59, 59)); }

const QByteArray kHead = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>";
const QByteArray kTail = "</body>\n";

} // namespace

int recorderSpeed(double speed)
{
    return qBound(1, qRound(speed), 32);
}

QByteArray replayStartXml(int channel, bool mainStream, int playSpeed, const QDateTime &start)
{
    return kHead + "<ReplayByTimeV2 version=\"1.1\"><seq>1</seq><streamType>" + streamName(mainStream)
           + "</streamType><playSpeed>" + num(playSpeed) + "</playSpeed>" + timeXml("startTime", start)
           + timeXml("endTime", endOfDay(start)) + "<sOffset>0</sOffset>"
           + durationList(channel, mainStream, start) + "</ReplayByTimeV2>" + kTail;
}

QByteArray replaySeekXml(int channel, bool mainStream, const QDateTime &to)
{
    return kHead + "<ReplaySeekV2 version=\"1.1\"><seq>1</seq>" + timeXml("seekTime", to)
           + durationList(channel, mainStream, to) + "</ReplaySeekV2>" + kTail;
}

bool seekInPlace(const QDate &replayDay, const QDateTime &to)
{
    return replayDay == to.date();
}

} // namespace rl::bc
