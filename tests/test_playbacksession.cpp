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
    void openBaichuan(int row, qint64 epoch, bool main, NativeWindows windows) override
    {
        opens.append({windows == NativeWindows::Backward
                          ? (main ? Route::BaichuanReverseMain : Route::BaichuanReverse)
                      : windows == NativeWindows::Forward ? Route::BaichuanForwardMain
                      : main                              ? Route::BaichuanMain
                                                          : Route::BaichuanSub,
                      row, epoch, {}});
    }
    void stop() override { ++stops; isStreaming = false; }
    // The live buffer (live-replay ticket 08b): what it holds, ms since the epoch.
    qint64 bufferFrom = -1, bufferTo = -1;
    struct BufferOpen { int row; qint64 epochMs; int direction; };
    QVector<BufferOpen> bufferOpens;
    bool bufferHas(int, qint64 epochMs, int) const override
    {
        return epochMs >= bufferFrom && epochMs <= bufferTo;
    }
    void openBuffer(int row, qint64 epochMs, int direction) override
    {
        bufferOpens.append({row, epochMs, direction});
    }
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

    // ---- the live buffer (live-replay ticket 08b) ----
    // A Live pane's replay plays its live buffer where the buffer holds the moment, both ways,
    // HD or not; elsewhere the recorder as before. Playback's panes never use it.
    void theLiveBufferPlaysWhereItHoldsTheMoment()
    {
        Fixture f;
        f.s.setUseLiveBuffer(true);
        f.t->bufferFrom = 1000000;
        f.t->bufferTo = 1060000;
        f.s.setDirection(-1);
        QCOMPARE(f.s.playAt(1059.5), Outcome::Opened);
        QCOMPARE(f.t->bufferOpens.size(), 1);
        QCOMPARE(f.t->bufferOpens.last().epochMs, qint64(1059500));
        QCOMPARE(f.t->bufferOpens.last().direction, -1);
        QVERIFY(f.t->opens.isEmpty());
        f.s.setDirection(1);
        f.s.playAt(1030);
        QCOMPARE(f.t->bufferOpens.last().direction, 1);
        // Before the buffer: the recorder.
        f.s.setDirection(-1);
        f.s.playAt(999);
        QCOMPARE(f.t->bufferOpens.size(), 2);
        QCOMPARE(f.t->opens.size(), 1);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanReverse);
    }

    void withoutTheLiveBufferTheRecorderPlays()
    {
        Fixture f;
        f.t->bufferFrom = 1000000;
        f.t->bufferTo = 1060000;
        f.s.playAt(1030);
        QVERIFY(f.t->bufferOpens.isEmpty());
        QCOMPARE(f.t->opens.size(), 1);
    }

    // After the buffer, a recorder stream at the same moment opens afresh (no seek in place of
    // a stream that isn't running).
    void theRecorderAfterTheBufferOpensAfresh()
    {
        Fixture f;
        f.s.setUseLiveBuffer(true);
        f.t->bufferFrom = 1000000;
        f.t->bufferTo = 1060000;
        f.s.playAt(1030);
        f.t->isStreaming = true;
        f.s.playAt(990);
        QCOMPARE(f.t->seeks, 0);
        QCOMPARE(f.t->opens.size(), 1);
    }

    // ---- transport choice ----
    void routeFollowsHdAndSpeedAndDirect()
    {
        QCOMPARE(PlaybackSession::chooseRoute(false, 1.0, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(false, 0.5, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(false, 2.0, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(true, 1.0, false), Route::BaichuanMain);
        QCOMPARE(PlaybackSession::chooseRoute(true, 0.5, false), Route::BaichuanMain);
        // Above 1x the recorder sends the main stream as keyframes only, so HD fast forward
        // fetches windows of it instead, up to 4x (wayfinder/live-replay ticket 09); above
        // that HD plays the sub stream at speed (recorder-paced, 16x keyframes only).
        QCOMPARE(PlaybackSession::chooseRoute(true, 2.0, false), Route::BaichuanForwardMain);
        QCOMPARE(PlaybackSession::chooseRoute(true, 4.0, false), Route::BaichuanForwardMain);
        QCOMPARE(PlaybackSession::chooseRoute(true, 8.0, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(true, 16.0, false), Route::BaichuanSub);
        QCOMPARE(PlaybackSession::chooseRoute(false, 4.0, false), Route::BaichuanSub);
        // Direct (Frigate) footage has one HTTP stream at 1x: HD and speed are ignored.
        QCOMPARE(PlaybackSession::chooseRoute(true, 4.0, true), Route::Http);
        // Backwards: windows of the stream (ticket 12); HD plays the main stream backwards up
        // to 4x while the link keeps up (live-replay tickets 06, 09), SD above; Frigate HTTP.
        QCOMPARE(PlaybackSession::chooseRoute(true, 1.0, false, -1), Route::BaichuanReverseMain);
        QCOMPARE(PlaybackSession::chooseRoute(true, 4.0, false, -1), Route::BaichuanReverseMain);
        QCOMPARE(PlaybackSession::chooseRoute(true, 8.0, false, -1), Route::BaichuanReverse);
        QCOMPARE(PlaybackSession::chooseRoute(true, 16.0, false, -1), Route::BaichuanReverse);
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
    // HD above 4x reopens on the sub stream (keyframes only on main at speed, ticket 10; HD
    // to 4x in windows, live-replay ticket 09); slow speeds keep HD.
    void speedAbove4xInHdReopensOnSub()
    {
        Fixture f;
        f.s.setHd(true);
        f.t->isStreaming = true;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.s.setSpeed(0.5);
        QCOMPARE(spy.count(), 0);
        f.s.setSpeed(8.0);
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
    // A link too slow for HD backwards drops the pane to SD (a reopen); the next open the
    // user makes tries HD again without a second reopen.
    void slowLinkDropsHdReverseToSdUntilTheNextOpen()
    {
        Fixture f;
        f.s.setHd(true);
        f.s.setDirection(-1);
        QCOMPARE(f.s.playAt(6000), Outcome::OpenedHd);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanReverseMain);
        f.t->isStreaming = true;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.s.setLinkSlow(true);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.route(), Route::BaichuanReverse);
        QCOMPARE(f.s.playAt(5990), Outcome::Opened);
        QSignalSpy changed(&f.s, &PlaybackSession::linkSlowChanged);
        f.s.clearLinkSlow();               // the user's next open: no reopen of its own
        QCOMPARE(spy.count(), 1);
        QCOMPARE(changed.count(), 1);      // ...but the HD chip hears of it
        QVERIFY(!f.s.linkSlow());
        QCOMPARE(f.s.playAt(5000), Outcome::OpenedHd);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanReverseMain);
    }
    // Changing direction is an open the user makes: HD is tried again.
    void directionChangeClearsTheSlowLink()
    {
        Fixture f;
        f.s.setHd(true);
        f.s.setDirection(-1);
        f.s.setLinkSlow(true);
        f.s.setDirection(1);
        QVERIFY(!f.s.linkSlow());
        f.s.setDirection(-1);
        QCOMPARE(f.s.route(), Route::BaichuanReverseMain);
    }
    // HD fast forward (live-replay ticket 09): 1x → 2x reopens on windows of the main stream,
    // 2x → 4x keeps the run, 4x → 8x reopens on the sub stream; a windowed run never seeks in
    // place (its windows are fetched ahead of the old moment).
    void hdFastForwardPlaysMainWindowsUpTo4x()
    {
        Fixture f;
        f.s.setHd(true);
        QCOMPARE(f.s.playAt(6000), Outcome::OpenedHd);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanMain);
        f.t->isStreaming = true;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        f.s.setSpeed(2.0);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.playAt(6000), Outcome::OpenedHd);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanForwardMain);
        f.s.setSpeed(4.0);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.playAt(7000), Outcome::OpenedHd); // a seek reopens
        QCOMPARE(f.t->seeks, 0);
        f.s.setSpeed(8.0);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(f.s.playAt(7000), Outcome::Opened);
        QCOMPARE(f.t->opens.last().route, Route::BaichuanSub);
    }
    // Too slow forwards (or the windows run out near now) drops to SD at speed until the user's
    // next open, as backwards; recorder-paced HD at 1x is not affected.
    void slowLinkDropsHdFastForwardToSd()
    {
        Fixture f;
        f.s.setHd(true);
        f.s.setSpeed(4.0);
        QCOMPARE(f.s.playAt(6000), Outcome::OpenedHd);
        f.t->isStreaming = true;
        QSignalSpy spy(&f.s, &PlaybackSession::reopenNeeded);
        QVERIFY(!f.s.hdHeldBack());
        f.s.setLinkSlow(true);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.s.route(), Route::BaichuanSub);
        QVERIFY(f.s.hdHeldBack());          // the HD chip says "too slow"
        f.s.setSpeed(1.0);
        QCOMPARE(f.s.route(), Route::BaichuanMain);
        QVERIFY(!f.s.hdHeldBack());         // recorder-paced HD at 1x is not held back
        f.s.setSpeed(8.0);
        QVERIFY(!f.s.hdHeldBack());         // SD at speed anyway
        f.s.setSpeed(2.0);
        QCOMPARE(f.s.route(), Route::BaichuanSub);
        QSignalSpy held(&f.s, &PlaybackSession::hdHeldBackChanged);
        f.s.clearLinkSlow();
        QCOMPARE(f.s.route(), Route::BaichuanForwardMain);
        QVERIFY(!f.s.hdHeldBack());
        QCOMPARE(held.count(), 1);
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
