#include "media/FrameTimeSei.h"

#include <QtTest>

using namespace rl;
using Codec = BcMediaParser::Codec;

namespace {

QByteArray sc() { return QByteArray("\x00\x00\x00\x01", 4); }

// NAL unit types in order, from an Annex-B access unit (H.264: low 5 bits; H.265: bits 1–6).
QList<int> nalTypes(const QByteArray &au, Codec codec)
{
    QList<int> out;
    for (int i = 0; i + 3 < au.size(); ++i) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            const quint8 h = quint8(au[i + 3]);
            out << (codec == Codec::H265 ? (h >> 1) & 0x3F : h & 0x1F);
            i += 2;
        }
    }
    return out;
}

} // namespace

// The recording time of each Playback frame (wayfinder/playback-controls ticket 11): the
// recorder client tags each access unit with a user-data SEI that the decoder hands back on
// the decoded frame, so the playhead can follow the frame on screen.
class TestFrameTime : public QObject
{
    Q_OBJECT
private slots:
    // The SEI goes in before the first picture slice, after the parameter sets.
    void h264SeiGoesBeforeTheFirstSlice()
    {
        const QByteArray au = sc() + QByteArray("\x67sps", 4) + sc() + QByteArray("\x68pps", 4)
                            + sc() + QByteArray("\x65slice", 6);
        const QByteArray tagged = frametime::tag(au, Codec::H264, 1763100000123);
        QCOMPARE(nalTypes(tagged, Codec::H264), (QList<int>{7, 8, 6, 5}));
        QVERIFY(tagged.endsWith(sc() + QByteArray("\x65slice", 6)));
    }

    void h264PFrameGetsTheSeiFirst()
    {
        const QByteArray au = sc() + QByteArray("\x41p", 2);
        QCOMPARE(nalTypes(frametime::tag(au, Codec::H264, 5), Codec::H264), (QList<int>{6, 1}));
    }

    void h265SeiIsAPrefixSei()
    {
        // VPS 32, SPS 33, PPS 34, IDR_W_RADL 19 (two-byte NAL headers).
        const QByteArray au = sc() + QByteArray("\x40\x01v", 3) + sc() + QByteArray("\x42\x01s", 3)
                            + sc() + QByteArray("\x44\x01p", 3) + sc() + QByteArray("\x26\x01i", 3);
        QCOMPARE(nalTypes(frametime::tag(au, Codec::H265, 7), Codec::H265),
                 (QList<int>{32, 33, 34, 39, 19}));
    }

    // Read back from the bytes the decoder hands over (UUID + payload, emulation removed).
    void roundTripsThroughTheSidePayload()
    {
        for (const qint64 ms : {qint64(0), qint64(1), qint64(1763100000123), qint64(0x0000000300000003)}) {
            for (Codec c : {Codec::H264, Codec::H265}) {
                const QByteArray au = sc() + (c == Codec::H264 ? QByteArray("\x41p", 2)
                                                               : QByteArray("\x02\x01p", 3));
                const QByteArray tagged = frametime::tag(au, c, ms);
                QCOMPARE(frametime::find(tagged, c), ms);
                const QByteArray side = frametime::userData(ms);
                QCOMPARE(frametime::fromUserData(reinterpret_cast<const uchar *>(side.constData()),
                                                 side.size()),
                         ms);
            }
        }
    }

    // A tag never contains a start code: zero bytes in the time are escaped (00 00 03).
    void timeWithZeroBytesIsEscaped()
    {
        const QByteArray au = sc() + QByteArray("\x41p", 2);
        const QByteArray tagged = frametime::tag(au, Codec::H264, 0x0000000100000000);
        // Only the two real start codes.
        QCOMPARE(nalTypes(tagged, Codec::H264).size(), 2);
        QCOMPARE(frametime::find(tagged, Codec::H264), qint64(0x0000000100000000));
    }

    // Anyone else's user data (another UUID, too short) is not a time.
    void otherUserDataIsIgnored()
    {
        QByteArray other = frametime::userData(42);
        other[0] = char(other[0] ^ 0xFF);
        QCOMPARE(frametime::fromUserData(reinterpret_cast<const uchar *>(other.constData()),
                                         other.size()),
                 qint64(-1));
        QCOMPARE(frametime::fromUserData(reinterpret_cast<const uchar *>(other.constData()), 10),
                 qint64(-1));
        QCOMPARE(frametime::find(sc() + QByteArray("\x41p", 2), Codec::H264), qint64(-1));
    }

    // An unknown codec is left alone.
    void unknownCodecUntouched()
    {
        const QByteArray au = sc() + QByteArray("\x41p", 2);
        QCOMPARE(frametime::tag(au, Codec::Unknown, 9), au);
    }
};

QTEST_GUILESS_MAIN(TestFrameTime)
#include "test_frametime.moc"
