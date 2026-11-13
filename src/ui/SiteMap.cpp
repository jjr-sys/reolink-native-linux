#include "ui/SiteMap.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace rl {

SiteMap::SiteMap(QObject *parent)
    : QObject(parent)
{
}

qint64 SiteMap::hostOfKey(const QString &key, bool *ok)
{
    const int colon = key.indexOf(QLatin1Char(':'));
    bool good = false;
    const qint64 id = colon > 0 ? key.left(colon).toLongLong(&good) : 0;
    if (ok)
        *ok = good;
    return good ? id : 0;
}

int SiteMap::channelOfKey(const QString &key, bool *ok)
{
    const int colon = key.indexOf(QLatin1Char(':'));
    bool good = false;
    const int channel = colon > 0 ? key.mid(colon + 1).toInt(&good) : 0;
    if (ok)
        *ok = good;
    return good ? channel : 0;
}

void SiteMap::setSaved(const QString &text)
{
    // A QML binding hands back what saveRequested just stored: nothing to reload.
    if (text == m_saved && !text.isEmpty())
        return;
    m_saved = text;
    m_site.clear();
    m_order.clear();
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    const QJsonObject o = doc.object();
    const QJsonObject hosts = o.value(QStringLiteral("hosts")).toObject();
    for (auto it = hosts.begin(); it != hosts.end(); ++it) {
        bool ok = false;
        const qint64 id = it.key().toLongLong(&ok);
        const QString site = it.value().toString().trimmed();
        if (ok && !site.isEmpty())
            m_site.insert(id, site);
    }
    for (const QJsonValue &v : o.value(QStringLiteral("order")).toArray()) {
        const QString s = v.toString().trimmed();
        if (!s.isEmpty() && !m_order.contains(s))
            m_order << s;
    }
    m_seeding = m_site.isEmpty();
    rebuild(false);
}

void SiteMap::setHosts(const QVariantList &hosts)
{
    m_hosts.clear();
    QSet<qint64> seen;
    for (const QVariant &v : hosts) {
        const QVariantMap h = v.toMap();
        bool ok = false;
        const qint64 id = h.value(QStringLiteral("id")).toLongLong(&ok);
        if (!ok || seen.contains(id))
            continue;
        seen.insert(id);
        m_hosts << QVariantMap{{QStringLiteral("id"), id},
                               {QStringLiteral("name"), h.value(QStringLiteral("name")).toString()}};
    }
    // New hosts: seeded from their names on a first launch, else a Site of their own.
    bool added = false;
    for (const QVariant &v : std::as_const(m_hosts)) {
        const qint64 id = v.toMap().value(QStringLiteral("id")).toLongLong();
        if (m_site.contains(id))
            continue;
        QString site = m_seeding ? seededSite(id) : nameOf(id).trimmed();
        if (site.isEmpty())
            site = tr("Host %1").arg(id);
        m_site.insert(id, site);
        added = true;
    }
    emit hostsChanged();
    rebuild(added);
}

QString SiteMap::nameOf(qint64 hostId) const
{
    for (const QVariant &v : m_hosts) {
        const QVariantMap h = v.toMap();
        if (h.value(QStringLiteral("id")).toLongLong() == hostId)
            return h.value(QStringLiteral("name")).toString();
    }
    return {};
}

// The first-launch guess: "<Site> <something>" joins that Site, and a name equal to an
// earlier host's but for case joins that host's Site.
QString SiteMap::seededSite(qint64 hostId) const
{
    const QString name = nameOf(hostId).trimmed();
    for (const QVariant &v : m_hosts) {
        const QVariantMap h = v.toMap();
        const qint64 id = h.value(QStringLiteral("id")).toLongLong();
        const QString other = h.value(QStringLiteral("name")).toString().trimmed();
        if (id == hostId || other.isEmpty())
            continue;
        if (name.startsWith(other + QLatin1Char(' '), Qt::CaseInsensitive))
            return m_site.value(id, other);
    }
    for (const QVariant &v : m_hosts) {
        const QVariantMap h = v.toMap();
        const qint64 id = h.value(QStringLiteral("id")).toLongLong();
        if (id == hostId)
            break;
        if (h.value(QStringLiteral("name")).toString().trimmed().compare(name, Qt::CaseInsensitive) == 0)
            return m_site.value(id, name);
    }
    // Existing Sites by case-insensitive name (a host seeded earlier in this run).
    for (const QString &s : std::as_const(m_site))
        if (s.compare(name, Qt::CaseInsensitive) == 0)
            return s;
    return name;
}

QString SiteMap::serialized() const
{
    QJsonObject hosts;
    QList<qint64> ids = m_site.keys();
    std::sort(ids.begin(), ids.end());
    for (qint64 id : ids)
        hosts.insert(QString::number(id), m_site.value(id));
    QJsonObject o;
    o.insert(QStringLiteral("hosts"), hosts);
    o.insert(QStringLiteral("order"), QJsonArray::fromStringList(m_order));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void SiteMap::rebuild(bool save)
{
    // The order keeps every Site some host is on (present or not); new Sites go at the end,
    // alphabetically among themselves.
    QSet<QString> used;
    for (const QString &s : std::as_const(m_site))
        used.insert(s);
    QStringList order;
    for (const QString &s : std::as_const(m_order))
        if (used.contains(s))
            order << s;
    QStringList fresh;
    for (const QString &s : std::as_const(used))
        if (!order.contains(s))
            fresh << s;
    std::sort(fresh.begin(), fresh.end(),
              [](const QString &a, const QString &b) { return a.localeAwareCompare(b) < 0; });
    order << fresh;
    const bool orderChanged = order != m_order;
    m_order = order;

    QSet<QString> present;
    for (const QVariant &v : std::as_const(m_hosts))
        present.insert(m_site.value(v.toMap().value(QStringLiteral("id")).toLongLong()));
    QStringList sites;
    for (const QString &s : std::as_const(m_order))
        if (present.contains(s))
            sites << s;
    m_sites = sites;
    ++m_revision;
    emit changed();
    if ((save || orderChanged) && !m_hosts.isEmpty()) {
        m_saved = serialized();
        emit saveRequested(m_saved);
    }
}

QString SiteMap::siteOf(qint64 hostId) const
{
    return m_site.value(hostId);
}

QString SiteMap::siteOfKey(const QString &key) const
{
    bool ok = false;
    const qint64 id = hostOfKey(key, &ok);
    return ok ? m_site.value(id) : QString();
}

QVariantList SiteMap::hostsOf(const QString &site) const
{
    QVariantList out;
    for (const QVariant &v : m_hosts) {
        const qint64 id = v.toMap().value(QStringLiteral("id")).toLongLong();
        if (m_site.value(id) == site)
            out << id;
    }
    return out;
}

void SiteMap::setSite(qint64 hostId, const QString &site)
{
    if (!m_site.contains(hostId))
        return;
    QString s = site.trimmed();
    if (s.isEmpty())
        s = nameOf(hostId).trimmed();
    if (s.isEmpty() || m_site.value(hostId) == s)
        return;
    m_site.insert(hostId, s);
    rebuild(true);
}

void SiteMap::moveSite(const QString &site, int dir)
{
    QStringList visible = m_sites;
    const int i = visible.indexOf(site);
    if (i < 0 || dir == 0)
        return;
    const int j = std::clamp(i + dir, 0, int(visible.size()) - 1);
    if (j == i)
        return;
    visible.move(i, j);
    QStringList order = visible;
    for (const QString &s : std::as_const(m_order))
        if (!order.contains(s))
            order << s;
    m_order = order;
    rebuild(true);
}

} // namespace rl
