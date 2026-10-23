#include "ui/LiveArrangement.h"

#include <QSignalSpy>
#include <QtTest>

using rl::LiveArrangement;

namespace {
QStringList keys(int n)
{
    QStringList l;
    for (int i = 0; i < n; ++i)
        l << QStringLiteral("h%1:0").arg(i);
    return l;
}
QString dashes(const QStringList &placed)
{
    QStringList out;
    for (int i = 0; i < LiveArrangement::kMaxPanes; ++i)
        out << (i < placed.size() && !placed[i].isEmpty() ? placed[i] : QStringLiteral("-"));
    return out.join(QLatin1Char(','));
}
} // namespace

// Live grid arrangement: which camera sits in which pane, paging, and the saved layout.
// Cameras are identified by "hostId:channel", never by a model row.
class TestLiveArrangement : public QObject
{
    Q_OBJECT
private slots:
    // A value saved by the QML-era page: shorter than the pane count, "-" for empty.
    void oldFormatLoads()
    {
        LiveArrangement a;
        a.setSaved(QStringLiteral("h0:0,-,h2:0,h1:0"));
        a.setCameras(keys(3));
        QVERIFY(a.restored());
        QCOMPARE(a.keyAt(0), QStringLiteral("h0:0"));
        QCOMPARE(a.keyAt(1), QString());
        QCOMPARE(a.keyAt(2), QStringLiteral("h2:0"));
        QCOMPARE(a.keyAt(3), QStringLiteral("h1:0"));
        QCOMPARE(a.paneKeys().size(), LiveArrangement::kMaxPanes);
    }
    void newCameraFillsFirstGap()
    {
        LiveArrangement a;
        a.setSaved(QStringLiteral("h0:0,-,h2:0,h1:0"));
        a.setCameras(keys(3) << QStringLiteral("h9:0"));
        QCOMPARE(a.keyAt(1), QStringLiteral("h9:0"));
    }
    void noSavedValueKeepsModelOrder()
    {
        LiveArrangement a;
        a.setCameras(keys(3));
        QCOMPARE(a.keyAt(0), QStringLiteral("h0:0"));
        QCOMPARE(a.keyAt(2), QStringLiteral("h2:0"));
        QCOMPARE(a.keyAt(3), QString());
    }
    void duplicatesAndUnknownInSavedValueAreRepaired()
    {
        LiveArrangement a;
        a.setSaved(QStringLiteral("h0:0,h0:0,zz:9,h1:0"));
        a.setCameras(keys(2));
        QCOMPARE(a.keyAt(0), QStringLiteral("h0:0"));
        QCOMPARE(a.paneKeys().count(QStringLiteral("h0:0")), 1);
        QCOMPARE(a.keyAt(3), QStringLiteral("h1:0"));
    }
    void serializedMatchesSavedFormat()
    {
        LiveArrangement a;
        a.setSaved(QStringLiteral("h0:0,-,h2:0,h1:0"));
        a.setCameras(keys(3));
        QCOMPARE(a.serialized(), dashes({"h0:0", "", "h2:0", "h1:0"}));
    }

