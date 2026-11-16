#pragma once

#include "media/ReversePlay.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>

#include <memory>

namespace rl {

class LiveBufferPool;

// The live buffer (wayfinder/live-replay tickets 05, 08, 08a, 08b): one camera's last five
// minutes of live frames as received (compressed), each stamped with the camera's clock (ms
// since the epoch), so Live replay can start from now and run back into the recorder's footage.
//
//  - Kept in whole keyframe groups: frames before a run's first keyframe are dropped (they
//    can't be decoded), and trimming takes a whole group at a time.
//  - A run is a stretch of one stream: a change of quality (SD grid ↔ HD maximised) or codec,
//    a gap (breakRun: the stream stopped), or a jump in the camera's clock (backwards, or more
//    than 5 s ahead) starts a new run; the older runs stay (ticket 08b: maximising keeps the
//    SD footage the grid was showing).
//  - Five minutes back from the newest frame: the oldest group kept is the last one starting at
//    or before newest − 5 min. A pool keeps all buffers under its byte cap besides.
// Frames are pushed from a player's worker and read from a replay source's: thread-safe.
class LiveBuffer
{
public:
    static constexpr qint64 kSpanMs = 5 * 60 * 1000;
    static constexpr qint64 kGapMs = 5000;

    explicit LiveBuffer(LiveBufferPool *pool = nullptr) : m_pool(pool) {}

    // A frame of the live stream (`main`: the HD main stream). Its epochMs must be set.
    void push(const reverse::Frame &f, bool main);
    // The stream stopped: the next frame starts a new run (from its keyframe).
    void breakRun();

    bool empty() const;
    qint64 oldestMs() const;   // -1 when empty
    qint64 newestMs() const;   // -1 when empty
    qint64 bytes() const;
    int runs() const;
    // Whether a run holds `ms` (from its first frame to its last).
    bool covers(qint64 ms) const;
    bool mainAt(qint64 ms) const;

    // The buffer's footage as spans, oldest first: runs less than kGapMs apart (a quality
    // change while it streams) make one span.
    struct Span {
        qint64 startMs = 0;
        qint64 endMs = 0;
    };
    QList<Span> spans() const;

    // The run holding `ms` (or, in a break of less than kGapMs after a run, that run): its
    // frames in decode order, from its oldest group to the group holding `ms` (whole); empty
    // when there is none. For reverse play from `ms`.
    struct Run {
        bool main = false;
        qint64 startMs = -1;          // its oldest frame
        QList<reverse::Frame> frames;
    };
    Run runAt(qint64 ms) const;
    // Forwards from `ms` within its run: from the keyframe of the group holding it
    // (fromKeyframe), else the frames after it; empty when no run holds it / nothing after.
    QList<reverse::Frame> framesFrom(qint64 ms, bool fromKeyframe) const;

private:
    friend class LiveBufferPool;
    struct Gop {
        int run = 0;
        bool main = false;
        qint64 bytes = 0;
        QList<reverse::Frame> frames;
    };
    // Pool: the oldest group's first frame time (-1 if it can't spare one), and dropping it.
    qint64 spareOldestMs() const;
    qint64 dropOldest();

    mutable QMutex m_mutex;
    LiveBufferPool *m_pool = nullptr;
    QList<Gop> m_gops;          // oldest first
    qint64 m_bytes = 0;
    int m_run = 0;              // the current run's id
    bool m_runOpen = false;     // the current run has had its keyframe
    bool m_runMain = false;
    BcMediaParser::Codec m_runCodec = BcMediaParser::Codec::Unknown;
    qint64 m_lastMs = -1;       // the newest frame pushed to the current run
};

namespace livebuffer {
// An RTSP frame's camera time: the stream's start from the first RTCP sender report
// (libavformat start_time_realtime, µs since the epoch) plus the frame's pts since the stream's
// first, in the stream's time base. -1 before any report (startRealtimeUs <= 0).
qint64 rtcpEpochMs(qint64 startRealtimeUs, qint64 ptsSinceStart, int tbNum, int tbDen);
// A keyframe access unit with the stream's parameter sets (Annex-B extradata) in front when it
// has none of its own (no SPS for H.264, no VPS for H.265): a buffered group must decode alone.
QByteArray withParameterSets(const QByteArray &annexB, const QByteArray &extradata,
                             BcMediaParser::Codec codec);
} // namespace livebuffer

// Every camera's live buffer, under one byte cap (ticket 08: 600 MB); over it, the oldest
// group across all buffers goes first, never a buffer's last group.
class LiveBufferPool
{
public:
    static constexpr qint64 kDefaultCap = 600LL * 1000 * 1000;
    explicit LiveBufferPool(qint64 capBytes = kDefaultCap) : m_cap(capBytes) {}

    // The camera's buffer ("hostId:channel"), made on first use.
    std::shared_ptr<LiveBuffer> buffer(const QString &key);
    std::shared_ptr<LiveBuffer> find(const QString &key) const;
    qint64 bytes() const;
    // Trim to the cap (each buffer calls it after a push).
    void enforce();

private:
    mutable QMutex m_mutex;
    qint64 m_cap;
    QHash<QString, std::shared_ptr<LiveBuffer>> m_buffers;
};

} // namespace rl
