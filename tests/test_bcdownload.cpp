#include "protocol/BaichuanDownload.h"
#include "protocol/BcCrypto.h"

#include <QElapsedTimer>
#include <QThread>
#include <QtTest>

#include <deque>

using namespace rl;
using Msg = BcDownloadPort::Message;
using Outcome = BcDownloadResult::Outcome;

namespace {

// Replays scripted replies and records what was sent.
class ScriptedPort : public BcDownloadPort
{
public:
    struct Sent {
        quint32 cmdId;
        int channel;
        QByteArray body;
    };
    std::vector<Sent> sent;
    std::deque<Msg> replies;
    bool lostWhenEmpty = false;
    std::function<void(int)> beforeNext; // called with the count of messages handed out
    int handedOut = 0;

    bool send(quint32 cmdId, int channel, const QByteArray &body) override
    {
        sent.push_back({cmdId, channel, body});
        return true;
    }
    std::optional<Msg> next(int waitMs, bool *lost) override
    {
        *lost = false;
        if (beforeNext)
            beforeNext(handedOut);
        if (replies.empty()) {
            if (lostWhenEmpty) {
                *lost = true;
                return std::nullopt;
            }
            QThread::msleep(quint32(qMin(waitMs, 5)));
            return std::nullopt;
        }
        Msg m = replies.front();
        replies.pop_front();
        ++handedOut;
        return m;
    }
};

Msg reply(quint16 status, QByteArray xml = "<body><sizeL>1</sizeL></body>", quint32 cmd = 143)
{
    return Msg{cmd, status, false, std::move(xml)};
}
Msg media(QByteArray bytes, quint32 cmd = 143)
{
    return Msg{cmd, 200, true, std::move(bytes)};
}

BcDownloadRequest request()
{
    BcDownloadRequest r;
    r.channel = 4;
    r.uid = QStringLiteral("95270001ABCDEF");
    r.start = QDateTime(QDate(2026, 11, 8), QTime(14, 5, 9));
    r.end = QDateTime(QDate(2026, 11, 8), QTime(14, 7, 9));
    return r;
}

} // namespace

class TestBcDownload : public QObject
{
    Q_OBJECT
private slots:
    // The body matches what Reolink's clients send: main stream forced by supportSub 0,
    // all logical channels, and the clip's own end (not the end of the day).
    void requestXml_namesTheRangeAndMainStream()
    {
        const QByteArray x = bcDownloadXml(request());
        QVERIFY(x.contains("<FileInfoList version=\"1.1\">"));
        QVERIFY(x.contains("<uid>95270001ABCDEF</uid>"));
        QVERIFY(x.contains("<logicChnBitmap>255</logicChnBitmap>"));
        QVERIFY(x.contains("<channelId>4</channelId>"));
        QVERIFY(x.contains("<supportSub>0</supportSub>"));
        QVERIFY(x.contains("<streamType>mainStream</streamType>"));
        QVERIFY(x.contains("<startTime><year>2026</year><month>11</month><day>8</day>"
                           "<hour>14</hour><minute>5</minute><second>9</second></startTime>"));
        QVERIFY(x.contains("<endTime><year>2026</year><month>11</month><day>8</day>"
                           "<hour>14</hour><minute>7</minute><second>9</second></endTime>"));
    }
    void requestXml_leavesOutAnEmptyUid()
    {
        BcDownloadRequest r = request();
        r.uid.clear();
        QVERIFY(!bcDownloadXml(r).contains("<uid>"));
    }

    // Only the first encryptLen bytes are ciphered; the remainder passes through.
    void binaryPayload_decryptsOnlyEncryptLen()
    {
        const QByteArray key = bc::aesKey(QStringLiteral("nonce"), QStringLiteral("pw"));
        const QByteArray plain = "ABCDEFGHIJKLMNOP-tail-in-the-clear";
        const QByteArray wire = bc::aesCfb(plain.left(16), key, false) + plain.mid(16);
        const QByteArray ext = "<Extension version=\"1.1\"><binaryData>1</binaryData>"
                               "<encryptLen>16</encryptLen></Extension>";
        QCOMPARE(bc::binaryPayload(ext, wire, key), plain);
    }
    void binaryPayload_withoutEncryptLenIsPlain()
    {
        const QByteArray ext = "<Extension version=\"1.1\"><binaryData>1</binaryData></Extension>";
        QCOMPARE(bc::binaryPayload(ext, "raw", QByteArray(16, 'k')), QByteArray("raw"));
    }

