#include "ui/SiteSelection.h"

#include "ui/SiteMap.h"

#include <QSet>

#include <algorithm>

namespace rl {

SiteSelection::SiteSelection(QObject *parent)
    : QObject(parent)
{
}

void SiteSelection::setSites(SiteMap *sites)
{
    if (m_sites == sites)
        return;
    if (m_sites)
        disconnect(m_sites, nullptr, this, nullptr);
    m_sites = sites;
    if (m_sites)
        connect(m_sites, &SiteMap::changed, this, &SiteSelection::regroup);
    emit sitesChanged();
    regroup();
}

QStringList SiteSelection::parse(const QString &text)
{
    QStringList out;
    for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString k = part.trimmed();
        bool ok = false;
        SiteMap::hostOfKey(k, &ok);
        const int colon = k.indexOf(QLatin1Char(':'));
        bool chOk = false;
        k.mid(colon + 1).toInt(&chOk);
        if (ok && chOk && !out.contains(k))
            out << k;
    }
    return out;
}

void SiteSelection::setSaved(const QString &text)
{
    m_saved = text;
    setKeys(parse(text), false);
}

void SiteSelection::setKeys(const QStringList &keys, bool save)
{
    if (keys == m_keys)
        return;
    m_keys = keys;
    regroup();
    if (save) {
        m_saved = serialized();
        emit saveRequested(m_saved);
    }
}

void SiteSelection::regroup()
{
    QStringList order = m_keys;
    if (m_sites) {
        const QStringList sites = m_sites->sites();
        const auto rank = [&](const QString &k) {
            const int i = int(sites.indexOf(m_sites->siteOfKey(k)));
            return i < 0 ? int(sites.size()) : i;
        };
        std::stable_sort(order.begin(), order.end(),
                         [&](const QString &a, const QString &b) { return rank(a) < rank(b); });
    }
    m_paneOrder = order;
    emit changed();
}

void SiteSelection::toggle(const QString &key)
{
    if (parse(key) != QStringList{key})
        return;
    QStringList keys = m_keys;
    if (!keys.removeOne(key))
        keys << key;
    dropUndo();
    setKeys(keys, true);
}

void SiteSelection::replace(const QStringList &keys)
{
    dropUndo();
    setKeys(parse(keys.join(QLatin1Char(','))), true);
}

int SiteSelection::clear()
{
    const int n = count();
    if (n == 0)
        return 0;
    const QStringList was = m_keys;
    setKeys({}, true);
    m_undo = was;
    emit undoChanged();
    return n;
}

void SiteSelection::undo()
{
    if (m_undo.isEmpty())
        return;
    const QStringList keys = m_undo;
    dropUndo();
    setKeys(keys, true);
}

void SiteSelection::dropUndo()
{
    if (m_undo.isEmpty())
        return;
    m_undo.clear();
    emit undoChanged();
}

bool SiteSelection::sameSite(const QString &a, const QString &b) const
{
    if (!m_sites)
        return true;
    const QString sa = m_sites->siteOfKey(a);
    return !sa.isEmpty() && sa == m_sites->siteOfKey(b);
}

bool SiteSelection::swap(const QString &a, const QString &b)
{
    const int i = int(m_keys.indexOf(a)), j = int(m_keys.indexOf(b));
    if (i < 0 || j < 0 || i == j || !sameSite(a, b))
        return false;
    QStringList keys = m_keys;
    keys.swapItemsAt(i, j);
    dropUndo();
    setKeys(keys, true);
    return true;
}

QString SiteSelection::neighbour(const QString &key, int dir) const
{
    const int n = int(m_paneOrder.size());
    const int i = int(m_paneOrder.indexOf(key));
    if (i < 0 || n < 2 || dir == 0)
        return {};
    return m_paneOrder.at(((i + dir) % n + n) % n);
}

int SiteSelection::countIn(const QString &site) const
{
    if (!m_sites)
        return 0;
    return int(std::count_if(m_keys.begin(), m_keys.end(),
                             [&](const QString &k) { return m_sites->siteOfKey(k) == site; }));
}

void SiteSelection::pruneHosts(const QVariantList &hostIds)
{
    QSet<qint64> live;
    for (const QVariant &v : hostIds)
        live.insert(v.toLongLong());
    QStringList keys;
    for (const QString &k : std::as_const(m_keys))
        if (live.contains(SiteMap::hostOfKey(k)))
            keys << k;
    setKeys(keys, true);
}

void SiteSelection::pruneCameras(const QVariantMap &listed)
{
    QStringList keys;
    for (const QString &k : std::as_const(m_keys)) {
        const QVariantList channels = listed.value(QString::number(SiteMap::hostOfKey(k))).toList();
        bool ok = false;
        const int channel = SiteMap::channelOfKey(k, &ok);
        if (channels.isEmpty() || !ok || channels.contains(channel))
            keys << k;
    }
    setKeys(keys, true);
}

QStringList SiteSelection::fromGrid(const QString &arrangement, int preset)
{
    static const int kPresets[] = {1, 4, 6, 9, 16, 20, 25};
    if (std::find(std::begin(kPresets), std::end(kPresets), preset) == std::end(kPresets))
        preset = 4;
    const QStringList panes = arrangement.split(QLatin1Char(','));
    QStringList page;
    for (int i = 0; i < preset && i < panes.size(); ++i)
        page << panes.at(i);
    return parse(page.join(QLatin1Char(',')));
}

} // namespace rl
