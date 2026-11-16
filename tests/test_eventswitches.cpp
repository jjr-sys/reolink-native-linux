#include "core/CredentialStore.h"
#include "core/Database.h"
#include "core/Paths.h"
#include "device/DeviceManager.h"
#include "device/EventManager.h"
#include "device/EventSwitches.h"
#include "device/FrigateApi.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;

// Event switches (wayfinder/events-feed "Build: event switches (core)"): a switched-off
// (recorder, camera, type) pair is not stored, pictured, notified or highlighted.
class TestEventSwitches : public QObject
{
    Q_OBJECT

    // Stores one event; with snapshotBytes >= 0 also writes its snapshot file of that
    // size. Returns the snapshot's path (empty without one).
    static QString addEvent(Database &db, qint64 host, int ch, qint64 ts, const char *type,
                            int snapshotBytes)
    {
        EventRecord e;
        e.hostId = host;
        e.channel = ch;
        e.timestamp = ts;
        e.type = QString::fromLatin1(type);
        e.camera = QStringLiteral("Cam %1").arg(ch);
        const qint64 id = db.addEvent(e);
        if (snapshotBytes < 0)
            return {};
        const QString path = Paths::thumbnailsDir() + QStringLiteral("/event_%1.jpg").arg(id);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(QByteArray(snapshotBytes, 'x')) != snapshotBytes)
            return {};
        f.close();
        db.setEventThumbnail(id, path);
        return path;
    }

