#include "core/Database.h"
#include "device/EventHistory.h"
#include "ui/EventFeed.h"

#include <QDateTime>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

using namespace rl;
using namespace rl::device;

// One Events column (wayfinder/site-tabs-redesign, step 6): a camera's events for one local
// day, filtered by its own type row, with "app not running" bands among them.
class TestEventFeed : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Database> m_db;
    int m_n = 0;
    TimeSpan m_day, m_prevDay;

    static const QStringList kAll() { return {"person", "vehicle", "pet", "motion", "status"}; }

    void add(qint64 host, int ch, qint64 ts, const QString &type)
    {
        EventRecord e;
        e.hostId = host;
        e.channel = ch;
        e.timestamp = ts;
        e.type = type;
        e.camera = QStringLiteral("Kitchen");
        QVERIFY(m_db->addEvent(e) > 0);
    }
    // A column on host 1 channel 0, the test day, every type supported, with the whole
    // day covered by one app run unless a test says otherwise.
    void setUp(EventFeed &c, const QStringList &supported = kAll())
    {
        c.setDatabase(m_db.get());
        c.setNow(m_day.to + 3600);
        c.setSupportedTypes(supported);
        c.setCameraKey(QStringLiteral("1:0"));
        c.setDayStart(m_day.from);
    }
    QStringList typesOf(const EventFeed &c) const
    {
        QStringList out;
        for (int i = 0; i < c.rowCount(); ++i)
            out << c.data(c.index(i), EventFeed::KindRole).toString() + QLatin1Char(':')
                       + c.data(c.index(i), EventFeed::TypeRole).toString();
        return out;
    }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_db = std::make_unique<Database>(m_dir.filePath(QStringLiteral("t%1.db").arg(++m_n)),
                                          QStringLiteral("col%1").arg(m_n));
        QVERIFY(m_db->open());
        const QTimeZone zone = QTimeZone::systemTimeZone();
        m_day = localDay(QDate(2026, 11, 5), zone);
        m_prevDay = localDay(QDate(2026, 11, 4), zone);
        const qint64 run = m_db->beginAppRun(m_prevDay.from);
        QVERIFY(m_db->touchAppRun(run, m_day.to + 3600));
    }
    void cleanup() { m_db.reset(); }

    void defaultTypesAreAllButMotionAndStatus()
    {
        QCOMPARE(EventFeed::defaultTypes(kAll()), QStringList({"person", "vehicle", "pet"}));
        EventFeed c;
        setUp(c);
        QCOMPARE(c.enabledTypes(), QStringList({"person", "vehicle", "pet"}));
        QVERIFY(c.hasTypes());
    }

    // Each button shows the day's count for its type, on or off; Status counts both ways.
    void countsEveryTypeForTheDay()
    {
        add(1, 0, m_day.from + 100, "person");
        add(1, 0, m_day.from + 200, "person");
        add(1, 0, m_day.from + 300, "motion");
        add(1, 0, m_day.from + 400, "offline");
        add(1, 0, m_day.from + 500, "online");
        add(1, 1, m_day.from + 600, "person");   // another camera
        add(1, 0, m_prevDay.from + 10, "person"); // another day
        EventFeed c;
        setUp(c);
        const QVariantMap counts = c.counts();
        QCOMPARE(counts.value("person").toInt(), 2);
        QCOMPARE(counts.value("motion").toInt(), 1);
        QCOMPARE(counts.value("status").toInt(), 2);
        QCOMPARE(counts.value("vehicle").toInt(), 0);
        QCOMPARE(typesOf(c), QStringList({"event:person", "event:person"}));
    }

    void togglingATypeFiltersThatColumnOnly()
    {
        add(1, 0, m_day.from + 100, "person");
        add(1, 0, m_day.from + 200, "motion");
        add(1, 0, m_day.from + 300, "offline");
        EventFeed a, b;
        setUp(a);
        setUp(b);
        QSignalSpy toggled(&a, &EventFeed::typesToggled);
        a.toggle(QStringLiteral("motion"));
        QCOMPARE(toggled.count(), 1);
        QCOMPARE(typesOf(a), QStringList({"event:motion", "event:person"}));
        QCOMPARE(typesOf(b), QStringList({"event:person"}));
        a.toggle(QStringLiteral("status"));
        QCOMPARE(typesOf(a), QStringList({"event:offline", "event:motion", "event:person"}));
        a.toggle(QStringLiteral("person"));
        QCOMPARE(typesOf(a), QStringList({"event:offline", "event:motion"}));
        // A type the camera doesn't support can't be turned on.
        EventFeed c;
        setUp(c, {"person", "motion"});
        c.toggle(QStringLiteral("vehicle"));
        QCOMPARE(c.enabledTypes(), QStringList({"person"}));
    }

    // A remembered set is kept to what the camera supports now, in display order.
    void restoredTypesFollowSupport()
    {
        EventFeed c;
        setUp(c, {"person", "vehicle", "motion", "status"});
        c.restoreTypes({"motion", "pet", "person"});
        QCOMPARE(c.enabledTypes(), QStringList({"person", "motion"}));

        // Remembered before the camera's types are known (it answers late): kept once they are.
        EventFeed late;
        late.restoreTypes({"motion", "person"});
        QVERIFY(late.enabledTypes().isEmpty());
        late.setSupportedTypes({"person", "vehicle", "motion", "status"});
        QCOMPARE(late.enabledTypes(), QStringList({"person", "motion"}));
    }

    // Toggling keeps a remembered type the camera hasn't reported yet.
    void togglingKeepsTypesNotYetSupported()
    {
        EventFeed c;
        setUp(c, {"person", "motion"});
        c.restoreTypes({"person", "vehicle"});
        QSignalSpy toggled(&c, &EventFeed::typesToggled);
        c.toggle(QStringLiteral("motion"));
        QCOMPARE(toggled.first().first().toStringList(), QStringList({"person", "vehicle", "motion"}));
        c.setSupportedTypes({"person", "vehicle", "motion"});
        QCOMPARE(c.enabledTypes(), QStringList({"person", "vehicle", "motion"}));
    }

    // New events go in at the top without resetting the list, so a scrolled column stays put.
    void newEventsInsertWithoutReset()
    {
        add(1, 0, m_day.from + 100, "person");
        EventFeed c;
        setUp(c);
        QSignalSpy reset(&c, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&c, &QAbstractItemModel::rowsInserted);
        add(1, 0, m_day.from + 200, "person");
        add(1, 0, m_day.from + 300, "vehicle");
        c.refresh();
        QCOMPARE(reset.count(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(typesOf(c), QStringList({"event:vehicle", "event:person", "event:person"}));
        // Nothing new: nothing happens.
        c.refresh();
        QCOMPARE(reset.count(), 0);
        QCOMPARE(inserted.count(), 1);
    }

    void aCameraWithNoTypesSaysSo()
    {
        add(1, 0, m_day.from + 100, "motion");
        EventFeed c;
        setUp(c, {});
        QVERIFY(!c.hasTypes());
        QCOMPARE(c.rowCount(), 0);
    }

    void switchingDaysQueriesAgain()
    {
        add(1, 0, m_day.from + 100, "person");
        add(1, 0, m_prevDay.from + 100, "vehicle");
        add(1, 0, m_prevDay.from + 200, "vehicle");
        EventFeed c;
        setUp(c);
        QCOMPARE(typesOf(c), QStringList({"event:person"}));
        c.setDayStart(m_prevDay.from);
        QCOMPARE(typesOf(c), QStringList({"event:vehicle", "event:vehicle"}));
        QCOMPARE(c.counts().value("vehicle").toInt(), 2);
        QCOMPARE(c.counts().value("person").toInt(), 0);
        // A new event stored on the shown day appears on refresh.
        add(1, 0, m_prevDay.from + 300, "person");
        c.refresh();
        QCOMPARE(typesOf(c), QStringList({"event:person", "event:vehicle", "event:vehicle"}));
    }

    // The band sits where the app wasn't running, newest first among the events.
    void gapBandIsPlacedFromTheRuns()
    {
        // init's run covers everything; use a fresh store with runs 01:00–03:00 and
        // 05:00–(day end + 1 h): not running before 01:00 and from 03:00 to 05:00.
        cleanup();
        m_db = std::make_unique<Database>(m_dir.filePath(QStringLiteral("gap%1.db").arg(++m_n)),
                                          QStringLiteral("col%1").arg(m_n));
        QVERIFY(m_db->open());
        const qint64 h = 3600;
        // The first recorded run, the evening before.
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_prevDay.from + 20 * h), m_prevDay.from + 21 * h));
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_day.from + 1 * h), m_day.from + 3 * h));
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_day.from + 5 * h), m_day.to + h));
        add(1, 0, m_day.from + 2 * h, "person");
        add(1, 0, m_day.from + 6 * h, "vehicle");
        EventFeed c;
        setUp(c);
        QCOMPARE(typesOf(c), QStringList({"event:vehicle", "gap:", "event:person", "gap:"}));
        QCOMPARE(c.data(c.index(1), EventFeed::FromRole).toLongLong(), m_day.from + 3 * h);
        QCOMPARE(c.data(c.index(1), EventFeed::ToRole).toLongLong(), m_day.from + 5 * h);
        QCOMPARE(c.data(c.index(3), EventFeed::FromRole).toLongLong(), m_day.from);
        QCOMPARE(c.data(c.index(3), EventFeed::ToRole).toLongLong(), m_day.from + 1 * h);
        // Today: nothing after now is a gap.
        c.setNow(m_day.from + 4 * h);
        c.refresh();
        QCOMPARE(c.data(c.index(1), EventFeed::ToRole).toLongLong(), m_day.from + 4 * h);

        // Before the first recorded run (events from a build that kept no runs) is
        // unknown, so no band: the day before only has one from 21:00.
        add(1, 0, m_prevDay.from + 2 * h, "person");
        c.setDayStart(m_prevDay.from);
        QCOMPARE(typesOf(c), QStringList({"gap:", "event:person"}));
        QCOMPARE(c.data(c.index(0), EventFeed::FromRole).toLongLong(), m_prevDay.from + 21 * h);
    }

    // The column says when the day is past what is kept.
    void notesADayNotKept()
    {
        EventFeed c;
        setUp(c);
        QVERIFY(c.keptNote().isEmpty());
        c.toggle(QStringLiteral("motion"));
        c.setNow(m_day.to + kMotionKeepSecs + 10);
        c.refresh();
        QVERIFY(c.keptNote().contains(QStringLiteral("24 hours")));
        c.setNow(m_day.to + kOtherKeepSecs + 10);
        c.refresh();
        QVERIFY(c.keptNote().contains(QStringLiteral("30 days")));
    }

    // Today's counts by recorder and type, for the nothing-selected view.
    void countsByHostForADay()
    {
        add(1, 0, m_day.from + 1, "person");
        add(1, 3, m_day.from + 2, "person");
        add(1, 3, m_day.from + 3, "offline");
        add(2, 0, m_day.from + 4, "motion");
        add(2, 0, m_prevDay.from + 4, "motion");
        const auto counts = m_db->eventCountsByHost(m_day.from, m_day.to);
        QCOMPARE(counts.value(1).value("person"), 2);
        QCOMPARE(counts.value(1).value("offline"), 1);
        QCOMPARE(counts.value(2).value("motion"), 1);
        QCOMPARE(counts.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestEventFeed)
#include "test_eventfeed.moc"
