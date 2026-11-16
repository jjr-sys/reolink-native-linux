#include "ui/PaneSync.h"

#include <QtGlobal>
#include <cmath>

namespace rl::panesync {

QString frameDriver(const QString &lead, const QList<Pane> &panes)
{
    auto drives = [](const Pane &p) { return p.shown && !p.frozen && !p.locked && p.covered; };
    for (const Pane &p : panes)
        if (p.key == lead && drives(p))
            return p.key;
    for (const Pane &p : panes)
        if (drives(p))
            return p.key;
    return {};
}

bool needsRestart(const Pane &pane, double playhead, double speed, bool isDriver, double nowMs)
{
    if (isDriver || !pane.shown || pane.frozen || pane.still || pane.locked || !pane.covered)
        return false;
    if (pane.lastRestartMs >= 0 && nowMs - pane.lastRestartMs < kRestartGapMs)
        return false;
    // 5 s of recording time at 1×, more at speed (a fast stream's frames run ahead in bursts).
    const bool off = pane.frameSecs >= 0
                     && std::abs(pane.frameSecs - playhead) > kInStepSecs * qMax(1.0, speed);
    return !pane.busy || off;
}

} // namespace rl::panesync