private slots:
    void initTestCase()
    {
        // Snapshots, settings and the keyring stay out of the real profile.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("rl-test-eventswitches"));
        QCoreApplication::setApplicationName(QStringLiteral("rl-test-eventswitches"));
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-eventswitches-test-bus");
    }

    void liveDetectionOfSwitchedOffPairGoesNowhere()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_live"));
        QVERIFY(db.open());
        CredentialStore credentials;
        DeviceManager devices(&db, &credentials);
        EventManager events(&db, &devices);
        QSignalSpy detections(&devices, &DeviceManager::detectionEvent);
        const int before = events.rowCount();

        QVERIFY(devices.eventSwitches()->setOff(7, 1, QStringLiteral("vehicle"), true));
        devices.reportDetection(7, 1, QStringLiteral("vehicle"), QStringLiteral("Gate"));
        // Row, snapshot, notification and Live highlight all hang off detectionEvent.
        QCOMPARE(detections.count(), 0);
        QCOMPARE(events.rowCount(), before);

        devices.reportDetection(7, 1, QStringLiteral("person"), QStringLiteral("Gate"));
        QCOMPARE(detections.count(), 1);
        QCOMPARE(events.rowCount(), before + 1);

        QVERIFY(devices.eventSwitches()->setOff(7, 1, QStringLiteral("vehicle"), false));
        devices.reportDetection(7, 1, QStringLiteral("vehicle"), QStringLiteral("Gate"));
        QCOMPARE(detections.count(), 2); // switched on: logs afresh
        QCOMPARE(events.rowCount(), before + 2);
    }

    void frigateCatchUpSkipsSwitchedOffPair()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_frigate"));
        QVERIFY(db.open());
        CredentialStore credentials;
        DeviceManager devices(&db, &credentials);
        EventManager events(&db, &devices);
        QVERIFY(devices.eventSwitches()->setOff(9, 2, QStringLiteral("vehicle"), true));

        const double now = double(QDateTime::currentSecsSinceEpoch());
        // Frigate lists newest first. Far enough apart that the burst gate keeps each.
        const QVector<frigate::Event> history = {
            {QStringLiteral("e3"), QStringLiteral("drive"), QStringLiteral("person"), now - 600},
            {QStringLiteral("e2"), QStringLiteral("drive"), QStringLiteral("car"), now - 1800},
            {QStringLiteral("e1"), QStringLiteral("drive"), QStringLiteral("truck"), now - 3600},
        };
        events.applyFrigateHistory(9, {{QStringLiteral("drive"), 2}}, history);

        const QVector<EventRecord> stored = db.recentEvents(10, {}, {9});
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.first().type, QStringLiteral("person"));
    }

    // Review #2, 4a: Frigate's history keeps a second type seen beside the first (bursts are
    // per camera and type), and a live Frigate event is filed at the time Frigate saw it, so a
    // later history sync finds it instead of filing it again.
    void frigateHistoryKeepsEachTypeAndFindsLiveRows()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_types"));
        QVERIFY(db.open());
        CredentialStore credentials;
        DeviceManager devices(&db, &credentials);
        EventManager events(&db, &devices);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        // Seen live while the app ran, reported late (after a Frigate outage).
        devices.reportDetection(9, 2, QStringLiteral("person"), QStringLiteral("Drive"), now - 40);
        QVector<EventRecord> stored = db.recentEvents(10, {}, {9});
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.first().timestamp, now - 40);
        // A time from a server clock ahead of ours is filed as now, never in the future.
        devices.reportDetection(9, 3, QStringLiteral("person"), QStringLiteral("Gate"), now + 600);
        QVERIFY(db.recentEvents(10, {}, {9}).first().timestamp <= QDateTime::currentSecsSinceEpoch());

        const QVector<frigate::Event> history = {   // newest first
            {QStringLiteral("e3"), QStringLiteral("drive"), QStringLiteral("person"), double(now - 40)},
            {QStringLiteral("e2"), QStringLiteral("drive"), QStringLiteral("car"), double(now - 595)},
            {QStringLiteral("e1"), QStringLiteral("drive"), QStringLiteral("person"), double(now - 600)},
        };
        events.applyFrigateHistory(9, {{QStringLiteral("drive"), 2}}, history);
        stored = db.recentEvents(10, {}, {9});
        QStringList seen;
        for (const EventRecord &r : stored)
            if (r.channel == 2)
                seen << QStringLiteral("%1@%2").arg(r.type).arg(now - r.timestamp);
        QCOMPARE(seen.join(","), QStringLiteral("person@40,vehicle@595,person@600"));
    }

    // wayfinder/architecture ticket 03: a history sync's rows go in the list at their place
    // by time (newest first), not on top, and a sync doesn't count anything unread.
    void historyRowsLandAtTheirPlaceInTheList()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_place"));
        QVERIFY(db.open());
        CredentialStore credentials;
        DeviceManager devices(&db, &credentials);
        EventManager events(&db, &devices);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        devices.reportDetection(7, 1, QStringLiteral("person"), QStringLiteral("Gate"));
        devices.reportDetection(7, 1, QStringLiteral("person"), QStringLiteral("Gate"), now - 3600);
        const int unread = events.unread();
        events.applyFrigateHistory(9, {{QStringLiteral("drive"), 2}},
            {{QStringLiteral("e1"), QStringLiteral("drive"), QStringLiteral("person"), double(now - 600)}});
        QCOMPARE(events.rowCount(), 3);
        QCOMPARE(events.unread(), unread);
        QList<qint64> times;
        for (int i = 0; i < 3; ++i)
            times << events.data(events.index(i), EventManager::TimestampRole).toLongLong();
        QVERIFY2(times.at(0) >= now && times.at(1) == now - 600 && times.at(2) == now - 3600,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(times.at(0)).arg(times.at(1)).arg(times.at(2))));

        // Ticket 02 (its rule is the Inbox's, test_inbox): the live report of an event the
        // sync filed is no new row and nothing unread.
        devices.reportDetection(9, 2, QStringLiteral("person"), QStringLiteral("Drive"), now - 597);
        QCOMPARE(events.rowCount(), 3);
        QCOMPARE(events.unread(), unread);
    }

    // An event that couldn't be stored is not counted as unread (nor pictured or announced).
    void anEventThatWasntStoredIsntCounted()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_unstored"));
        QVERIFY(db.open());
        CredentialStore credentials;
        DeviceManager devices(&db, &credentials);
        EventManager events(&db, &devices);
        devices.reportDetection(9, 2, QStringLiteral("person"), QStringLiteral("Drive"));
        QCOMPARE(events.unread(), 1);
        {   // the store breaks under it
            QSqlDatabase other = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("breaker"));
            other.setDatabaseName(dir.filePath("t.db"));
            QVERIFY(other.open());
            QVERIFY(QSqlQuery(other).exec(QStringLiteral("DROP TABLE events")));
            other.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("breaker"));
        devices.reportDetection(9, 2, QStringLiteral("vehicle"), QStringLiteral("Drive"));
        QCOMPARE(events.unread(), 1);
    }

    void switchingOffReportsThenDeletesOnlyThatPair()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_off"));
        QVERIFY(db.open());
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        // Two vehicles on camera 2 (one with a 1000-byte snapshot, one with 234 bytes),
        // a person on the same camera, and a vehicle on camera 3.
        const QString a = addEvent(db, 9, 2, now - 100, "vehicle", 1000);
        const QString b = addEvent(db, 9, 2, now - 200, "vehicle", 234);
        const QString keepPerson = addEvent(db, 9, 2, now - 300, "person", 50);
        const QString keepOtherCam = addEvent(db, 9, 3, now - 400, "vehicle", 60);
        addEvent(db, 9, 2, now - 500, "vehicle", -1); // no snapshot

        CredentialStore credentials;
        DeviceManager devices(&db, &credentials);
        EventManager events(&db, &devices);
        QCOMPARE(events.rowCount(), 5);

        const QVariantMap stats = events.switchStats(9, 2, QStringLiteral("vehicle"));
        QCOMPARE(stats.value("count").toInt(), 3);
        QCOMPARE(stats.value("bytes").toLongLong(), 1234);

        QVERIFY(events.switchOff(9, 2, QStringLiteral("vehicle")));
        QVERIFY(devices.eventSwitches()->isOff(9, 2, QStringLiteral("vehicle")));
        QCOMPARE(events.rowCount(), 2);
        QVERIFY(!QFile::exists(a));
        QVERIFY(!QFile::exists(b));
        QVERIFY(QFile::exists(keepPerson));
        QVERIFY(QFile::exists(keepOtherCam));
        QCOMPARE(events.switchStats(9, 2, QStringLiteral("vehicle")).value("count").toInt(), 0);
        QCOMPARE(events.switchStats(9, 2, QStringLiteral("person")).value("count").toInt(), 1);

        QVERIFY(events.switchOn(9, 2, QStringLiteral("vehicle")));
        QVERIFY(!devices.eventSwitches()->isOff(9, 2, QStringLiteral("vehicle")));
        devices.reportDetection(9, 2, QStringLiteral("vehicle"), QStringLiteral("Drive"));
        QCOMPARE(events.rowCount(), 3);
    }

    void statusRefusesToSwitchOffAndKeepsItsEvents()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_off_status"));
        QVERIFY(db.open());
        addEvent(db, 9, 2, QDateTime::currentSecsSinceEpoch() - 100, "offline", -1);
        CredentialStore credentials;
        DeviceManager devices(&db, &credentials);
        EventManager events(&db, &devices);
        QVERIFY(!events.switchOff(9, 2, QStringLiteral("offline")));
        QCOMPARE(events.rowCount(), 1);
    }

    void switchedOffPairSurvivesRestart()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("t.db");
        {
            Database db(path, QStringLiteral("sw_restart_1"));
            QVERIFY(db.open());
            EventSwitches sw(&db);
            QVERIFY(!sw.isOff(1, 2, QStringLiteral("vehicle")));
            QVERIFY(sw.setOff(1, 2, QStringLiteral("vehicle"), true));
            QVERIFY(sw.isOff(1, 2, QStringLiteral("vehicle")));
            QVERIFY(!sw.isOff(1, 2, QStringLiteral("person"))); // only that type
            QVERIFY(!sw.isOff(1, 3, QStringLiteral("vehicle"))); // only that camera
            QVERIFY(!sw.isOff(4, 2, QStringLiteral("vehicle"))); // only that recorder
        }
        Database db(path, QStringLiteral("sw_restart_2"));
        QVERIFY(db.open());
        EventSwitches sw(&db);
        QVERIFY(sw.isOff(1, 2, QStringLiteral("vehicle")));
        QVERIFY(sw.setOff(1, 2, QStringLiteral("vehicle"), false));
        Database db3(path, QStringLiteral("sw_restart_3"));
        QVERIFY(db3.open());
        QVERIFY(!EventSwitches(&db3).isOff(1, 2, QStringLiteral("vehicle")));
    }

    void statusCannotBeSwitchedOff()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_status"));
        QVERIFY(db.open());
        EventSwitches sw(&db);
        QSignalSpy changed(&sw, &EventSwitches::changed);
        QVERIFY(!sw.setOff(1, 0, QStringLiteral("online"), true));
        QVERIFY(!sw.setOff(1, 0, QStringLiteral("offline"), true));
        QVERIFY(!sw.isOff(1, 0, QStringLiteral("online")));
        QVERIFY(!sw.isOff(1, 0, QStringLiteral("offline")));
        QCOMPARE(changed.count(), 0);
    }

    void dogCatIsPet()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("sw_pet"));
        QVERIFY(db.open());
        EventSwitches sw(&db);
        QSignalSpy changed(&sw, &EventSwitches::changed);
        QVERIFY(sw.setOff(1, 0, QStringLiteral("dog_cat"), true));
        QVERIFY(sw.isOff(1, 0, QStringLiteral("pet")));
        QVERIFY(sw.isOff(1, 0, QStringLiteral("dog_cat")));
        QCOMPARE(changed.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestEventSwitches)
#include "test_eventswitches.moc"
