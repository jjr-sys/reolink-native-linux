#include "BaichuanDownload.h"

#include "protocol/BaichuanControl.h"
#include "protocol/BcCrypto.h"

#include <QElapsedTimer>

namespace rl {

namespace {

QByteArray timeXml(const char *tag, const QDateTime &dt)
{
    const QDate d = dt.date();
    const QTime t = dt.time();
    return QStringLiteral("<%1><year>%2</year><month>%3</month><day>%4</day>"
                          "<hour>%5</hour><minute>%6</minute><second>%7</second></%1>")
        .arg(QLatin1String(tag))
        .arg(d.year())
        .arg(d.month())
        .arg(d.day())
        .arg(t.hour())
        .arg(t.minute())
        .arg(t.second())
        .toUtf8();
}

class ControlPort : public BcDownloadPort
{
public:
    explicit ControlPort(BaichuanControl &c) : m_c(c), m_messId(c.nextMessId()) {}
    bool send(quint32 cmdId, int channel, const QByteArray &bodyXml) override
    {
        return m_c.sendRequest(cmdId, channel, m_messId, bodyXml);
    }
    std::optional<Message> next(int waitMs, bool *lost) override
    {
        const auto m = m_c.readMessage(waitMs, lost);
        if (!m)
            return std::nullopt;
        return Message{m->cmdId, m->status, m->binary, m->body};
    }

private:
    BaichuanControl &m_c;
    quint32 m_messId;
};

qint64 xmlNumber(const QByteArray &xml, const char *tag, bool *ok)
{
    const QByteArray open = QByteArray("<") + tag + ">";
    const int p = xml.indexOf(open);
    const int q = p < 0 ? -1 : xml.indexOf("</", p);
    if (q < 0) {
        *ok = false;
        return 0;
    }
    return xml.mid(p + open.size(), q - p - open.size()).trimmed().toLongLong(ok);
}

// sizeL + sizeH << 32 from the accepting reply, or -1 when absent or nonsense.
qint64 announcedSize(const QByteArray &xml)
{
    bool okL = false, okH = false;
    const qint64 lo = xmlNumber(xml, "sizeL", &okL);
    const qint64 hi = xmlNumber(xml, "sizeH", &okH);
    if (!okL || lo < 0 || lo > 0xFFFFFFFFll || (okH && (hi < 0 || hi > 0xFFFF)))
        return -1;
    return (okH ? hi << 32 : 0) + lo;
}

bool isEnd(quint16 status)
{
    return status == 300 || status == 331;
}

} // namespace

std::unique_ptr<BcDownloadPort> bcDownloadPort(BaichuanControl &control)
{
    return std::make_unique<ControlPort>(control);
}

QByteArray bcDownloadXml(const BcDownloadRequest &req)
{
    const QByteArray uidEl =
        req.uid.isEmpty() ? QByteArray() : "<uid>" + req.uid.toUtf8() + "</uid>\n";
    // supportSub 1 would force the sub stream whatever streamType says (reolink_aio#240).
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n"
           "<FileInfo>\n" + uidEl
           + "<logicChnBitmap>255</logicChnBitmap>\n<channelId>" + QByteArray::number(req.channel)
           + "</channelId>\n<supportSub>0</supportSub>\n<streamType>mainStream</streamType>\n"
           + timeXml("startTime", req.start) + "\n" + timeXml("endTime", req.end)
           + "\n</FileInfo>\n</FileInfoList>\n</body>\n";
}

BcDownloadResult runBcDownload(BcDownloadPort &port, const BcDownloadRequest &req,
                               const std::function<void(const QByteArray &)> &media,
                               const BcDownloadOptions &opts)
{
    BcDownloadResult r;
    auto cancelled = [&] { return opts.cancel && opts.cancel->load(); };
    auto stop = [&] { port.send(kBcCmdDownloadStop, req.channel, {}); };

    if (!port.send(kBcCmdDownload, req.channel, bcDownloadXml(req))) {
        r.outcome = BcDownloadResult::Outcome::Lost;
        return r;
    }

    bool accepted = false;
    bool askedCaller = false;
    QElapsedTimer quiet;
    quiet.start();
    const int waitMs = qBound(1, opts.stallMs, 300);
    for (;;) {
        if (cancelled()) {
            stop();
            r.outcome = BcDownloadResult::Outcome::Cancelled;
            return r;
        }
        bool lost = false;
        const std::optional<BcDownloadPort::Message> m = port.next(waitMs, &lost);
        if (lost) {
            r.outcome = BcDownloadResult::Outcome::Lost;
            return r;
        }
        if (cancelled())
            continue; // handled at the top, before any more footage is taken
        if (!m) {
            if (quiet.elapsed() >= opts.stallMs) {
                stop();
                r.outcome = BcDownloadResult::Outcome::Stalled;
                return r;
            }
            continue;
        }
        if (m->cmdId != kBcCmdDownload && m->cmdId != kBcCmdDownloadStop)
            continue;
        quiet.restart();
        if (m->cmdId == kBcCmdDownload && m->binary) {
            accepted = true; // footage can race ahead of the reply
            r.mediaBytes += m->body.size();
            media(m->body);
            continue;
        }
        if (opts.trace)
            opts.trace(*m);
        if (m->cmdId != kBcCmdDownload)
            continue;
        const bool end = isEnd(m->status) || (accepted && m->status == 200 && m->body.isEmpty());
        if (end) {
            stop();
            r.status = m->status;
            r.outcome = r.mediaBytes > 0 ? BcDownloadResult::Outcome::Done
                                         : BcDownloadResult::Outcome::NoData;
            return r;
        }
        if (m->status == 200 || m->status == 201) {
            accepted = true;
            if (!askedCaller && opts.onAccepted) {
                askedCaller = true;
                if (!opts.onAccepted(announcedSize(m->body))) {
                    stop();
                    r.outcome = BcDownloadResult::Outcome::Cancelled;
                    return r;
                }
            }
            continue;
        }
        // An error status: before acceptance a refusal, after it a failure part way.
        if (accepted)
            stop();
        r.status = m->status;
        r.outcome = accepted ? BcDownloadResult::Outcome::Failed
                             : BcDownloadResult::Outcome::Refused;
        return r;
    }
}

} // namespace rl
