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
    // supportSub 1 forces the sub stream whatever streamType says (reolink_aio#240).
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n"
           "<FileInfo>\n" + uidEl
           + "<logicChnBitmap>255</logicChnBitmap>\n<channelId>" + QByteArray::number(req.channel)
           + "</channelId>\n<supportSub>" + QByteArray(req.mainStream ? "0" : "1")
           + "</supportSub>\n<streamType>"
           + QByteArray(req.mainStream ? "mainStream" : "subStream") + "</streamType>\n"
           + timeXml("startTime", req.start) + "\n" + timeXml("endTime", req.end)
           + "\n</FileInfo>\n</FileInfoList>\n</body>\n";
}

QByteArray bcBinaryPayload(const QByteArray &extensionXml, const QByteArray &payload,
                           const QByteArray &aesKey)
{
    const int p = extensionXml.indexOf("<encryptLen>");
    if (p < 0)
        return payload;
    const int q = extensionXml.indexOf("</encryptLen>", p);
    const int n = q < 0 ? 0 : extensionXml.mid(p + 12, q - p - 12).trimmed().toInt();
    if (n <= 0)
        return payload;
    return bc::aesCfb(payload.left(n), aesKey, true) + payload.mid(n);
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
            continue;
        }
        if (!accepted) {
            r.status = m->status;
            r.outcome = BcDownloadResult::Outcome::Refused;
            return r;
        }
    }
}

} // namespace rl
