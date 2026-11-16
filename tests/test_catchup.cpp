#include "media/CatchUp.h"

#include <QtTest>

using namespace rl;

// ● Live without waiting for a keyframe (wayfinder/live-replay ticket 08d): while its pane
// replays, a live player receives but doesn't decode. It holds the packets that bring its
// decoder back to now — from the newest keyframe, or (no keyframe yet) everything since replay
// began, since the decoder still holds the group it stopped in — and when replay ends it decodes
// them unseen and carries on, instead of dropping packets until the next keyframe.
class TestCatchUp : public QObject
{
    Q_OBJECT

private slots:
    void aKeyframeStartsTheHeldGroupAfresh()
    {
        CatchUp c;
        c.begin(true);
        QCOMPARE(c.onPacket(false), CatchUp::Hold::Keep);
        QCOMPARE(c.onPacket(false), CatchUp::Hold::Keep);
        QCOMPARE(c.held(), 2);
        QCOMPARE(c.onPacket(true), CatchUp::Hold::Restart);   // drop the two, keep the keyframe
        QCOMPARE(c.held(), 1);
        QCOMPARE(c.onPacket(false), CatchUp::Hold::Keep);
        QCOMPARE(c.held(), 2);
    }

    void aDecoderThatHasDecodedContinuesFromWhereItStopped()
    {
        CatchUp c;
        c.begin(true);
        c.onPacket(false);
        c.onPacket(false);
        QCOMPARE(c.end(false), CatchUp::Resume::FeedHeld);
    }

    void aFreshDecoderNeedsAKeyframe()
    {
        // Replay from a session's start (a reconnect while replaying): nothing decoded yet, so
        // packets before the first keyframe are no use.
        CatchUp c;
        c.begin(false);
        QCOMPARE(c.onPacket(false), CatchUp::Hold::Drop);
        QCOMPARE(c.held(), 0);
        QCOMPARE(c.end(false), CatchUp::Resume::AwaitKeyframe);

        c.begin(false);
        c.onPacket(false);
        QCOMPARE(c.onPacket(true), CatchUp::Hold::Restart);
        c.onPacket(false);
        QCOMPARE(c.held(), 2);
        QCOMPARE(c.end(false), CatchUp::Resume::FeedHeld);
    }

    void endingOnAKeyframeNeedsNothingHeld()
    {
        CatchUp c;
        c.begin(true);
        c.onPacket(true);
        c.onPacket(false);
        QCOMPARE(c.end(true), CatchUp::Resume::Continue);
    }

    void nothingMissedContinues()
    {
        // Replay ended before a packet arrived: the decoder lost nothing.
        CatchUp c;
        c.begin(true);
        QCOMPARE(c.end(false), CatchUp::Resume::Continue);
    }

    void tooLongWithoutAKeyframeFallsBackToWaiting()
    {
        CatchUp c;
        c.begin(true);
        for (int i = 0; i < CatchUp::kMaxHeld; ++i)
            QCOMPARE(c.onPacket(false), CatchUp::Hold::Keep);
        QCOMPARE(c.onPacket(false), CatchUp::Hold::Overflow);   // drop what's held
        QCOMPARE(c.held(), 0);
        QCOMPARE(c.onPacket(false), CatchUp::Hold::Drop);
        QCOMPARE(c.end(false), CatchUp::Resume::AwaitKeyframe);

        // A keyframe after the overflow starts a good group again.
        c.begin(true);
        for (int i = 0; i <= CatchUp::kMaxHeld; ++i)
            c.onPacket(false);
        QCOMPARE(c.onPacket(true), CatchUp::Hold::Restart);
        QCOMPARE(c.end(false), CatchUp::Resume::FeedHeld);
    }

    void beginForgetsTheLastReplay()
    {
        CatchUp c;
        c.begin(true);
        c.onPacket(true);
        c.onPacket(false);
        c.end(false);
        c.begin(true);
        QCOMPARE(c.held(), 0);
        QCOMPARE(c.end(false), CatchUp::Resume::Continue);
    }
};

QTEST_GUILESS_MAIN(TestCatchUp)
#include "test_catchup.moc"
