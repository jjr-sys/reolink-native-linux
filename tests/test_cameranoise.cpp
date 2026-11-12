#include "core/CredentialStore.h"
#include "core/Database.h"
#include "device/DeviceManager.h"
#include "device/EventManager.h"
#include "device/EventSwitches.h"
#include "ui/CameraNoise.h"

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;

// The switch controls in a camera's feed (wayfinder/events-feed step 21): a noise card
// for each type logging 1,000+ a day (per day of app running over the last 24 h), Keep
// logging remembered per camera and type, and the types switched off there.
class TestCameraNoise : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_path;
    std::unique_ptr<Database> m_db;
    int m_n = 0;
    const qint64 m_now = 1'800'000'000;

    QString conn() const { return QStringLiteral("noise%1").arg(m_n); }
    // `n` events of a type spread over the `secs` before now, in one transaction.
    void addMany(qint64 host, int ch, const QString &type, int n, qint64 secs = 86400,
                 const QString &thumbPrefix = {})
    {
        QSqlDatabase db = QSqlDatabase::database(conn());
        QVERIFY(db.transaction());
        for (int i = 0; i < n; ++i) {
            EventRecord e;
            e.hostId = host;
            e.channel = ch;
            e.timestamp = m_now - 1 - qint64(i) * (secs - 2) / qMax(1, n);
            e.type = type;
            e.camera = QStringLiteral("Driveway");
            if (!thumbPrefix.isEmpty())
                e.thumbnail = thumbPrefix + QString::number(i) + QStringLiteral(".jpg");
            QVERIFY(m_db->addEvent(e) > 0);
        }
        QVERIFY(db.commit());
    }
    void ran(qint64 from, qint64 to)
    {
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(from), to));
    }
    void setUp(CameraNoise &c, const QString &key = QStringLiteral("1:0"))
    {
        c.setDatabase(m_db.get());
        c.setNow(m_now);
        c.setCameraKey(key);
        c.refresh();
    }
    static QStringList cardTypes(const CameraNoise &c)
    {
        QStringList out;
        for (const QVariant &v : c.cards())
            out << v.toMap().value("type").toString();
        return out;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("rl-test-cameranoise"));
        QCoreApplication::setApplicationName(QStringLiteral("rl-test-cameranoise"));
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-cameranoise-test-bus");
    }
    void init()
    {
        m_path = m_dir.filePath(QStringLiteral("t%1.db").arg(++m_n));
        m_db = std::make_unique<Database>(m_path, conn());
        QVERIFY(m_db->open());
    }
    void cleanup() { m_db.reset(); }

    void aCardAtAThousandADay()
    {
        ran(m_now - 3 * 86400, m_now);
        addMany(1, 0, "vehicle", 1000, 86400, "/snap/v");
        addMany(1, 0, "person", 999);
        addMany(1, 0, "vehicle", 50, 86400 * 2); // older than a day: some fall outside
        addMany(1, 1, "person", 1200);            // another camera
        CameraNoise c;
        setUp(c);
        QCOMPARE(cardTypes(c), QStringList({"vehicle"}));
        const QVariantMap card = c.cards().first().toMap();
        QVERIFY(card.value("perDay").toInt() >= 1000);
        QVERIFY(card.value("perDay").toInt() < 1030);
        QCOMPARE(card.value("everySecs").toInt(), 86400 / card.value("perDay").toInt());
        // The four newest snapshots of that type.
        QCOMPARE(card.value("thumbnails").toStringList(),
                 QStringList({"/snap/v0.jpg", "/snap/v1.jpg", "/snap/v2.jpg", "/snap/v3.jpg"}));
    }

    // Per day of app running: 600 in the 12 h the app ran is 1,200 a day.
    void theRateCountsOnlyTimeTheAppRan()
    {
        ran(m_now - 3 * 86400, m_now - 86400); // ran before, then not for 12 h
        ran(m_now - 12 * 3600, m_now);
        addMany(1, 0, "pet", 600, 12 * 3600);
        CameraNoise c;
        setUp(c);
        QCOMPARE(cardTypes(c), QStringList({"pet"}));
        const int perDay = c.cards().first().toMap().value("perDay").toInt();
        QVERIFY(perDay >= 1150 && perDay <= 1250);
        // The confirmation's rate, for any type: the same measure; 0 when it logged none.
        QCOMPARE(c.perDay(QStringLiteral("pet")), perDay);
        QCOMPARE(c.perDay(QStringLiteral("dog_cat")), perDay); // filed as Pet
        QCOMPARE(c.perDay(QStringLiteral("vehicle")), 0);
    }

    // Under an hour of running says nothing yet: a burst isn't a daily rate.
    void underAnHourOfRunningShowsNoCard()
    {
        ran(m_now - 3 * 86400, m_now - 86400);
        ran(m_now - 1800, m_now);
        addMany(1, 0, "person", 200, 1800); // 9,600 a day if it were a rate
        CameraNoise c;
        setUp(c);
        QVERIFY(c.cards().isEmpty());
        QCOMPARE(c.perDay(QStringLiteral("person")), -1); // not known yet
    }

    // A fresh install: only the time since the first recorded run counts, so 2 h of
    // running with 200 events is 2,400 a day, and 30 min isn't enough to say.
    void aFreshInstallCountsOnlySinceItsFirstRun()
    {
        ran(m_now - 2 * 3600, m_now);
        addMany(1, 0, "person", 200, 2 * 3600);
        CameraNoise c;
        setUp(c);
        QCOMPARE(cardTypes(c), QStringList({"person"}));
        const int perDay = c.perDay(QStringLiteral("person"));
        QVERIFY(perDay >= 2350 && perDay <= 2450);

        cleanup();
        init();
        ran(m_now - 1800, m_now);
        addMany(1, 0, "person", 200, 1800);
        CameraNoise fresh;
        setUp(fresh);
        QVERIFY(fresh.cards().isEmpty());
        QCOMPARE(fresh.perDay(QStringLiteral("person")), -1);
    }

    void keepLoggingIsRemembered()
    {
        ran(m_now - 86400, m_now);
        addMany(1, 0, "vehicle", 1500);
        addMany(1, 0, "person", 1500);
        addMany(2, 0, "vehicle", 1500);
        {
            CameraNoise c;
            setUp(c);
            QCOMPARE(cardTypes(c), QStringList({"person", "vehicle"}));
            QSignalSpy spy(&c, &CameraNoise::refreshed);
            c.keepLogging(QStringLiteral("vehicle"));
            QVERIFY(spy.count() >= 1);
            QCOMPARE(cardTypes(c), QStringList({"person"}));
        }
        m_db.reset();
        m_db = std::make_unique<Database>(m_path, conn() + QStringLiteral("b"));
        QVERIFY(m_db->open());
        CameraNoise c;
        setUp(c);
        QCOMPARE(cardTypes(c), QStringList({"person"}));
        CameraNoise other; // the same type on another camera still has its card
        setUp(other, QStringLiteral("2:0"));
        QCOMPARE(cardTypes(other), QStringList({"vehicle"}));
    }

    // What is off on this camera, in the type order; Status is never off.
    void listsTheTypesSwitchedOffHere()
    {
        ran(m_now - 86400, m_now);
        addMany(1, 0, "person", 1500);
        EventSwitches switches(m_db.get());
        QVERIFY(switches.setOff(1, 0, QStringLiteral("motion"), true));
        QVERIFY(switches.setOff(1, 0, QStringLiteral("person"), true));
        QVERIFY(switches.setOff(1, 3, QStringLiteral("vehicle"), true)); // another camera
        CameraNoise c;
        c.setSwitches(&switches);
        setUp(c);
        QCOMPARE(c.offTypes(), QStringList({"person", "motion"}));
        // A switched-off type has no card (its rows would be gone anyway).
        QVERIFY(c.cards().isEmpty());
    }

    // Turn on: the feed's line follows the switch and the pair logs again.
    void turningOnResumesLogging()
    {
        CredentialStore credentials;
        DeviceManager devices(m_db.get(), &credentials);
        EventManager events(m_db.get(), &devices);
        QVERIFY(events.switchOff(7, 1, QStringLiteral("vehicle")));
        CameraNoise c;
        c.setEvents(&events);
        c.setNow(m_now);
        c.setCameraKey(QStringLiteral("7:1"));
        c.refresh();
        QCOMPARE(c.offTypes(), QStringList({"vehicle"}));

        QVERIFY(events.switchOn(7, 1, QStringLiteral("vehicle")));
        QVERIFY(c.offTypes().isEmpty()); // the switch change re-reads at once
        const int before = events.rowCount();
        devices.reportDetection(7, 1, QStringLiteral("vehicle"), QStringLiteral("Gate"));
        QCOMPARE(events.rowCount(), before + 1);
    }
};

QTEST_MAIN(TestCameraNoise)
#include "test_cameranoise.moc"
