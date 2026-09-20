#include "ActivityPolicy.h"

#include <algorithm>

namespace rl::activity {

void ActivityPolicy::setBaseline(const QVector<int> &rows)
{
    const int old = m_tiles.size();
    m_tiles.resize(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        TileState &t = m_tiles[i];
        t.baseline = rows.at(i);
        if (i >= old || !t.active)
            t.shown = rows.at(i);
    }
}

void ActivityPolicy::setPinned(int tile, bool pinned)
{
    if (tile >= 0 && tile < m_tiles.size())
        m_tiles[tile].pinned = pinned;
}

void ActivityPolicy::setExcluded(int row, bool excluded)
{
    if (excluded)
        m_excluded.insert(row);
    else
        m_excluded.remove(row);
}

void ActivityPolicy::reset()
{
    m_queue.clear();
    m_starts.clear();
    for (TileState &t : m_tiles) {
        t.shown = t.baseline;
        t.active = false;
        t.kind = Kind::None;
    }
}

QVector<int> ActivityPolicy::queuedRows() const
{
    QVector<Queued> q = m_queue;
    std::stable_sort(q.begin(), q.end(), [](const Queued &a, const Queued &b) {
        if (priorityOf(a.kind) != priorityOf(b.kind))
            return priorityOf(a.kind) > priorityOf(b.kind);
        return a.seq < b.seq;
    });
    QVector<int> rows;
    for (const Queued &e : q)
        rows.append(e.row);
    return rows;
}

int ActivityPolicy::tileShowing(int row) const
{
    for (int i = 0; i < m_tiles.size(); ++i)
        if (m_tiles.at(i).shown == row)
            return i;
    return -1;
}

void ActivityPolicy::detect(int row, Kind kind, qint64 now, qint64 trigger)
{
    if (trigger < 0)
        trigger = now;
    if (m_excluded.contains(row) || kind == Kind::None)
        return;
    // Already on screen: mark it (or extend it) in place, never a second tile.
    const int t = tileShowing(row);
    if (t >= 0) {
        TileState &ts = m_tiles[t];
        if (!ts.active) {
            ts.active = true;
            ts.shownSince = now;
            ts.kind = kind;
            ts.triggerMs = trigger;
        } else if (priorityOf(kind) > priorityOf(ts.kind)) {
            ts.kind = kind;
        }
        ts.holdUntil = std::max(ts.holdUntil, now + m_p.holdMs);
        return;
    }
    for (Queued &q : m_queue) {
        if (q.row == row) {
            if (priorityOf(kind) > priorityOf(q.kind))
                q.kind = kind;
            return;
        }
    }
    m_queue.append({row, kind, trigger, m_seq++});
}

// A free tile first (idle longest), else — for AI activity only — the motion-held
// tile that has been shown its minimum time, earliest hold first.
int ActivityPolicy::pickTile(Kind kind, qint64 now) const
{
    int best = -1;
    for (int i = 0; i < m_tiles.size(); ++i) {
        const TileState &t = m_tiles.at(i);
        if (t.pinned || t.active)
            continue;
        if (best < 0 || t.idleSince < m_tiles.at(best).idleSince)
            best = i;
    }
    if (best >= 0 || priorityOf(kind) < 2)
        return best;
    for (int i = 0; i < m_tiles.size(); ++i) {
        const TileState &t = m_tiles.at(i);
        if (t.pinned || !t.active || t.kind != Kind::Motion || now - t.shownSince < m_p.minShownMs)
            continue;
        if (best < 0 || t.holdUntil < m_tiles.at(best).holdUntil)
            best = i;
    }
    return best;
}

bool ActivityPolicy::startAllowed(qint64 host, qint64 now)
{
    QList<qint64> &l = m_starts[host];
    while (!l.isEmpty() && l.first() <= now - m_p.startWindowMs)
        l.removeFirst();
    return l.size() < m_p.maxStartsPerHost;
}

QVector<Change> ActivityPolicy::tick(qint64 now)
{
    QVector<Change> out;

    // 1. Release tiles whose hold is over.
    QVector<int> released;
    for (int i = 0; i < m_tiles.size(); ++i) {
        TileState &t = m_tiles[i];
        if (t.active && !t.pinned && now >= t.holdUntil) {
            t.active = false;
            t.kind = Kind::None;
            t.idleSince = now;
            released.append(i);
        } else if (t.active && t.pinned && now >= t.holdUntil) {
            t.active = false; // pinned tiles keep their camera; only the badge ends
            t.kind = Kind::None;
        }
    }

    // 2. Give queued activity a tile, AI first, then in arrival order.
    QVector<Queued> order = m_queue;
    std::stable_sort(order.begin(), order.end(), [](const Queued &a, const Queued &b) {
        if (priorityOf(a.kind) != priorityOf(b.kind))
            return priorityOf(a.kind) > priorityOf(b.kind);
        return a.seq < b.seq;
    });
    for (const Queued &q : order) {
        const int existing = tileShowing(q.row);
        if (existing >= 0) { // it appeared on screen meanwhile (baseline rearranged)
            TileState &ts = m_tiles[existing];
            if (!ts.active) {
                ts.active = true;
                ts.shownSince = now;
                ts.triggerMs = q.trigger;
            }
            ts.kind = priorityOf(q.kind) > priorityOf(ts.kind) ? q.kind : ts.kind;
            ts.holdUntil = std::max(ts.holdUntil, now + m_p.holdMs);
            m_queue.removeIf([&](const Queued &e) { return e.seq == q.seq; });
            continue;
        }
        const int tile = pickTile(q.kind, now);
        if (tile < 0)
            continue;
        const qint64 host = hostOf(q.row);
        if (!startAllowed(host, now))
            continue;
        recordStart(host, now);
        TileState &t = m_tiles[tile];
        t.shown = q.row;
        t.active = true;
        t.kind = q.kind;
        t.shownSince = now;
        t.holdUntil = now + m_p.holdMs;
        t.triggerMs = q.trigger;
        released.removeAll(tile);
        Change c;
        c.tile = tile;
        c.row = q.row;
        c.activity = true;
        c.kind = q.kind;
        c.triggerMs = q.trigger;
        c.replayFromMs = q.trigger - m_p.replayLeadMs;
        out.append(c);
        m_queue.removeIf([&](const Queued &e) { return e.seq == q.seq; });
    }

    // 3. Released tiles still showing an activity camera go back to their baseline.
    for (int tile : released) {
        TileState &t = m_tiles[tile];
        if (t.shown == t.baseline)
            continue;
        const qint64 host = hostOf(t.baseline);
        if (!startAllowed(host, now)) {
            t.active = true; // keep showing it a little longer; retried next tick
            t.holdUntil = now + 200;
            continue;
        }
        recordStart(host, now);
        t.shown = t.baseline;
        Change c;
        c.tile = tile;
        c.row = t.baseline;
        out.append(c);
    }
    return out;
}

} // namespace rl::activity
