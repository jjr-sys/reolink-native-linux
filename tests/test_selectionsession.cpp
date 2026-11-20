#include "ui/SelectionSession.h"

#include <QAbstractListModel>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using rl::SelectionSession;
using rl::SiteMap;

namespace {

// A device list shaped like DeviceManager's, as far as the session reads it.
class FakeDevices : public QAbstractListModel
{
    Q_OBJECT
public:
    struct Row {
        qint64 host;
        int channel;
        int rotation = 0;
        bool fisheye = false;
        QString view;
    };
    enum { HostRole = Qt::UserRole + 1, ChannelRole, RotationRole, FisheyeRole, ViewRole };

    int rowCount(const QModelIndex & = {}) const override { return int(rows.size()); }
    QVariant data(const QModelIndex &i, int role) const override
    {
        const Row &r = rows.at(i.row());
        switch (role) {
        case HostRole: return r.host;
        case ChannelRole: return r.channel;
        case RotationRole: return r.rotation;
        case FisheyeRole: return r.fisheye;
        case ViewRole: return r.view;
        }
        return {};
    }
    QHash<int, QByteArray> roleNames() const override
    {
        return {{HostRole, "hostId"}, {ChannelRole, "channel"}, {RotationRole, "rotationOverride"},
                {FisheyeRole, "isFisheye"}, {ViewRole, "viewMode"}};
    }
    Q_INVOKABLE QVariantMap listedCameras() const { return listed; }

    void add(qint64 host, int channel)
    {
        beginInsertRows({}, int(rows.size()), int(rows.size()));
        rows.append({host, channel});
        endInsertRows();
    }
    void removeHost(qint64 host)
    {
        for (int i = int(rows.size()) - 1; i >= 0; --i) {
            if (rows.at(i).host != host)
                continue;
            beginRemoveRows({}, i, i);
            rows.removeAt(i);
            endRemoveRows();
        }
    }
    void reset(const QList<Row> &r)
    {
        beginResetModel();
        rows = r;
        endResetModel();
    }
    void setRotation(int row, int deg)
    {
        rows[row].rotation = deg;
        emit dataChanged(index(row), index(row), {RotationRole});
    }
    void setView(int row, bool fisheye, const QString &view)
    {
        rows[row].fisheye = fisheye;
        rows[row].view = view;
        emit dataChanged(index(row), index(row), {FisheyeRole, ViewRole});
    }
    QList<Row> rows;
    QVariantMap listed;
};

QVariantMap host(qint64 id, const QString &name)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}};
}
QVariantMap cost(double sub, double main)
{
    return {{QStringLiteral("sub"), sub}, {QStringLiteral("main"), main}};
}

// Farm = hosts 2 and 4 (5 Mbit/s), Woorabinda = host 1 (7 Mbit/s); a controllable clock.
struct Fixture {
    SiteMap map;
    FakeDevices devices;
    SelectionSession s;
    qint64 clock = 1'000'000;

    explicit Fixture(bool live = true)
    {
        map.setHosts({host(1, QStringLiteral("Woorabinda")), host(2, QStringLiteral("Farm")),
                      host(4, QStringLiteral("Farm Hikvision"))});
        s.setClock([this] { return clock; });
        s.setSites(&map);
        s.setDevices(&devices);
        s.setBudgets({{QStringLiteral("Farm"), 5.0}, {QStringLiteral("Woorabinda"), 7.0}});
        s.setCosts({{QStringLiteral("2:0"), cost(1.5, 6.0)}, {QStringLiteral("2:1"), cost(1.0, 4.0)},
                    {QStringLiteral("1:0"), cost(0.5, 6.0)}, {QStringLiteral("1:1"), cost(0.5, 6.0)}});
        s.setOverBudgetAdvice(QStringLiteral("Deselect a camera or switch one out of HD."));
        if (live) {   // Live View's parameters
            s.setStoreGroup(QStringLiteral("liveSelection"));
            s.setGridGroup(QStringLiteral("liveGrid"));
            s.setGridKey(QStringLiteral("arrangement"));
            s.setGridPresetKey(QStringLiteral("preset"));
            s.setSeedCount(4);
            s.setWaitMs(8000);
            s.setSettleMs(500);
        } else {   // Playback's
            s.setStoreGroup(QStringLiteral("playbackSelection"));
            s.setGridGroup(QStringLiteral("playback"));
            s.setGridKey(QStringLiteral("gridArrangement"));
            s.setSettleMs(300);
            s.setSettleOnSelection(true);
        }
    }
    void advance(qint64 ms)
    {
        clock += ms;
        s.tick();
    }
    static QStringList keysOf(const QVariantList &panes, const FakeDevices &d)
    {
        QStringList out;
        for (const QVariant &p : panes) {
            const auto &r = d.rows.at(p.toMap().value(QStringLiteral("row")).toInt());
            out << QStringLiteral("%1:%2").arg(r.host).arg(r.channel);
        }
        return out;
    }
};

