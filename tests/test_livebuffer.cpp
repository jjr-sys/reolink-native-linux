#include "media/LiveBuffer.h"

#include <QtTest>

using namespace rl;
using reverse::Frame;

// The live buffer (wayfinder/live-replay tickets 05, 08, 08b): each streaming recorder pane keeps
// its last 5 minutes of compressed live frames, stamped with the camera's clock, so Live replay
// can start from now and run back into the recorder's footage. Kept in whole keyframe groups
// (nothing before a keyframe can be decoded); a quality change or a gap starts a new run and the
// older runs stay; all buffers together stay under a byte cap, oldest groups going first.
class TestLiveBuffer : public QObject
{
    Q_OBJECT

    static Frame frame(qint64 ms, bool key, int bytes = 100)
    {
        Frame f;
        f.key = key;
        f.epochMs = ms;
        f.codec = BcMediaParser::Codec::H264;
        f.annexB = QByteArray(bytes, 'x');
        return f;
    }
    // `secs` seconds of 10 fps from `fromMs`, a keyframe every `gopSecs`.
    static void feed(LiveBuffer &b, qint64 fromMs, int secs, int gopSecs = 1, bool main = false,
                     int bytes = 100)
    {
        for (int i = 0; i < secs * 10; ++i)
            b.push(frame(fromMs + i * 100, i % (gopSecs * 10) == 0, bytes), main);
    }

private slots:
    void framesBeforeTheFirstKeyframeAreDropped()
    {
        LiveBuffer b;
        b.push(frame(1000, false), false);
        b.push(frame(1100, false), false);
        QVERIFY(b.empty());
        QCOMPARE(b.oldestMs(), qint64(-1));
        b.push(frame(1200, true), false);
        b.push(frame(1300, false), false);
        QCOMPARE(b.oldestMs(), qint64(1200));
        QCOMPARE(b.newestMs(), qint64(1300));
        QCOMPARE(b.bytes(), qint64(200));
    }

    void itCoversFromItsOldestToItsNewestFrame()
    {
        LiveBuffer b;
        feed(b, 10000, 5);   // 10.0 .. 14.9 s
        QVERIFY(b.covers(10000));
        QVERIFY(b.covers(12345));
        QVERIFY(b.covers(14900));
        QVERIFY(!b.covers(9999));
        QVERIFY(!b.covers(15500));
    }

    void itKeepsFiveMinutesInWholeGroups()
    {
        LiveBuffer b;
        feed(b, 0, 400, 2);   // 0 .. 399.9 s, a keyframe every 2 s
        // The oldest group kept is the last one starting at or before newest − 5 min.
        QCOMPARE(b.newestMs(), qint64(399900));
        QCOMPARE(b.oldestMs(), qint64(98000));
        QVERIFY(b.newestMs() - b.oldestMs() >= LiveBuffer::kSpanMs);
        QVERIFY(b.newestMs() - b.oldestMs() < LiveBuffer::kSpanMs + 2000);
    }

    void aQualityChangeStartsANewRunAndKeepsTheOlderOne()
    {
        LiveBuffer b;
        feed(b, 0, 10, 1, /*main=*/false);         // SD 0 .. 9.9 s
        feed(b, 10000, 10, 1, /*main=*/true);      // HD 10 .. 19.9 s
        QCOMPARE(b.runs(), 2);
        QCOMPARE(b.oldestMs(), qint64(0));
        QVERIFY(b.covers(5000));
        QVERIFY(b.covers(15000));
        QVERIFY(b.mainAt(15000));
        QVERIFY(!b.mainAt(5000));
        // Reverse from 15 s plays the HD run only, up to its frame at 15 s.
        const LiveBuffer::Run hd = b.runAt(15000);
        QVERIFY(hd.main);
        QCOMPARE(hd.frames.first().epochMs, qint64(10000));
        QVERIFY(hd.frames.last().epochMs >= 15000);
        QCOMPARE(hd.startMs, qint64(10000));
        // From the start of the HD run the SD run comes next (the reverse source carries on).
        const LiveBuffer::Run sd = b.runAt(9900);
        QVERIFY(!sd.main);
        QCOMPARE(sd.frames.size(), 100);
    }

    void aNewRunWaitsForItsKeyframe()
    {
        LiveBuffer b;
        feed(b, 0, 2, 1, false);
        b.push(frame(2000, false), true);   // HD, but not a keyframe yet: dropped
        QCOMPARE(b.runs(), 1);
        QCOMPARE(b.newestMs(), qint64(1900));
        b.push(frame(2100, true), true);
        QCOMPARE(b.runs(), 2);
        QCOMPARE(b.newestMs(), qint64(2100));
    }

