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
    m_asked.clear();
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
        return kIgnore;
    const double at = direction < 0 ? previousEnd(key, playhead) : nextStart(key, playhead);
    return at < 0 ? kNothing : at;
}

bool PlaybackDay::wantSearch(const QString &key, double nowMs, bool force)
{
    Asked &a = m_asked[key];
    const bool known = m_segments.contains(key);
    const bool settled = known && (!m_segments.value(key).isEmpty() || a.n >= kSearchTries);
    if (!force && (settled || (a.at >= 0 && nowMs - a.at < kSearchGapMs)))
        return false;
    a.at = nowMs;
    ++a.n;
    return true;
}

double PlaybackDay::advance(double playhead, double dt, double speed, int direction, double driverSecs)
{
    if (driverSecs >= 0)
        return std::clamp(driverSecs, 0.0, kDayEnd);
    if (playhead >= kDayEnd && direction > 0)
        return kDayEnd;
    return std::clamp(playhead + dt * speed * direction, 0.0, kDayEnd);
}

panesync::Pane PlaybackDay::paneFrom(const QVariantMap &m, double playhead) const
{
    panesync::Pane p;
    p.key = m.value(QStringLiteral("key")).toString();
    p.shown = m.value(QStringLiteral("shown"), true).toBool();
    p.frozen = m.value(QStringLiteral("frozen")).toBool();
    p.still = m.value(QStringLiteral("still")).toBool();
    p.busy = m.value(QStringLiteral("busy")).toBool();
    p.frameSecs = m.value(QStringLiteral("frameSecs"), -1).toDouble();
    p.lastRestartMs = m.value(QStringLiteral("lastRestartMs"), -1).toDouble();
    p.covered = covered(p.key, playhead);
    return p;
}

QList<panesync::Pane> PlaybackDay::panesFrom(const QVariantList &panes, double playhead) const
{
    QList<panesync::Pane> out;
    for (const QVariant &v : panes)
        out.append(paneFrom(v.toMap(), playhead));
    return out;
}

QString PlaybackDay::frameDriver(const QVariantList &panes, double playhead) const
{
    // Frames follow the selected pane (whichever kind), as the bar's tools do.
    return panesync::frameDriver(m_selected, panesFrom(panes, playhead));
}

double PlaybackDay::frameDriverSecs(const QVariantList &panes, double playhead) const
{
    const QList<panesync::Pane> list = panesFrom(panes, playhead);
    const QString key = panesync::frameDriver(m_selected, list);
    for (const panesync::Pane &p : list)
        if (p.key == key)
            return p.frameSecs;
    return -1;
}

bool PlaybackDay::needsRestart(const QVariantMap &pane, double playhead, double speed,
                               bool offNormal, const QString &driverKey, double nowMs) const
{
    panesync::Pane p = paneFrom(pane, playhead);
    p.locked = offNormal && m_direct.contains(p.key);
    return panesync::needsRestart(p, playhead, speed, p.key == driverKey, nowMs);
}

} // namespace rl
