#include "HighlightPolicy.h"

namespace rl::highlight {

Kind kindOf(const QString &type)
{
    if (type == QLatin1String("person")) return Kind::Person;
    if (type == QLatin1String("vehicle")) return Kind::Vehicle;
    if (type == QLatin1String("pet") || type == QLatin1String("dog_cat")) return Kind::Pet;
    if (type == QLatin1String("motion")) return Kind::Motion;
    return Kind::None;
}

QString nameOf(Kind k)
{
    switch (k) {
    case Kind::Motion: return QStringLiteral("motion");
    case Kind::Pet: return QStringLiteral("pet");
    case Kind::Vehicle: return QStringLiteral("vehicle");
    case Kind::Person: return QStringLiteral("person");
    default: return QString();
    }
}

Style styleOf(const QString &name)
{
    if (name == QLatin1String("steady")) return Style::Steady;
    if (name == QLatin1String("flashing")) return Style::Flashing;
    return Style::FlashThenSteady;
}

QString nameOf(Style s)
{
    switch (s) {
    case Style::Steady: return QStringLiteral("steady");
    case Style::Flashing: return QStringLiteral("flashing");
    default: return QStringLiteral("flash-then-steady");
    }
}

Rules defaultRules()
{
    auto rule = [](bool on, const char *colour) {
        Rule r;
        r.on = on;
        r.colour = QString::fromLatin1(colour);
        return r;
    };
    return {
        {Kind::Person, rule(true, "#f5c518")},
        {Kind::Vehicle, rule(true, "#3d8bfd")},
        {Kind::Pet, rule(true, "#3cc46a")},
        {Kind::Motion, rule(false, "#9aa0a6")},
    };
}

QString HighlightPolicy::detect(const QString &camera, Kind kind, qint64 now)
{
    const auto it = m_rules.constFind(kind);
    if (it == m_rules.constEnd() || !it->on)
        return {};
    const Rule &r = *it;
    if (!r.allCameras && !r.cameras.contains(camera))
        return {};
    const Override o = r.overrides.value(camera);
    if (o.off)
        return {};

    Active &a = m_active[camera][kind];
    if (a.until <= now)
        a.since = now;
    a.until = now + qint64(r.seconds) * 1000;
    a.last = now;

    const QString sound = o.sound.isEmpty() ? r.sound : o.sound;
    if (sound.isEmpty() || sound == QLatin1String("none"))
        return {};
    const QString k = key(camera, kind);
    const auto last = m_lastSound.constFind(k);
    if (last != m_lastSound.constEnd() && now - *last < qint64(r.cooldownSeconds) * 1000)
        return {};
    if (m_lastAnySound >= 0 && now - m_lastAnySound < kCollapseMs)
        return {};
    m_lastSound[k] = now;
    m_lastAnySound = now;
    return sound;
}

bool HighlightPolicy::expire(qint64 now)
{
    bool changed = false;
    for (auto cam = m_active.begin(); cam != m_active.end();) {
        for (auto t = cam->begin(); t != cam->end();) {
            if (t->until <= now) {
                t = cam->erase(t);
                changed = true;
            } else {
                ++t;
            }
        }
        cam = cam->isEmpty() ? m_active.erase(cam) : std::next(cam);
    }
    return changed;
}

Look HighlightPolicy::look(const QString &camera, qint64 now) const
{
    Look l;
    const auto cam = m_active.constFind(camera);
    if (cam == m_active.constEnd())
        return l;
    Active best;
    for (auto t = cam->constBegin(); t != cam->constEnd(); ++t)
        if (t->until > now && int(t.key()) > int(l.kind)) {
            l.kind = t.key();
            best = *t;
        }
    if (l.kind == Kind::None)
        return l;
    const Rule r = m_rules.value(l.kind);
    const Override o = r.overrides.value(camera);
    l.colour = o.colour.isEmpty() ? r.colour : o.colour;
    l.style = r.style;
    l.since = best.since;
    l.until = best.until;
    l.last = best.last;
    l.flashUntil = r.style == Style::Flashing ? best.until
                 : r.style == Style::FlashThenSteady ? best.since + kFlashMs : 0;
    return l;
}

QList<QString> HighlightPolicy::highlighted(qint64 now) const
{
    QList<QString> out;
    for (auto cam = m_active.constBegin(); cam != m_active.constEnd(); ++cam)
        if (look(cam.key(), now).kind != Kind::None)
            out.append(cam.key());
    return out;
}

} // namespace rl::highlight