    // Accepted, two media messages (a block may cross them: the bytes go on in order),
    // then status 300: done, and 144 closes it on the same channel.
    void download_endsOnStatus300AndSends144()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("1001-head|N0dc-par"), media("t-two|05wb"), reply(300, {})};
        QByteArray got;
        const BcDownloadResult r =
            runBcDownload(port, request(), [&](const QByteArray &b) { got += b; });
        QCOMPARE(r.outcome, Outcome::Done);
        QCOMPARE(r.mediaBytes, qint64(got.size()));
        QCOMPARE(got, QByteArray("1001-head|N0dc-part-two|05wb"));
        QCOMPARE(port.sent.size(), size_t(2));
        QCOMPARE(port.sent[0].cmdId, kBcCmdDownload);
        QCOMPARE(port.sent[0].channel, 4);
        QCOMPARE(port.sent[0].body, bcDownloadXml(request()));
        QCOMPARE(port.sent[1].cmdId, kBcCmdDownloadStop);
        QCOMPARE(port.sent[1].channel, 4);
        QVERIFY(port.sent[1].body.isEmpty());
    }
    void download_endsOnStatus331()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("x"), reply(331, {})};
        const BcDownloadResult r = runBcDownload(port, request(), [](const QByteArray &) {});
        QCOMPARE(r.outcome, Outcome::Done);
        QCOMPARE(r.status, quint16(331));
        QCOMPARE(port.sent.back().cmdId, kBcCmdDownloadStop);
    }
    void download_endsOnAnEmpty143()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("x"), reply(200, {})};
        const BcDownloadResult r = runBcDownload(port, request(), [](const QByteArray &) {});
        QCOMPARE(r.outcome, Outcome::Done);
        QCOMPARE(port.sent.back().cmdId, kBcCmdDownloadStop);
    }
    // Media before the accepting reply still counts (the reply and data can race).
    void download_takesMediaBeforeTheReply()
    {
        ScriptedPort port;
        port.replies = {media("early"), reply(200), media("late"), reply(300, {})};
        QByteArray got;
        const BcDownloadResult r =
            runBcDownload(port, request(), [&](const QByteArray &b) { got += b; });
        QCOMPARE(r.outcome, Outcome::Done);
        QCOMPARE(got, QByteArray("earlylate"));
    }
    // Messages for other commands (keep-alives, pushed alarms) are not footage.
    void download_ignoresOtherCommands()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("alarm", 33), reply(200, "<x/>", 93), media("ok"), reply(300, {})};
        QByteArray got;
        runBcDownload(port, request(), [&](const QByteArray &b) { got += b; });
        QCOMPARE(got, QByteArray("ok"));
    }
    void download_endingWithoutFootageIsNoData()
    {
        ScriptedPort port;
        port.replies = {reply(200), reply(300, {})};
        const BcDownloadResult r = runBcDownload(port, request(), [](const QByteArray &) {});
        QCOMPARE(r.outcome, Outcome::NoData);
        QCOMPARE(port.sent.back().cmdId, kBcCmdDownloadStop);
    }
    // A refusal (e.g. another download already running) is not acknowledged with 144.
    void download_refusalReportsTheStatus()
    {
        ScriptedPort port;
        port.replies = {reply(400, {})};
        const BcDownloadResult r = runBcDownload(port, request(), [](const QByteArray &) {});
        QCOMPARE(r.outcome, Outcome::Refused);
        QCOMPARE(r.status, quint16(400));
        QCOMPARE(port.sent.size(), size_t(1));
    }
    // An error after the recorder accepted fails at once (not after the stall wait) and
    // closes the download.
    void download_errorAfterAcceptFailsAndSends144()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("a"), reply(500, {}), media("b")};
        BcDownloadOptions o;
        o.stallMs = 5000;
        QElapsedTimer t;
        t.start();
        const BcDownloadResult r = runBcDownload(port, request(), [](const QByteArray &) {}, o);
        QVERIFY(t.elapsed() < 1000);
        QCOMPARE(r.outcome, Outcome::Refused);
        QCOMPARE(r.status, quint16(500));
        QCOMPARE(port.sent.back().cmdId, kBcCmdDownloadStop);
    }
    // The size the recorder announces in its first reply (sizeL + sizeH << 32) can veto the
    // download before any footage is taken, e.g. when the disk is too small.
    void download_acceptHookSeesAnnouncedSize()
    {
        ScriptedPort port;
        port.replies = {reply(200, "<body><FileInfoList><FileInfo><sizeL>11989232</sizeL>"
                                   "<sizeH>1</sizeH></FileInfo></FileInfoList></body>"),
                        media("a"), reply(300, {})};
        qint64 seen = 0;
        BcDownloadOptions o;
        o.onAccepted = [&](qint64 bytes) { seen = bytes; return false; };
        QByteArray got;
        const BcDownloadResult r =
            runBcDownload(port, request(), [&](const QByteArray &b) { got += b; }, o);
        QCOMPARE(seen, (qint64(1) << 32) + 11989232);
        QCOMPARE(r.outcome, Outcome::Cancelled);
        QVERIFY(got.isEmpty());
        QCOMPARE(port.sent.back().cmdId, kBcCmdDownloadStop);
    }
    // A size the recorder gets wrong (-1 is seen in the wild) is reported as unknown.
    void download_acceptHookUnknownSize()
    {
        ScriptedPort port;
        port.replies = {reply(200, "<body><sizeL>-1</sizeL><sizeH>0</sizeH></body>"), media("a"),
                        reply(300, {})};
        qint64 seen = 0;
        BcDownloadOptions o;
        o.onAccepted = [&](qint64 bytes) { seen = bytes; return true; };
        QCOMPARE(runBcDownload(port, request(), [](const QByteArray &) {}, o).outcome, Outcome::Done);
        QCOMPARE(seen, qint64(-1));
    }

    void download_cancelSends144()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("a"), media("b"), media("c"), reply(300, {})};
        std::atomic<bool> cancel{false};
        port.beforeNext = [&](int n) {
            if (n == 2)
                cancel = true;
        };
        BcDownloadOptions o;
        o.cancel = &cancel;
        QByteArray got;
        const BcDownloadResult r =
            runBcDownload(port, request(), [&](const QByteArray &b) { got += b; }, o);
        QCOMPARE(r.outcome, Outcome::Cancelled);
        QCOMPARE(got, QByteArray("a"));
        QCOMPARE(port.sent.back().cmdId, kBcCmdDownloadStop);
    }
    void download_stallGivesUpAndSends144()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("a")};
        BcDownloadOptions o;
        o.stallMs = 60;
        const BcDownloadResult r = runBcDownload(port, request(), [](const QByteArray &) {}, o);
        QCOMPARE(r.outcome, Outcome::Stalled);
        QCOMPARE(port.sent.back().cmdId, kBcCmdDownloadStop);
    }
    void download_lostConnection()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("a")};
        port.lostWhenEmpty = true;
        const BcDownloadResult r = runBcDownload(port, request(), [](const QByteArray &) {});
        QCOMPARE(r.outcome, Outcome::Lost);
    }
    void download_traceSeesControlMessages()
    {
        ScriptedPort port;
        port.replies = {reply(200), media("a"), reply(300, {})};
        std::vector<quint16> seen;
        BcDownloadOptions o;
        o.trace = [&](const Msg &m) { seen.push_back(m.status); };
        runBcDownload(port, request(), [](const QByteArray &) {}, o);
        QCOMPARE(seen, (std::vector<quint16>{200, 300}));
    }
};

QTEST_GUILESS_MAIN(TestBcDownload)
#include "test_bcdownload.moc"
