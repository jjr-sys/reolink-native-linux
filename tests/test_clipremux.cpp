#include "device/FrigateApi.h"
#include "media/ClipLoader.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

// Needs a real Frigate clip (fragmented MP4, no duration): set RL_TEST_CLIP to one.
// Skipped when unset, so the suite runs anywhere.
class TestClipRemux : public QObject
{
    Q_OBJECT
private slots:
    void remuxGivesIndexedClipWithRealDuration()
    {
        const QString clip = qEnvironmentVariable("RL_TEST_CLIP");
        if (clip.isEmpty())
            QSKIP("RL_TEST_CLIP not set");
        QTemporaryDir dir;
        const QString dst = dir.filePath("out.mp4");
        QString err;
        const qint64 ms = rl::remuxToIndexedMp4(clip, dst, &err);
        QVERIFY2(ms > 0, qPrintable(err));
        qInfo() << "duration ms" << ms;
        QVERIFY(QFileInfo(dst).size() > 1000);
        if (qEnvironmentVariableIsSet("RL_TEST_KEEP"))
            QFile::copy(dst, qEnvironmentVariable("RL_TEST_KEEP"));
    }
    void missingFileFails()
    {
        QString err;
        QCOMPARE(rl::remuxToIndexedMp4("/nonexistent.mp4", "/tmp/never.mp4", &err), qint64(-1));
        QVERIFY(!err.isEmpty());
    }

    // A loader that goes (the page closing, the app quitting) while its clip downloads stops
    // the download: the request closes at once and nothing of it runs on (it used to run on
    // for up to 90 s and hold up quitting; wayfinder/architecture ticket 05).
    void aLoaderThatGoesStopsItsDownload()
    {
        QTcpServer server;
        auto *loader = new rl::ClipLoader;
        QTcpSocket *request = stalledRequest(server, *loader, QStringLiteral("GET /clip.mp4"));
        QVERIFY(request);
        delete loader;
        QTRY_COMPARE_WITH_TIMEOUT(request->state(), QAbstractSocket::UnconnectedState, 3000);
    }

    // The same while it is still signing in to Frigate: going doesn't wait out the login.
    void aLoaderThatGoesStopsItsSignIn()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rl::frigate::setCredentials(QStringLiteral("127.0.0.1"), server.serverPort(),
                                    QStringLiteral("viewer"), QStringLiteral("secret"));
        auto *loader = new rl::ClipLoader;
        QTcpSocket *request = stalledRequest(server, *loader, QStringLiteral("POST /api/login"));
        QVERIFY(request);
        QElapsedTimer took;
        took.start();
        delete loader;   // waits for its worker, which must stop now, not in 90 s
        QVERIFY2(took.elapsed() < 2000, "going must not wait out the sign-in");
        QTRY_COMPARE_WITH_TIMEOUT(request->state(), QAbstractSocket::UnconnectedState, 3000);
        rl::frigate::setCredentials(QStringLiteral("127.0.0.1"), server.serverPort(), {}, {});
    }

    // Cancel, or a newer load, stops the download before it.
    void cancelStopsTheDownload()
    {
        QTcpServer server;
        rl::ClipLoader loader;
        QTcpSocket *request = stalledRequest(server, loader, QStringLiteral("GET /clip.mp4"));
        QVERIFY(request);
        loader.cancel();
        QCOMPARE(loader.state(), rl::ClipLoader::State::Idle);
        QTRY_COMPARE_WITH_TIMEOUT(request->state(), QAbstractSocket::UnconnectedState, 3000);
    }
    void aNewerLoadStopsTheOneBefore()
    {
        QTcpServer server;
        rl::ClipLoader loader;
        QTcpSocket *first = stalledRequest(server, loader, QStringLiteral("GET /clip.mp4"));
        QVERIFY(first);
        loader.load(QStringLiteral("http://127.0.0.1:%1/next.mp4").arg(server.serverPort()));
        QCOMPARE(loader.state(), rl::ClipLoader::State::Loading);
        QTRY_COMPARE_WITH_TIMEOUT(first->state(), QAbstractSocket::UnconnectedState, 3000);
    }

private:
    // Starts `loader` on a clip from `server` (listening here), which takes the request and
    // never answers; returns that request once its first line starts with `firstLine`.
    static QTcpSocket *stalledRequest(QTcpServer &server, rl::ClipLoader &loader,
                                      const QString &firstLine)
    {
        if (!server.isListening() && !server.listen(QHostAddress::LocalHost))
            return nullptr;
        loader.load(QStringLiteral("http://127.0.0.1:%1/clip.mp4").arg(server.serverPort()));
        if (!server.waitForNewConnection(5000))
            return nullptr;
        QTcpSocket *request = server.nextPendingConnection();
        if (!request->waitForReadyRead(5000)
            || !request->peek(64).startsWith(firstLine.toLatin1()))
            return nullptr;
        return request;
    }
};
QTEST_GUILESS_MAIN(TestClipRemux)
#include "test_clipremux.moc"
