#include "core/CredentialStore.h"
#include "core/Database.h"
#include "core/Paths.h"
#include "device/DeviceManager.h"
#include "device/EventManager.h"
#include "device/FirstSwitchOffs.h"

#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;
using device::KnownCamera;

// The first switch-offs (wayfinder/events-feed "Build: first switch-offs on first launch"):
// a one-time list from settings, "Recorder/Camera/type" with * wildcards, resolved to the
// cameras the app knows.
class TestFirstSwitchOffs : public QObject
{
    Q_OBJECT

    static QVector<KnownCamera> cameras()
    {
        const QStringList reolink = {"person", "vehicle", "pet", "motion", "status"};
        return {
            {1, "Woorabinda", 0, "Driveway", reolink},
            {1, "Woorabinda", 7, "Patio", reolink},
            {2, "Farm", 0, "Gathering Hall", {"motion", "status"}},
            {2, "Farm", 6, "Carpark", reolink},
            {3, "Lascelles", 1, "Kitchen", {"person", "status"}}, // Frigate: no motion
            {5, "lascelles", 0, "Kitchen", reolink},
            {6, "Old", 0, "Shed", {}}, // types not known yet: anything goes
        };
    }

    static QStringList keys(const QVector<EventSwitchKey> &pairs)
    {
        QStringList out;
        for (const EventSwitchKey &k : pairs)
            out << QStringLiteral("%1:%2:%3").arg(k.hostId).arg(k.channel).arg(k.type);
        out.sort();
        return out;
    }

    // A recorder named as in the list. It has no password and points at a closed local
    // port, so nothing leaves the machine.
    static qint64 addRecorder(Database &db, const QString &name)
    {
        HostRecord h;
        h.kind = QStringLiteral("nvr");
        h.name = name;
        h.addr = QStringLiteral("127.0.0.1");
        h.port = 9;
        h.https = false;
        return db.addHost(h);
    }

