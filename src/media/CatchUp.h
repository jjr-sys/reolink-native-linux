#pragma once

namespace rl {

// ● Live without waiting for a keyframe (wayfinder/live-replay ticket 08d). While its pane
// replays, a live player receives packets but doesn't decode them. This decides which of them
// it holds so that, when replay ends, decoding them unseen brings its decoder back to now:
//  - from the newest keyframe received (a keyframe drops what was held before it);
//  - before any keyframe, everything since replay began — but only when the decoder has decoded
//    something (it still holds the group it stopped in); a fresh decoder needs a keyframe.
// Fed only keyframe flags; the player keeps the packets themselves.
class CatchUp
{
public:
    // A stream without a keyframe for this many packets is broken: stop holding and wait for
    // one (≈13 s at 30 fps; groups are 1–4 s).
    static constexpr int kMaxHeld = 400;

    enum class Hold {
        Keep,       // hold this packet too
        Restart,    // drop what's held, then hold this keyframe
        Drop,       // don't hold it (no use to the decoder)
        Overflow,   // drop what's held and this; hold nothing until a keyframe
    };
    enum class Resume {
        Continue,       // decode on from this packet (nothing missed, or it is a keyframe)
        FeedHeld,       // decode the held packets unseen first, then this one
        AwaitKeyframe,  // drop packets until a keyframe (as before 08d)
    };

    // Replay begins; `decoderReady`: the decoder has decoded since its last flush.
    void begin(bool decoderReady);
    // A packet received while replaying.
    Hold onPacket(bool key);
    // Replay ends; `key`: the first live packet after it is a keyframe. Forgets what was held.
    Resume end(bool key);
    int held() const { return m_held; }

private:
    bool m_ready = false;    // the held packets (or the decoder) start from a keyframe
    bool m_missed = false;   // a packet was dropped since begin()
    int m_held = 0;
};

} // namespace rl
