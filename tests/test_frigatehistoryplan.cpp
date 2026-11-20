#include "core/Database.h"
#include "device/EventSwitches.h"
#include "device/FrigateHistoryPlan.h"
#include "device/Inbox.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace rl;
using rl::device::FrigateHistoryPlan;
using rl::device::SnapshotFetch;
using rl::device::fetchPictureFor;
using Verdict = Inbox::Verdict;

// A Frigate host's history sync: which events go to the Inbox, and which filed rows then
// fetch Frigate's own snapshot, so a rerun or a live report doesn't fetch a picture twice.
class TestFrigateHistoryPlan : public QObject
{
    Q_OBJECT

    static constexpr qint64 kNow = 1'800'000'000;
    static constexpr qint64 kHost = 7;

    struct Store {
        QTemporaryDir dir;
        Database db;
        bool opened = false;
        EventSwitches switches;
        Inbox inbox;
        explicit Store(const char *name)
            : db(dir.filePath("t.db"), QString::fromLatin1(name)),
              switches((opened = db.open(), &db)),
              inbox(&db, &switches, [] { return kNow; })
        {
            if (!opened)
                qFatal("test store didn't open");
        }
    };

    static frigate::Event ev(const char *id, const char *camera, const char *label, double at)
    {
        return {QString::fromLatin1(id), QString::fromLatin1(camera), QString::fromLatin1(label),
                at};
    }
    static const QHash<QString, int> &cameras()
    {
        static const QHash<QString, int> c = {{"front_door", 0}, {"driveway", 2}};
        return c;
    }
    static Inbox::Filing filing(Verdict v, qint64 rowId, bool needsThumbnail)
    {
        Inbox::Filing f;
        f.verdict = v;
        f.row.id = rowId;
        f.needsThumbnail = needsThumbnail;
        return f;
    }

private slots:
    // ---- What to file -------------------------------------------------------------------
    void eventsBecomeCandidatesOnTheirCameraChannel()
    {
        const FrigateHistoryPlan plan(kHost, {ev("a", "driveway", "car", kNow - 600)}, cameras());
        QCOMPARE(plan.batch().size(), 1);
        const Inbox::Candidate &c = plan.batch().first();
        QCOMPARE(c.hostId, kHost);
        QCOMPARE(c.channel, 2);
        QCOMPARE(c.type, QStringLiteral("vehicle"));
        QCOMPARE(c.camera, QStringLiteral("Driveway"));   // Frigate's name, as shown
        QCOMPARE(c.at, double(kNow - 600));
    }

    void anUnknownCameraIsSkipped()
    {
        const FrigateHistoryPlan plan(kHost,
                                      {ev("a", "garden", "person", kNow - 600),
                                       ev("b", "front_door", "person", kNow - 500)},
                                      cameras());
        QCOMPARE(plan.batch().size(), 1);
        QCOMPARE(plan.batch().first().channel, 0);
    }

    void aLabelTheAppDoesNotModelIsSkipped()
    {
        const FrigateHistoryPlan plan(kHost,
                                      {ev("a", "front_door", "bird", kNow - 600),
                                       ev("b", "front_door", "dog", kNow - 500)},
                                      cameras());
        QCOMPARE(plan.batch().size(), 1);
        QCOMPARE(plan.batch().first().type, QStringLiteral("pet"));
    }

    // ---- Which pictures to fetch --------------------------------------------------------
    void eachRowNeedingAPictureFetchesItsOwnFrigateEvent()
    {
        // Skipped events between them mustn't shift the ids against the filings.
        const FrigateHistoryPlan plan(kHost,
                                      {ev("x", "garden", "person", kNow - 900),
                                       ev("a", "front_door", "person", kNow - 800),
                                       ev("y", "front_door", "bird", kNow - 700),
                                       ev("b", "driveway", "car", kNow - 600)},
                                      cameras());
        QCOMPARE(plan.batch().size(), 2);
        const auto fetch = plan.snapshots(
            {filing(Verdict::Stored, 11, true), filing(Verdict::Stored, 12, true)}, {});
        QCOMPARE(fetch, (QVector<SnapshotFetch>{{11, "a"}, {12, "b"}}));
    }

    void onlyRowsWithoutAPictureFetch()
    {
        const FrigateHistoryPlan plan(kHost,
                                      {ev("a", "front_door", "person", kNow - 900),
                                       ev("b", "front_door", "person", kNow - 890),
                                       ev("c", "front_door", "person", kNow - 600),
                                       ev("d", "front_door", "person", kNow - 300)},
                                      cameras());
        const auto fetch = plan.snapshots({filing(Verdict::Stored, 21, true),
                                           filing(Verdict::SameBurst, 0, false),
                                           filing(Verdict::SameEvent, 5, false),   // has one
                                           filing(Verdict::SameEvent, 6, true)},   // never came
                                          {});
        QCOMPARE(fetch, (QVector<SnapshotFetch>{{21, "a"}, {6, "d"}}));
    }

    void aRowWhoseSnapshotIsDueIsNotFetchedAgain()
    {
        const FrigateHistoryPlan plan(kHost,
                                      {ev("a", "front_door", "person", kNow - 900),
                                       ev("b", "front_door", "person", kNow - 600)},
                                      cameras());
        const auto fetch = plan.snapshots(
            {filing(Verdict::SameEvent, 31, true), filing(Verdict::Stored, 32, true)}, {31});
        QCOMPARE(fetch, (QVector<SnapshotFetch>{{32, "b"}}));
    }

