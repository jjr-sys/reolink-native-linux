#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QVector>

#include <functional>

// Activity-mode tile policy: which camera each live-grid tile shows while activity
// (person / vehicle / pet / visitor / motion) is happening. Pure logic with an
// injected clock — no Qt objects, no I/O — so timing rules are tested by handing it
// millisecond timestamps. Rules (wayfinder ticket 11):
//   - a detection asks for a tile; AI detections outrank plain motion
//   - a tile keeps its camera at least 30 s after the last detection, and at least
//     15 s from when it was actually shown (queue waits do not burn that time)
//   - motion only takes a free tile; an AI detection may also take a tile held for
//     motion once that tile has been shown for its minimum time
//   - pinned tiles are never replaced; a full grid queues (nothing is dropped)
//   - at most maxStartsPerHost new streams per NVR per window
//   - a released tile returns to its baseline camera
// The caller applies the returned Changes (start the stream, replay or live) and
// calls tick() about once a second.
namespace rl::activity {

enum class Kind { None = 0, Motion = 1, Person = 2, Vehicle = 3, Pet = 4, Visitor = 5 };

inline int priorityOf(Kind k) { return k == Kind::Motion ? 1 : (k == Kind::None ? 0 : 2); }

struct Params {
    qint64 holdMs = 30'000;
    qint64 minShownMs = 15'000;
    qint64 replayLeadMs = 10'000;
    int maxStartsPerHost = 2;
    qint64 startWindowMs = 2'000;
};

struct TileState {
    int baseline = -1;      // the user's own camera for this tile
    int shown = -1;         // camera on screen now
    bool active = false;    // held because of activity
    bool pinned = false;
    Kind kind = Kind::None;
    qint64 shownSince = 0;  // when the current camera began showing
    qint64 holdUntil = 0;
    qint64 triggerMs = 0;   // when the activity that took the tile was detected
    qint64 idleSince = 0;   // when the tile last became free
};

// One stream switch the UI must perform.
struct Change {
    int tile = -1;
    int row = -1;                 // camera to show
    bool activity = false;        // false: back to the baseline camera (live)
    Kind kind = Kind::None;
    qint64 triggerMs = 0;
    qint64 replayFromMs = 0;      // > 0: play the recording from here; 0: live
};

class ActivityPolicy
{
public:
    explicit ActivityPolicy(Params p = {}) : m_p(p) {}

    // Host (NVR) of a camera row, for the per-NVR start budget. Unset: one host.
    void setHostLookup(std::function<qint64(int)> f) { m_hostOf = std::move(f); }

    // The user's arrangement. Tiles not held for activity show it immediately (the UI
    // rearranged them itself); held tiles keep their activity camera and return to the
    // new baseline when released.
    void setBaseline(const QVector<int> &rows);
    void setPinned(int tile, bool pinned);
    void setExcluded(int row, bool excluded);
    void reset(); // leave Activity mode: forget the queue and any activity state

    // `trigger` is when the activity really began if that is earlier than `now` (the
    // poller sees it up to ~10 s late); it only moves the replay start. Default: now.
    void detect(int row, Kind kind, qint64 now, qint64 trigger = -1);
    QVector<Change> tick(qint64 now);

    int tileCount() const { return m_tiles.size(); }
    const TileState &tile(int i) const { return m_tiles.at(i); }
    int queued() const { return m_queue.size(); }
    // The camera rows waiting for a tile, most urgent first.
    QVector<int> queuedRows() const;

private:
    struct Queued {
        int row;
        Kind kind;
        qint64 trigger;
        qint64 seq;
    };
    qint64 hostOf(int row) const { return m_hostOf ? m_hostOf(row) : 0; }
    int tileShowing(int row) const;
    int pickTile(Kind kind, qint64 now) const;
    bool startAllowed(qint64 host, qint64 now);
    void recordStart(qint64 host, qint64 now) { m_starts[host].append(now); }

    Params m_p;
    std::function<qint64(int)> m_hostOf;
    QVector<TileState> m_tiles;
    QVector<Queued> m_queue;
    QSet<int> m_excluded;
    QHash<qint64, QList<qint64>> m_starts;
    qint64 m_seq = 0;
};

} // namespace rl::activity
