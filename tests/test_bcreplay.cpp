#include "protocol/BcReplay.h"

#include <QDateTime>
#include <QtTest>

using namespace rl;

// Recorder-paced replay on Reolink NVRs (wayfinder/playback-controls ticket 10): cmd 381
// ReplayByTimeV2 starts it at a speed the recorder paces, 383 ReplaySeekV2 moves it in place,
// 382 stops it. Bodies as measured on the RLN8-410 and RLN36 (ticket 08 section 2b, ticket 10).
class TestBcReplay : public QObject
{
    Q_OBJECT

    static QDateTime at(int h, int m, int s) { return QDateTime(QDate(2026, 11, 14), QTime(h, m, s)); }

private slots:
    // The recorder takes whole speeds; below 1x it plays at 1x and the app slows it.
    void recorderSpeed_data()
    {
        QTest::addColumn<double>("speed");
        QTest::addColumn<int>("recorder");
        QTest::newRow("quarter") << 0.25 << 1;
        QTest::newRow("half") << 0.5 << 1;
        QTest::newRow("one") << 1.0 << 1;
        QTest::newRow("two") << 2.0 << 2;
        QTest::newRow("eight") << 8.0 << 8;
        QTest::newRow("sixteen") << 16.0 << 16;
        QTest::newRow("capped") << 64.0 << 32;
        QTest::newRow("nonsense") << 0.0 << 1;
    }
    void recorderSpeed()
    {
        QFETCH(double, speed);
        QFETCH(int, recorder);
        QCOMPARE(bc::recorderSpeed(speed), recorder);
    }

    void startBody()
    {
        const QByteArray x = bc::replayStartXml(6, false, 8, at(8, 16, 59));
        QVERIFY(x.contains("<ReplayByTimeV2 version=\"1.1\">"));
        QVERIFY(x.contains("<seq>1</seq>"));
        QVERIFY(x.contains("<streamType>subStream</streamType><playSpeed>8</playSpeed>"));
        QVERIFY(x.contains("<startTime><year>2026</year><month>11</month><day>14</day>"
                           "<hour>8</hour><minute>16</minute><second>59</second></startTime>"));
        QVERIFY(x.contains("<endTime><year>2026</year><month>11</month><day>14</day>"
                           "<hour>23</hour><minute>59</minute><second>59</second></endTime>"));
        QVERIFY(x.contains("<sOffset>0</sOffset>"));
        // The day's window in seconds of the day, from the start to the end of the day.
        QVERIFY(x.contains("<durationList><year>2026</year><month>11</month><day>14</day>"
                           "<duration><channelId>6</channelId><streamType>subStream</streamType>"
                           "<times><i><s>29819</s><e>86399</e></i></times></duration></durationList>"));
        QVERIFY(bc::replayStartXml(0, true, 1, at(0, 0, 0)).contains("<streamType>mainStream</streamType>"));
    }

    void seekBody()
    {
        const QByteArray x = bc::replaySeekXml(0, false, at(11, 30, 23));
        QVERIFY(x.contains("<ReplaySeekV2 version=\"1.1\"><seq>1</seq>"));
        QVERIFY(x.contains("<seekTime><year>2026</year><month>11</month><day>14</day>"
                           "<hour>11</hour><minute>30</minute><second>23</second></seekTime>"));
        QVERIFY(x.contains("<times><i><s>41423</s><e>86399</e></i></times>"));
        QVERIFY(!x.contains("playSpeed"));
    }

    // A seek within the replay's day moves it in place (383), before or after where it started;
    // to another day the replay restarts (382 then 381).
    void seekStaysOnTheDay()
    {
        const QDate day = at(11, 0, 0).date();
        QVERIFY(bc::seekInPlace(day, at(23, 59, 0)));
        QVERIFY(bc::seekInPlace(day, at(0, 0, 1)));
        QVERIFY(!bc::seekInPlace(day, at(11, 0, 0).addDays(-1)));
    }
};

QTEST_GUILESS_MAIN(TestBcReplay)
#include "test_bcreplay.moc"
