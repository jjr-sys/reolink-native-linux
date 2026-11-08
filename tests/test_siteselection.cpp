#include "ui/SiteMap.h"
#include "ui/SiteSelection.h"

#include <QSignalSpy>
#include <QtTest>

using rl::SiteMap;
using rl::SiteSelection;

namespace {
QVariantMap host(qint64 id, const QString &name)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}};
}
// Sites in order Farm (hosts 2, 4), Lascelles (3, 5), Woorabinda (1).
void seed(SiteMap &m)
{
    m.setHosts({host(1, QStringLiteral("Woorabinda")), host(2, QStringLiteral("Farm")),
                host(3, QStringLiteral("Lascelles")), host(4, QStringLiteral("Farm Hikvision")),
                host(5, QStringLiteral("lascelles"))});
}
} // namespace

// A page's selection of cameras: tap order kept, panes grouped by Site in Site tab order.
class TestSiteSelection : public QObject
{
    Q_OBJECT
private slots:
    void paneOrderGroupsBySiteThenTapOrder()
    {
        SiteMap m;
        seed(m);
        SiteSelection s;
        s.setSites(&m);
        for (const char *k : {"1:3", "3:0", "2:5", "1:1", "5:2", "4:0"})
            s.toggle(QString::fromLatin1(k));
        QCOMPARE(s.keys(), QStringList({"1:3", "3:0", "2:5", "1:1", "5:2", "4:0"}));
        QCOMPARE(s.paneOrder(), QStringList({"2:5", "4:0", "3:0", "5:2", "1:3", "1:1"}));
    }
    // Moving a Site re-groups at once.
    void paneOrderFollowsSiteOrder()
    {
        SiteMap m;
        seed(m);
        SiteSelection s;
        s.setSites(&m);
        s.toggle(QStringLiteral("2:0"));
        s.toggle(QStringLiteral("1:0"));
        QCOMPARE(s.paneOrder(), QStringList({"2:0", "1:0"}));
        QSignalSpy spy(&s, &SiteSelection::changed);
        m.moveSite(QStringLiteral("Woorabinda"), -2);
        m.moveSite(QStringLiteral("Woorabinda"), -1);
        QCOMPARE(s.paneOrder(), QStringList({"1:0", "2:0"}));
        QVERIFY(spy.count() >= 1);
    }
    // Cameras whose host is not known yet sit after every Site, in tap order.
    void unknownHostGoesLast()
    {
        SiteMap m;
        seed(m);
        SiteSelection s;
        s.setSites(&m);
        s.toggle(QStringLiteral("9:0"));
        s.toggle(QStringLiteral("1:0"));
        QCOMPARE(s.paneOrder(), QStringList({"1:0", "9:0"}));
    }

    void toggleAddsAndRemovesExactlyOne()
    {
        SiteSelection s;
        QSignalSpy save(&s, &SiteSelection::saveRequested);
        s.toggle(QStringLiteral("1:0"));
        s.toggle(QStringLiteral("1:1"));
        QCOMPARE(s.count(), 2);
        QVERIFY(s.contains(QStringLiteral("1:1")));
        s.toggle(QStringLiteral("1:0"));
        QCOMPARE(s.keys(), QStringList({"1:1"}));
        QVERIFY(!s.contains(QStringLiteral("1:0")));
        QCOMPARE(save.count(), 3);
        QCOMPARE(save.last().at(0).toString(), QStringLiteral("1:1"));
        s.toggle(QString()); // ignored
        QCOMPARE(s.count(), 1);
    }

    void clearThenUndoRestoresTheSameSelection()
    {
        SiteMap m;
        seed(m);
        SiteSelection s;
        s.setSites(&m);
        s.setSaved(QStringLiteral("1:3,3:0,2:5"));
        const QStringList before = s.keys(), panes = s.paneOrder();
        QCOMPARE(s.clear(), 3);
        QCOMPARE(s.count(), 0);
        QVERIFY(s.canUndo());
        s.undo();
        QCOMPARE(s.keys(), before);
        QCOMPARE(s.paneOrder(), panes);
        QVERIFY(!s.canUndo());
    }
    // Undo is for the clear just made: a later change, or the toast going, ends it.
    void undoEndsOnNextChange()
    {
        SiteSelection s;
        s.setSaved(QStringLiteral("1:0,1:1"));
        s.clear();
        s.toggle(QStringLiteral("2:0"));
        QVERIFY(!s.canUndo());
        s.undo();
        QCOMPARE(s.keys(), QStringList({"2:0"}));
        s.clear();
        s.dropUndo();
        QVERIFY(!s.canUndo());
        QCOMPARE(s.clear(), 0); // nothing to clear: no undo offered
        QVERIFY(!s.canUndo());
    }