    void aGapStartsANewRun()
    {
        LiveBuffer b;
        feed(b, 0, 3);
        b.breakRun();          // the stream stopped (a hidden pane)
        feed(b, 60000, 3);     // and came back a minute later
        QCOMPARE(b.runs(), 2);
        QVERIFY(!b.covers(30000));   // nothing in the gap
        QVERIFY(b.covers(1000));
        QVERIFY(b.covers(61000));
        // A jump in the camera's clock (backwards, or more than 5 s ahead) is a gap too.
        feed(b, 70000, 2);
        QCOMPARE(b.runs(), 3);
    }

    void runAtIsEmptyOutsideTheBuffer()
    {
        LiveBuffer b;
        feed(b, 10000, 2);
        QVERIFY(b.runAt(5000).frames.isEmpty());
        QVERIFY(b.runAt(50000).frames.isEmpty());
    }

    void thePoolDropsTheOldestGroupsAcrossBuffersOverItsCap()
    {
        LiveBufferPool pool(30000);   // bytes
        auto a = pool.buffer(QStringLiteral("5:0"));
        auto c = pool.buffer(QStringLiteral("1:6"));
        QCOMPARE(pool.buffer(QStringLiteral("5:0")), a);   // one per camera
        feed(*a, 0, 10);         // 100 frames × 100 B = 10 000 B, 0 .. 9.9 s
        feed(*c, 5000, 10);      // 10 000 B, 5 .. 14.9 s
        QCOMPARE(pool.bytes(), qint64(20000));
        feed(*a, 10000, 15);     // +15 000 B, a now runs to 24.9 s: 35 000 B in all
        QVERIFY(pool.bytes() <= 30000);
        // The oldest groups went first: a's from 0 s, not c's from 5 s.
        QCOMPARE(a->oldestMs(), qint64(5000));
        QCOMPARE(c->oldestMs(), qint64(5000));
        QCOMPARE(pool.find(QStringLiteral("9:9")), std::shared_ptr<LiveBuffer>());
    }

    // ---- Stamping frames in the player (ticket 08a: every live stream has the camera's clock)
    void anRtspFrameTakesTheRtcpWallClock()
    {
        // start_time_realtime (µs) + (pts − the stream's first pts) in a 90 kHz time base.
        QCOMPARE(livebuffer::rtcpEpochMs(1794775880000000LL, 90000 * 3 + 45000, 1, 90000),
                 qint64(1794775883500));
        QCOMPARE(livebuffer::rtcpEpochMs(0, 900, 1, 90000), qint64(-1));    // no report yet
        QCOMPARE(livebuffer::rtcpEpochMs(-1, 900, 1, 90000), qint64(-1));
    }

    void aKeyframeWithoutParameterSetsGetsTheStreams()
    {
        const QByteArray sps("\x00\x00\x00\x01\x67\x42", 6);
        const QByteArray pps("\x00\x00\x00\x01\x68\xce", 6);
        const QByteArray idr("\x00\x00\x00\x01\x65\x88", 6);
        const QByteArray extra = sps + pps;
        QCOMPARE(livebuffer::withParameterSets(idr, extra, BcMediaParser::Codec::H264), extra + idr);
        // Already there: unchanged.
        QCOMPARE(livebuffer::withParameterSets(sps + pps + idr, extra, BcMediaParser::Codec::H264),
                 sps + pps + idr);
        // H.265: the VPS (type 32) is the marker.
        const QByteArray vps("\x00\x00\x00\x01\x40\x01", 6);
        const QByteArray hidr("\x00\x00\x00\x01\x26\x01", 6);
        QCOMPARE(livebuffer::withParameterSets(hidr, vps, BcMediaParser::Codec::H265), vps + hidr);
        QCOMPARE(livebuffer::withParameterSets(vps + hidr, vps, BcMediaParser::Codec::H265), vps + hidr);
        // No extradata: unchanged.
        QCOMPARE(livebuffer::withParameterSets(idr, QByteArray(), BcMediaParser::Codec::H264), idr);
    }

    void thePoolNeverEmptiesABufferBelowOneGroup()
    {
        LiveBufferPool pool(500);
        auto a = pool.buffer(QStringLiteral("5:0"));
        feed(*a, 0, 3);   // 3 groups of 1 000 B
        QVERIFY(!a->empty());
        QCOMPARE(a->oldestMs(), qint64(2000));
    }
};

QTEST_GUILESS_MAIN(TestLiveBuffer)
#include "test_livebuffer.moc"
