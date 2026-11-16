#include "core/Database.h"
#include "device/EventSwitches.h"
#include "device/Inbox.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace rl;
using Source = Inbox::Source;
using Verdict = Inbox::Verdict;

// The one way an event reaches the store (wayfinder/architecture ticket 03): every source
// hands the Inbox a candidate and gets back what became of it.
class TestInbox : public QObject
{
    Q_OBJECT

    static constexpr qint64 kNow = 1'800'000'000;

    struct Store {
        QTemporaryDir dir;
        Database db;
        bool opened = false;
        EventSwitches switches;
        Inbox inbox;
        explicit Store(const char *name, int cap = 1000)
            : db(dir.filePath("t.db"), QString::fromLatin1(name)),
              switches((opened = db.open(), &db)),
              inbox(&db, &switches, [] { return kNow; }, cap)
        {
            if (!opened)
                qFatal("test store didn't open");
        }
        QVector<EventRecord> rows(qint64 host) const { return db.recentEvents(100, {}, {host}); }
    };

    static Inbox::Candidate seen(qint64 host, int ch, const char *type, double at = 0)
    {
        return {host, ch, QString::fromLatin1(type), QStringLiteral("Cam %1").arg(ch), at};
    }

private slots:
    // ---- Live ---------------------------------------------------------------------------
    void aLiveEventIsStoredAndAnnounced()
    {
        Store s("live");
        const Inbox::Filing f = s.inbox.file(seen(7, 1, "person"), Source::Live);
        QCOMPARE(f.verdict, Verdict::Stored);
        QVERIFY(f.announce);
        QVERIFY(f.needsThumbnail);
        QVERIFY(f.row.id > 0);
        QCOMPARE(f.row.timestamp, kNow);   // no source time: now
        QCOMPARE(s.rows(7).size(), 1);
    }

    void aLiveEventIsFiledAtTheSourcesTimeButNeverAheadOfOurs()
    {
        Store s("live_time");
        QCOMPARE(s.inbox.file(seen(9, 2, "person", kNow - 40), Source::Live).row.timestamp, kNow - 40);
        QCOMPARE(s.inbox.file(seen(9, 3, "person", kNow + 600), Source::Live).row.timestamp, kNow);
    }

    void aSwitchedOffPairIsNotStored()
    {
        Store s("live_off");
        QVERIFY(s.switches.setOff(7, 1, QStringLiteral("vehicle"), true));
        const Inbox::Filing f = s.inbox.file(seen(7, 1, "vehicle"), Source::Live);
        QCOMPARE(f.verdict, Verdict::SwitchedOff);
        QVERIFY(!f.announce && !f.needsThumbnail);
        QCOMPARE(s.rows(7).size(), 0);
        QCOMPARE(s.inbox.file(seen(7, 1, "person"), Source::Live).verdict, Verdict::Stored);
    }

    // Ticket 02: a live report at the source's time of an event history filed is the same
    // event; one with no source time ("now", Reolink) always files.
    void aLiveReportOfAStoredEventIsTheSameEvent()
    {
        Store s("live_same");
        const Inbox::Filing h = s.inbox.file(seen(9, 2, "person", kNow - 60), Source::History);
        QCOMPARE(h.verdict, Verdict::Stored);

        const Inbox::Filing again = s.inbox.file(seen(9, 2, "person", kNow - 57), Source::Live);
        QCOMPARE(again.verdict, Verdict::SameEvent);
        QCOMPARE(again.row.id, h.row.id);
        QVERIFY(!again.announce);
        QCOMPARE(s.rows(9).size(), 1);

        QCOMPARE(s.inbox.file(seen(9, 2, "person", kNow - 10), Source::Live).verdict,
                 Verdict::Stored);   // 50 s on: a new event
        s.inbox.file(seen(7, 1, "person"), Source::Live);
        QCOMPARE(s.inbox.file(seen(7, 1, "person"), Source::Live).verdict, Verdict::Stored);
    }

    // Frigate's clock ahead of ours: history files it at our now, and the live report (also
    // capped at now) meets it.
    void aServerClockAheadStillMeetsItsOwnEvent()
    {
        Store s("clock_ahead");
        QCOMPARE(s.inbox.file(seen(9, 2, "vehicle", kNow + 600), Source::History).row.timestamp, kNow);
        QCOMPARE(s.inbox.file(seen(9, 2, "vehicle", kNow + 600), Source::Live).verdict,
                 Verdict::SameEvent);
    }

