#pragma once

#include <QList>
#include <QString>

namespace rl::panesync {

// The rules Playback and Live replay share for panes that play one playhead together
// (architecture review #2, candidate 1). Times are in the caller's units (seconds into the day
// for Playback, epoch seconds for Live replay); wall times are milliseconds from any clock.
struct Pane
{
    QString key;
    bool shown = true;        // on screen (another maximised hides it: it plays nothing)
    bool frozen = false;      // holding its last picture until a new one (a skip)
    bool still = false;       // showing a still, not a stream
    bool busy = false;        // opening or playing
    bool locked = false;      // can't play at this speed (Frigate off 1× forward)
    bool covered = false;     // its camera has footage at the playhead
    double frameSecs = -1;    // the time of the frame on screen; -1 none yet
    double lastRestartMs = -1;
};

// Whose frames move the playhead: `lead`'s pane while it is shown, not frozen, not locked and covered;
// else the first such pane in `panes`' order; else "" (none: the wall clock).
QString frameDriver(const QString &lead, const QList<Pane> &panes);

// A pane that isn't driving plays at its own pace: where it is covered and not playing, or more
// than 5 s × speed from the playhead, it starts again at the playhead — not within 3 s of its
// last restart, and never while hidden, frozen, on a still or locked.
inline constexpr double kInStepSecs = 5;
inline constexpr double kRestartGapMs = 3000;
bool needsRestart(const Pane &pane, double playhead, double speed, bool isDriver, double nowMs);

} // namespace rl::panesync
