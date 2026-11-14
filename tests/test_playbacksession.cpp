#include "device/RecordingFileSnap.h"
#include "media/PlaybackSession.h"

#include <QtTest>

using namespace rl;
namespace {
using Route = PlaybackSession::Route;
using Outcome = PlaybackSession::Outcome;

// Scripted adapter: records what the session asked of the device, answers from fields.
class Transport final : public PlaybackTransport
{
public:
    struct Open { Route route; int row; qint64 epoch; QString url; };
    QVector<Open> opens;
    int stops = 0;
    int seeks = 0;
    bool isStreaming = false;
    bool isBusy = false;
    bool isDirect = false;
    bool seekWorks = true;
    QString url = "http://local/flv";
    bool streaming() const override { return isStreaming; }
    bool busy() const override { return isBusy; }
    bool direct(int) const override { return isDirect; }
    QString httpUrl(int, qint64) override { return url; }
    bool seekInPlace(int, qint64) override { ++seeks; return seekWorks; }
    void openHttp(int row, qint64 epoch, const QString &u) override
    {
        opens.append({Route::Http, row, epoch, u});
    }
    void openBaichuan(int row, qint64 epoch, bool main, bool reverse) override
    {
        opens.append({reverse ? Route::BaichuanReverse : main ? Route::BaichuanMain : Route::BaichuanSub,
                      row, epoch, {}});
    }
    void stop() override { ++stops; isStreaming = false; }
};

struct Fixture {
    Transport *t = new Transport;
    PlaybackSession s{std::unique_ptr<PlaybackTransport>(t)};
    Fixture() { s.setDeviceRow(3); }
};
using Files = QVector<std::pair<qint64, qint64>>;
} // namespace

class TestPlaybackSession : public QObject
{
    Q_OBJECT
private slots:
    // ---- file snapping (moved out of DeviceManager::playbackUrl) ----
    void snapInsideFileSeeksIntoIt()
    {
        const Files f{{1000, 1100}, {1200, 1300}};
        const auto s = snapToRecordingFile(f, 1250);
        QVERIFY(s.found);
        QCOMPARE(s.fileStart, qint64(1200));
        QCOMPARE(s.seekSecs, 50);
    }
    void snapInGapUsesNearestEarlierFile()
    {
        const Files f{{1000, 1100}, {1200, 1300}};
        const auto s = snapToRecordingFile(f, 1150);
        QVERIFY(s.found);
        QCOMPARE(s.fileStart, qint64(1000));
        QCOMPARE(s.seekSecs, 100); // clamped to the file length
    }
    void snapBeforeAnyFileIsNotFound()
    {
        QVERIFY(!snapToRecordingFile(Files{{1000, 1100}}, 999).found);
        QVERIFY(!snapToRecordingFile(Files{}, 5).found);
    }

