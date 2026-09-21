#include "media/ClipLoader.h"

#include <QFile>
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
};
QTEST_GUILESS_MAIN(TestClipRemux)
#include "test_clipremux.moc"
