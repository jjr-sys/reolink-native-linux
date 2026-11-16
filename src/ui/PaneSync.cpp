#include "ui/PaneSync.h"

#include <QtGlobal>
#include <cmath>

namespace rl::panesync {

Pane paneFrom(const QVariantMap &m)
{
    Pane p;
    p.key = m.value(QStringLiteral("key")).toString();
    p.shown = m.value(QStringLiteral("shown"), true).toBool();
    p.frozen = m.value(QStringLiteral("frozen")).toBool();
    p.still = m.value(QStringLiteral("still")).toBool();
    p.busy = m.value(QStringLiteral("busy")).toBool();
    p.aligning = m.value(QStringLiteral("aligning")).toBool();
    p.frameSecs = m.value(QStringLiteral("frameSecs"), -1).toDouble();
    p.lastOpenMs = m.value(QStringLiteral("lastOpenMs"), -1).toDouble();
    return p;
}

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
    if (!pane.shown || pane.frozen || pane.still || pane.locked || pane.aligning || !pane.covered)
        return false;
    if (pane.lastOpenMs >= 0 && nowMs - pane.lastOpenMs < kRestartGapMs)
        return false;
    // Not playing where its camera has footage: start it — the driver too (a lead that joined
    // and never opened moves nothing, and the playhead would wait for it forever).
    if (!pane.busy)
        return true;
    // Drifted: 5 s of recording time at 1×, more at speed (a fast stream's frames run ahead in
    // bursts). Never the driver: its frames are what move the playhead.
    return !isDriver && pane.frameSecs >= 0
           && std::abs(pane.frameSecs - playhead) > kInStepSecs * qMax(1.0, speed);
}

} // namespace rl::panesync
