#include "core/Database.h"
#include "device/EventSwitches.h"
#include "ui/EventCameraList.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;

// The Events page's camera list (wayfinder/events-feed step 18): the selected Site's
// cameras, the most recently active first; cameras that can only log Status now go last.
class TestEventCameraList : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
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
    static QVariantMap camera(const char *key, const char *name, const QStringList &types)
    {
        return {{"key", key}, {"name", name}, {"types", types}};
    }
    static QStringList keys(const EventCameraList &l)
    {
        QStringList out;
        for (int i = 0; i < l.rowCount(); ++i)
            out << l.data(l.index(i), EventCameraList::KeyRole).toString();
        return out;
    }
    static const QStringList kReolink() { return {"person", "vehicle", "pet", "motion", "status"}; }

private slots:
    void init()
    {
        m_db = std::make_unique<Database>(m_dir.filePath(QStringLiteral("t%1.db").arg(++m_n)),
                                          QStringLiteral("cams%1").arg(m_n));
        QVERIFY(m_db->open());
    }
    void cleanup() { m_db.reset(); }

    void mostRecentlyActiveFirstAndStatusOnlyLast()
    {
        EventSwitches switches(m_db.get());
        // Gate: all its types switched off but Status, so it can only log Status now.
        QVERIFY(switches.setOff(1, 5, QStringLiteral("person"), true));
        QVERIFY(switches.setOff(1, 5, QStringLiteral("motion"), true));
        add(1, 0, 1000, "person");   // Driveway: older
        add(1, 7, 2000, "pet");      // Patio: newest
        add(1, 7, 2500, "online");   // Status doesn't count as activity
        add(1, 2, 3000, "offline");  // Entrance: Status only, so no activity
        add(1, 5, 4000, "person");   // Gate: stored before it was switched off
        add(2, 0, 5000, "person");   // another recorder, not in this list
        EventCameraList l;
        l.setDatabase(m_db.get());
        l.setSwitches(&switches);
        l.setCameras({camera("1:0", "Driveway", kReolink()), camera("1:2", "Entrance", kReolink()),
                      camera("1:5", "Gate", {"person", "motion", "status"}),
                      camera("1:7", "Patio", kReolink()),
                      camera("1:9", "Hall", {"motion", "status"})});
        // Hall has Motion on: it can log, it just hasn't. Gate can only log Status.
        QCOMPARE(keys(l), QStringList({"1:7", "1:0", "1:2", "1:9", "1:5"}));
        QCOMPARE(l.data(l.index(0), EventCameraList::LastRole).toLongLong(), 2000);
        QCOMPARE(l.data(l.index(0), EventCameraList::NameRole).toString(), QStringLiteral("Patio"));
        QCOMPARE(l.data(l.index(2), EventCameraList::LastRole).toLongLong(), 0);
        QVERIFY(!l.data(l.index(3), EventCameraList::StatusOnlyRole).toBool());
        QVERIFY(l.data(l.index(4), EventCameraList::StatusOnlyRole).toBool());
        QCOMPARE(l.first(), QStringLiteral("1:7"));
    }

    void reordersWhenEventsOrSwitchesChange()
    {
        EventSwitches switches(m_db.get());
        add(1, 0, 1000, "person");
        add(1, 7, 2000, "pet");
        EventCameraList l;
        l.setDatabase(m_db.get());
        l.setSwitches(&switches);
        l.setCameras({camera("1:0", "Driveway", kReolink()), camera("1:7", "Patio", kReolink())});
        QCOMPARE(keys(l), QStringList({"1:7", "1:0"}));
        add(1, 0, 3000, "vehicle");
        l.refresh();
        QCOMPARE(keys(l), QStringList({"1:0", "1:7"}));
        // Patio switched off down to Status only: it goes last at once.
        for (const char *t : {"person", "vehicle", "pet", "motion"})
            QVERIFY(switches.setOff(1, 7, QString::fromLatin1(t), true));
        add(1, 7, 4000, "pet"); // stored before the switch, say
        l.refresh();
        QCOMPARE(keys(l), QStringList({"1:0", "1:7"}));
        QVERIFY(l.data(l.index(1), EventCameraList::StatusOnlyRole).toBool());
    }

    void emptyWithoutCameras()
    {
        EventCameraList l;
        l.setDatabase(m_db.get());
        QCOMPARE(l.rowCount(), 0);
        QVERIFY(l.first().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestEventCameraList)
#include "test_eventcameralist.moc"
