#include "core/Database.h"
#include "device/EventHistory.h"
#include "ui/EventFeed.h"

#include <QDateTime>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <ctime>

using namespace rl;
using namespace rl::device;

// One camera's Events feed (wayfinder/events-feed step 18): everything kept (30 days) for
// one camera, newest first, filtered by its type chips, each row keyed by its local day for
// the sticky day headers, with "app not running" bands among the events.
class TestEventFeed : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Database> m_db;
    int m_n = 0;
    TimeSpan m_day, m_prevDay;
    qint64 m_now = 0; // an hour after the end of m_day

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
    // An event with a snapshot of the given pixel size (an empty size: a path to nothing).
    void addSnap(qint64 host, int ch, qint64 ts, QSize size)
    {
        const QString path = m_dir.filePath(QStringLiteral("snap%1_%2.jpg").arg(++m_snaps).arg(ts));
        if (!size.isEmpty()) {
            QImage img(size, QImage::Format_RGB32);
            img.fill(Qt::gray);
            QVERIFY(img.save(path, "JPG"));
        }
        EventRecord e;
        e.hostId = host;
        e.channel = ch;
        e.timestamp = ts;
        e.type = QStringLiteral("person");
        e.camera = QStringLiteral("Kitchen");
        e.thumbnail = path;
        QVERIFY(m_db->addEvent(e) > 0);
    }
    int m_snaps = 0;
    // A feed of host 1 channel 0, every type supported, the clock an hour after the test
    // day, with one app run covering everything unless a test says otherwise.
    void setUp(EventFeed &f, const QStringList &supported = kAll())
    {
        f.setDatabase(m_db.get());
        f.setNow(m_now);
        f.setSupportedTypes(supported);
        f.setCameraKey(QStringLiteral("1:0"));
    }
    static QStringList rows(const EventFeed &f, int role = EventFeed::TypeRole)
    {
        QStringList out;
        for (int i = 0; i < f.rowCount(); ++i)
            out << f.data(f.index(i), EventFeed::KindRole).toString() + QLatin1Char(':')
                       + f.data(f.index(i), role).toString();
        return out;
    }
    static QStringList days(const EventFeed &f)
    {
        QStringList out;
        for (int i = 0; i < f.rowCount(); ++i)
            out << f.data(f.index(i), EventFeed::DayRole).toString();
        return out;
    }