    // ---- transport choice ----
    void routeFollowsHdAndSpeedAndDirect()
    {
        QCOMPARE(PlaybackSession::chooseRoute(false, 1.0, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(false, 0.5, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(false, 2.0, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(true, 1.0, false), Route::BaichuanMain);
        QCOMPARE(PlaybackSession::chooseRoute(true, 0.5, false), Route::BaichuanMain);
        // Above 1x the recorder sends the main stream as keyframes only, so HD plays the sub
        // stream at speed and comes back at 1x (wayfinder/playback-controls ticket 10).
        QCOMPARE(PlaybackSession::chooseRoute(true, 2.0, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(true, 16.0, false), Route::BaichuanSub);
        // Direct (Frigate) footage has one HTTP stream at 1x: HD and speed are ignored.
        QCOMPARE(PlaybackSession::chooseRoute(true, 4.0, true), Route::Http);
        // Backwards: the sub stream in windows (ticket 12), whatever HD says; Frigate stays HTTP.
        QCOMPARE(PlaybackSession::chooseRoute(true, 1.0, false, -1), Route::BaichuanReverse);
        QCOMPARE(PlaybackSession::chooseRoute(false, 0.25, false, -1), Route::BaichuanReverse);
        QCOMPARE(PlaybackSession::chooseRoute(false, 1.0, true, -1), Route::Http);
    }
    void sdAtNormalSpeedOpensBaichuanSub()
    {
        Fixture f;
        QCOMPARE(f.s.playAt(5000), Outcome::Opened);
        QCOMPARE(f.t->opens.size(), 1);
        QCOMPARE(f.t->opens[0].route, Route::BaichuanSub);
        QCOMPARE(f.t->opens[0].epoch, qint64(5000));
    }
    void directOpensHttpAtTheGivenMoment()
    {
        Fixture f;
        f.t->isDirect = true;
        QCOMPARE(f.s.playAt(5000), Outcome::Opened);
        QCOMPARE(f.t->opens[0].route, Route::Http);
        QCOMPARE(f.t->opens[0].url, QString("http://local/flv"));
    }
    void httpWithNoUrlDoesNothing()
    {
        Fixture f;
        f.t->isDirect = true;
        f.t->url.clear();
        QCOMPARE(f.s.playAt(5000), Outcome::NoSource);
        QVERIFY(f.t->opens.isEmpty());
        QCOMPARE(f.t->stops, 0);
    }
    void fastSpeedUsesBaichuanSub()
    {
        Fixture f;
        f.s.setSpeed(2.0);
        f.s.playAt(5000);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanSub);
    }
    void hdUsesBaichuanMainAndReportsIt()
    {
        Fixture f;
        f.s.setHd(true);
        QCOMPARE(f.s.playAt(5000), Outcome::OpenedHd);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanMain);
    }
    void directRowStaysOnHttp()
    {
        Fixture f;
        f.t->isDirect = true;
        f.s.setHd(true);
        f.s.setSpeed(2.0);
        f.s.playAt(5000);
        QCOMPARE(f.t->opens.last().route, Route::Http);
    }

    // ---- seek or reopen ----
    void streamingBaichuanSeeksInPlace()
    {
        Fixture f;
        f.s.setHd(true);
        QCOMPARE(f.s.playAt(5000), Outcome::OpenedHd);
        f.t->isStreaming = true;
        QCOMPARE(f.s.playAt(6000), Outcome::SeekedInPlace);
        QCOMPARE(f.t->opens.size(), 1);
        QCOMPARE(f.t->seeks, 1);
    }
    void failedInPlaceSeekReopens()
    {
        Fixture f;
        f.s.setHd(true);
        QCOMPARE(f.s.playAt(5000), Outcome::OpenedHd);
        f.t->isStreaming = true;
        f.t->seekWorks = false;
        QCOMPARE(f.s.playAt(6000), Outcome::OpenedHd);
        QCOMPARE(f.t->opens.size(), 2);
    }
    void notStreamingNeverTriesInPlaceSeek()
    {
        Fixture f;
        f.s.setHd(true);
        f.s.playAt(6000);
        QCOMPARE(f.t->seeks, 0);
    }
    void httpAlwaysReopens()
    {
        Fixture f;
        f.t->isDirect = true;
        f.t->isStreaming = true;
        f.s.playAt(6000);
        QCOMPARE(f.t->seeks, 0);
        QCOMPARE(f.t->opens.size(), 1);
    }
    void gapStopsAndOpensNothing()
    {
        Fixture f;
        QCOMPARE(f.s.playAt(6000, /*covered=*/false), Outcome::Gap);
        QCOMPARE(f.t->stops, 1);
        QVERIFY(f.t->opens.isEmpty());
    }
    void noDeviceRowOpensNothingOverBaichuan()
    {
        Fixture f;
        f.s.setDeviceRow(-1);
        f.s.setHd(true);
        QCOMPARE(f.s.playAt(6000), Outcome::NoSource);
        QVERIFY(f.t->opens.isEmpty());
    }
    void playSubAtIgnoresHd()
    {
        Fixture f; // event jump: always the light sub stream
        f.s.setHd(true);
        f.s.setSpeed(2.0);
        f.s.playSubAt(7000);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanSub);
    }
    void playSubAtOnDirectUsesHttp()
    {
        Fixture f;
        f.t->isDirect = true;
        f.s.playSubAt(7000);
        QCOMPARE(f.t->opens.last().route, Route::Http);
    }

    // ---- reopen on route change ----
    void speedChangeKeepsTheNativeSessionOpen()
    {
        Fixture f; // one native sub session paces itself at any speed: no reopen
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.t->isStreaming = true;
        f.s.setSpeed(2.0);
        f.s.setSpeed(1.0);
        QCOMPARE(spy.count(), 0);
    }
    void togglingHdWhilePlayingAsksForReopen()
    {
        Fixture f;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.t->isStreaming = true;
        f.s.setHd(true);
        QCOMPARE(spy.count(), 1);
    }
    void speedChangeWhileIdleDoesNotReopen()
    {
        Fixture f;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.s.setSpeed(2.0);
        QCOMPARE(spy.count(), 0);
    }
    void connectingCountsAsPlaying()
    {
        Fixture f;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.t->isBusy = true;
        f.s.setHd(true);
        QCOMPARE(spy.count(), 1);
    }
    // HD above 1x reopens on the sub stream (keyframes only on main at speed, ticket 10);
    // slow speeds keep HD.
    void speedAboveOneInHdReopensOnSub()
    {
        Fixture f;
        f.s.setHd(true);
        f.t->isStreaming = true;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.s.setSpeed(0.5);
        QCOMPARE(spy.count(), 0);
        f.s.setSpeed(2.0);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.route(), Route::BaichuanSub);
    }
    // The reopen after a route change really opens the other stream: an in-place seek
    // would keep the running one (the user saw SD stay after 16x from HD, ticket 11).
    void backTo1xFromSpeedReopensHdNotSeek()
    {
        Fixture f;
        f.s.setHd(true);
        QCOMPARE(f.s.playAt(6000), Outcome::OpenedHd);
        f.t->isStreaming = true;
        f.s.setSpeed(16.0);
        QCOMPARE(f.s.playAt(6100), Outcome::Opened);       // sub, not a seek of main
        f.s.setSpeed(1.0);
        QCOMPARE(f.s.playAt(6200), Outcome::OpenedHd);     // main again, not a seek of sub
        QCOMPARE(f.t->seeks, 0);
        QCOMPARE(f.t->opens.size(), 3);
        QCOMPARE(f.t->opens.at(2).route, Route::BaichuanMain);
        // Same route again: a seek in place.
        QCOMPARE(f.s.playAt(6300), Outcome::SeekedInPlace);
    }
    void hdToggleWhilePlayingOpensTheOtherStream()
    {
        Fixture f;
        QCOMPARE(f.s.playAt(6000), Outcome::Opened);
        f.t->isStreaming = true;
        f.s.setHd(true);
        QCOMPARE(f.s.playAt(6000), Outcome::OpenedHd);
        f.s.setHd(false);
        QCOMPARE(f.s.playAt(6000), Outcome::Opened);
        QCOMPARE(f.t->seeks, 0);
    }
    // Reverse play (ticket 12): a change of direction reopens; speed within reverse doesn't;
    // a reverse run never seeks in place.
    void directionChangeReopensReverse()
    {
        Fixture f;
        QCOMPARE(f.s.playAt(6000), Outcome::Opened);
        f.t->isStreaming = true;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.s.setDirection(-1);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.playAt(6000), Outcome::Opened);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanReverse);
        f.s.setSpeed(4.0);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.playAt(5000), Outcome::Opened); // a jump backwards reopens
        QCOMPARE(f.t->seeks, 0);
        f.s.setDirection(1);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(f.s.playAt(5000), Outcome::Opened);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanSub);
    }
    void hdOnDirectRowDoesNotReopen()
    {
        Fixture f;
        f.t->isDirect = true;
        f.t->isStreaming = true;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.s.setHd(true);
        QCOMPARE(spy.count(), 0);
    }
    void stopStops()
    {
        Fixture f;
        f.s.stop();
        QCOMPARE(f.t->stops, 1);
    }
};

QTEST_MAIN(TestPlaybackSession)
#include "test_playbacksession.moc"
