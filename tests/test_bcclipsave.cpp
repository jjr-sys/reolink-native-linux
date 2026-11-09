#include "device/BcClipSave.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <deque>

using namespace rl;
using Msg = BcDownloadPort::Message;

namespace {

class ScriptedPort : public BcDownloadPort
{
public:
    std::deque<Msg> replies;
    std::vector<quint32> sent;
    bool send(quint32 cmdId, int, const QByteArray &) override
    {
        sent.push_back(cmdId);
        return true;
    }
    std::optional<Msg> next(int, bool *lost) override
    {
        *lost = replies.empty();
        if (replies.empty())
            return std::nullopt;
        Msg m = replies.front();
        replies.pop_front();
        return m;
    }
};

BcDownloadRequest request(qint64 startEpoch, int secs)
{
    BcDownloadRequest r;
    r.start = QDateTime::fromSecsSinceEpoch(startEpoch);
    r.end = QDateTime::fromSecsSinceEpoch(startEpoch + secs);
    return r;
}

} // namespace

class TestBcClipSave : public QObject
{
    Q_OBJECT
private slots:
    void noFootage_failsAndLeavesNoFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("c.mp4");
        ScriptedPort port;
        port.replies = {Msg{143, 200, false, "<body/>"}, Msg{143, 300, false, {}}};
        const BcClipSaveResult r = saveBcClip(port, request(1000, 15), path, nullptr);
        QVERIFY(!r.ok());
        QCOMPARE(r.download.outcome, BcDownloadResult::Outcome::NoData);
        QVERIFY(!QFile::exists(path));
    }
    // Bytes that hold no video: the download "succeeds", the file does not.
    void footageWithoutVideo_failsAndLeavesNoFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("c.mp4");
        ScriptedPort port;
        port.replies = {Msg{143, 200, false, "<body/>"}, Msg{143, 200, true, "not media"},
                        Msg{143, 300, false, {}}};
        const BcClipSaveResult r = saveBcClip(port, request(1000, 15), path, nullptr);
        QVERIFY(!r.ok());
        QVERIFY(r.download.ok());
        QVERIFY(!r.writeError.isEmpty());
        QVERIFY(!QFile::exists(path));
    }
    void cancelled_leavesNoFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("c.mp4");
        ScriptedPort port;
        port.replies = {Msg{143, 200, false, "<body/>"}, Msg{143, 200, true, "x"}};
        std::atomic<bool> cancel{true};
        const BcClipSaveResult r = saveBcClip(port, request(1000, 15), path, &cancel);
        QCOMPARE(r.download.outcome, BcDownloadResult::Outcome::Cancelled);
        QVERIFY(!QFile::exists(path));
    }

    // Needs a captured stream (see test_bcmp4writer): RL_TEST_BCM, RL_TEST_BCM_START (its
    // first second, Unix time) and RL_TEST_BCM_SECS. Delivered in 16 KB messages.
    void realStream_savesAndReportsProgress()
    {
        const QString src = qEnvironmentVariable("RL_TEST_BCM");
        if (src.isEmpty())
            QSKIP("RL_TEST_BCM not set");
        const qint64 start = qEnvironmentVariable("RL_TEST_BCM_START").toLongLong();
        const int secs = qEnvironmentVariable("RL_TEST_BCM_SECS", "15").toInt();
        QFile in(src);
        QVERIFY(in.open(QIODevice::ReadOnly));
        ScriptedPort port;
        port.replies.push_back(Msg{143, 200, false, "<body/>"});
        while (!in.atEnd())
            port.replies.push_back(Msg{143, 200, true, in.read(16 * 1024)});
        port.replies.push_back(Msg{143, 300, false, {}});

        QTemporaryDir dir;
        const QString path = dir.filePath("c.mp4");
        std::vector<int> seen;
        const BcClipSaveResult r =
            saveBcClip(port, request(start, secs), path, nullptr, [&](int p) { seen.push_back(p); });
        QVERIFY2(r.ok(), qPrintable(r.writeError));
        QVERIFY(QFileInfo(path).size() > 100000);
        // The NVR rounds to whole key-frame groups (2 s on the Farm cameras).
        QVERIFY(qAbs(r.durationMs - secs * 1000) < 2500);
        QVERIFY(!seen.empty());
        QVERIFY(std::is_sorted(seen.begin(), seen.end()));
        QVERIFY(seen.front() >= 0 && seen.back() <= 99);
        QVERIFY2(seen.back() >= 80, qPrintable(QString::number(seen.back())));
        QCOMPARE(port.sent.back(), kBcCmdDownloadStop);
    }
};

QTEST_GUILESS_MAIN(TestBcClipSave)
#include "test_bcclipsave.moc"