QVariant stored(const QString &group, const QString &key)
{
    QSettings q;
    q.beginGroup(group);
    return q.value(key);
}
void store(const QString &group, const QString &key, const QVariant &v)
{
    QSettings q;
    q.beginGroup(group);
    q.setValue(key, v);
}

} // namespace

// One page's camera-selection session, tested through its interface with a scripted device
// list and a controlled clock (architecture review, selection session).
class TestSelectionSession : public QObject
{
    Q_OBJECT
    QTemporaryDir settingsDir;

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("rl-test"));
        QCoreApplication::setApplicationName(QStringLiteral("test_selectionsession"));
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    }
    void init() { QSettings().clear(); }

    // ---- rows ----
    void rowsFollowTheDeviceList()
    {
        Fixture f;
        f.devices.add(1, 0);
        f.devices.add(2, 1);
        QCOMPARE(f.s.rowOf(QStringLiteral("2:1")), 1);
        QCOMPARE(f.s.keyOfRow(0), QStringLiteral("1:0"));
        QCOMPARE(f.s.rowOf(QStringLiteral("9:9")), -1);
        QCOMPARE(f.s.keyOfRow(7), QString());
        f.devices.removeHost(1);   // rows shift
        QCOMPARE(f.s.rowOf(QStringLiteral("2:1")), 0);
        QCOMPARE(f.s.rowOf(QStringLiteral("1:0")), -1);
        f.devices.reset({{4, 0}});
        QCOMPARE(f.s.rowOf(QStringLiteral("4:0")), 0);
        QCOMPARE(f.s.rowOf(QStringLiteral("2:1")), -1);
    }
    void stepSkipsCamerasNotReportedYet()
    {
        Fixture f;
        f.devices.add(1, 0);
        f.devices.add(2, 0);
        f.s.selection()->setSaved(QStringLiteral("2:0,1:0,2:7"));   // pane order 2:0 2:7 1:0
        QCOMPARE(f.s.stepFrom(QStringLiteral("2:0"), 1), QStringLiteral("1:0"));   // 2:7 has no row
        QCOMPARE(f.s.stepFrom(QStringLiteral("1:0"), 1), QStringLiteral("2:0"));   // wraps
        QCOMPARE(f.s.stepFrom(QStringLiteral("2:0"), -1), QStringLiteral("1:0"));
        f.s.selection()->setSaved(QStringLiteral("2:0"));
        QCOMPARE(f.s.stepFrom(QStringLiteral("2:0"), 1), QString());   // nowhere to go
    }

    // ---- the stored selection ----
    void restoreTakesTheStoredKeys()
    {
        store(QStringLiteral("liveSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("liveSelection"), QStringLiteral("keys"), QStringLiteral("2:0,1:0"));
        Fixture f;
        f.s.start();
        QCOMPARE(f.s.selection()->keys(), QStringList({"2:0", "1:0"}));
    }
    void firstLaunchTakesTheOldGridPageOnce()
    {
        QString arr = QStringLiteral("1:0,1:1,2:0,2:1,3:0");
        store(QStringLiteral("liveGrid"), QStringLiteral("arrangement"), arr);
        store(QStringLiteral("liveGrid"), QStringLiteral("preset"), 4);
        Fixture f;
        f.s.start();
        QCOMPARE(f.s.selection()->keys(), QStringList({"1:0", "1:1", "2:0", "2:1"}));
        QCOMPARE(stored(QStringLiteral("liveSelection"), QStringLiteral("keys")).toString(),
                 QStringLiteral("1:0,1:1,2:0,2:1"));
        QVERIFY(stored(QStringLiteral("liveSelection"), QStringLiteral("migrated")).toBool());
        // The old grid changing afterwards is ignored: migrated.
        store(QStringLiteral("liveGrid"), QStringLiteral("arrangement"), QStringLiteral("1:0"));
        Fixture g;
        g.s.start();
        QCOMPARE(g.s.selection()->count(), 4);
    }
    void playbackMigratesFromItsOwnGridWithFourPanesAPage()
    {
        store(QStringLiteral("playback"), QStringLiteral("gridArrangement"),
              QStringLiteral("1:0,2:0,2:1,1:1,2:2"));
        Fixture f(false);
        f.s.start();
        QCOMPARE(f.s.selection()->keys(), QStringList({"1:0", "2:0", "2:1", "1:1"}));
        QVERIFY(stored(QStringLiteral("playbackSelection"), QStringLiteral("migrated")).toBool());
        // Not Live's store.
        QVERIFY(!stored(QStringLiteral("liveSelection"), QStringLiteral("migrated")).isValid());
    }
    void userChangesAreSaved()
    {
        Fixture f;
        f.s.start();
        f.s.selection()->toggle(QStringLiteral("2:0"));
        f.s.selection()->toggle(QStringLiteral("1:0"));
        QCOMPARE(stored(QStringLiteral("liveSelection"), QStringLiteral("keys")).toString(),
                 QStringLiteral("2:0,1:0"));
    }
    // Nothing to migrate: Live selects the first cameras once they have arrived and the wait
    // is over, within the Site budget; Playback does not.
    void seedsTheFirstCamerasWhenThereIsNothingToMigrate()
    {
        Fixture f;
        f.s.start();
        QCOMPARE(f.s.selection()->count(), 0);
        f.devices.add(2, 0);   // 1.5
        f.devices.add(2, 1);   // 1.0
        f.devices.add(1, 0);
        QCOMPARE(f.s.selection()->count(), 0);   // not before the wait is over
        f.advance(8000);
        QVERIFY(f.s.waitOver());
        QCOMPARE(f.s.selection()->keys(), QStringList({"2:0", "2:1", "1:0"}));
    }
    void seedingStopsAtTheBudgetAndAtTheCount()
    {
        Fixture f;
        f.s.setBudgets({{QStringLiteral("Farm"), 2.0}, {QStringLiteral("Woorabinda"), 7.0}});
        f.s.start();
        f.devices.add(2, 0);   // 1.5
        f.devices.add(2, 1);   // would be 2.5 > 2
        for (int c = 0; c < 5; ++c)
            f.devices.add(1, c);
        f.advance(8000);
        QCOMPARE(f.s.selection()->keys(), QStringList({"2:0", "1:0", "1:1", "1:2"}));
    }
    void seedingHappensAfterTheWaitToo()
    {
        Fixture f;
        f.s.start();
        f.advance(8000);   // no cameras yet
        f.devices.add(1, 0);
        QCOMPARE(f.s.selection()->keys(), QStringList({"1:0"}));
        f.devices.add(1, 1);
        QCOMPARE(f.s.selection()->keys(), QStringList({"1:0"}));   // once only
    }
    void noSeedingWhenSomethingMigrated()
    {
        store(QStringLiteral("liveGrid"), QStringLiteral("arrangement"), QStringLiteral("2:0"));
        Fixture f;
        f.s.start();
        f.devices.add(1, 0);
        f.devices.add(2, 0);
        f.advance(8000);
        QCOMPARE(f.s.selection()->keys(), QStringList({"2:0"}));
    }
    void playbackNeverSeeds()
    {
        Fixture f(false);
        f.s.start();
        f.devices.add(1, 0);
        f.advance(60000);
        QCOMPARE(f.s.selection()->count(), 0);
    }

    // ---- the mosaic panes ----
    void panesAreTheSelectedCamerasInPaneOrderWithTheirShapeAndLarge()
    {
        Fixture f(false);
        f.devices.add(1, 0);
        f.devices.add(2, 0);
        f.devices.add(2, 1);
        f.s.start();
        f.s.setLarge({QStringLiteral("2:1")});
        f.s.selection()->toggle(QStringLiteral("1:0"));
        f.s.selection()->toggle(QStringLiteral("2:1"));
        f.s.selection()->toggle(QStringLiteral("2:0"));
        f.advance(300);   // Playback settles a selection change
        // Site tab order (Farm, Woorabinda); tap order within a Site.
        QCOMPARE(Fixture::keysOf(f.s.panes(), f.devices), QStringList({"2:1", "2:0", "1:0"}));
        const QVariantMap second = f.s.panes().at(0).toMap();
        QCOMPARE(second.value(QStringLiteral("large")).toBool(), true);
        QCOMPARE(f.s.panes().at(1).toMap().value(QStringLiteral("large")).toBool(), false);
        QCOMPARE(second.value(QStringLiteral("row")).toInt(), 2);
        QCOMPARE(second.value(QStringLiteral("ratio")).toDouble(), 16.0 / 9.0);
    }
    void largeChangesRepackAtOnce()
    {
        Fixture f;
        f.devices.add(1, 0);
        store(QStringLiteral("liveSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("liveSelection"), QStringLiteral("keys"), QStringLiteral("1:0"));
        f.s.start();
        f.advance(8000);
        QSignalSpy spy(&f.s, &SelectionSession::panesChanged);
        f.s.setLarge({QStringLiteral("1:0")});
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.panes().at(0).toMap().value(QStringLiteral("large")).toBool(), true);
    }
    // Live builds nothing until the selected cameras are reported or the wait is over.
    void launchWaitHoldsTheFirstMosaicUntilTheSelectionIsHere()
    {
        store(QStringLiteral("liveSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("liveSelection"), QStringLiteral("keys"), QStringLiteral("1:0,2:0"));
        Fixture f;
        f.s.start();
        f.devices.add(1, 0);
        f.advance(500);
        QVERIFY(f.s.panes().isEmpty());   // 2:0 has not reported
        QVERIFY(!f.s.waitOver());
        f.devices.add(2, 0);
        f.advance(500);   // the settle time: everyone is here, so it builds
        QCOMPARE(Fixture::keysOf(f.s.panes(), f.devices), QStringList({"2:0", "1:0"}));
    }
    void launchWaitEndsAndBuildsWithWhatIsThere()
    {
        store(QStringLiteral("liveSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("liveSelection"), QStringLiteral("keys"), QStringLiteral("1:0,2:0"));
        Fixture f;
        f.s.start();
        f.devices.add(1, 0);
        f.devices.add(2, 5);   // host 2 is here, its camera 2:0 is not yet
        QSignalSpy over(&f.s, &SelectionSession::waitOverChanged);
        f.advance(7999);
        QVERIFY(f.s.panes().isEmpty());
        f.advance(1);
        QCOMPARE(over.count(), 1);
        QCOMPARE(Fixture::keysOf(f.s.panes(), f.devices), QStringList({"1:0"}));
        f.devices.add(2, 0);   // a late camera gets its pane after the settle time
        f.advance(500);
        QCOMPARE(f.s.panes().size(), 2);
    }
    void noWaitMeansNoHold()
    {
        Fixture f(false);
        QVERIFY(f.s.waitOver());
        f.devices.add(1, 0);
        f.s.start();
        f.s.selection()->toggle(QStringLiteral("2:0"));   // not reported
        f.s.selection()->toggle(QStringLiteral("1:0"));
        f.advance(300);
        QCOMPARE(Fixture::keysOf(f.s.panes(), f.devices), QStringList({"1:0"}));
    }

    // ---- settling ----
    // Cameras connecting together cause one re-pack: the gather time starts at the first change
    // and is not pushed back by later ones.
    void rowChangesAreGatheredIntoOneRepack()
    {
        Fixture f;
        store(QStringLiteral("liveSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("liveSelection"), QStringLiteral("keys"), QStringLiteral("1:0,1:1"));
        f.s.start();
        f.advance(8000);
        QSignalSpy spy(&f.s, &SelectionSession::panesChanged);
        f.devices.add(1, 0);
        f.advance(300);
        f.devices.add(1, 1);
        f.advance(199);
        QCOMPARE(spy.count(), 0);
        f.advance(1);   // 500 after the first
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.panes().size(), 2);
        f.advance(5000);
        QCOMPARE(spy.count(), 1);
    }
    void playbackGathersASelectionChangeLiveDoesNot()
    {
        Fixture live;
        live.devices.add(1, 0);
        live.s.start();
        live.advance(8000);
        QSignalSpy a(&live.s, &SelectionSession::panesChanged);
        live.s.selection()->toggle(QStringLiteral("1:0"));
        QCOMPARE(a.count(), 1);   // at once

        Fixture pb(false);
        pb.devices.add(1, 0);
        pb.s.start();
        QSignalSpy b(&pb.s, &SelectionSession::panesChanged);
        pb.s.selection()->toggle(QStringLiteral("1:0"));
        QCOMPARE(b.count(), 0);
        pb.advance(299);
        QCOMPARE(b.count(), 0);
        pb.advance(1);
        QCOMPARE(b.count(), 1);
        QCOMPARE(pb.s.panes().size(), 1);
    }

    // ---- pruning ----
    void removedRecordersAreDroppedOnceTheListHasBeenStill()
    {
        store(QStringLiteral("playbackSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("playbackSelection"), QStringLiteral("keys"), QStringLiteral("1:0,2:0,9:0"));
        Fixture f(false);
        f.devices.add(1, 0);
        f.devices.add(2, 0);
        f.s.start();
        QSignalSpy pruned(&f.s, &SelectionSession::pruned);
        f.advance(7999);
        QCOMPARE(f.s.selection()->count(), 3);
        f.advance(1);
        QCOMPARE(f.s.selection()->keys(), QStringList({"1:0", "2:0"}));   // host 9 is gone
        QCOMPARE(pruned.count(), 1);
    }
    // The countdown restarts at every row change (Playback's rule; Live now follows it).
    void aRowChangeRestartsThePruneCountdown()
    {
        store(QStringLiteral("playbackSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("playbackSelection"), QStringLiteral("keys"), QStringLiteral("9:0"));
        Fixture f(false);
        f.devices.add(1, 0);
        f.s.start();
        f.advance(6000);
        f.devices.add(1, 1);
        f.advance(6000);
        QCOMPARE(f.s.selection()->count(), 1);
        f.advance(2000);
        QCOMPARE(f.s.selection()->count(), 0);
        // And a removal does too.
        f.s.selection()->toggle(QStringLiteral("9:0"));
        f.advance(1000);
        f.devices.removeHost(1);
        f.devices.add(2, 0);
        f.advance(7999);
        QCOMPARE(f.s.selection()->count(), 1);
        f.advance(1);
        QCOMPARE(f.s.selection()->count(), 0);
    }
    // With no devices listed there is nothing to judge by.
    void nothingIsPrunedWithoutADeviceList()
    {
        store(QStringLiteral("playbackSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("playbackSelection"), QStringLiteral("keys"), QStringLiteral("1:0"));
        Fixture f(false);
        f.s.start();
        QSignalSpy pruned(&f.s, &SelectionSession::pruned);
        f.advance(60000);
        QCOMPARE(f.s.selection()->count(), 1);
        QCOMPARE(pruned.count(), 0);
    }
    void camerasGoneFromAFrigateServerAreDropped()
    {
        store(QStringLiteral("playbackSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("playbackSelection"), QStringLiteral("keys"), QStringLiteral("2:0,2:3,1:5"));
        Fixture f(false);
        f.devices.add(2, 0);
        f.devices.add(1, 0);
        f.devices.listed = {{QStringLiteral("2"), QVariantList{0, 1}}};   // 2:3 is gone; host 1 is a recorder
        f.s.start();
        f.advance(8000);
        QCOMPARE(f.s.selection()->keys(), QStringList({"2:0", "1:5"}));
    }
    void liveAlsoPrunesWhenTheWaitEnds()
    {
        store(QStringLiteral("liveSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("liveSelection"), QStringLiteral("keys"), QStringLiteral("9:0,1:0"));
        Fixture f;
        f.devices.add(1, 0);
        f.s.start();
        f.advance(8000);
        QCOMPARE(f.s.selection()->keys(), QStringList({"1:0"}));
        QCOMPARE(Fixture::keysOf(f.s.panes(), f.devices), QStringList({"1:0"}));
    }

    // ---- shapes ----
    void aMeasuredShapeIsRememberedBetweenLaunchesAndShared()
    {
        Fixture live, playback(false);
        live.devices.add(1, 0);
        playback.devices.add(1, 0);
        live.s.noteFrame(QStringLiteral("1:0"), 4.0 / 3.0);
        QCOMPARE(stored(QStringLiteral("mosaicShapes"), QStringLiteral("1:0")).toString(),
                 QStringLiteral("1.3333"));
        // The other page's session sees it at once: one store.
        QCOMPARE(playback.s.ratioOf(QStringLiteral("1:0")), 1.3333);
        QCOMPARE(playback.s.rememberedFrame(QStringLiteral("1:0")), 1.3333);
        // Decoder jitter (under 0.5 %) keeps what is remembered.
        live.s.noteFrame(QStringLiteral("1:0"), 1.3333 * 1.003);
        QCOMPARE(stored(QStringLiteral("mosaicShapes"), QStringLiteral("1:0")).toString(),
                 QStringLiteral("1.3333"));
        live.s.noteFrame(QStringLiteral("1:0"), 0);
        live.s.noteFrame(QString(), 1.5);
        QCOMPARE(live.s.rememberedFrame(QStringLiteral("1:0")), 1.3333);
    }
    void shapeRule()
    {
        Fixture f;
        f.devices.add(1, 0);
        f.devices.add(1, 1);
        f.devices.add(1, 2);
        QCOMPARE(f.s.ratioOf(QStringLiteral("1:0")), 16.0 / 9.0);   // never seen
        QCOMPARE(f.s.ratioOf(QStringLiteral("9:9")), 16.0 / 9.0);   // not a camera here
        f.s.noteFrame(QStringLiteral("1:0"), 2.0);
        QCOMPARE(f.s.ratioOf(QStringLiteral("1:0")), 2.0);
        f.devices.setRotation(0, 90);   // a quarter turn swaps width and height
        QCOMPARE(f.s.ratioOf(QStringLiteral("1:0")), 0.5);
        f.s.noteFrame(QStringLiteral("1:1"), 1.0);
        f.devices.setView(1, true, QStringLiteral("panorama"));   // a fisheye view's own shape
        QCOMPARE(f.s.ratioOf(QStringLiteral("1:1")), 3.3);
        f.devices.setView(1, false, QStringLiteral("panorama"));   // not a fisheye: its picture
        QCOMPARE(f.s.ratioOf(QStringLiteral("1:1")), 1.0);
        f.devices.setView(1, true, QString());   // no view chosen: its picture
        QCOMPARE(f.s.ratioOf(QStringLiteral("1:1")), 1.0);
    }
    void aShapeChangeOfASelectedCameraRepacksOnceSettled()
    {
        Fixture f;
        store(QStringLiteral("liveSelection"), QStringLiteral("migrated"), true);
        store(QStringLiteral("liveSelection"), QStringLiteral("keys"), QStringLiteral("1:0"));
        f.devices.add(1, 0);
        f.devices.add(1, 1);
        f.s.start();
        f.advance(8000);
        QCOMPARE(f.s.panes().at(0).toMap().value(QStringLiteral("ratio")).toDouble(), 16.0 / 9.0);
        QSignalSpy spy(&f.s, &SelectionSession::panesChanged);
        f.s.noteFrame(QStringLiteral("1:0"), 4.0 / 3.0);
        QCOMPARE(spy.count(), 0);
        f.advance(500);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.panes().at(0).toMap().value(QStringLiteral("ratio")).toDouble(), 1.3333);
        // A rotation does the same, with no frame measured.
        f.devices.setRotation(0, 90);
        f.advance(500);
        QCOMPARE(spy.count(), 2);
        // A camera that is not selected changes the layout of nothing.
        f.s.noteFrame(QStringLiteral("1:1"), 2.0);
        f.devices.setRotation(1, 90);
        f.advance(5000);
        QCOMPARE(spy.count(), 2);
        // Nor does the same shape again, nor unrelated device changes.
        f.s.noteFrame(QStringLiteral("1:0"), 4.0 / 3.0);
        emit f.devices.dataChanged(f.devices.index(0), f.devices.index(1));
        f.advance(5000);
        QCOMPARE(spy.count(), 2);
    }

    // ---- budget wording ----
    void hdRefusalIsEmptyWhenItFits()
    {
        Fixture f;
        f.s.selection()->setSaved(QStringLiteral("2:1"));   // Farm 1.0; HD adds 3 -> 4 of 5
        QCOMPARE(f.s.hdRefusal(QStringLiteral("2:1")), QString());
        f.s.selection()->setSaved(QStringLiteral("1:0"));
        QCOMPARE(f.s.hdRefusal(QStringLiteral("1:0")), QString());   // 6 of 7 Woorabinda
    }
    // The wording both pages used: Site, used / budget in Mbit/s (one decimal, none when
    // whole), what HD needs, then the advice.
    void hdRefusalTextIsTheOneBothPagesUsed()
    {
        Fixture f;
        f.s.selection()->setSaved(QStringLiteral("2:0,2:1"));   // Farm 2.5 of 5; HD adds 3 -> 5.5
        QCOMPARE(f.s.hdRefusal(QStringLiteral("2:1")),
                 QStringLiteral("Farm is at 2.5 / 5 Mbit/s; HD needs 3 more. "
                                "Deselect a camera or switch one out of HD."));
    }
};

QTEST_GUILESS_MAIN(TestSelectionSession)
#include "test_selectionsession.moc"
