#include "ui/PaneBarRules.h"

#include <QCoreApplication>
#include <QtGlobal>

namespace rl {
namespace panebar {

namespace {

QString tr(const char *text) { return QCoreApplication::translate("PaneBar", text); }

QString speedText(double speed)
{
    if (speed == 0.5)
        return QStringLiteral("½×");
    if (speed == 0.25)
        return QStringLiteral("¼×");
    return QString::number(speed) + QStringLiteral("×");
}

// The page's common tools: shown on the pill while it has room, the rest under ⋯.
QStringList commonTools(Page page)
{
    switch (page) {
    case Page::Live:
        return {QStringLiteral("positions"), QStringLiteral("scene"), QStringLiteral("tour"),
                QStringLiteral("haOffline"), QStringLiteral("snapshot"), QStringLiteral("record"),
                QStringLiteral("ptz"), QStringLiteral("talk")};
    case Page::Replay:
    case Page::Playback:
        return {QStringLiteral("snapshot")};
    case Page::Events:
        return {QStringLiteral("newer"), QStringLiteral("older")};
    }
    return {};
}

Page pageFrom(const QString &s)
{
    if (s == QLatin1String("replay"))
        return Page::Replay;
    if (s == QLatin1String("playback"))
        return Page::Playback;
    if (s == QLatin1String("events"))
        return Page::Events;
    if (s != QLatin1String("live"))
        qWarning("PaneBarRules: unknown page \"%s\" (taken as live)", qPrintable(s));
    return Page::Live;
}

State stateFrom(const QVariantMap &m)
{
    State s;
    s.page = pageFrom(m.value(QStringLiteral("page")).toString());
    const QString cam = m.value(QStringLiteral("camera")).toString();
    s.camera = cam == QLatin1String("frigate") ? Camera::Frigate
             : cam == QLatin1String("stream")  ? Camera::Stream
                                               : Camera::Recorder;
    if (s.camera == Camera::Recorder && cam != QLatin1String("recorder"))
        qWarning("PaneBarRules: unknown camera \"%s\" (taken as a recorder)", qPrintable(cam));
    s.direction = m.value(QStringLiteral("direction"), 1).toInt() < 0 ? -1 : 1;
    s.speed = m.value(QStringLiteral("speed"), 1.0).toDouble();
    s.paused = m.value(QStringLiteral("paused"), false).toBool();
    s.speedLocked = m.value(QStringLiteral("speedLocked"), false).toBool();
    s.reverseAvailable = m.value(QStringLiteral("reverseAvailable"), true).toBool();
    return s;
}

} // namespace

bool hasTransport(const State &s)
{
    if (s.camera == Camera::Stream)
        return false;
    if (s.page == Page::Live)
        return s.camera == Camera::Recorder;   // Live replay: Reolink recorder cameras only
    return true;
}

QHash<QString, Button> transport(const State &s, int step)
{
    const QStringList ids{kSkipBack, kFastBack, kSlowBack, kBack, kPause,
                          kForward, kSlowForward, kFastForward, kSkipForward};
    QHash<QString, Button> out;
    const bool any = hasTransport(s);
    for (const QString &id : ids) {
        Button b;
        b.shown = any && !(step >= NoSkips && (id == kSkipBack || id == kSkipForward));
        b.enabled = b.shown;
        out.insert(id, b);
    }
    if (!any)
        return out;

    const bool live = s.page == Page::Live;
    const bool moving = !live && !s.paused;
    auto grey = [&out](const QString &id, const QString &why) {
        Button &b = out[id];
        if (b.enabled) {
            b.enabled = false;
            b.reason = why;
        }
    };
    const QStringList speeds{kFastBack, kSlowBack, kBack, kSlowForward, kFastForward};
    const QStringList backs{kFastBack, kSlowBack, kBack};

    if (live) {
        for (const QString &id : {kPause, kForward, kSlowForward, kFastForward, kSkipForward})
            grey(id, tr("Nothing ahead of live — press ◀ or ↺ to replay"));
    } else if (s.page == Page::Events && s.camera == Camera::Frigate) {
        // A downloaded clip file: any speed forwards, none backwards (ticket 12).
        for (const QString &id : backs)
            grey(id, tr("Frigate clips play forwards only"));
    } else if (s.camera == Camera::Frigate || s.speedLocked) {
        for (const QString &id : speeds)
            grey(id, tr("Frigate plays at 1× forward only"));
    } else if (!s.reverseAvailable) {
        for (const QString &id : backs)
            grey(id, tr("Reverse play isn't available for this camera"));
    }

    const bool fast = s.speed >= 2.0, slow = s.speed < 1.0, one = s.speed == 1.0;
    out[kBack].lit = moving && s.direction < 0 && one;
    out[kForward].lit = moving && s.direction > 0 && one;
    out[kFastBack].lit = moving && s.direction < 0 && fast;
    out[kFastForward].lit = moving && s.direction > 0 && fast;
    out[kSlowBack].lit = moving && s.direction < 0 && slow;
    out[kSlowForward].lit = moving && s.direction > 0 && slow;
    for (const QString &id : speeds)
        out[id].lit = out[id].lit && out[id].enabled;   // a greyed button is never lit

    const bool captions = step < NoCaptions;
    for (const QString &id : {kFastBack, kFastForward, kSlowBack, kSlowForward}) {
        Button &b = out[id];
        const bool isFast = id == kFastBack || id == kFastForward;
        if (b.lit)
            b.caption = speedText(s.speed);
        else if (captions)
            b.caption = isFast ? tr("fast") : tr("slow");
    }
    return out;
}

Press press(const State &s, const QString &idOrKey)
{
    QString id = idOrKey;
    if (idOrKey == QLatin1String("J"))
        id = s.page == Page::Live ? kBack : kFastBack;
    else if (idOrKey == QLatin1String("K"))
        id = kPause;
    else if (idOrKey == QLatin1String("L"))
        id = kFastForward;
    else if (idOrKey == QLatin1String("Left"))
        id = kSkipBack;
    else if (idOrKey == QLatin1String("Right"))
        id = kSkipForward;

    const auto buttons = transport(s, Full);   // a key acts whatever the pill has room for
    const auto it = buttons.constFind(id);
    if (it == buttons.cend() || !it->shown)
        return {};
    if (!it->enabled)
        return {QString(), it->reason};
    return {id, QString()};
}

ToolSplit tools(Page page, const QStringList &available, int step)
{
    ToolSplit out;
    const QStringList common = commonTools(page);
    const int room = step >= AllToolsInMenu ? 0 : step >= FewTools ? 1 : int(available.size());
    for (const QString &id : available) {
        if (common.contains(id) && out.shown.size() < room)
            out.shown << id;
        else
            out.menu << id;
    }
    return out;
}

int fitStep(double room, const QList<double> &widthAtStep)
{
    for (int i = 0; i < widthAtStep.size(); ++i) {
        if (widthAtStep.at(i) <= room)
            return i;
    }
    return widthAtStep.isEmpty() ? 0 : int(widthAtStep.size()) - 1;
}

} // namespace panebar

bool PaneBarRules::hasTransport(const QVariantMap &state) const
{
    return panebar::hasTransport(panebar::stateFrom(state));
}

QVariantMap PaneBarRules::transport(const QVariantMap &state, int step) const
{
    QVariantMap out;
    const auto buttons = panebar::transport(panebar::stateFrom(state), step);
    for (auto it = buttons.cbegin(); it != buttons.cend(); ++it) {
        out.insert(it.key(), QVariantMap{{QStringLiteral("shown"), it->shown},
                                         {QStringLiteral("enabled"), it->enabled},
                                         {QStringLiteral("lit"), it->lit},
                                         {QStringLiteral("caption"), it->caption},
                                         {QStringLiteral("reason"), it->reason}});
    }
    return out;
}

QVariantMap PaneBarRules::press(const QVariantMap &state, const QString &idOrKey) const
{
    const panebar::Press p = panebar::press(panebar::stateFrom(state), idOrKey);
    return {{QStringLiteral("id"), p.id}, {QStringLiteral("refusal"), p.refusal}};
}

QVariantMap PaneBarRules::tools(const QString &page, const QStringList &available, int step) const
{
    const panebar::ToolSplit s = panebar::tools(panebar::pageFrom(page), available, step);
    return {{QStringLiteral("shown"), s.shown}, {QStringLiteral("menu"), s.menu}};
}

int PaneBarRules::fitStep(double room, const QVariantList &widthAtStep) const
{
    QList<double> widths;
    for (const QVariant &w : widthAtStep)
        widths << w.toDouble();
    return panebar::fitStep(room, widths);
}

} // namespace rl