    // The gap ticket 02 left: the stored row's thumbnail never came, so the live report of
    // the same event asks for one.
    void theSameEventWithoutAThumbnailAsksForOne()
    {
        Store s("same_picture");
        const Inbox::Filing h = s.inbox.file(seen(9, 2, "person", kNow - 60), Source::History);
        QVERIFY(s.inbox.file(seen(9, 2, "person", kNow - 58), Source::Live).needsThumbnail);
        s.db.setEventThumbnail(h.row.id, QStringLiteral("/tmp/x.jpg"));
        QVERIFY(!s.inbox.file(seen(9, 2, "person", kNow - 58), Source::Live).needsThumbnail);
    }

    // ---- History ------------------------------------------------------------------------
    void historyIsStoredButNotAnnounced()
    {
        Store s("hist");
        const Inbox::Filing f = s.inbox.file(seen(9, 2, "person", kNow - 600), Source::History);
        QCOMPARE(f.verdict, Verdict::Stored);
        QVERIFY(!f.announce);
        QVERIFY(f.needsThumbnail);
        QCOMPARE(f.row.timestamp, kNow - 600);
    }

    void historyRerunFindsWhatItFiled()
    {
        Store s("hist_rerun");
        s.inbox.file(seen(9, 2, "person", kNow - 600), Source::History);
        QCOMPARE(s.inbox.file(seen(9, 2, "person", kNow - 600), Source::History).verdict,
                 Verdict::SameEvent);
        QCOMPARE(s.rows(9).size(), 1);
    }

    // A batch files oldest first, one per camera, type and burst; a second type beside
    // the first is its own event.
    void aHistoryBatchKeepsOnePerBurstAndEachType()
    {
        Store s("hist_batch");
        const QVector<Inbox::Filing> f = s.inbox.fileHistory({   // newest first, as Frigate
            seen(9, 2, "person", kNow - 40), seen(9, 2, "person", kNow - 590),
            seen(9, 2, "vehicle", kNow - 595), seen(9, 2, "person", kNow - 600)});
        QCOMPARE(f.size(), 4);                          // in the batch's order
        QCOMPARE(f.at(0).verdict, Verdict::Stored);     // person at 40
        QCOMPARE(f.at(1).verdict, Verdict::SameBurst);  // person 10 s after 600 (filed first)
        QCOMPARE(f.at(2).verdict, Verdict::Stored);     // vehicle at 595
        QCOMPARE(f.at(3).verdict, Verdict::Stored);
        QCOMPARE(f.at(3).row.timestamp, kNow - 600);
        QStringList stored;
        for (const EventRecord &r : s.rows(9))
            stored << QStringLiteral("%1@%2").arg(r.type).arg(kNow - r.timestamp);
        QCOMPARE(stored.join(","), QStringLiteral("person@40,vehicle@595,person@600"));
    }

    void historyOlderThanTheStoreKeepsIsTooOld()
    {
        Store s("hist_old");
        const qint64 month = device::kOtherKeepSecs, day = device::kMotionKeepSecs;
        QCOMPARE(s.inbox.file(seen(9, 2, "person", kNow - month - 1), Source::History).verdict,
                 Verdict::TooOld);
        QCOMPARE(s.inbox.file(seen(9, 2, "person", kNow - month + 60), Source::History).verdict,
                 Verdict::Stored);
        QCOMPARE(s.inbox.file(seen(9, 2, "motion", kNow - day - 1), Source::History).verdict,
                 Verdict::TooOld);
        QCOMPARE(s.rows(9).size(), 1);
    }

    // One too old to keep doesn't take its burst: the next one, just inside, is stored.
    void aTooOldEventDoesntClaimItsBurst()
    {
        Store s("hist_old_burst");
        const qint64 cutoff = kNow - device::kOtherKeepSecs;
        const QVector<Inbox::Filing> f = s.inbox.fileHistory(
            {seen(9, 2, "person", cutoff + 5), seen(9, 2, "person", cutoff - 5)});
        QCOMPARE(f.at(1).verdict, Verdict::TooOld);
        QCOMPARE(f.at(0).verdict, Verdict::Stored);
    }

