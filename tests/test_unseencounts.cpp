#include "core/Database.h"
#include "core/LocalSeenStore.h"
#include "device/EventSwitches.h"
#include "device/UnseenCounts.h"

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;

// Unseen counts (wayfinder/events-feed step 20): per camera, its logged Person, Vehicle,
// Pet and Visitor events after it was last opened on this desktop. Opening it marks them
// seen; the times live in the app's database and survive a restart.
class TestUnseenCounts : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_path;
    std::unique_ptr<Database> m_db;
    int m_n = 0;

    void add(qint64 host, int ch, qint64 ts, const QString &type)
    {
        EventRecord e;
        e.hostId = host;
        e.channel = ch;
        e.timestamp = ts;
        e.type = type;
        e.camera = QStringLiteral("Cam");
        QVERIFY(m_db->addEvent(e) > 0);
    }
    void reopen()
    {
        m_db.reset();
        m_db = std::make_unique<Database>(m_path, QStringLiteral("unseen%1b").arg(m_n));
        QVERIFY(m_db->open());
    }

private slots:
    void init()
    {
        m_path = m_dir.filePath(QStringLiteral("t%1.db").arg(++m_n));
        m_db = std::make_unique<Database>(m_path, QStringLiteral("unseen%1").arg(m_n));
        QVERIFY(m_db->open());
    }
    void cleanup() { m_db.reset(); }

    void countsPerTypeAfterLastOpened()
    {
        LocalSeenStore store(m_db.get());
        QVERIFY(store.markSeen(1, 0, 1000));
        add(1, 0, 900, "person");    // before: seen
        add(1, 0, 1000, "person");   // at the time: seen
        add(1, 0, 1001, "person");
        add(1, 0, 1002, "person");
        add(1, 0, 1003, "vehicle");
        add(1, 0, 1004, "pet");
        add(1, 0, 1005, "visitor");
        add(1, 0, 1006, "motion");   // not counted
        add(1, 0, 1007, "offline");  // Status: not counted
        add(2, 3, 1008, "person");   // another camera, never opened: counts all

        UnseenCounts unseen;
        unseen.setDatabase(m_db.get());
        unseen.setStore(&store);
        unseen.refresh();
        const QVariantMap c = unseen.countsFor(QStringLiteral("1:0"));
        QCOMPARE(c.value("person").toInt(), 2);
        QCOMPARE(c.value("vehicle").toInt(), 1);
        QCOMPARE(c.value("pet").toInt(), 1);
        QCOMPARE(c.value("visitor").toInt(), 1);
        QVERIFY(!c.contains("motion"));
        QVERIFY(!c.contains("offline"));
        QCOMPARE(unseen.countsFor(QStringLiteral("2:3")).value("person").toInt(), 1);
        QVERIFY(unseen.countsFor(QStringLiteral("9:9")).isEmpty());
        QCOMPARE(unseen.total(), 6);
        QCOMPARE(unseen.totalForHosts({1}), 5);
        QCOMPARE(unseen.totalForHosts({2, 7}), 1);
        QCOMPARE(unseen.totalForHosts({}), 0);
    }

    void openingClearsAndKeepsTheLine()
    {
        LocalSeenStore store(m_db.get());
        QVERIFY(store.markSeen(1, 0, 1000));
        add(1, 0, 1500, "person");
        UnseenCounts unseen;
        unseen.setDatabase(m_db.get());
        unseen.setStore(&store);
        unseen.setNow(2000);
        unseen.refresh();
        QCOMPARE(unseen.total(), 1);

        unseen.setOpenCamera(QStringLiteral("1:0"));
        QCOMPARE(unseen.seenLine(), qint64(1000)); // where the person last looked
        QCOMPARE(store.seenAt(1, 0), qint64(2000));
        unseen.refresh();
        QCOMPARE(unseen.total(), 0);
        QVERIFY(unseen.countsFor(QStringLiteral("1:0")).isEmpty());

        // Arriving while it is open: seen, and the line stays put.
        unseen.setNow(3000);
        add(1, 0, 2500, "vehicle");
        unseen.refresh();
        QCOMPARE(unseen.total(), 0);
        QCOMPARE(unseen.seenLine(), qint64(1000));
        QCOMPARE(store.seenAt(1, 0), qint64(3000));

        // A recorder clock ahead of the desktop still counts as seen while open.
        add(1, 0, 3600, "person");
        unseen.refresh();
        QCOMPARE(unseen.total(), 0);
        QVERIFY(store.seenAt(1, 0) >= 3600);

        // Leaving marks it seen up to that moment, even before a refresh.
        add(1, 0, 3700, "pet");
        unseen.setNow(3800);
        unseen.setOpenCamera(QString());
        QCOMPARE(store.seenAt(1, 0), qint64(3800));
        QCOMPARE(unseen.seenLine(), qint64(-1));
        unseen.refresh();
        QCOMPARE(unseen.total(), 0);

        // Closed: new events count again; reopening moves the line.
        add(1, 0, 4000, "pet");
        unseen.refresh();
        QCOMPARE(unseen.total(), 1);
        unseen.setNow(5000);
        unseen.setOpenCamera(QStringLiteral("1:0"));
        QCOMPARE(unseen.seenLine(), qint64(3800));
        // A camera never opened: its line is 0 (everything new).
        unseen.setOpenCamera(QStringLiteral("2:3"));
        QCOMPARE(unseen.seenLine(), qint64(0));
    }

    void changesAreSignalled()
    {
        LocalSeenStore store(m_db.get());
        UnseenCounts unseen;
        unseen.setDatabase(m_db.get());
        unseen.setStore(&store);
        unseen.refresh();
        QSignalSpy spy(&unseen, &UnseenCounts::changed);
        add(1, 0, 100, "person");
        unseen.refresh();
        QCOMPARE(spy.count(), 1);
        unseen.refresh(); // nothing new: quiet
        QCOMPARE(spy.count(), 1);
    }

    void switchedOffPairsNotCounted()
    {
        LocalSeenStore store(m_db.get());
        EventSwitches switches(m_db.get());
        add(1, 0, 100, "person");
        add(1, 0, 101, "vehicle");
        // Rows left behind (switching off deletes them; a pet filed as dog_cat stays off).
        QVERIFY(switches.setOff(1, 0, QStringLiteral("vehicle"), true));
        UnseenCounts unseen;
        unseen.setDatabase(m_db.get());
        unseen.setStore(&store);
        unseen.setSwitches(&switches);
        unseen.refresh();
        const QVariantMap c = unseen.countsFor(QStringLiteral("1:0"));
        QCOMPARE(c.value("person").toInt(), 1);
        QVERIFY(!c.contains("vehicle"));
        QCOMPARE(unseen.total(), 1);
    }

    void survivesRestart()
    {
        {
            LocalSeenStore store(m_db.get());
            QVERIFY(store.markSeen(1, 0, 1000));
            QVERIFY(store.markSeen(1, 0, 1200)); // replaces
        }
        add(1, 0, 1100, "person");
        add(1, 0, 1300, "person");
        reopen();
        LocalSeenStore store(m_db.get());
        QCOMPARE(store.seenAt(1, 0), qint64(1200));
        QCOMPARE(store.seenAt(1, 1), qint64(0));
        UnseenCounts unseen;
        unseen.setDatabase(m_db.get());
        unseen.setStore(&store);
        unseen.refresh();
        QCOMPARE(unseen.countsFor(QStringLiteral("1:0")).value("person").toInt(), 1);
    }

    // First launch with this step: every camera already known starts seen, so nothing
    // is unseen until new events arrive. (A camera added later has no row, which counts
    // everything it logs: the same as starting it at first sight.)
    void upgradeStartsEveryKnownCameraSeen()
    {
        add(1, 0, 100, "person");
        add(2, 4, 200, "vehicle");
        const qint64 ahead = QDateTime::currentSecsSinceEpoch() + 3600; // a fast recorder clock
        add(4, 0, ahead, "person");
        {
            QSqlQuery q(QSqlDatabase::database(QStringLiteral("unseen%1").arg(m_n)));
            QVERIFY(q.exec("DROP TABLE camera_seen"));
            QVERIFY(q.exec("UPDATE schema_version SET version = 4"));
            QVERIFY(q.exec("INSERT INTO hosts (id, kind, name, addr) VALUES (3, 'nvr', 'N', 'x')"));
            QVERIFY(q.exec("INSERT INTO channels (host_id, idx, name) VALUES (3, 1, 'Gate')"));
        }
        reopen();
        QVERIFY(m_db->schemaVersion() >= 5);
        LocalSeenStore store(m_db.get());
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVERIFY(store.seenAt(1, 0) >= now - 5);
        QVERIFY(store.seenAt(2, 4) >= now - 5);
        QVERIFY(store.seenAt(3, 1) >= now - 5);
        QCOMPARE(store.seenAt(4, 0), ahead);
        UnseenCounts unseen;
        unseen.setDatabase(m_db.get());
        unseen.setStore(&store);
        unseen.refresh();
        QCOMPARE(unseen.total(), 0);
    }
};

QTEST_GUILESS_MAIN(TestUnseenCounts)
#include "test_unseencounts.moc"
