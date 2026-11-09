#include "core/Database.h"
#include "device/EventHistory.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

using namespace rl;

class TestDatabase : public QObject
{
    Q_OBJECT

    // Current schema version; bump alongside new migrations in Database.cpp.
    static constexpr int kSchemaVersion = 3;

    static constexpr qint64 kHour = 3600;
    static constexpr qint64 kDay = 24 * kHour;
    static constexpr qint64 kNow = 1794200000;

    static qint64 add(Database &db, qint64 host, int ch, qint64 ts, const char *type,
                      const QString &thumb = {})
    {
        EventRecord e;
        e.hostId = host;
        e.channel = ch;
        e.timestamp = ts;
        e.type = QString::fromLatin1(type);
        e.camera = QStringLiteral("Cam %1").arg(ch);
        e.thumbnail = thumb;
        return db.addEvent(e);
    }

    static QStringList types(const QVector<EventRecord> &evs)
    {
        QStringList out;
        for (const EventRecord &e : evs)
            out.append(e.type);
        return out;
    }

private slots:
    void migratesToCurrent()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_migrate"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QCOMPARE(db.schemaVersion(), kSchemaVersion);
    }

    void openIsIdempotent()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("t.db");
        {
            Database db(path, QStringLiteral("test_idem_1"));
            QVERIFY(db.open());
        }
        Database db2(path, QStringLiteral("test_idem_2"));
        QVERIFY(db2.open());
        QCOMPARE(db2.schemaVersion(), kSchemaVersion);
    }

    void eventCrud()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_events"));
        QVERIFY(db.open());
        EventRecord e;
        e.hostId = 7;
        e.timestamp = 1700000000;
        e.type = QStringLiteral("person");
        e.camera = QStringLiteral("Front Door");
        QVERIFY(db.addEvent(e) > 0);
        e.timestamp = 1700000100;
        e.type = QStringLiteral("vehicle");
        QVERIFY(db.addEvent(e) > 0);

        const QVector<EventRecord> evs = db.recentEvents(10);
        QCOMPARE(evs.size(), 2);
        QCOMPARE(evs[0].type, QStringLiteral("vehicle")); // newest first
        QCOMPARE(evs[1].type, QStringLiteral("person"));
        QVERIFY(db.clearEvents());
        QCOMPARE(db.recentEvents(10).size(), 0);
    }

    void channelNameOverrides()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_chname"));
        QVERIFY(db.open());
        HostRecord rec;
        rec.kind = QStringLiteral("nvr");
        rec.name = QStringLiteral("Farm");
        rec.addr = QStringLiteral("192.168.2.183");
        const qint64 id = db.addHost(rec);
        QVERIFY(id > 0);

        QVERIFY(db.channelNameOverrides(id).isEmpty());
        QVERIFY(db.setChannelNameOverride(id, 8, QStringLiteral("  Driveway ")));
        QCOMPARE(db.channelNameOverrides(id).value(8), QStringLiteral("Driveway"));
        QVERIFY(db.setChannelNameOverride(id, 8, QStringLiteral("Front Drive"))); // replaces
        QCOMPARE(db.channelNameOverrides(id).size(), 1);
        QCOMPARE(db.channelNameOverrides(id).value(8), QStringLiteral("Front Drive"));
        QVERIFY(db.channelNameOverrides(id + 1).isEmpty()); // per host
        QVERIFY(db.setChannelNameOverride(id, 8, QStringLiteral("  "))); // clears
        QVERIFY(db.channelNameOverrides(id).isEmpty());
    }


    // A v2 store (the one in use before Events storage) keeps its rows through v3.
    void migratesV2RowsToV3()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("t.db");
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "v2raw");
            raw.setDatabaseName(path);
            QVERIFY(raw.open());
            QSqlQuery q(raw);
            QVERIFY(q.exec("CREATE TABLE schema_version (version INTEGER NOT NULL)"));
            QVERIFY(q.exec("INSERT INTO schema_version VALUES (2)"));
            QVERIFY(q.exec("CREATE TABLE hosts (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL,"
                           " name TEXT NOT NULL, addr TEXT NOT NULL, port INTEGER NOT NULL DEFAULT 443,"
                           " https INTEGER NOT NULL DEFAULT 1, username TEXT NOT NULL DEFAULT '',"
                           " model TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')))"));
            QVERIFY(q.exec("CREATE TABLE channels (host_id INTEGER NOT NULL REFERENCES hosts(id) ON DELETE CASCADE,"
                           " idx INTEGER NOT NULL, name TEXT NOT NULL DEFAULT '', model TEXT NOT NULL DEFAULT '',"
                           " PRIMARY KEY (host_id, idx))"));
            QVERIFY(q.exec("CREATE TABLE layouts (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL,"
                           " preset INTEGER NOT NULL DEFAULT 4, panes_json TEXT NOT NULL DEFAULT '[]')"));
            QVERIFY(q.exec("CREATE TABLE events (id INTEGER PRIMARY KEY AUTOINCREMENT, host_id INTEGER NOT NULL,"
                           " channel INTEGER NOT NULL DEFAULT 0, ts INTEGER NOT NULL, type TEXT NOT NULL,"
                           " camera TEXT NOT NULL DEFAULT '', thumbnail TEXT NOT NULL DEFAULT '')"));
            QVERIFY(q.exec("INSERT INTO hosts (kind, name, addr) VALUES ('nvr', 'Farm', '10.0.0.1')"));
            QVERIFY(q.exec("INSERT INTO channels (host_id, idx, name) VALUES (1, 3, 'Gate')"));
            QVERIFY(q.exec("INSERT INTO events (host_id, channel, ts, type) VALUES (1, 3, 100, 'person')"));
            raw.close();
        }
        QSqlDatabase::removeDatabase("v2raw");

        Database db(path, QStringLiteral("test_v2v3"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QCOMPARE(db.schemaVersion(), kSchemaVersion);
        QCOMPARE(db.recentEvents(10).size(), 1);
        QCOMPARE(db.channelNameOverrides(1).value(3), QStringLiteral("Gate"));
        QVERIFY(db.setChannelEventTypes(1, 3, {"person", "motion"}));
        QCOMPARE(db.channelEventTypes(1).value(3), QStringList({"person", "motion"}));
    }

    // Motion is kept 24 h, everything else (Status included) 7 days. The thumbnails of
    // the removed rows come back so their files can go too.
    void pruneKeepsMotion24hOthers7Days()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_prune"));
        QVERIFY(db.open());
        add(db, 1, 0, kNow - 23 * kHour, "motion");
        add(db, 1, 0, kNow - 25 * kHour, "motion", "/t/old-motion.jpg");
        add(db, 1, 0, kNow - 6 * kDay, "person");
        add(db, 1, 0, kNow - 6 * kDay, "offline");
        add(db, 1, 0, kNow - 8 * kDay, "vehicle", "/t/old-vehicle.jpg");
        add(db, 1, 0, kNow - 8 * kDay, "online");

        QStringList gone = db.purgeExpiredEvents(kNow, kDay, 7 * kDay);
        gone.sort();
        QCOMPARE(gone, QStringList({"/t/old-motion.jpg", "/t/old-vehicle.jpg"}));
        QStringList kept = types(db.recentEvents(10));
        kept.sort();
        QCOMPARE(kept, QStringList({"motion", "offline", "person"}));
    }

    // The per-recorder cap counts everything except motion, and trims only those.
    void capCountsEverythingButMotion()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_cap"));
        QVERIFY(db.open());
        add(db, 1, 0, kNow - 50, "person", "/t/p1.jpg");   // oldest capped: trimmed
        add(db, 1, 0, kNow - 40, "motion", "/t/m1.jpg");   // motion: never trimmed by the cap
        add(db, 1, 0, kNow - 30, "offline");               // oldest left after p1: trimmed
        add(db, 1, 0, kNow - 20, "vehicle");
        add(db, 1, 1, kNow - 10, "person");
        add(db, 1, 0, kNow - 5, "motion");
        add(db, 2, 0, kNow - 60, "person");                // another recorder: untouched

        QCOMPARE(db.countCappedEvents(1), 4);
        QCOMPARE(db.countCappedEvents(2), 1);

        const QStringList gone = db.trimHostEvents(1, 2);
        QCOMPARE(gone, QStringList({"/t/p1.jpg"}));
        QCOMPARE(db.countCappedEvents(1), 2);
        QCOMPARE(db.countCappedEvents(2), 1);
        QCOMPARE(types(db.eventsBetween(1, 0, 0, kNow + 1)),
                 QStringList({"motion", "vehicle", "motion"}));

        // Equal timestamps: the cut falls between rows by id, keeping exactly `keep`.
        add(db, 3, 0, kNow, "person");
        add(db, 3, 0, kNow, "person");
        add(db, 3, 0, kNow, "person");
        db.trimHostEvents(3, 2);
        QCOMPARE(db.countCappedEvents(3), 2);
        // Under the cap: nothing to do.
        QVERIFY(db.trimHostEvents(3, 5).isEmpty());
        QCOMPARE(db.countCappedEvents(3), 2);
    }

    // A day query is a half-open [from, to) range on one camera, newest first,
    // optionally narrowed to some types.
    void dayQueryRespectsBoundaries()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_day"));
        QVERIFY(db.open());
        const qint64 from = 1794153600, to = from + kDay;
        add(db, 1, 2, from - 1, "person");  // previous day
        add(db, 1, 2, from, "person");      // first second of the day
        add(db, 1, 2, from + 100, "motion");
        add(db, 1, 2, to - 1, "vehicle");   // last second of the day
        add(db, 1, 2, to, "person");        // next day
        add(db, 1, 3, from + 50, "person"); // other camera
        add(db, 2, 2, from + 50, "person"); // other recorder

        const QVector<EventRecord> day = db.eventsBetween(1, 2, from, to);
        QCOMPARE(types(day), QStringList({"vehicle", "motion", "person"}));
        QCOMPARE(day.last().timestamp, from);
        QCOMPARE(types(db.eventsBetween(1, 2, from, to, {"person", "vehicle"})),
                 QStringList({"vehicle", "person"}));
    }

    // End to end: a local day (Perth, UTC+8) picks out the right rows.
    void localDayQuery()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_localday"));
        QVERIFY(db.open());
        const QTimeZone perth("Australia/Perth");
        const device::TimeSpan d = device::localDay(QDate(2026, 11, 9), perth);
        add(db, 1, 0, QDateTime(QDate(2026, 11, 8), QTime(23, 59, 59), perth).toSecsSinceEpoch(), "pet");
        add(db, 1, 0, QDateTime(QDate(2026, 11, 9), QTime(0, 0), perth).toSecsSinceEpoch(), "person");
        add(db, 1, 0, QDateTime(QDate(2026, 11, 9), QTime(23, 59, 59), perth).toSecsSinceEpoch(), "vehicle");
        add(db, 1, 0, QDateTime(QDate(2026, 11, 10), QTime(0, 0), perth).toSecsSinceEpoch(), "pet");
        QCOMPARE(types(db.eventsBetween(1, 0, d.from, d.to)), QStringList({"vehicle", "person"}));
    }

    // The current Events page: newest-N with its type / recorder / camera filters.
    void recentEventsFilters()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_recent"));
        QVERIFY(db.open());
        add(db, 1, 0, 10, "person");
        add(db, 1, 1, 20, "motion");
        add(db, 2, 0, 30, "person");
        add(db, 3, 0, 40, "vehicle");
        QCOMPARE(db.recentEvents(2).size(), 2);
        QCOMPARE(db.recentEvents(10, QStringLiteral("person")).size(), 2);
        QCOMPARE(db.recentEvents(10, {}, {1, 3}).size(), 3);
        QCOMPARE(db.recentEvents(10, {}, {}, QStringLiteral("Cam 1")).size(), 1);
        QCOMPARE(db.recentEvents(10, QStringLiteral("person"), {1}).size(), 1);
    }

    void eventLookups()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_lookup"));
        QVERIFY(db.open());
        QVERIFY(!db.hostHasEvents(1));
        const qint64 id = add(db, 1, 4, 1000, "person");
        QVERIFY(db.hostHasEvents(1));
        QVERIFY(!db.hostHasEvents(2));

        QVERIFY(db.eventNear(1, 4, 1020, 30).has_value());
        QCOMPARE(db.eventNear(1, 4, 1020, 30)->id, id);
        QVERIFY(!db.eventNear(1, 4, 1030, 30).has_value()); // window is exclusive
        QVERIFY(!db.eventNear(1, 5, 1000, 30).has_value());

        // Setting a thumbnail says whether the row is still there.
        QVERIFY(db.setEventThumbnail(id, "/t/a.jpg"));
        QVERIFY(!db.setEventThumbnail(id + 99, "/t/b.jpg"));

        QCOMPARE(db.eventCameras(1), QStringList({"Cam 4"}));
        QVERIFY(db.renameEventCamera(1, "Cam 4", "Gate"));
        QCOMPARE(db.eventCameras(1), QStringList({"Gate"}));
    }

    // Each launch is one run; its last_seen moves with the heartbeat.
    void appRuns()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_runs"));
        QVERIFY(db.open());
        const qint64 a = db.beginAppRun(1000);
        QVERIFY(db.touchAppRun(a, 1600));
        const qint64 b = db.beginAppRun(5000);
        QVERIFY(b != a);
        QVERIFY(db.touchAppRun(b, 5060));
        db.beginAppRun(90000);

        // Runs overlapping [1500, 6000), oldest first.
        const auto runs = db.appRuns(1500, 6000);
        QCOMPARE(runs.size(), 2);
        QCOMPARE(runs[0].first, 1000);
        QCOMPARE(runs[0].second, 1600);
        QCOMPARE(runs[1].first, 5000);
        QCOMPARE(runs[1].second, 5060);
        QCOMPARE(db.appRuns(1700, 4000).size(), 0);
    }

    // Supported types are kept per channel, and survive clearing a name override.
    void channelEventTypes()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_chtypes"));
        QVERIFY(db.open());
        HostRecord rec;
        rec.kind = QStringLiteral("nvr");
        rec.name = QStringLiteral("Farm");
        rec.addr = QStringLiteral("10.0.0.2");
        const qint64 id = db.addHost(rec);
        QVERIFY(db.setChannelEventTypes(id, 2, {"person", "vehicle", "motion", "status"}));
        QVERIFY(db.setChannelNameOverride(id, 2, "Gate"));
        QVERIFY(db.setChannelNameOverride(id, 2, ""));
        QVERIFY(db.channelNameOverrides(id).isEmpty());
        QCOMPARE(db.channelEventTypes(id).value(2),
                 QStringList({"person", "vehicle", "motion", "status"}));
        QVERIFY(db.setChannelEventTypes(id, 2, {"motion", "status"}));
        QCOMPARE(db.channelEventTypes(id).value(2), QStringList({"motion", "status"}));
    }

    void hostCrud()
    {
        QTemporaryDir dir;
        Database db(dir.filePath("t.db"), QStringLiteral("test_crud"));
        QVERIFY(db.open());

        HostRecord rec;
        rec.kind = QStringLiteral("camera");
        rec.name = QStringLiteral("Front Door");
        rec.addr = QStringLiteral("192.168.1.20");
        rec.port = 443;
        rec.https = true;
        rec.username = QStringLiteral("admin");
        const qint64 id = db.addHost(rec);
        QVERIFY2(id > 0, qPrintable(db.lastError()));

        QVector<HostRecord> hosts = db.hosts();
        QCOMPARE(hosts.size(), 1);
        QCOMPARE(hosts[0].name, QStringLiteral("Front Door"));
        QCOMPARE(hosts[0].https, true);

        hosts[0].name = QStringLiteral("Back Door");
        hosts[0].model = QStringLiteral("RLC-810A");
        QVERIFY(db.updateHost(hosts[0]));
        QCOMPARE(db.hosts()[0].name, QStringLiteral("Back Door"));
        QCOMPARE(db.hosts()[0].model, QStringLiteral("RLC-810A"));

        QVERIFY(db.removeHost(id));
        QCOMPARE(db.hosts().size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestDatabase)
#include "test_database.moc"
