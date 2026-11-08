#include "ui/SiteBudget.h"
#include "ui/SiteMap.h"
#include "ui/SiteSelection.h"

#include <QSignalSpy>
#include <QtTest>

using rl::SiteBudget;
using rl::SiteMap;
using rl::SiteSelection;

namespace {
QVariantMap host(qint64 id, const QString &name)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}};
}
QVariantMap cost(double sub, double main)
{
    return {{QStringLiteral("sub"), sub}, {QStringLiteral("main"), main}};
}
// Farm = hosts 2 and 4, Woorabinda = host 1; Farm's budget 5 Mbit/s.
struct Fixture {
    SiteMap map;
    SiteSelection selection;
    SiteBudget budget;
    Fixture()
    {
        map.setHosts({host(1, QStringLiteral("Woorabinda")), host(2, QStringLiteral("Farm")),
                      host(4, QStringLiteral("Farm Hikvision"))});
        selection.setSites(&map);
        budget.setSites(&map);
        budget.setSelection(&selection);
        budget.setBudgets({{QStringLiteral("Farm"), 5.0}, {QStringLiteral("Woorabinda"), 7.0}});
        budget.setCosts({{QStringLiteral("2:0"), cost(1.5, 6.0)},
                         {QStringLiteral("2:1"), cost(1.0, 4.0)},
                         {QStringLiteral("2:2"), cost(0.5, 3.0)},
                         {QStringLiteral("4:0"), cost(1.0, 1.0)},
                         {QStringLiteral("1:0"), cost(0.5, 6.0)}});
    }
};
} // namespace

// Each Site's link carries a budget in Mbit/s; every selected pane costs its camera's
// bitrate, sub or HD (wayfinder/site-tabs-redesign, step 2).
class TestSiteBudget : public QObject
{
    Q_OBJECT
private slots:
    void costSumsMixSubAndHd()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0,2:1,4:0,1:0"));
        QCOMPARE(f.budget.used(QStringLiteral("Farm")), 3.5);
        f.budget.setHd({QStringLiteral("2:1")});
        QCOMPARE(f.budget.used(QStringLiteral("Farm")), 6.5); // 1.5 + 4 + 1
        QCOMPARE(f.budget.used(QStringLiteral("Woorabinda")), 0.5);
        QCOMPARE(f.budget.used(QStringLiteral("Nowhere")), 0.0);
    }

    // A card greys exactly when adding it would go over: reaching the budget is fine.
    void addFitsUpToTheBudgetExactly()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0,2:1"));   // 2.5 of 5
        QVERIFY(f.budget.canAdd(QStringLiteral("2:2")));   // 3.0
        f.budget.setCosts({{QStringLiteral("2:0"), cost(1.5, 6.0)},
                           {QStringLiteral("2:1"), cost(1.0, 4.0)},
                           {QStringLiteral("2:2"), cost(2.5, 3.0)}});
        QVERIFY(f.budget.canAdd(QStringLiteral("2:2")));   // exactly 5.0
        f.budget.setCosts({{QStringLiteral("2:0"), cost(1.5, 6.0)},
                           {QStringLiteral("2:1"), cost(1.0, 4.0)},
                           {QStringLiteral("2:2"), cost(2.6, 3.0)}});
        QVERIFY(!f.budget.canAdd(QStringLiteral("2:2")));  // 5.1
        QCOMPARE(f.budget.needed(QStringLiteral("2:2")), 2.6);
        // Already selected: always "fits" (tapping it removes it).
        QVERIFY(f.budget.canAdd(QStringLiteral("2:0")));
        // Another Site's budget is untouched.
        QVERIFY(f.budget.canAdd(QStringLiteral("1:0")));
    }
    // A camera remembered in HD comes back in HD, so adding it costs its main stream.
    void addingAnHdCameraCostsItsMainStream()
    {
        Fixture f;
        f.budget.setHd({QStringLiteral("2:2")});
        f.selection.setSaved(QStringLiteral("2:0")); // 1.5
        QCOMPARE(f.budget.needed(QStringLiteral("2:2")), 3.0);
        QVERIFY(f.budget.canAdd(QStringLiteral("2:2")));  // 4.5
        f.selection.setSaved(QStringLiteral("2:0,2:1"));  // 2.5
        QVERIFY(!f.budget.canAdd(QStringLiteral("2:2"))); // 5.5
    }

    // Switching a pane to HD swaps its sub cost for its main cost.
    void hdIsRefusedOverTheBudget()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:1,2:2")); // 1.5
        QCOMPARE(f.budget.hdExtra(QStringLiteral("2:1")), 3.0);
        QVERIFY(f.budget.canHd(QStringLiteral("2:1")));  // 0.5 + 4 = 4.5
        f.selection.setSaved(QStringLiteral("2:0,2:1"));  // 2.5
        QVERIFY(!f.budget.canHd(QStringLiteral("2:1"))); // 1.5 + 4 = 5.5
        QVERIFY(!f.budget.canHd(QStringLiteral("2:0"))); // 6 + 1 = 7
        // Already HD: nothing to refuse.
        f.budget.setHd({QStringLiteral("2:0")});
        QVERIFY(f.budget.canHd(QStringLiteral("2:0")));
    }

    // A selected camera with no row (its NVR is down) keeps its pane and still counts, at
    // its known cost or, never seen, the Reolink default (0.5 sub / 6 HD).
    void offlineSelectedCameraStillCounts()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0,2:9"));
        QCOMPARE(f.budget.used(QStringLiteral("Farm")), 2.0);
        f.budget.setHd({QStringLiteral("2:9")});
        QCOMPARE(f.budget.used(QStringLiteral("Farm")), 7.5);
    }

    void budgetsDefaultForUnnamedSites()
    {
        Fixture f;
        QCOMPARE(f.budget.budgetOf(QStringLiteral("Farm")), 5.0);
        QCOMPARE(f.budget.budgetOf(QStringLiteral("Shed")), 5.0); // the default
        f.budget.setDefaultBudget(8.0);
        QCOMPARE(f.budget.budgetOf(QStringLiteral("Shed")), 8.0);
        // A non-positive stored budget is ignored.
        f.budget.setBudgets({{QStringLiteral("Farm"), 0.0}});
        QCOMPARE(f.budget.budgetOf(QStringLiteral("Farm")), 8.0);
    }

    // Unknown host: no Site, so no budget to stop it.
    void cameraWithNoSiteIsNeverRefused()
    {
        Fixture f;
        QVERIFY(f.budget.canAdd(QStringLiteral("9:0")));
        QVERIFY(f.budget.canHd(QStringLiteral("9:0")));
    }

    void revisionFollowsEveryInput()
    {
        Fixture f;
        QSignalSpy spy(&f.budget, &SiteBudget::changed);
        f.selection.toggle(QStringLiteral("2:0"));
        f.budget.setHd({QStringLiteral("2:0")});
        f.budget.setBudgets({{QStringLiteral("Farm"), 9.0}});
        f.map.setSite(4, QStringLiteral("Hik"));
        QVERIFY(spy.count() >= 4);
    }
};

QTEST_GUILESS_MAIN(TestSiteBudget)
#include "test_sitebudget.moc"
