#include "ui/PlaybackDay.h"

#include <QVariantMap>
#include <algorithm>

namespace rl {

PlaybackDay::PlaybackDay(QObject *parent) : QObject(parent) {}

void PlaybackDay::setPaneOrder(const QStringList &keys)
{
    if (m_order == keys)
        return;
    m_order = keys;
    emit changed();
}

void PlaybackDay::setSelectedKey(const QString &key)
{
    if (m_selected == key)
        return;
    m_selected = key;
    emit changed();
}

void PlaybackDay::setDirectKeys(const QStringList &keys)
{
    if (m_direct == keys)
        return;
    m_direct = keys;
    emit changed();
}

void PlaybackDay::setSegments(const QString &key, const QVariantList &segments)
{
    QList<Span> spans;
    for (const QVariant &v : segments) {
        const QVariantMap m = v.toMap();
        spans.append({m.value(QStringLiteral("start")).toDouble(), m.value(QStringLiteral("end")).toDouble()});
    }
    m_segments.insert(key, spans);
}

void PlaybackDay::clear()
{
    m_segments.clear();
    m_searches.clear();
}

QString PlaybackDay::leadKey() const
{
    if (!m_selected.isEmpty() && !m_direct.contains(m_selected) && m_order.contains(m_selected))
        return m_selected;
    for (const QString &k : m_order)
        if (!m_direct.contains(k))
            return k;
    return {};
}

bool PlaybackDay::covered(const QString &key, double sec) const
{
    for (const Span &s : m_segments.value(key))
        if (sec >= s.start && sec <= s.end)
            return true;
    return false;
}

double PlaybackDay::previousEnd(const QString &key, double sec) const
{
    double best = -1;
    for (const Span &s : m_segments.value(key))
        if (s.end < sec - 0.5 && s.end > best)
            best = s.end;
    return best;
}

double PlaybackDay::nextStart(const QString &key, double sec) const
{
    double best = -1;
    for (const Span &s : m_segments.value(key))
        if (s.start > sec + 0.5 && (best < 0 || s.start < best))
            best = s.start;
    return best;
}

double PlaybackDay::landing(double sec, int direction) const
{
    const QString lead = leadKey();
    if (direction >= 0 || lead.isEmpty() || covered(lead, sec))
        return sec;
    return previousEnd(lead, sec);
}

double PlaybackDay::carryOn(const QString &key, int direction, double playhead) const
{
    if (direction == 0 || key != leadKey())
        return Ignore;
    const double at = direction < 0 ? previousEnd(key, playhead) : nextStart(key, playhead);
    return at < 0 ? double(Nothing) : at;
}

bool PlaybackDay::wantSearch(const QString &key, double nowMs, bool force)
{
    Searches &a = m_searches[key];
    const bool known = m_segments.contains(key);
    const bool settled = known && (!m_segments.value(key).isEmpty() || a.count >= kSearchTries);
    if (!force && (settled || (a.lastAskedMs >= 0 && nowMs - a.lastAskedMs < kSearchGapMs)))
        return false;
    a.lastAskedMs = nowMs;
    ++a.count;
    return true;
}

void PlaybackDay::notReady(const QString &key)
{
    const auto it = m_searches.find(key);
    if (it == m_searches.end())
        return;
    it->count = qMax(0, it->count - 1);
    it->lastAskedMs = -1;
}

double PlaybackDay::advance(double playhead, double dt, double speed, int direction, double driverSecs)
{
    if (driverSecs >= 0)
        return std::clamp(driverSecs, 0.0, kDayEnd);
    if (playhead >= kDayEnd && direction > 0)
        return kDayEnd;
    return std::clamp(playhead + dt * speed * direction, 0.0, kDayEnd);
}

panesync::Pane PlaybackDay::paneFrom(const QVariantMap &m, double playhead, bool offNormal) const
{
    panesync::Pane p = panesync::paneFrom(m);
    p.covered = covered(p.key, playhead);
    p.locked = offNormal && m_direct.contains(p.key);   // Frigate plays at 1× forward only
    return p;
}

QList<panesync::Pane> PlaybackDay::panesFrom(const QVariantList &panes, double playhead,
                                             bool offNormal) const
{
    QList<panesync::Pane> out;
    for (const QVariant &v : panes)
        out.append(paneFrom(v.toMap(), playhead, offNormal));
    return out;
}

QString PlaybackDay::frameDriver(const QVariantList &panes, double playhead, bool offNormal) const
{
    return panesync::frameDriver(leadKey(), panesFrom(panes, playhead, offNormal));
}

double PlaybackDay::frameDriverSecs(const QVariantList &panes, double playhead, bool offNormal) const
{
    const QList<panesync::Pane> list = panesFrom(panes, playhead, offNormal);
    const QString key = panesync::frameDriver(leadKey(), list);
    for (const panesync::Pane &p : list)
        if (p.key == key)
            return p.frameSecs;
    return -1;
}

bool PlaybackDay::needsRestart(const QVariantMap &pane, double playhead, double speed,
                               bool offNormal, const QString &driverKey, double nowMs) const
{
    const panesync::Pane p = paneFrom(pane, playhead, offNormal);
    return panesync::needsRestart(p, playhead, speed, p.key == driverKey, nowMs);
}

} // namespace rl