    // A host whose cameras arrive late gets them back in their saved panes, and the
    // stored layout is not overwritten with stand-ins meanwhile.
    void missingCameraWaitsInItsPane()
    {
        LiveArrangement a;
        QSignalSpy saves(&a, &LiveArrangement::saveRequested);
        a.setSaved(QStringLiteral("h0:0,-,x:9"));
        a.setCameras({"h0:0", "h1:0", "h2:0"});
        QVERIFY(a.hasPending());
        QCOMPARE(a.keyAt(2), QStringLiteral("h2:0")); // stand-in
        a.assign(5, QStringLiteral("h1:0"));
        QCOMPARE(saves.count(), 0);

        a.setCameras({"h0:0", "h1:0", "h2:0", "x:9"});
        QVERIFY(!a.hasPending());
        QCOMPARE(a.keyAt(2), QStringLiteral("x:9"));
        QCOMPARE(a.paneKeys().count(QStringLiteral("h2:0")), 1); // the stand-in found a new gap
        QCOMPARE(saves.count(), 1);
        QCOMPARE(saves.at(0).at(0).toString(), a.serialized());
    }
    void pendingGivesUp()
    {
        LiveArrangement a;
        a.setGiveUpMs(5);
        QSignalSpy saves(&a, &LiveArrangement::saveRequested);
        a.setSaved(QStringLiteral("h0:0,x:9"));
        a.setCameras({"h0:0", "h1:0"});
        QVERIFY(a.hasPending());
        QTRY_VERIFY(!a.hasPending());
        QCOMPARE(saves.count(), 1);
    }

    void assignSwapsAndMoves()
    {
        LiveArrangement a;
        a.setCameras(keys(3));
        QSignalSpy saves(&a, &LiveArrangement::saveRequested);
        a.assign(0, QStringLiteral("h2:0")); // swap
        QCOMPARE(a.keyAt(0), QStringLiteral("h2:0"));
        QCOMPARE(a.keyAt(2), QStringLiteral("h0:0"));
        a.assign(7, QStringLiteral("h1:0")); // into an empty pane, leaving a gap
        QCOMPARE(a.keyAt(7), QStringLiteral("h1:0"));
        QCOMPARE(a.keyAt(1), QString());
        QCOMPARE(saves.count(), 2);
        a.assign(7, QStringLiteral("h1:0")); // already there
        a.assign(-1, QStringLiteral("h1:0"));
        a.assign(LiveArrangement::kMaxPanes, QStringLiteral("h1:0"));
        a.assign(3, QStringLiteral("nope:0"));
        QCOMPARE(saves.count(), 2);
        QCOMPARE(a.paneOf(QStringLiteral("h1:0")), 7);
        QCOMPARE(a.paneOf(QStringLiteral("nope:0")), -1);
    }

    // Identity is the key: removing a camera never moves the others.
    void removalKeepsOthersInPlace()
    {
        LiveArrangement a;
        a.setCameras(keys(4));
        a.setMaximized(QStringLiteral("h1:0"));
        a.setSelected(QStringLiteral("h3:0"));
        QSignalSpy saves(&a, &LiveArrangement::saveRequested);
        a.setCameras({"h0:0", "h2:0", "h3:0"});
        QCOMPARE(a.keyAt(0), QStringLiteral("h0:0"));
        QCOMPARE(a.keyAt(1), QString());
        QCOMPARE(a.keyAt(2), QStringLiteral("h2:0"));
        QCOMPARE(a.keyAt(3), QStringLiteral("h3:0"));
        QCOMPARE(a.maximized(), QString());
        QCOMPARE(a.selected(), QStringLiteral("h3:0"));
        QVERIFY(saves.count() >= 1);
        QCOMPARE(saves.last().at(0).toString(), a.serialized());
    }
    void reorderedModelDoesNotMoveCameras()
    {
        LiveArrangement a;
        a.setCameras(keys(3));
        a.setCameras({"h2:0", "h0:0", "h1:0"});
        QCOMPARE(a.keyAt(0), QStringLiteral("h0:0"));
        QCOMPARE(a.keyAt(2), QStringLiteral("h2:0"));
    }
    void modelResetRestoresFromLastSave()
    {
        LiveArrangement a;
        a.setCameras(keys(3));
        a.assign(0, QStringLiteral("h2:0"));
        a.reset();
        QVERIFY(!a.restored());
        a.setCameras(keys(3));
        QVERIFY(a.restored());
        QCOMPARE(a.keyAt(0), QStringLiteral("h2:0"));
    }

