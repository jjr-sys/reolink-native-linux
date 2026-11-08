#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>

namespace rl {

// Which recorders (hosts) make up each Site, and the order of the Site tabs
// (wayfinder/site-tabs-redesign). Stored explicitly, never guessed from names after the
// first launch.
//
//  - Give it the stored text first (setSaved), then the hosts that exist (setHosts) each
//    time the device model changes: a list of { id, name }.
//  - With nothing stored, the hosts present are seeded from their names: a host named
//    "<Site> <something>" joins that Site, and names equal but for case are one Site
//    (so "Lascelles" and "lascelles" are one). Hosts that turn up later in that same run
//    are seeded the same way.
//  - Once anything is stored, a host not in the map gets a Site of its own, named after it.
//  - A host absent from setHosts keeps its Site (it may answer late); a Site with no host
//    present is left out of `sites`.
//  - saveRequested(text) says the map changed and is worth storing.
//
// Stored text: {"hosts": {"<hostId>": "<Site>", ...}, "order": ["<Site>", ...]}.
class SiteMap : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString saved READ saved WRITE setSaved)
    Q_PROPERTY(QVariantList hosts READ hosts WRITE setHosts NOTIFY hostsChanged)
    // The Sites with at least one host present, in Site tab order.
    Q_PROPERTY(QStringList sites READ sites NOTIFY changed)
    // Bumped on every change, for QML bindings that call the functions below.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit SiteMap(QObject *parent = nullptr);

    QString saved() const { return m_saved; }
    void setSaved(const QString &text);
    QVariantList hosts() const { return m_hosts; }
    void setHosts(const QVariantList &hosts);
    QStringList sites() const { return m_sites; }
    int revision() const { return m_revision; }
    QString serialized() const;

    // "" for a host never seen.
    Q_INVOKABLE QString siteOf(qint64 hostId) const;
    // The Site of a camera key "hostId:channel"; "" when unknown.
    Q_INVOKABLE QString siteOfKey(const QString &key) const;
    // The hosts present at a Site, in host list order.
    Q_INVOKABLE QVariantList hostsOf(const QString &site) const;
    // Move a host to a Site (trimmed); blank puts it on a Site of its own name.
    Q_INVOKABLE void setSite(qint64 hostId, const QString &site);
    // Move a Site `dir` places along the tab order, stopping at either end.
    Q_INVOKABLE void moveSite(const QString &site, int dir);

    static qint64 hostOfKey(const QString &key, bool *ok = nullptr);

signals:
    void hostsChanged();
    void changed();
    void saveRequested(const QString &text);

private:
    QString nameOf(qint64 hostId) const;
    QString seededSite(qint64 hostId) const;
    void rebuild(bool save);

    QString m_saved;
    bool m_seeding = true;           // nothing stored: guess from names this run
    QVariantList m_hosts;
    QHash<qint64, QString> m_site;   // hostId -> Site, including hosts not present
    QStringList m_order;             // stored tab order, may name Sites not present
    QStringList m_sites;
    int m_revision = 0;
};

} // namespace rl