    // History and live ask the same question of a filing (the due-set guard is symmetric).
    void historyAndLiveAskTheSameQuestion()
    {
        const QSet<qint64> due = {2};
        const QVector<Inbox::Filing> filed = {filing(Verdict::Stored, 1, true),
                                              filing(Verdict::SameEvent, 2, true),
                                              filing(Verdict::SameEvent, 3, false),
                                              filing(Verdict::SwitchedOff, 0, false)};
        QVector<frigate::Event> events;
        for (int i = 0; i < filed.size(); ++i)
            events.append({QStringLiteral("e%1").arg(i), "front_door", "person",
                           double(kNow - 1000 + 100 * i)});
        const FrigateHistoryPlan plan(kHost, events, cameras());
        QVector<qint64> live;
        for (const Inbox::Filing &f : filed)
            if (fetchPictureFor(f, due))
                live.append(f.row.id);
        QVector<qint64> history;
        for (const SnapshotFetch &s : plan.snapshots(filed, due))
            history.append(s.rowId);
        QCOMPARE(live, (QVector<qint64>{1}));
        QCOMPARE(history, live);
    }

    // ---- Through the Inbox --------------------------------------------------------------
    void aRerunWhileTheSnapshotsAreDueFetchesNothing()
    {
        Store s("rerun");
        const QVector<frigate::Event> events = {ev("a", "front_door", "person", kNow - 900),
                                                ev("b", "driveway", "car", kNow - 600)};
        const FrigateHistoryPlan plan(kHost, events, cameras());
        QSet<qint64> due;
        const auto first = plan.snapshots(s.inbox.fileHistory(plan.batch()), due);
        QCOMPARE(first.size(), 2);
        for (const SnapshotFetch &f : first)
            due.insert(f.rowId);   // as EventManager marks them

        const FrigateHistoryPlan again(kHost, events, cameras());
        const QVector<Inbox::Filing> refiled = s.inbox.fileHistory(again.batch());
        QCOMPARE(refiled.at(0).verdict, Verdict::SameEvent);
        QCOMPARE(refiled.at(1).verdict, Verdict::SameEvent);
        QVERIFY(again.snapshots(refiled, due).isEmpty());
    }

    void aRerunAfterThePicturesLandedFetchesNothing()
    {
        Store s("landed");
        const QVector<frigate::Event> events = {ev("a", "front_door", "person", kNow - 900)};
        const FrigateHistoryPlan plan(kHost, events, cameras());
        const auto first = plan.snapshots(s.inbox.fileHistory(plan.batch()), {});
        QCOMPARE(first.size(), 1);
        QVERIFY(s.db.setEventThumbnail(first.first().rowId, "/x/event.jpg"));   // landed

        const FrigateHistoryPlan again(kHost, events, cameras());
        QVERIFY(again.snapshots(s.inbox.fileHistory(again.batch()), {}).isEmpty());
    }

    void aRerunAfterAFailedFetchTriesAgain()
    {
        Store s("failed");
        const QVector<frigate::Event> events = {ev("a", "front_door", "person", kNow - 900)};
        const FrigateHistoryPlan plan(kHost, events, cameras());
        const auto first = plan.snapshots(s.inbox.fileHistory(plan.batch()), {});
        QCOMPARE(first.size(), 1);
        // The fetch failed: no picture stored, and the row is no longer due.
        const FrigateHistoryPlan again(kHost, events, cameras());
        QCOMPARE(again.snapshots(s.inbox.fileHistory(again.batch()), {}), first);
    }

    void aLiveReportWhileHistorysSnapshotIsDueWaitsForIt()
    {
        Store s("live_waits");
        const FrigateHistoryPlan plan(kHost, {ev("a", "front_door", "person", kNow - 60)},
                                      cameras());
        const auto fetch = plan.snapshots(s.inbox.fileHistory(plan.batch()), {});
        QCOMPARE(fetch.size(), 1);
        const QSet<qint64> due = {fetch.first().rowId};

        // Frigate's live feed reports the same event (at its own start time).
        const Inbox::Filing live = s.inbox.file(
            {kHost, 0, "person", "Front Door", double(kNow - 58)}, Inbox::Source::Live);
        QCOMPARE(live.verdict, Verdict::SameEvent);
        QCOMPARE(live.row.id, fetch.first().rowId);
        QVERIFY(live.needsThumbnail);
        QVERIFY(!fetchPictureFor(live, due));   // waits for Frigate's snapshot
        QVERIFY(fetchPictureFor(live, {}));     // none on its way: grab a frame
    }

    void aHistoryRunAfterALiveReportStillFetchesTheSnapshot()
    {
        // The live capture isn't recorded as due (as found): history still fetches Frigate's.
        Store s("live_first");
        const Inbox::Filing live = s.inbox.file(
            {kHost, 0, "person", "Front Door", double(kNow - 60)}, Inbox::Source::Live);
        QVERIFY(fetchPictureFor(live, {}));
        const FrigateHistoryPlan plan(kHost, {ev("a", "front_door", "person", kNow - 61)},
                                      cameras());
        QCOMPARE(plan.snapshots(s.inbox.fileHistory(plan.batch()), {}),
                 (QVector<SnapshotFetch>{{live.row.id, "a"}}));
    }
};

QTEST_GUILESS_MAIN(TestFrigateHistoryPlan)
#include "test_frigatehistoryplan.moc"
