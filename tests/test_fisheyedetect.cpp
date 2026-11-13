#include "device/FisheyeDetect.h"

#include <QtTest>

using namespace rl;

// Which cameras get the six fisheye views: the rule in device/FisheyeDetect.h.
class TestFisheyeDetect : public QObject
{
    Q_OBJECT

private slots:
    // Fisheye on every Site (wayfinder/fisheye-and-framing ticket 12): the person's choice wins; else a square
    // main stream is a fisheye; else (size not read this time, e.g. the RLN8 answered 502) the lens
    // remembered from an earlier read; else not a fisheye.
    void fisheyeRule_data()
    {
        QTest::addColumn<int>("userChoice");
        QTest::addColumn<QSize>("mainSize");
        QTest::addColumn<int>("remembered");
        QTest::addColumn<bool>("fisheye");
        QTest::newRow("square main stream") << -1 << QSize(2560, 2560) << -1 << true;
        QTest::newRow("wide main stream") << -1 << QSize(3840, 2160) << -1 << false;
        QTest::newRow("size not read, remembered fisheye") << -1 << QSize() << 1 << true;
        QTest::newRow("size not read, remembered flat") << -1 << QSize() << 0 << false;
        QTest::newRow("size not read, nothing remembered") << -1 << QSize() << -1 << false;
        QTest::newRow("a fresh size beats the memory") << -1 << QSize(2560, 1440) << 1 << false;
        QTest::newRow("person says fisheye") << 1 << QSize(3840, 2160) << 0 << true;
        QTest::newRow("person says not") << 0 << QSize(2560, 2560) << 1 << false;
    }
    void fisheyeRule()
    {
        QFETCH(int, userChoice);
        QFETCH(QSize, mainSize);
        QFETCH(int, remembered);
        QFETCH(bool, fisheye);
        QCOMPARE(device::isFisheye(userChoice, mainSize, remembered), fisheye);
    }

    // What to remember from a read: 1 square, 0 not, -1 nothing to learn (size not read).
    void lensSeenFromSize()
    {
        QCOMPARE(device::lensSeen(QSize(2560, 2560)), 1);
        QCOMPARE(device::lensSeen(QSize(1920, 1080)), 0);
        QCOMPARE(device::lensSeen(QSize()), -1);
        QCOMPARE(device::lensSeen(QSize(0, 0)), -1);
    }
};

QTEST_APPLESS_MAIN(TestFisheyeDetect)
#include "test_fisheyedetect.moc"
