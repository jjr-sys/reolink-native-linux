#include "device/DownloadPlan.h"

#include <QtTest>

using namespace rl;

class TestDownloads : public QObject
{
    Q_OBJECT
private slots:
    // An NVR serves one download at a time; another NVR is unaffected.
    void nextRunnable_oneAtATimePerNvr()
    {
        std::vector<DlSlot> s = {{1, DlState::Downloading}, {1, DlState::Queued}, {2, DlState::Queued}};
        QCOMPARE(nextRunnable(s), 2); // host 1 is busy, so the queued item on host 2 goes
        s[2].state = DlState::Downloading;
        QCOMPARE(nextRunnable(s), -1);
        s[0].state = DlState::Done;
        QCOMPARE(nextRunnable(s), 1); // host 1 is free again
    }
    void nextRunnable_preparingCountsAsBusy()
    {
        std::vector<DlSlot> s = {{1, DlState::Preparing}, {1, DlState::Queued}};
        QCOMPARE(nextRunnable(s), -1);
    }
    void nextRunnable_isFirstInFirstOut()
    {
        std::vector<DlSlot> s = {{1, DlState::Queued}, {1, DlState::Queued}, {2, DlState::Queued}};
        QCOMPARE(nextRunnable(s), 0);
        s[0].state = DlState::Preparing;
        QCOMPARE(nextRunnable(s), 2);
    }
    void nextRunnable_finishedItemsDoNotBlock()
    {
        std::vector<DlSlot> s = {{1, DlState::Failed}, {1, DlState::Cancelled}, {1, DlState::Done},
                                 {1, DlState::Queued}};
        QCOMPARE(nextRunnable(s), 3);
    }

    // The site is in the name, so two "Kitchen"s at different sites cannot clash.
    void fileName_includesSiteAndCamera()
    {
        const QDateTime t(QDate(2026, 9, 19), QTime(14, 20, 0));
        QCOMPARE(downloadFileName("Woorabinda", "Kitchen", t), QString("Woorabinda_Kitchen_20260919_142000.mp4"));
        QVERIFY(downloadFileName("Farm", "Kitchen", t) != downloadFileName("Woorabinda", "Kitchen", t));
    }
    void fileName_cleansUnsafeCharacters()
    {
        const QDateTime t(QDate(2026, 9, 19), QTime(1, 2, 3));
        QCOMPARE(downloadFileName("My Site/1", "Front  Door!", t), QString("My_Site_1_Front_Door_20260919_010203.mp4"));
        QCOMPARE(downloadFileName("", "  ", t), QString("camera_camera_20260919_010203.mp4"));
    }
    void fileName_marksParts()
    {
        const QDateTime t(QDate(2026, 9, 19), QTime(14, 20, 0));
        QCOMPARE(downloadFileName("A", "B", t, 2, 3), QString("A_B_20260919_142000_part2.mp4"));
        QCOMPARE(downloadFileName("A", "B", t, 1, 1), QString("A_B_20260919_142000.mp4"));
    }

    // The NVR needs about 1.6 s per minute to cut a clip: the wait must comfortably cover
    // that, from a short clip to a very long one, and stay bounded.
    void prepareTimeout_scalesWithTheRange()
    {
        QVERIFY(prepareTimeoutSecs(120) >= 60);
        QVERIFY(prepareTimeoutSecs(30 * 60) > 30 * 60 * 3 / 100); // 30 min needs ~48 s; ours is far above
        QVERIFY(prepareTimeoutSecs(30 * 60) >= 240);
        QCOMPARE(prepareTimeoutSecs(100 * 3600), 3600L);
    }
};

QTEST_GUILESS_MAIN(TestDownloads)
#include "test_downloads.moc"
