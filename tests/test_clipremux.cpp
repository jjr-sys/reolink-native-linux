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
        QTcpServer server;   // takes the request and never answers
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto *loader = new rl::ClipLoader;
        loader->load(QStringLiteral("http://127.0.0.1:%1/clip.mp4").arg(server.serverPort()));
        QVERIFY(server.waitForNewConnection(5000));
        QTcpSocket *request = server.nextPendingConnection();
        QVERIFY(request->waitForReadyRead(5000));   // the GET is in

        QElapsedTimer took;
        took.start();
        delete loader;
        QVERIFY2(took.elapsed() < 2000, "the loader must not wait out the download");
        QTRY_COMPARE_WITH_TIMEOUT(request->state(), QAbstractSocket::UnconnectedState, 3000);
    }

    // A newer load (or cancel) stops the one before it the same way.
    void cancelStopsTheDownload()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rl::ClipLoader loader;
        loader.load(QStringLiteral("http://127.0.0.1:%1/clip.mp4").arg(server.serverPort()));
        QVERIFY(server.waitForNewConnection(5000));
        QTcpSocket *request = server.nextPendingConnection();
        QVERIFY(request->waitForReadyRead(5000));
        loader.cancel();
        QCOMPARE(loader.state(), rl::ClipLoader::State::Idle);
        QTRY_COMPARE_WITH_TIMEOUT(request->state(), QAbstractSocket::UnconnectedState, 3000);
    }
};
QTEST_GUILESS_MAIN(TestClipRemux)
#include "test_clipremux.moc"