private slots:
    void initTestCase()
    {
        // Day keys follow local time; Sydney has a daylight-saving change to test against.
        qputenv("TZ", "Australia/Sydney");
        tzset();
    }

    void init()
    {
        QVERIFY(m_dir.isValid());
        m_db = std::make_unique<Database>(m_dir.filePath(QStringLiteral("t%1.db").arg(++m_n)),
                                          QStringLiteral("feed%1").arg(m_n));
        QVERIFY(m_db->open());
        const QTimeZone zone = QTimeZone::systemTimeZone();
        m_day = localDay(QDate(2026, 11, 5), zone);
        m_prevDay = localDay(QDate(2026, 11, 4), zone);
        m_now = m_day.to + 3600;
        const qint64 run = m_db->beginAppRun(m_now - kOtherKeepSecs - 86400);
        QVERIFY(m_db->touchAppRun(run, m_now));
    }
    void cleanup() { m_db.reset(); }

    void defaultTypesAreAllButMotionAndStatus()
    {
        QCOMPARE(EventFeed::defaultTypes(kAll()), QStringList({"person", "vehicle", "pet"}));
        // A camera that can only log Status (and Motion, off by default) shows Status.
        QCOMPARE(EventFeed::defaultTypes({"motion", "status"}), QStringList({"status"}));
        QCOMPARE(EventFeed::defaultTypes({"status"}), QStringList({"status"}));
        QCOMPARE(EventFeed::defaultTypes({"motion"}), QStringList());
        EventFeed f;
        setUp(f);
        QCOMPARE(f.enabledTypes(), QStringList({"person", "vehicle", "pet"}));
        QVERIFY(f.hasTypes());
    }

    // One camera only, everything kept, newest first: not one day.
    void oneCameraOverTheWholeKeptWindow()
    {
        add(1, 0, m_day.from + 100, "person");
        add(1, 0, m_prevDay.from + 100, "vehicle");
        add(1, 0, m_now - kOtherKeepSecs + 60, "pet");   // just inside what is kept
        add(1, 0, m_now - kOtherKeepSecs - 60, "person"); // just outside
        add(1, 1, m_day.from + 200, "person");            // another camera
        add(2, 0, m_day.from + 300, "person");            // another recorder
        EventFeed f;
        setUp(f);
        QCOMPARE(rows(f), QStringList({"event:person", "event:vehicle", "event:pet"}));
    }

    // Each type chip shows the kept window's count for its type, on or off; Status counts
    // both ways.
    void countsEveryTypeOverTheWindow()
    {
        add(1, 0, m_day.from + 100, "person");
        add(1, 0, m_prevDay.from + 200, "person");
        add(1, 0, m_day.from + 300, "motion");
        add(1, 0, m_day.from + 400, "offline");
        add(1, 0, m_day.from + 500, "online");
        add(1, 1, m_day.from + 600, "person"); // another camera
        EventFeed f;
        setUp(f);
        const QVariantMap counts = f.counts();
        QCOMPARE(counts.value("person").toInt(), 2);
        QCOMPARE(counts.value("motion").toInt(), 1);
        QCOMPARE(counts.value("status").toInt(), 2);
        QCOMPARE(counts.value("vehicle").toInt(), 0);
        QCOMPARE(rows(f), QStringList({"event:person", "event:person"}));
    }

    // Each row carries its local day, for the sticky headers; midnight splits them.
    void rowsAreKeyedByLocalDay()
    {
        add(1, 0, m_day.from + 1, "person");     // 00:00:01 on the 5th
        add(1, 0, m_day.from - 1, "person");     // 23:59:59 on the 4th
        add(1, 0, m_prevDay.from + 3600, "vehicle");
        EventFeed f;
        setUp(f);
        QCOMPARE(days(f), QStringList({"2026-11-05", "2026-11-04", "2026-11-04"}));
    }

    // Sydney's clocks went forward at 02:00 on 4 October 2026: that day is 23 hours long,
    // and an event either side of the change is still on the same day.
    void dayKeysFollowDaylightSaving()
    {
        const QTimeZone zone = QTimeZone::systemTimeZone();
        const QDateTime before(QDate(2026, 10, 4), QTime(1, 30), zone);
        const QDateTime after(QDate(2026, 10, 4), QTime(23, 30), zone);
        const QDateTime nextDay(QDate(2026, 10, 5), QTime(0, 30), zone);
        m_now = nextDay.toSecsSinceEpoch() + 3600;
        add(1, 0, before.toSecsSinceEpoch(), "person");
        add(1, 0, after.toSecsSinceEpoch(), "person");
        add(1, 0, nextDay.toSecsSinceEpoch(), "person");
        EventFeed f;
        setUp(f);
        QCOMPARE(days(f), QStringList({"2026-10-05", "2026-10-04", "2026-10-04"}));
    }

    void togglingATypeFiltersThatFeedOnly()
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
        QCOMPARE(rows(a), QStringList({"event:motion", "event:person"}));
        QCOMPARE(rows(b), QStringList({"event:person"}));
        a.toggle(QStringLiteral("status"));
        QCOMPARE(rows(a), QStringList({"event:offline", "event:motion", "event:person"}));
        a.toggle(QStringLiteral("person"));
        QCOMPARE(rows(a), QStringList({"event:offline", "event:motion"}));
        // A type the camera doesn't support can't be turned on.
        EventFeed c;
        setUp(c, {"person", "motion"});
        c.toggle(QStringLiteral("vehicle"));
        QCOMPARE(c.enabledTypes(), QStringList({"person"}));
    }

    // A remembered set is kept to what the camera supports now, in display order.
    void restoredTypesFollowSupport()
    {
        EventFeed f;
        setUp(f, {"person", "vehicle", "motion", "status"});
        f.restoreTypes({"motion", "pet", "person"});
        QCOMPARE(f.enabledTypes(), QStringList({"person", "motion"}));

        // Remembered before the camera's types are known (it answers late): kept once they are.
        EventFeed late;
        late.restoreTypes({"motion", "person"});
        QVERIFY(late.enabledTypes().isEmpty());
        late.setSupportedTypes({"person", "vehicle", "motion", "status"});
        QCOMPARE(late.enabledTypes(), QStringList({"person", "motion"}));
    }

    // Another camera with nothing remembered goes back to the defaults, and keeps
    // following them as its types become known.
    void useDefaultTypesFollowsSupport()
    {
        EventFeed f;
        setUp(f, {"person", "motion"});
        f.restoreTypes({"motion"});
        f.useDefaultTypes();
        QCOMPARE(f.enabledTypes(), QStringList({"person"}));
        f.setSupportedTypes({"person", "vehicle", "motion", "status"});
        QCOMPARE(f.enabledTypes(), QStringList({"person", "vehicle"}));
    }

    // Toggling keeps a remembered type the camera hasn't reported yet.
    void togglingKeepsTypesNotYetSupported()
    {
        EventFeed f;
        setUp(f, {"person", "motion"});
        f.restoreTypes({"person", "vehicle"});
        QSignalSpy toggled(&f, &EventFeed::typesToggled);
        f.toggle(QStringLiteral("motion"));
        QCOMPARE(toggled.first().first().toStringList(), QStringList({"person", "vehicle", "motion"}));
        f.setSupportedTypes({"person", "vehicle", "motion"});
        QCOMPARE(f.enabledTypes(), QStringList({"person", "vehicle", "motion"}));
    }

    // New events go in at the top without resetting the list, so a scrolled feed stays put.
    void newEventsInsertWithoutReset()
    {
        add(1, 0, m_day.from + 100, "person");
        EventFeed f;
        setUp(f);
        QSignalSpy reset(&f, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&f, &QAbstractItemModel::rowsInserted);
        add(1, 0, m_day.from + 200, "person");
        add(1, 0, m_day.from + 300, "vehicle");
        f.refresh();
        QCOMPARE(reset.count(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(rows(f), QStringList({"event:vehicle", "event:person", "event:person"}));
        // Nothing new: nothing happens.
        f.refresh();
        QCOMPARE(reset.count(), 0);
        QCOMPARE(inserted.count(), 1);
    }

    // Unseen counts (step 20): events after the time the camera had been seen up to are
    // new (blue dot); the "Seen up to here" line sits on the first row at or before it,
    // and the date bar's band covers the rows above (newUntilRow).
    void seenLineMarksNewRows()
    {
        add(1, 0, m_day.from + 100, "person");
        add(1, 0, m_day.from + 200, "vehicle");
        add(1, 0, m_day.from + 300, "pet");
        add(1, 0, m_day.from + 400, "person");
        EventFeed f;
        setUp(f);
        QCOMPARE(f.newUntilRow(), 0);
        QCOMPARE(rows(f, EventFeed::NewRole),
                 QStringList({"event:false", "event:false", "event:false", "event:false"}));

        QSignalSpy changed(&f, &QAbstractItemModel::dataChanged);
        f.setSeenLine(m_day.from + 200); // at the vehicle: it was seen
        QVERIFY(changed.count() > 0);
        QCOMPARE(f.newUntilRow(), 2);
        QCOMPARE(rows(f, EventFeed::NewRole),
                 QStringList({"event:true", "event:true", "event:false", "event:false"}));
        QCOMPARE(rows(f, EventFeed::SeenLineRole),
                 QStringList({"event:false", "event:false", "event:true", "event:false"}));

        // A new event slides in at the top: new, and the line stays on the same event.
        add(1, 0, m_day.from + 500, "vehicle");
        f.refresh();
        QCOMPARE(f.newUntilRow(), 3);
        QCOMPARE(rows(f, EventFeed::SeenLineRole),
                 QStringList({"event:false", "event:false", "event:false", "event:true",
                              "event:false"}));

        // Everything new: the band covers the whole feed, with no line to draw.
        f.setSeenLine(m_day.from + 50);
        QCOMPARE(f.newUntilRow(), 5);
        QVERIFY(!rows(f, EventFeed::SeenLineRole).contains("event:true"));
        // The feed shrinking under an all-new band (a chip turned off) stays in range.
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
        f.toggle(QStringLiteral("person"));
        QCOMPARE(f.newUntilRow(), 3);
        f.toggle(QStringLiteral("person"));
        QCOMPARE(f.newUntilRow(), 5);
        // Nothing new: no band, no line.
        f.setSeenLine(m_day.from + 900);
        QCOMPARE(f.newUntilRow(), 0);
        QVERIFY(!rows(f, EventFeed::SeenLineRole).contains("event:true"));
        // Never opened: everything is new, with no line. None open: nothing marked.
        f.setSeenLine(0);
        QCOMPARE(f.newUntilRow(), 5);
        QVERIFY(!rows(f, EventFeed::SeenLineRole).contains("event:true"));
        f.setSeenLine(-1);
        QCOMPARE(f.newUntilRow(), 0);
    }

    // ↑/↓ in the clip player (step 22): the next newer/older event that has a clip,
    // skipping "app not running" bands and Status lines; an event is found by its id.
    void steppingSkipsGapsAndStatus()
    {
        cleanup();
        m_db = std::make_unique<Database>(m_dir.filePath(QStringLiteral("step%1.db").arg(++m_n)),
                                          QStringLiteral("feed%1").arg(m_n));
        QVERIFY(m_db->open());
        const qint64 h = 3600;
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_now - kOtherKeepSecs - 86400), m_day.from + 3 * h));
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_day.from + 5 * h), m_now));
        add(1, 0, m_day.from + 1 * h, "person");
        add(1, 0, m_day.from + 2 * h, "vehicle");
        add(1, 0, m_day.from + 6 * h, "offline");
        add(1, 0, m_day.from + 7 * h, "pet");
        EventFeed f;
        setUp(f);
        f.restoreTypes({"person", "vehicle", "pet", "status"});
        QCOMPARE(rows(f), QStringList({"event:pet", "event:offline", "gap:", "event:vehicle",
                                       "event:person"}));
        QCOMPARE(f.stepEvent(0, 1), 3);   // older: past the Status line and the gap
        QCOMPARE(f.stepEvent(3, 1), 4);
        QCOMPARE(f.stepEvent(4, 1), -1);  // nothing older
        QCOMPARE(f.stepEvent(3, -1), 0);  // newer
        QCOMPARE(f.stepEvent(0, -1), -1);
        QCOMPARE(f.stepEvent(-1, 1), 0);  // from nothing: the newest
        const qint64 id = f.data(f.index(3), EventFeed::IdRole).toLongLong();
        QCOMPARE(f.rowOfId(id), 3);
        QCOMPARE(f.rowOfId(-5), -1);
        const QVariantMap r = f.rowAt(3);
        QCOMPARE(r.value("type").toString(), QStringLiteral("vehicle"));
        QCOMPARE(r.value("timestamp").toLongLong(), m_day.from + 2 * h);
        QCOMPARE(r.value("eventId").toLongLong(), id);
        QVERIFY(f.rowAt(99).isEmpty());
    }

    // A gap (app not running) is never new itself, but sits inside the band.
    void aGapAmongNewRowsIsNotNew()
    {
        cleanup();
        m_db = std::make_unique<Database>(m_dir.filePath(QStringLiteral("gap%1.db").arg(++m_n)),
                                          QStringLiteral("feed%1").arg(m_n));
        QVERIFY(m_db->open());
        const qint64 h = 3600;
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_now - kOtherKeepSecs - 86400), m_day.from + 3 * h));
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_day.from + 5 * h), m_now));
        add(1, 0, m_day.from + 2 * h, "person");
        add(1, 0, m_day.from + 6 * h, "vehicle");
        EventFeed f;
        setUp(f);
        f.setSeenLine(m_day.from + 2 * h);
        QCOMPARE(rows(f, EventFeed::NewRole),
                 QStringList({"event:true", "gap:false", "event:false"}));
        QCOMPARE(f.newUntilRow(), 2);
    }

    // Two events of a type in the same second are told apart: a new one in that second
    // goes in at the top, and a snapshot lands on its own row.
    void sameSecondEventsAreToldApart()
    {
        add(1, 0, m_day.from + 100, "person");
        EventFeed f;
        setUp(f);
        QSignalSpy reset(&f, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&f, &QAbstractItemModel::rowsInserted);
        add(1, 0, m_day.from + 100, "person");
        f.refresh();
        QCOMPARE(f.rowCount(), 2);
        QCOMPARE(reset.count() + inserted.count(), 1);
        const qint64 newest = f.data(f.index(0), EventFeed::IdRole).toLongLong();
        QVERIFY(m_db->setEventThumbnail(newest, QStringLiteral("/tmp/new.jpg")));
        f.refresh();
        QCOMPARE(f.data(f.index(0), EventFeed::ThumbnailRole).toString(), QStringLiteral("/tmp/new.jpg"));
        QVERIFY(f.data(f.index(1), EventFeed::ThumbnailRole).toString().isEmpty());
    }

    // The clock moving on (a refresh a minute later) doesn't reset a scrolled feed either.
    // An old event leaving the window goes from the bottom in place.
    void theClockMovingOnDoesNotReset()
    {
        add(1, 0, m_day.from + 100, "person");
        add(1, 0, m_now - kOtherKeepSecs + 30, "vehicle"); // leaves within the minute
        EventFeed f;
        setUp(f);
        QCOMPARE(rows(f), QStringList({"event:person", "event:vehicle"}));
        QSignalSpy reset(&f, &QAbstractItemModel::modelReset);
        QSignalSpy removed(&f, &QAbstractItemModel::rowsRemoved);
        f.setNow(m_now + 60);
        f.refresh();
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(rows(f), QStringList({"event:person"}));
    }

    void switchingCameraQueriesAgain()
    {
        add(1, 0, m_day.from + 100, "person");
        add(1, 3, m_prevDay.from + 100, "vehicle");
        EventFeed f;
        setUp(f);
        QCOMPARE(rows(f), QStringList({"event:person"}));
        f.setCameraKey(QStringLiteral("1:3"));
        QCOMPARE(rows(f), QStringList({"event:vehicle"}));
        QCOMPARE(f.counts().value("person").toInt(), 0);
    }

    // wayfinder/fisheye-and-framing step 06: the feed's cards take its camera's picture shape,
    // read from the newest snapshot's file header; 16:9 when there is none.
    void pictureRatioIsSixteenByNineWithoutSnapshots()
    {
        add(1, 0, m_day.from + 100, "person");
        EventFeed f;
        setUp(f);
        QCOMPARE(f.pictureRatio(), 16.0 / 9);
    }

    void pictureRatioComesFromTheNewestSnapshot()
    {
        addSnap(1, 0, m_day.from + 100, QSize(640, 360));   // older, a different shape
        addSnap(1, 0, m_day.from + 200, QSize(640, 853));   // the Doorbell's shape
        add(1, 0, m_day.from + 300, "vehicle");              // newest, no snapshot
        EventFeed f;
        setUp(f);
        QCOMPARE(f.pictureRatio(), 640.0 / 853);
    }

    void pictureRatioFollowsTheCamera()
    {
        addSnap(1, 0, m_day.from + 100, QSize(640, 640));
        addSnap(1, 3, m_day.from + 100, QSize(640, 180));
        EventFeed f;
        setUp(f);
        QCOMPARE(f.pictureRatio(), 1.0);
        QSignalSpy spy(&f, &EventFeed::pictureRatioChanged);
        f.setCameraKey(QStringLiteral("1:3"));
        QCOMPARE(f.pictureRatio(), 640.0 / 180);
        QCOMPARE(spy.count(), 1);
        f.refresh();                                         // unchanged: no signal
        QCOMPARE(spy.count(), 1);
    }

    void anUnreadableSnapshotMeansSixteenByNine()
    {
        addSnap(1, 0, m_day.from + 100, QSize());            // the file is gone
        EventFeed f;
        setUp(f);
        QCOMPARE(f.pictureRatio(), 16.0 / 9);
    }

    void aCameraWithNoTypesSaysSo()
    {
        add(1, 0, m_day.from + 100, "motion");
        EventFeed f;
        setUp(f, {});
        QVERIFY(!f.hasTypes());
        QCOMPARE(f.rowCount(), 0);
    }

    // The band sits where the app wasn't running, newest first among the events, keyed by
    // the day it ends on.
    void gapBandIsPlacedFromTheRuns()
    {
        cleanup();
        m_db = std::make_unique<Database>(m_dir.filePath(QStringLiteral("gap%1.db").arg(++m_n)),
                                          QStringLiteral("feed%1").arg(m_n));
        QVERIFY(m_db->open());
        const qint64 h = 3600;
        // Runs: the evening before 20:00–21:00, then 01:00–03:00, then 05:00 to now.
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_prevDay.from + 20 * h), m_prevDay.from + 21 * h));
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_day.from + 1 * h), m_day.from + 3 * h));
        QVERIFY(m_db->touchAppRun(m_db->beginAppRun(m_day.from + 5 * h), m_now));
        add(1, 0, m_day.from + 2 * h, "person");
        add(1, 0, m_day.from + 6 * h, "vehicle");
        EventFeed f;
        setUp(f);
        QCOMPARE(rows(f), QStringList({"event:vehicle", "gap:", "event:person", "gap:"}));
        QCOMPARE(f.data(f.index(1), EventFeed::FromRole).toLongLong(), m_day.from + 3 * h);
        QCOMPARE(f.data(f.index(1), EventFeed::ToRole).toLongLong(), m_day.from + 5 * h);
        // The overnight gap, 21:00 the 4th to 01:00 the 5th, sits under the 5th.
        QCOMPARE(f.data(f.index(3), EventFeed::FromRole).toLongLong(), m_prevDay.from + 21 * h);
        QCOMPARE(f.data(f.index(3), EventFeed::ToRole).toLongLong(), m_day.from + 1 * h);
        QCOMPARE(days(f), QStringList({"2026-11-05", "2026-11-05", "2026-11-05", "2026-11-05"}));
    }

    // Today's counts by recorder and type (the Site cards).
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