    void historySkipsASwitchedOffPair()
    {
        Store s("hist_off");
        QVERIFY(s.switches.setOff(9, 2, QStringLiteral("vehicle"), true));
        QCOMPARE(s.inbox.file(seen(9, 2, "vehicle", kNow - 600), Source::History).verdict,
                 Verdict::SwitchedOff);
        QCOMPARE(s.rows(9).size(), 0);
    }

    // ---- Status -------------------------------------------------------------------------
    void aHostGoingOfflineFilesARowPerCameraAndIsAnnouncedOnce()
    {
        Store s("status");
        const QVector<Inbox::Filing> f = s.inbox.fileStatus(
            7, -1, QStringLiteral("NVR"), false, {{1, QStringLiteral("Gate")}, {2, QStringLiteral("Yard")}});
        QCOMPARE(f.size(), 2);
        QCOMPARE(f.at(0).verdict, Verdict::Stored);
        QCOMPARE(f.at(0).row.type, QStringLiteral("offline"));
        QVERIFY(f.at(0).announce);
        QVERIFY(!f.at(1).announce);
        QVERIFY(!f.at(0).needsThumbnail);
        QCOMPARE(s.rows(7).size(), 2);
    }

    // Every camera was already offline: no row, still told once (as before).
    void aStatusChangeWithNoRowsIsStillAnnounced()
    {
        Store s("status_none");
        const QVector<Inbox::Filing> f = s.inbox.fileStatus(7, -1, QStringLiteral("NVR"), true, {});
        QCOMPARE(f.size(), 1);
        QCOMPARE(f.at(0).verdict, Verdict::NotStored);
        QVERIFY(f.at(0).announce);
        QCOMPARE(s.rows(7).size(), 0);
    }

    // ---- Keeping the store in bounds ----------------------------------------------------
    void aRecorderOverTheCapKeepsItsNewestAndSaysSo()
    {
        Store s("cap", 3);
        QSignalSpy trimmed(&s.inbox, &Inbox::trimmed);
        for (int i = 5; i >= 1; --i)
            s.inbox.file(seen(7, 1, "person", kNow - i * 100), Source::History);
        s.inbox.file(seen(7, 1, "motion", kNow - 50), Source::Live);   // outside the cap
        QVERIFY(trimmed.count() >= 1);
        int capped = 0;
        for (const EventRecord &r : s.rows(7))
            capped += r.type == QLatin1String("person");
        QCOMPARE(capped, 3);
        QCOMPARE(s.rows(7).size(), 4);
    }

    // Rows deleted another way (a switch-off) are counted afresh, so the cap doesn't trim
    // too early.
    void aRecountForgetsTheRecordersCount()
    {
        Store s("recount", 2);
        QSignalSpy trimmed(&s.inbox, &Inbox::trimmed);
        s.inbox.file(seen(7, 1, "person", kNow - 300), Source::History);
        s.inbox.file(seen(7, 1, "vehicle", kNow - 200), Source::History);
        QVERIFY(s.db.deleteEvents(7, 1, QStringLiteral("vehicle")));
        s.inbox.recount(7);
        s.inbox.file(seen(7, 1, "person", kNow - 100), Source::History);
        QCOMPARE(trimmed.count(), 0);
        QCOMPARE(s.rows(7).size(), 2);
    }

    void retentionDropsOldRowsAndTheirThumbnails()
    {
        Store s("retention");
        const Inbox::Filing old = s.inbox.file(seen(7, 1, "motion", kNow - 3600), Source::History);
        s.inbox.file(seen(7, 1, "person", kNow - 3600), Source::History);
        const QString picture = s.dir.filePath("event_1.jpg");
        QFile f(picture);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        QVERIFY(s.db.setEventThumbnail(old.row.id, picture));

        Inbox later(&s.db, &s.switches, [] { return kNow + device::kMotionKeepSecs; });
        QSignalSpy trimmed(&later, &Inbox::trimmed);
        later.enforceRetention();
        QCOMPARE(trimmed.count(), 1);
        QCOMPARE(s.rows(7).size(), 1);
        QCOMPARE(s.rows(7).first().type, QStringLiteral("person"));
        QVERIFY(!QFile::exists(picture));
    }
};

QTEST_GUILESS_MAIN(TestInbox)
#include "test_inbox.moc"
