#include "ui/SavedSelections.h"
#include "ui/SiteBudget.h"

#include <QSignalSpy>
#include <QtTest>

using rl::SavedSelections;
using rl::SiteBudget;
using rl::SiteMap;
using rl::SiteSelection;

namespace {
// The [mosaics] list as the named-mosaic tabs stored it.
const char *kMosaics =
    R"([{"id":"mmvekrtth","name":"Ehiwa","cameras":["1:4","2:2","1:8","5:0","3:0"],)"
    R"("grouping":"site","siteOrder":["Farm"],"large":["3:0"],"hd":[]},)"
    R"({"id":"m2","name":"Gates","cameras":["2:5","2:6"],"large":[],"hd":["2:6"]}])";

QVariantMap host(qint64 id, const QString &name)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}};
}
QVariantMap cost(double sub, double main)
{
    return {{QStringLiteral("sub"), sub}, {QStringLiteral("main"), main}};
}
} // namespace

// Saved selections (wayfinder/site-tabs-redesign, step 3): named sets of cameras, recalled
// into a page's selection. They replace the named mosaics.
class TestSavedSelections : public QObject
{
    Q_OBJECT
private slots:
    // The named mosaics become saved selections with the same cameras, in the same order.
    void mosaicsBecomeSavedSelections()
    {
        const QVariantList l = SavedSelections::fromMosaics(QString::fromLatin1(kMosaics));
        QCOMPARE(l.size(), 2);
        const QVariantMap ehiwa = l.at(0).toMap();
        QCOMPARE(ehiwa.value("name").toString(), QStringLiteral("Ehiwa"));
        QCOMPARE(ehiwa.value("cameras").toStringList(),
                 QStringList({"1:4", "2:2", "1:8", "5:0", "3:0"}));
        QVERIFY(!ehiwa.value("id").toString().isEmpty());
        QCOMPARE(l.at(1).toMap().value("name").toString(), QStringLiteral("Gates"));
        QVERIFY(SavedSelections::fromMosaics(QStringLiteral("not json")).isEmpty());
        QVERIFY(SavedSelections::fromMosaics(QString()).isEmpty());
    }
    // HD and Large go per camera: one that was HD or Large anywhere (a named mosaic or the
    // grid-page mosaic) stays so.
    void hdAndLargeBecomePerCamera()
    {
        const QString mosaics = QString::fromLatin1(kMosaics);
        QCOMPARE(SavedSelections::mosaicKeys(mosaics, QStringLiteral("large"), QStringLiteral("5:0,3:0,3:1")),
                 QStringList({"5:0", "3:0", "3:1"}));
        QCOMPARE(SavedSelections::mosaicKeys(mosaics, QStringLiteral("hd"), QStringLiteral("3:0,5:0")),
                 QStringList({"3:0", "5:0", "2:6"}));
        QCOMPARE(SavedSelections::mosaicKeys(mosaics, QStringLiteral("hd"), QString()),
                 QStringList({"2:6"}));
        QCOMPARE(SavedSelections::mosaicKeys(QString(), QStringLiteral("hd"), QStringLiteral("1:0")),
                 QStringList({"1:0"}));
    }

    void addRenameDeletePersist()
    {
        SavedSelections s;
        QSignalSpy save(&s, &SavedSelections::saveRequested);
        const QString id = s.add(QStringLiteral("  Night  "), {"1:0", "1:1", "1:0"});
        QCOMPARE(s.list().size(), 1);
        QCOMPARE(s.camerasOf(id), QStringList({"1:0", "1:1"}));
        QCOMPARE(s.find(id).value("name").toString(), QStringLiteral("Night"));
        s.rename(id, QStringLiteral("Evening"));
        s.rename(id, QStringLiteral("   ")); // blank: ignored
        const QString other = s.add(QStringLiteral("Gates"), {"2:5"});
        s.remove(other);
        QCOMPARE(save.count(), 4);

        SavedSelections again;
        again.setSaved(save.last().at(0).toString());
        QCOMPARE(again.list().size(), 1);
        QCOMPARE(again.find(id).value("name").toString(), QStringLiteral("Evening"));
        QCOMPARE(again.camerasOf(id), QStringList({"1:0", "1:1"}));
        // Restoring is not a change worth storing.
        QSignalSpy quiet(&again, &SavedSelections::saveRequested);
        again.setSaved(save.last().at(0).toString());
        QCOMPARE(quiet.count(), 0);
    }
    void nothingToSaveOrBadInput()
    {
        SavedSelections s;
        QVERIFY(s.add(QStringLiteral("Empty"), {}).isEmpty());
        QVERIFY(s.add(QString(), {"1:0"}).isEmpty());
        s.rename(QStringLiteral("nope"), QStringLiteral("x"));
        s.remove(QStringLiteral("nope"));
        QVERIFY(s.list().isEmpty());
        s.setSaved(QStringLiteral("{broken"));
        QVERIFY(s.list().isEmpty());
    }
    void nextNameIsFree()
    {
        SavedSelections s;
        QCOMPARE(s.nextName(QStringLiteral("Selection")), QStringLiteral("Selection 1"));
        s.add(QStringLiteral("Selection 1"), {"1:0"});
        QCOMPARE(s.nextName(QStringLiteral("Selection")), QStringLiteral("Selection 2"));
    }

    // Recalling replaces the selection, adding cameras in order and skipping any that don't
    // fit their Site's budget; the skipped ones are named.
    void recallSkipsCamerasOverTheBudget()
    {
        SiteMap map;
        map.setHosts({host(1, QStringLiteral("Woorabinda")), host(2, QStringLiteral("Farm"))});
        SiteSelection selection;
        selection.setSites(&map);
        selection.setSaved(QStringLiteral("1:7"));
        SiteBudget budget;
        budget.setSites(&map);
        budget.setSelection(&selection);
        budget.setBudgets({{QStringLiteral("Farm"), 3.0}, {QStringLiteral("Woorabinda"), 7.0}});
        budget.setCosts({{QStringLiteral("2:0"), cost(1.5, 6)}, {QStringLiteral("2:1"), cost(1.0, 4)},
                         {QStringLiteral("2:2"), cost(1.0, 4)}, {QStringLiteral("1:0"), cost(0.5, 6)}});
        budget.setHd({QStringLiteral("1:0")});
        const QStringList skipped =
            budget.recall({"2:0", "2:1", "2:2", "1:0", "9:0"});
        // 2:0 + 2:1 = 2.5; 2:2 would make 3.5 > 3. 1:0 in HD: 6 <= 7. 9:0 has no Site.
        QCOMPARE(skipped, QStringList({"2:2"}));
        QCOMPARE(selection.keys(), QStringList({"2:0", "2:1", "1:0", "9:0"}));
        // The selection it replaced (1:7) doesn't count against the recall.
        QVERIFY(!selection.contains(QStringLiteral("1:7")));
    }
};

QTEST_GUILESS_MAIN(TestSavedSelections)
#include "test_savedselections.moc"
