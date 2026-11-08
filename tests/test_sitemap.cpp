#include "ui/SiteMap.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

using rl::SiteMap;

namespace {
QVariantMap host(qint64 id, const QString &name)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}};
}
// The hosts in the user's database when the redesign was specified.
QVariantList household()
{
    return {host(1, QStringLiteral("Woorabinda")), host(2, QStringLiteral("Farm")),
            host(3, QStringLiteral("Lascelles")), host(4, QStringLiteral("Farm Hikvision")),
            host(5, QStringLiteral("lascelles"))};
}
QVariantList ids(std::initializer_list<qint64> l)
{
    QVariantList out;
    for (qint64 i : l)
        out << i;
    return out;
}
} // namespace

// Sites: which recorders (hosts) belong together, stored explicitly rather than guessed
// from host names each time.
class TestSiteMap : public QObject
{
    Q_OBJECT
private slots:
    // First launch: the old name-prefix guess plus a case-insensitive match seeds the map.
    void firstLaunchMergesLascellesAndFarmStream()
    {
        SiteMap m;
        m.setHosts(household());
        QCOMPARE(m.siteOf(3), QStringLiteral("Lascelles"));
        QCOMPARE(m.siteOf(5), QStringLiteral("Lascelles"));
        QCOMPARE(m.siteOf(4), QStringLiteral("Farm"));
        QCOMPARE(m.siteOf(2), QStringLiteral("Farm"));
        QCOMPARE(m.siteOf(1), QStringLiteral("Woorabinda"));
        QCOMPARE(m.sites(), QStringList({"Farm", "Lascelles", "Woorabinda"}));
        QCOMPARE(m.hostsOf(QStringLiteral("Lascelles")), ids({3, 5}));
        QCOMPARE(m.hostsOf(QStringLiteral("Farm")), ids({2, 4}));
    }
    void siteOfKeyReadsTheHost()
    {
        SiteMap m;
        m.setHosts(household());
        QCOMPARE(m.siteOfKey(QStringLiteral("5:0")), QStringLiteral("Lascelles"));
        QCOMPARE(m.siteOfKey(QStringLiteral("4:12")), QStringLiteral("Farm"));
        QCOMPARE(m.siteOfKey(QStringLiteral("99:0")), QString());
        QCOMPARE(m.siteOfKey(QStringLiteral("junk")), QString());
    }
    // The seeded map is stored, so later launches no longer guess.
    void seedIsSaved()
    {
        SiteMap m;
        QSignalSpy spy(&m, &SiteMap::saveRequested);
        m.setHosts(household());
        QVERIFY(spy.count() >= 1);
        SiteMap again;
        again.setSaved(spy.last().at(0).toString());
        again.setHosts(household());
        QCOMPARE(again.siteOf(5), QStringLiteral("Lascelles"));
        QCOMPARE(again.sites(), m.sites());
    }

    // After the first launch a new host is a Site of its own, even if its name looks
    // like an existing Site's.
    void newHostDefaultsToItsOwnSite()
    {
        SiteMap m;
        m.setHosts(household());
        QSignalSpy spy(&m, &SiteMap::saveRequested);
        SiteMap later;
        later.setSaved(m.serialized());
        QVariantList hosts = household();
        hosts << host(6, QStringLiteral("Farm Shed")) << host(7, QStringLiteral("woorabinda"));
        later.setHosts(hosts);
        QCOMPARE(later.siteOf(6), QStringLiteral("Farm Shed"));
        QCOMPARE(later.siteOf(7), QStringLiteral("woorabinda"));
        QCOMPARE(later.siteOf(5), QStringLiteral("Lascelles"));
        QCOMPARE(later.sites().last(), QStringLiteral("woorabinda"));
    }

    // Hosts that answer late keep their stored Site; a Site with no hosts present is hidden
    // but not forgotten.
    void absentHostKeepsItsSite()
    {
        SiteMap m;
        m.setHosts(household());
        SiteMap later;
        later.setSaved(m.serialized());
        later.setHosts({host(1, QStringLiteral("Woorabinda"))});
        QCOMPARE(later.sites(), QStringList({"Woorabinda"}));
        later.setHosts(household());
        QCOMPARE(later.siteOf(5), QStringLiteral("Lascelles"));
        QCOMPARE(later.sites(), QStringList({"Farm", "Lascelles", "Woorabinda"}));
    }

    void setSiteMovesAHostAndSaves()
    {
        SiteMap m;
        m.setHosts(household());
        QSignalSpy spy(&m, &SiteMap::saveRequested);
        QSignalSpy changed(&m, &SiteMap::changed);
        m.setSite(4, QStringLiteral("  Hik  "));
        QCOMPARE(m.siteOf(4), QStringLiteral("Hik"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(changed.count(), 1);
        QVERIFY(m.sites().contains(QStringLiteral("Hik")));
        // Blank puts it back on a Site of its own name.
        m.setSite(4, QStringLiteral(" "));
        QCOMPARE(m.siteOf(4), QStringLiteral("Farm Hikvision"));
        // Unknown host: nothing happens.
        m.setSite(42, QStringLiteral("X"));
        QCOMPARE(spy.count(), 2);
    }
    // Emptying a Site removes it from the order; moving a host back appends it again.
    void emptiedSiteLeavesTheOrder()
    {
        SiteMap m;
        m.setHosts(household());
        m.setSite(1, QStringLiteral("Farm"));
        QCOMPARE(m.sites(), QStringList({"Farm", "Lascelles"}));
        m.setSite(1, QStringLiteral("Woorabinda"));
        QCOMPARE(m.sites(), QStringList({"Farm", "Lascelles", "Woorabinda"}));
    }

    void moveSiteReordersAndPersists()
    {
        SiteMap m;
        m.setHosts(household());
        m.moveSite(QStringLiteral("Woorabinda"), -1);
        QCOMPARE(m.sites(), QStringList({"Farm", "Woorabinda", "Lascelles"}));
        m.moveSite(QStringLiteral("Woorabinda"), -1);
        m.moveSite(QStringLiteral("Woorabinda"), -1); // already first: no-op
        QCOMPARE(m.sites(), QStringList({"Woorabinda", "Farm", "Lascelles"}));
        SiteMap again;
        again.setSaved(m.serialized());
        again.setHosts(household());
        QCOMPARE(again.sites(), QStringList({"Woorabinda", "Farm", "Lascelles"}));
    }

    void badSavedTextSeedsAsFirstLaunch()
    {
        SiteMap m;
        m.setSaved(QStringLiteral("{not json"));
        m.setHosts(household());
        QCOMPARE(m.siteOf(5), QStringLiteral("Lascelles"));
    }
};

QTEST_GUILESS_MAIN(TestSiteMap)
#include "test_sitemap.moc"
