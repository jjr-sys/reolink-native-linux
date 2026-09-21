#include "protocol/BaichuanChannels.h"

#include <QtTest>

// Real NVR replies (cmd 146) list stream tables grouped by a channel bitmask; the attached
// cameras are the union of the masks.
class TestBaichuanChannels : public QObject
{
    Q_OBJECT
private slots:
    void unionOfMasksIsTheChannelSet()
    {
        // 257 = ch0+ch8, 68 = ch2+ch6, 8 = ch3, 144 = ch4+ch7, 32 = ch5 (an RLN36 with 8 cameras)
        const QByteArray xml =
            "<body><StreamInfoList>"
            "<StreamInfo><channelBits>257</channelBits></StreamInfo>"
            "<StreamInfo><channelBits>257</channelBits></StreamInfo>"
            "<StreamInfo><channelBits>68</channelBits></StreamInfo>"
            "<StreamInfo><channelBits>8</channelBits></StreamInfo>"
            "<StreamInfo><channelBits>144</channelBits></StreamInfo>"
            "<StreamInfo><channelBits>32</channelBits></StreamInfo>"
            "</StreamInfoList></body>";
        const QVector<int> want{0, 2, 3, 4, 5, 6, 7, 8};
        QCOMPARE(rl::bc::channelsFromStreamInfo(xml), want);
    }
    void emptyOrGarbageGivesNoChannels()
    {
        QVERIFY(rl::bc::channelsFromStreamInfo({}).isEmpty());
        QVERIFY(rl::bc::channelsFromStreamInfo("<body><nope/></body>").isEmpty());
        QVERIFY(rl::bc::channelsFromStreamInfo("<channelBits>abc</channelBits>").isEmpty());
    }
    void highBitsAreKept()
    {
        const QVector<int> want{35};
        QCOMPARE(rl::bc::channelsFromStreamInfo("<channelBits>34359738368</channelBits>"), want);
    }
};

QTEST_GUILESS_MAIN(TestBaichuanChannels)
#include "test_baichuanchannels.moc"
