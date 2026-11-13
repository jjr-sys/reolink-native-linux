#include "device/EventSnapshot.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;

// Event snapshots are kept in HD (wayfinder/fisheye-and-framing ticket 11): the camera's own
// JPEG untouched up to 2560 px wide (sharper and smaller than re-encoding it), wider pictures
// shrunk to 2560 px.
class TestEventSnapshot : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;

    static QByteArray jpeg(int w, int h)
    {
        QImage img(w, h, QImage::Format_RGB32);
        img.fill(Qt::darkCyan);
        QByteArray out;
        QBuffer buf(&out);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "JPG", 90);
        return out;
    }
    QByteArray readAll(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

private slots:
    void theStoredWidthIsCappedAt2560()
    {
        QCOMPARE(device::storedSnapshotWidth(2560), 2560);
        QCOMPARE(device::storedSnapshotWidth(1920), 1920);
        QCOMPARE(device::storedSnapshotWidth(3840), 2560);
        QCOMPARE(device::storedSnapshotWidth(4512), 2560);
        QCOMPARE(device::storedSnapshotWidth(0), 0);
    }

    void aFisheyeCircleIsKeptAsTheCameraSentIt()
    {
        const QByteArray in = jpeg(2560, 2560);
        const QString path = m_dir.filePath("circle.jpg");
        QVERIFY(device::storeEventSnapshot(in, path));
        QCOMPARE(readAll(path), in);   // byte for byte: no re-encode
    }

    void aSmallerPictureIsKeptToo()
    {
        const QByteArray in = jpeg(1920, 2560);   // the Doorbell, portrait
        const QString path = m_dir.filePath("doorbell.jpg");
        QVERIFY(device::storeEventSnapshot(in, path));
        QCOMPARE(readAll(path), in);
    }

    void aWiderPictureIsShrunkTo2560()
    {
        const QString path = m_dir.filePath("wide.jpg");
        QVERIFY(device::storeEventSnapshot(jpeg(4512, 2512), path));
        const QSize s = QImageReader(path).size();
        QCOMPARE(s.width(), 2560);
        QVERIFY(std::abs(s.height() - qRound(2512 * 2560.0 / 4512)) <= 1);
    }

    void somethingUnreadableIsStillWritten()
    {
        const QByteArray junk("not a picture");
        const QString path = m_dir.filePath("junk.jpg");
        QVERIFY(device::storeEventSnapshot(junk, path));
        QCOMPARE(readAll(path), junk);
    }

    void anUnwritablePlaceFails()
    {
        QVERIFY(!device::storeEventSnapshot(jpeg(100, 100), m_dir.filePath("missing/dir/x.jpg")));
    }
};

QTEST_GUILESS_MAIN(TestEventSnapshot)
#include "test_eventsnapshot.moc"