    static void addEvent(Database &db, qint64 host, int ch, const QString &camera,
                         const char *type, int snapshotBytes)
    {
        EventRecord e;
        e.hostId = host;
        e.channel = ch;
        e.timestamp = QDateTime::currentSecsSinceEpoch() - 60;
        e.type = QString::fromLatin1(type);
        e.camera = camera;
        const qint64 id = db.addEvent(e);
        if (snapshotBytes <= 0)
            return;
        const QString path = Paths::thumbnailsDir() + QStringLiteral("/event_%1.jpg").arg(id);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(snapshotBytes, 'x'));
        f.close();
        db.setEventThumbnail(id, path);
    }

    // Woorabinda: Patio (person 2 with 100-byte snapshots, pet 1); Farm: Carpark (vehicle 1,
    // 50 bytes; motion 1). The list switches off Patio Person and Motion everywhere.
    struct Site {
        QTemporaryDir dir;
        Database db{dir.filePath("t.db"), QStringLiteral("first_%1").arg(++s_db)};
        qint64 woorabinda = -1, farm = -1;
        Site()
        {
            QVERIFY(db.open());
            woorabinda = addRecorder(db, QStringLiteral("Woorabinda"));
            farm = addRecorder(db, QStringLiteral("Farm"));
            addEvent(db, woorabinda, 7, QStringLiteral("Patio"), "person", 100);
            addEvent(db, woorabinda, 7, QStringLiteral("Patio"), "person", 100);
            addEvent(db, woorabinda, 7, QStringLiteral("Patio"), "pet", 0);
            addEvent(db, farm, 6, QStringLiteral("Carpark"), "vehicle", 50);
            addEvent(db, farm, 6, QStringLiteral("Carpark"), "motion", 0);
            // Known from the channel list only: nothing stored yet.
            db.setChannelEventTypes(farm, 0, {QStringLiteral("motion"), QStringLiteral("status")});
            db.setChannelEventTypes(farm, 1, {QStringLiteral("person"), QStringLiteral("status")});
        }
        static inline int s_db = 0;
    };

    static void setList(const QStringList &entries)
    {
        QSettings s;
        s.remove(QStringLiteral("events"));
        if (!entries.isEmpty())
            s.setValue(QStringLiteral("events/firstSwitchOffs"), entries);
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("rl-test-firstswitchoffs"));
        QCoreApplication::setApplicationName(QStringLiteral("rl-test-firstswitchoffs"));
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-firstswitchoffs-test-bus");
    }

    void pendingListsThePairsAndTheirTotal()
    {
        setList({"Woorabinda/Patio/person", "*/*/motion"});
        Site site;
        CredentialStore credentials;
        DeviceManager devices(&site.db, &credentials);
        EventManager events(&site.db, &devices);
        const QVariantMap pending = events.firstSwitchOffs();
        QCOMPARE(pending.value("count").toInt(), 3); // 2 Patio Person + 1 Carpark Motion
        QCOMPARE(pending.value("bytes").toLongLong(), 200);
        QStringList named;
        for (const QVariant &v : pending.value("pairs").toList()) {
            const QVariantMap m = v.toMap();
            named << QStringLiteral("%1/%2/%3=%4")
                         .arg(m.value("recorder").toString(), m.value("camera").toString(),
                              m.value("type").toString())
                         .arg(m.value("count").toInt());
        }
        named.sort();
        QCOMPARE(named, QStringList({"Farm/Carpark/motion=1", "Farm/Channel 1/motion=0",
                                     "Woorabinda/Patio/motion=0", "Woorabinda/Patio/person=2"}));
    }

    void confirmSwitchesOffExactlyThoseAndAsksNoMore()
    {
        setList({"Woorabinda/Patio/person", "*/*/motion"});
        Site site;
        CredentialStore credentials;
        DeviceManager devices(&site.db, &credentials);
        EventManager events(&site.db, &devices);
        QVERIFY(events.confirmFirstSwitchOffs());

        EventSwitches *sw = devices.eventSwitches();
        QVERIFY(sw->isOff(site.woorabinda, 7, QStringLiteral("person")));
        QVERIFY(sw->isOff(site.woorabinda, 7, QStringLiteral("motion")));
        QVERIFY(sw->isOff(site.farm, 6, QStringLiteral("motion")));
        QVERIFY(sw->isOff(site.farm, 0, QStringLiteral("motion")));
        QVERIFY(!sw->isOff(site.woorabinda, 7, QStringLiteral("pet")));
        QVERIFY(!sw->isOff(site.farm, 6, QStringLiteral("vehicle")));
        QCOMPARE(sw->offPairs().size(), 4); // not Farm channel 2: it can't report Motion
        QCOMPARE(events.rowCount(), 2); // Patio Pet and Carpark Vehicle stay
        QVERIFY(events.firstSwitchOffs().isEmpty());

        // Shown once: a later launch doesn't ask again.
        EventManager again(&site.db, &devices);
        QVERIFY(again.firstSwitchOffs().isEmpty());
    }

    void cancelChangesNothingAndAsksNoMore()
    {
        setList({"Woorabinda/Patio/person", "*/*/motion"});
        Site site;
        CredentialStore credentials;
        DeviceManager devices(&site.db, &credentials);
        EventManager events(&site.db, &devices);
        events.declineFirstSwitchOffs();
        QVERIFY(devices.eventSwitches()->offPairs().isEmpty());
        QCOMPARE(events.rowCount(), 5);
        QVERIFY(events.firstSwitchOffs().isEmpty());
        QVERIFY(!events.confirmFirstSwitchOffs()); // nothing pending any more
        QVERIFY(devices.eventSwitches()->offPairs().isEmpty());
    }

    void withoutAListNothingIsAsked()
    {
        setList({});
        Site site;
        CredentialStore credentials;
        DeviceManager devices(&site.db, &credentials);
        EventManager events(&site.db, &devices);
        QVERIFY(events.firstSwitchOffs().isEmpty());
    }

    void namedPairsMatchRecorderExactlyAndCameraIgnoringCase()
    {
        const auto pairs = device::resolveFirstSwitchOffs(
            {"Woorabinda/PATIO/person", "Farm/Carpark/vehicle", "lascelles/Kitchen/person",
             "woorabinda/Driveway/vehicle"},
            cameras());
        // "lascelles" is the RLN8 (host 5), not the Frigate host "Lascelles"; and
        // "woorabinda" names no recorder.
        QCOMPARE(keys(pairs), QStringList({"1:7:person", "2:6:vehicle", "5:0:person"}));
    }

    void wildcardsSkipCamerasThatCantReportTheType()
    {
        const auto pairs = device::resolveFirstSwitchOffs({"*/*/motion"}, cameras());
        QCOMPARE(keys(pairs),
                 QStringList({"1:0:motion", "1:7:motion", "2:0:motion", "2:6:motion",
                              "5:0:motion", "6:0:motion"}));
    }

    void unmatchedMalformedAndStatusEntriesAreSkipped()
    {
        const auto pairs = device::resolveFirstSwitchOffs(
            {"Nowhere/Patio/person", "Woorabinda/Nothing/person", "bad entry",
             "Woorabinda/Patio/online", "Woorabinda/Driveway/dog_cat", "Woorabinda/Driveway/pet",
             " Farm / Carpark / vehicle "},
            cameras());
        QCOMPARE(keys(pairs), QStringList({"1:0:pet", "2:6:vehicle"}));
    }
};

QTEST_GUILESS_MAIN(TestFirstSwitchOffs)
#include "test_firstswitchoffs.moc"