    void paging()
    {
        LiveArrangement a;
        a.setCameras(keys(10));
        a.setPreset(4);
        QCOMPARE(a.pageCount(), 3);
        QCOMPARE(a.pageKeys(), keys(4));
        a.turnPage(1);
        QCOMPARE(a.page(), 1);
        QCOMPARE(a.pageOffset(), 4);
        QCOMPARE(a.pageKeys(), keys(8).mid(4));
        a.turnPage(1);
        QCOMPARE(a.pageKeys(), keys(10).mid(8));
        a.turnPage(1); // wraps
        QCOMPARE(a.page(), 0);
        a.turnPage(-1);
        QCOMPARE(a.page(), 2);
        a.setPreset(9); // a new grid size starts at the first page
        QCOMPARE(a.page(), 0);
        QCOMPARE(a.pageCount(), 2);
        a.setPreset(7); // not a preset
        QCOMPARE(a.preset(), 9);
    }
    void pageClampsWhenCamerasShrink()
    {
        LiveArrangement a;
        a.setCameras(keys(10));
        a.setPreset(4);
        a.setPage(2);
        a.setCameras(keys(5));
        QCOMPARE(a.curPage(), 1);
    }
    void singlePageDoesNotTurn()
    {
        LiveArrangement a;
        a.setCameras(keys(3));
        a.turnPage(1);
        QCOMPARE(a.page(), 0);
    }
    void neighbourWrapsInPaneOrder()
    {
        LiveArrangement a;
        a.setCameras(keys(3));
        QCOMPARE(a.neighbour(QStringLiteral("h0:0"), 1), QStringLiteral("h1:0"));
        QCOMPARE(a.neighbour(QStringLiteral("h0:0"), -1), QStringLiteral("h2:0"));
        QCOMPARE(a.neighbour(QStringLiteral("h2:0"), 1), QStringLiteral("h0:0"));
        // an explicit order (a mosaic tab) wins
        QCOMPARE(a.neighbour(QStringLiteral("h0:0"), 1, {"h0:0", "h2:0"}), QStringLiteral("h2:0"));
        QCOMPARE(a.neighbour(QStringLiteral("zz:0"), 1), QString());
        LiveArrangement one;
        one.setCameras(keys(1));
        QCOMPARE(one.neighbour(QStringLiteral("h0:0"), 1), QString());
    }

    void savedKeysReadsOldValue()
    {
        QCOMPARE(LiveArrangement::savedKeys(QStringLiteral("a:0,-,b:1,,-")), (QStringList{"a:0", "b:1"}));
        QVERIFY(LiveArrangement::savedKeys(QString()).isEmpty());
    }
    void presetAndPageSignals()
    {
        LiveArrangement a;
        a.setCameras(keys(10));
        QSignalSpy preset(&a, &LiveArrangement::presetChanged), page(&a, &LiveArrangement::pageChanged);
        a.setPreset(4);
        a.setPreset(4);
        QCOMPARE(preset.count(), 0); // already 4
        a.turnPage(1);
        QCOMPARE(page.count(), 1);
        a.setPreset(6);
        QCOMPARE(preset.count(), 1);
        QCOMPARE(page.count(), 2); // back to the first page
        a.setPreset(9);
        QCOMPARE(page.count(), 2); // already on it
    }

    // A named mosaic's drop rule: a camera not in it is added before the target,
    // one already in it swaps places.
    void dropIntoOrder()
    {
        const QStringList order{"a", "b", "c"};
        QCOMPARE(LiveArrangement::dropInto(order, "b", "z"), (QStringList{"a", "z", "b", "c"}));
        QCOMPARE(LiveArrangement::dropInto(order, "a", "c"), (QStringList{"c", "b", "a"}));
        QCOMPARE(LiveArrangement::dropInto(order, "q", "z"), order);
        QCOMPARE(LiveArrangement::dropInto(order, "a", "a"), order);
    }
};

QTEST_GUILESS_MAIN(TestLiveArrangement)
#include "test_livearrangement.moc"