    void savedTextRoundTripsAndRepairs()
    {
        SiteSelection s;
        s.setSaved(QStringLiteral(" 1:0 ,,1:0,junk,2:3"));
        QCOMPARE(s.keys(), QStringList({"1:0", "2:3"}));
        QCOMPARE(s.serialized(), QStringLiteral("1:0,2:3"));
        // Restoring is not a change worth storing.
        QSignalSpy save(&s, &SiteSelection::saveRequested);
        s.setSaved(QStringLiteral("2:3"));
        QCOMPARE(save.count(), 0);
    }

    // Drag swap trades two panes' places, within a Site only.
    void swapWithinASiteOnly()
    {
        SiteMap m;
        seed(m);
        SiteSelection s;
        s.setSites(&m);
        s.setSaved(QStringLiteral("2:0,1:0,4:0,1:1"));
        QVERIFY(s.sameSite(QStringLiteral("2:0"), QStringLiteral("4:0")));
        QVERIFY(s.swap(QStringLiteral("2:0"), QStringLiteral("4:0")));
        QCOMPARE(s.paneOrder(), QStringList({"4:0", "2:0", "1:0", "1:1"}));
        QVERIFY(!s.sameSite(QStringLiteral("4:0"), QStringLiteral("1:0")));
        QVERIFY(!s.swap(QStringLiteral("4:0"), QStringLiteral("1:0")));
        QVERIFY(!s.swap(QStringLiteral("4:0"), QStringLiteral("8:8")));
        QCOMPARE(s.paneOrder(), QStringList({"4:0", "2:0", "1:0", "1:1"}));
    }

    // ← / → on a maximised pane: the next pane in pane order, wrapping.
    void neighbourWrapsInPaneOrder()
    {
        SiteMap m;
        seed(m);
        SiteSelection s;
        s.setSites(&m);
        s.setSaved(QStringLiteral("1:0,2:0,3:0"));
        QCOMPARE(s.paneOrder(), QStringList({"2:0", "3:0", "1:0"}));
        QCOMPARE(s.neighbour(QStringLiteral("3:0"), 1), QStringLiteral("1:0"));
        QCOMPARE(s.neighbour(QStringLiteral("1:0"), 1), QStringLiteral("2:0"));
        QCOMPARE(s.neighbour(QStringLiteral("2:0"), -1), QStringLiteral("1:0"));
        QCOMPARE(s.neighbour(QStringLiteral("7:0"), 1), QString());
        s.setSaved(QStringLiteral("1:0"));
        QCOMPARE(s.neighbour(QStringLiteral("1:0"), 1), QString());
    }

    void keysInSite()
    {
        SiteMap m;
        seed(m);
        SiteSelection s;
        s.setSites(&m);
        s.setSaved(QStringLiteral("5:1,1:0,3:2"));
        QCOMPARE(s.countIn(QStringLiteral("Lascelles")), 2);
        QCOMPARE(s.countIn(QStringLiteral("Farm")), 0);
    }

    // Keys of recorders that no longer exist are dropped; late ones are not touched.
    void pruneDropsGoneHostsOnly()
    {
        SiteSelection s;
        s.setSaved(QStringLiteral("1:0,6:0,2:1"));
        s.pruneHosts({1, 2});
        QCOMPARE(s.keys(), QStringList({"1:0", "2:1"}));
    }

    // First launch: the cameras on the grid page that was showing (page one, as the page
    // was never stored): the first `preset` panes, empty panes left out.
    void migrationTakesTheFirstGridPage()
    {
        QCOMPARE(SiteSelection::fromGrid(QStringLiteral("1:0,-,2:0,1:1,3:0,3:1"), 4),
                 QStringList({"1:0", "2:0", "1:1"}));
        QCOMPARE(SiteSelection::fromGrid(QStringLiteral("1:0,-,2:0,1:1,3:0,3:1"), 6),
                 QStringList({"1:0", "2:0", "1:1", "3:0", "3:1"}));
        QCOMPARE(SiteSelection::fromGrid(QStringLiteral("-,-,1:0"), 1), QStringList());
        QCOMPARE(SiteSelection::fromGrid(QString(), 4), QStringList());
        // An unknown preset falls back to the default 4.
        QCOMPARE(SiteSelection::fromGrid(QStringLiteral("1:0,1:1,1:2,1:3,1:4"), 7).size(), 4);
    }
    // Playback's first launch takes its 4-pane grid ([playback] gridArrangement), the
    // panes in order, a camera placed twice once.
    void playbackMigrationTakesTheGrid()
    {
        QCOMPARE(SiteSelection::fromGrid(QStringLiteral("2:2,2:6,2:0,2:8"), 4),
                 QStringList({"2:2", "2:6", "2:0", "2:8"}));
        QCOMPARE(SiteSelection::fromGrid(QStringLiteral("2:2,-,2:2,1:0"), 4),
                 QStringList({"2:2", "1:0"}));
    }
};

QTEST_GUILESS_MAIN(TestSiteSelection)
#include "test_siteselection.moc"
