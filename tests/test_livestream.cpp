#include "media/LiveStream.h"

#include <QtTest>
#include <QTemporaryDir>
#include <condition_variable>
#include <map>
#include <mutex>

using namespace rl;
namespace {
using State = StreamPlayer::State;
using Mode = LiveStreamTransport::Mode;
using Task = LiveStreamScheduler::Task;

QVariantMap camera(qint64 host = 7, int channel = 0)
{
    return {{"hostId", host}, {"channel", channel}};
}

class Clock final : public LiveStreamScheduler
{
public:
    struct Entry { qint64 at; std::function<void()> callback; };
    qint64 now = 0;
    std::map<Task, Entry> pending;
    void schedule(Task task, int ms, std::function<void()> callback) override
    {
        pending[task] = {now + ms, std::move(callback)};
    }
    void cancel(Task task) override { pending.erase(task); }
    int delay(Task task) const { return int(pending.at(task).at - now); }
    void advance(int ms)
    {
        const qint64 end = now + ms;
        for (;;) {
            auto next = pending.end();
            for (auto it = pending.begin(); it != pending.end(); ++it)
                if (it->second.at <= end && (next == pending.end() || it->second.at < next->second.at))
                    next = it;
            if (next == pending.end())
                break;
            now = next->second.at;
            auto callback = std::move(next->second.callback);
            pending.erase(next);
            callback();
        }
        now = end;
    }
};

class Transport final : public LiveStreamTransport
{
public:
    struct Open { QVariantMap camera; bool main; Mode mode; quint64 attempt; };
    QVector<Open> opens;
    Description source{true, false, "rtsp://local/camera"};
    qint64 frames = 0; // pictures decoded by the current connection
    qint64 framesDecoded() const override { return frames; }
    bool exists = true;
    bool stopped = true;
    quint64 current = 0;
    Description describe(const QVariantMap &, bool) override
    {
        auto result = source;
        result.exists = exists;
        return result;
    }
    void open(const QVariantMap &camera, bool main, Mode mode, quint64 attempt) override
    {
        current = attempt;
        frames = 0;
        opens.append({camera, main, mode, attempt});
        stopped = false;
        // Packet-source replacement can synchronously publish Stopped.
        emit stateChanged(attempt, State::Stopped, {});
        emit stateChanged(attempt, State::Connecting, {});
    }
    void stop() override
    {
        stopped = true;
        emit stateChanged(current, State::Stopped, {});
    }
    void publish(State state, const QString &error = {})
    {
        emit stateChanged(current, state, error);
    }
    void fail(const QString &error = "read error / stalled")
    {
        const quint64 failed = current;
        emit stateChanged(failed, State::Error, error);
        emit stateChanged(failed, State::Stopped, {});
    }
    void update() { emit changed(); }
};

struct View {
    Transport *transport = new Transport;
    Clock *clock = new Clock;
    LiveStream stream{std::unique_ptr<LiveStreamTransport>(transport),
                      std::unique_ptr<LiveStreamScheduler>(clock)};
    void start(bool main = false)
    {
        stream.setMainStream(main);
        stream.setCamera(camera());
    }
};

// Hold a real player's worker in its packet reader until stop unblocks it.
void startBlockedSource(StreamPlayer &player)
{
    struct Gate {
        std::mutex mutex;
        std::condition_variable wake;
        bool stopped = false;
    };
    auto gate = std::make_shared<Gate>();
    player.setPacketSource([gate](unsigned char *, int) {
        std::unique_lock lock(gate->mutex);
        gate->wake.wait(lock, [&] { return gate->stopped; });
        return 0;
    }, "h264", [gate] {
        std::lock_guard lock(gate->mutex);
        gate->stopped = true;
        gate->wake.notify_all();
    });
    player.start();
}
} // namespace

class TestLiveStream : public QObject
{
    Q_OBJECT
private slots:
    // Seek in place (architecture review #2, 3a): each player seeks its own running source.
    // Playback starts every pane in turn, so a seek kept anywhere but on the stream itself
    // belonged to the last pane opened and every other pane reconnected on each scrub.
    void seekGoesToThisPlayersOwnSource()
    {
        auto blocked = [](StreamPlayer &p, std::function<bool(qint64)> onSeek) {
            auto gate = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
            auto stopped = std::make_shared<bool>(false);
            p.setPacketSource([gate, stopped](unsigned char *, int) {
                std::unique_lock lock(gate->first);
                gate->second.wait(lock, [&] { return *stopped; });
                return 0;
            }, "h264", [gate, stopped] {
                std::lock_guard lock(gate->first);
                *stopped = true;
                gate->second.notify_all();
            }, 0, std::move(onSeek));
        };
        StreamPlayer a, b, none;
        QList<qint64> seenA, seenB;
        blocked(a, [&seenA](qint64 e) { seenA << e; return true; });
        blocked(b, [&seenB](qint64 e) { seenB << e; return true; });
        blocked(none, {});
        QVERIFY(!a.seekSource(100));   // not started: nothing running to seek
        a.start();
        b.start();
        none.start();
        QVERIFY(a.seekSource(1000));
        QVERIFY(b.seekSource(2000));
        QVERIFY(!none.seekSource(3000)); // a source without seek (live, the live buffer)
        QCOMPARE(seenA, QList<qint64>{1000});
        QCOMPARE(seenB, QList<qint64>{2000});
        a.stop();
        QVERIFY(!a.seekSource(1500));    // stopped: reopen instead
        QCOMPARE(seenA, QList<qint64>{1000});
        a.setSource(QStringLiteral("rtsp://local/x")); // a URL source never seeks
        QVERIFY(!a.seekSource(1600));
    }
    void directAndFrigateMainUseUrl_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("direct") << "rtsp://local/direct";
        QTest::newRow("frigate") << "rtsp://local:8554/front_door";
    }
    void directAndFrigateMainUseUrl()
    {
        QFETCH(QString, url);
        View v;
        v.transport->source.direct = true;
        v.transport->source.url = url;
        v.start(true);
        QCOMPARE(v.transport->opens.size(), 1);
        QCOMPARE(v.transport->opens.last().mode, Mode::Url);
        QVERIFY(!v.stream.degraded());
        QVERIFY(v.clock->pending.empty());
    }

    // A silent refusal: the stream opens and no picture ever comes. "No picture" shows
    // after 8 s and the stream is reopened at 8, 16, 32, then every 60 s.
    void noPictureReopensAtGrowingIntervals()
    {
        View v;
        v.stream.setWatchPicture(true);
        v.start();
        QCOMPARE(v.transport->opens.size(), 1);
        v.clock->advance(7999);
        QVERIFY(!v.stream.noPicture());
        QCOMPARE(v.transport->opens.size(), 1);
        v.clock->advance(1);
        QVERIFY(v.stream.noPicture());
        QCOMPARE(v.transport->opens.size(), 2); // t = 8 s
        const QVector<int> waits{16000, 32000, 60000, 60000};
        for (int ms : waits) {
            v.clock->advance(ms - 1);
            const int before = int(v.transport->opens.size());
            v.clock->advance(1);
            QCOMPARE(int(v.transport->opens.size()), before + 1); // 24, 56, 116, 176 s
            QVERIFY(v.stream.noPicture());
        }
        // A picture: all well, and the next stall waits 8 s again.
        v.transport->frames = 3;
        v.clock->advance(1000);
        QVERIFY(!v.stream.noPicture());
        const int opened = int(v.transport->opens.size());
        v.clock->advance(7999);
        QCOMPARE(int(v.transport->opens.size()), opened);
        v.clock->advance(1);
        QCOMPARE(int(v.transport->opens.size()), opened + 1);
    }
    // Pictures stop arriving after a while: 8 s after the last one counts too.
    void noPictureAfterTheLastFrame()
    {
        View v;
        v.stream.setWatchPicture(true);
        v.start();
        v.transport->publish(State::Streaming);
        for (int t = 0; t < 20; ++t) {
            ++v.transport->frames;
            v.clock->advance(1000);
        }
        QCOMPARE(v.transport->opens.size(), 1);
        v.clock->advance(8000);
        QVERIFY(v.stream.noPicture());
        QCOMPARE(v.transport->opens.size(), 2);
        // The reopened connection counts its own pictures from 0: its first one clears it.
        v.transport->frames = 1;
        v.clock->advance(1000);
        QVERIFY(!v.stream.noPicture());
        QCOMPARE(v.transport->opens.size(), 2);
    }
    // While its pane replays (live-replay ticket 08b) the stream stays connected, receiving
    // into the live buffer with no picture: no "No picture", no reopen; the watch resumes after.
    void receiveOnlyKeepsTheConnectionWithoutWatching()
    {
        View v;
        v.stream.setWatchPicture(true);
        v.start();
        v.transport->publish(State::Streaming);
        v.transport->frames = 5;
        v.clock->advance(1000);
        v.stream.setReceiveOnly(true);
        QVERIFY(v.transport->player.receiveOnly());
        QVERIFY(v.transport->player.muted());
        v.clock->advance(60000);
        QVERIFY(!v.stream.noPicture());
        QCOMPARE(v.transport->opens.size(), 1);
        QVERIFY(!v.transport->stopped);
        v.stream.setReceiveOnly(false);
        QVERIFY(!v.transport->player.receiveOnly());
        v.clock->advance(7999);
        QCOMPARE(v.transport->opens.size(), 1);
        v.clock->advance(1);
        QVERIFY(v.stream.noPicture());   // no picture 8 s after replay ended: watched again
    }
    // Hiding the pane, leaving the page or changing the stream on purpose forgets it.
    void noPictureEndsWithTheView()
    {
        View v;
        v.stream.setWatchPicture(true);
        v.start();
        v.clock->advance(8000);
        QVERIFY(v.stream.noPicture());
        v.stream.setViewVisible(false);
        QVERIFY(!v.stream.noPicture());
        QVERIFY(!v.clock->pending.count(Task::Watch));
        v.stream.setViewVisible(true);
        v.clock->advance(7999);
        QVERIFY(!v.stream.noPicture());
        v.stream.setMainStream(true);
        v.clock->advance(7999);
        QVERIFY(!v.stream.noPicture());
        v.clock->advance(1);
        QVERIFY(v.stream.noPicture());
        v.stream.setActive(false);
        QVERIFY(!v.stream.noPicture());
        QVERIFY(!v.clock->pending.count(Task::Watch));
    }
    // Shown again within the 5 s grace, the same connection is kept, and watched again.
    void noPictureWatchResumesAfterAShortHide()
    {
        View v;
        v.stream.setWatchPicture(true);
        v.start();
        v.clock->advance(2000);
        v.stream.setViewVisible(false);
        v.clock->advance(2000);
        v.stream.setViewVisible(true);
        QCOMPARE(v.transport->opens.size(), 1);
        v.clock->advance(8000);
        QVERIFY(v.stream.noPicture());
        QCOMPARE(v.transport->opens.size(), 2);
    }
    // A camera that is gone or still waiting for its recorder says so, not "No picture".
    void noPictureLeavesUnavailableAlone()
    {
        View v;
        v.stream.setWatchPicture(true);
        v.start();
        v.transport->exists = false;
        v.transport->update();
        v.clock->advance(120000);
        QVERIFY(!v.stream.noPicture());
        QCOMPARE(v.transport->opens.size(), 1);
        QCOMPARE(v.stream.state(), State::Stopped);
    }

    // Off unless asked for: other views keep their own recovery only.
    void noPictureWatchIsOptIn()
    {
        View v;
        v.start();
        v.clock->advance(120000);
        QCOMPARE(v.transport->opens.size(), 1);
        QVERIFY(!v.stream.noPicture());
    }

    void mainRetriesThenFallbackRemainsUntilQualityChanges()
    {
        View v;
        v.start(true);
        QCOMPARE(v.transport->opens.last().mode, Mode::BaichuanMain);
        for (int n = 0; n < 3; ++n) {
            v.transport->fail();
            QVERIFY(v.stream.recovering());
            QCOMPARE(v.clock->delay(Task::Retry), 1500);
            v.clock->advance(1500);
            QCOMPARE(v.transport->opens.last().mode, Mode::BaichuanMain);
        }
        v.transport->fail();
        QCOMPARE(v.transport->opens.size(), 5);
        QCOMPARE(v.transport->opens.last().mode, Mode::Url);
        QVERIFY(v.stream.degraded());
        v.transport->publish(State::Streaming);
        v.clock->advance(120000);
        v.transport->update();
        QCOMPARE(v.transport->opens.size(), 5);
        QVERIFY(v.stream.degraded());
        v.stream.setMainStream(false);
        v.stream.setMainStream(true);
        QVERIFY(!v.stream.degraded());
        QCOMPARE(v.transport->opens.last().mode, Mode::BaichuanMain);
    }

    void successfulFramesResetMainRetryBudget()
    {
        View v;
        v.start(true);
        for (int n = 0; n < 3; ++n) {
            v.transport->fail();
            v.clock->advance(1500);
        }
        v.transport->publish(State::Streaming);
        v.transport->fail();
        QVERIFY(!v.stream.degraded());
        QVERIFY(v.clock->pending.contains(Task::Retry));
    }

    void substreamKeepsRecoveringWithCappedBackoff()
    {
        View v;
        v.start();
        v.transport->fail("Stream not found: 404");
        QCOMPARE(v.transport->opens.last().mode, Mode::BaichuanSub);
        const QVector<int> delays{1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
        for (int ms : delays) {
            v.transport->fail();
            QCOMPARE(v.clock->delay(Task::Retry), ms);
            v.clock->advance(ms);
            QCOMPARE(v.transport->opens.last().mode, Mode::BaichuanSub);
        }
        v.transport->publish(State::Streaming);
        QVERIFY(!v.stream.recovering());
        v.transport->fail();
        QCOMPARE(v.clock->delay(Task::Retry), 1000);
    }

    void releaseCancelsRetryAndRejectsLateAttempt()
    {
        View v;
        v.start(true);
        const quint64 old = v.transport->current;
        v.transport->fail();
        auto lateRetry = v.clock->pending.at(Task::Retry).callback;
        v.stream.release();
        QVERIFY(v.clock->pending.empty());
        QVERIFY(v.transport->stopped);
        lateRetry();
        emit v.transport->stateChanged(old, State::Streaming, {});
        v.clock->advance(60000);
        QCOMPARE(v.transport->opens.size(), 1);
        QCOMPARE(v.stream.state(), State::Stopped);
        v.transport->update();
        QCOMPARE(v.transport->opens.size(), 1);
    }

    void pageExitCancelsRecoveryAndReentryStartsFresh()
    {
        View v;
        v.start(true);
        v.transport->fail();
        v.stream.setActive(false);
        v.clock->advance(60000);
        QCOMPARE(v.transport->opens.size(), 1);
        v.stream.setActive(true);
        QCOMPARE(v.transport->opens.size(), 2);
        QCOMPARE(v.transport->opens.last().mode, Mode::BaichuanMain);
    }

    void hiddenGraceIsFixedSilentAndReusesStream()
    {
        View v;
        v.start();
        v.transport->publish(State::Streaming);
        v.stream.setMuted(false);
        QVERIFY(!v.stream.muted());
        v.stream.setViewVisible(false);
        QVERIFY(v.stream.muted());
        v.clock->advance(2000);
        v.transport->update();
        QCOMPARE(v.clock->delay(Task::Hide), 3000);
        v.stream.setViewVisible(true);
        QVERIFY(!v.stream.muted());
        QCOMPARE(v.transport->opens.size(), 1);
        v.stream.setViewVisible(false);
        v.clock->advance(4999);
        QVERIFY(!v.transport->stopped);
        v.clock->advance(1);
        QVERIFY(v.transport->stopped);
        QCOMPARE(v.stream.state(), State::Stopped);
        v.transport->update();
        QCOMPARE(v.transport->opens.size(), 1);
        v.stream.setViewVisible(true);
        QCOMPARE(v.transport->opens.size(), 2);
    }

    void hiddenFailureDoesNotOpenAnotherStream()
    {
        View v;
        v.start(true);
        v.stream.setViewVisible(false);
        v.transport->fail();
        v.clock->advance(60000);
        QCOMPARE(v.transport->opens.size(), 1);
        QVERIFY(v.transport->stopped);
        v.stream.setViewVisible(true);
        QCOMPARE(v.transport->opens.size(), 2);
    }

    void hiddenQualityChangeDoesNotExtendGraceOrOpenAStream()
    {
        View v;
        v.start();
        v.stream.setViewVisible(false);
        v.clock->advance(2000);
        v.stream.setMainStream(true);
        QCOMPARE(v.clock->delay(Task::Hide), 3000);
        QCOMPARE(v.transport->opens.size(), 1);
        v.clock->advance(3000);
        QVERIFY(v.transport->stopped);
        v.stream.setViewVisible(true);
        QCOMPARE(v.transport->opens.size(), 2);
        QCOMPARE(v.transport->opens.last().mode, Mode::BaichuanMain);
    }

    void initialHiddenViewDoesNotConnect()
    {
        View v;
        v.stream.classBegin();
        v.start(true);
        v.stream.setViewVisible(false);
        v.stream.componentComplete();
        QVERIFY(v.transport->opens.empty());
        v.stream.setViewVisible(true);
        QCOMPARE(v.transport->opens.size(), 1);
    }

    void cameraAndQualityReplacementRejectOldCallbacks()
    {
        View v;
        v.start(true);
        v.transport->fail();
        const quint64 old = v.transport->current;
        auto lateRetry = v.clock->pending.at(Task::Retry).callback;
        v.stream.setCamera(camera(8, 3));
        QCOMPARE(v.transport->opens.last().camera, camera(8, 3));
        lateRetry();
        emit v.transport->stateChanged(old, State::Error, "old failure");
        emit v.transport->stateChanged(old, State::Streaming, {});
        QCOMPARE(v.stream.state(), State::Connecting);
        QVERIFY(v.stream.errorString().isEmpty());
        QCOMPARE(v.transport->opens.size(), 2);
        QVERIFY(v.clock->pending.empty());
        v.stream.setMainStream(false);
        QCOMPARE(v.transport->opens.size(), 3);
        QVERIFY(v.clock->pending.empty()); // synchronous replacement stop is intentional
    }

    void cameraDisappearanceStopsEvenWhileHidden()
    {
        View v;
        v.start();
        v.stream.setViewVisible(false);
        v.transport->exists = false;
        v.transport->update();
        QVERIFY(v.transport->stopped);
        QCOMPARE(v.stream.camera(), camera());
        QVERIFY(v.clock->pending.empty());
        v.transport->exists = true;
        v.transport->update();
        QCOMPARE(v.transport->opens.size(), 1);
        v.stream.setViewVisible(true);
        QCOMPARE(v.transport->opens.last().camera, camera());
    }

    void terminalErrorsAndFiniteSourcesDoNotLoop()
    {
        View v;
        v.start(true);
        v.transport->fail("unsupported codec");
        QVERIFY(!v.stream.recovering());
        QVERIFY(v.clock->pending.empty());
        v.transport->update();
        QCOMPARE(v.transport->opens.size(), 1);
        v.stream.setCamera(camera(8));
        QCOMPARE(v.transport->opens.size(), 2);

        View direct;
        direct.transport->source.direct = true;
        direct.transport->source.url = "/tmp/finite.mp4";
        direct.start();
        direct.transport->publish(State::Stopped);
        direct.transport->update();
        QCOMPARE(direct.transport->opens.size(), 1);
        QVERIFY(direct.clock->pending.empty());
    }

    void queuedPlayerDeliveriesCannotChangeReplacement()
    {
        // Keep the replacement's real worker blocked while already-queued old
        // callbacks arrive. This exercises the delivery checks, not a copied policy.
        StreamPlayer player;
        player.setMuted(false);
        auto old = std::make_shared<StreamPlayer::Session>();
        QSignalSpy audio(&player, &StreamPlayer::hasAudioChanged);
        QSignalSpy ticks(&player, &StreamPlayer::framesDecodedChanged);
        QSignalSpy saved(&player, &StreamPlayer::recordingSaved);
        QMetaObject::invokeMethod(&player, [&player, old] {
            player.applyStateFromWorker(old, State::Error, "obsolete error");
            player.applyHasAudioFromWorker(old, true);
            player.applyAudioFromWorker(old, QByteArray(100, '\0'));
            player.applyAudioFlush(old);
            player.applyFramesTickFromWorker(old);
            player.applyRecordingState(old, true, {}, {});
            player.applyRecordingState(old, false, "old-clip.mp4", {});
        }, Qt::QueuedConnection);
        startBlockedSource(player);
        QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);
        QCOMPARE(player.state(), State::Connecting);
        QVERIFY(player.errorString().isEmpty());
        QVERIFY(!player.hasAudio());
        QVERIFY(!player.recording());
        QCOMPARE(audio.size(), 0);
        QCOMPARE(ticks.size(), 0);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().first().toString(), QString("old-clip.mp4"));
        player.stop();
    }

    void previousBaichuanAudioFeedCannotAnnounceReplacementTrack()
    {
        StreamPlayer player; // muted: announcing a track needs no actual AAC decode
        auto oldFeed = player.audioFeed();
        startBlockedSource(player);
        oldFeed("old track");
        QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);
        QVERIFY(player.hasAudio());
        player.stop();

        auto currentFeed = player.audioFeed();
        startBlockedSource(player);
        QVERIFY(!player.hasAudio());
        oldFeed("late old track");
        QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);
        QVERIFY(!player.hasAudio());
        currentFeed("current track");
        QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);
        QVERIFY(player.hasAudio());
        player.stop();
    }

    void productionTransportResolvesExactCameraAfterRowRemoval()
    {
        // No credentials or camera traffic: these are nonexistent local files.
        // Disable keyring access so temporary database IDs cannot touch real secrets.
        const QByteArray bus = qgetenv("DBUS_SESSION_BUS_ADDRESS");
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-livestream-test-bus");
        QTemporaryDir dir;
        Database db(dir.filePath("devices.db"), "livestream_devices");
        QVERIFY(db.open());
        HostRecord host;
        host.kind = "stream";
        host.addr = dir.filePath("first.mp4");
        const qint64 first = db.addHost(host);
        host.addr = dir.filePath("chosen.mp4");
        const qint64 chosen = db.addHost(host);
        CredentialStore credentials;
        {
            DeviceManager devices(&db, &credentials);
            LiveStream stream;
            stream.setDevices(&devices);
            stream.setMainStream(true);
            stream.setCamera(camera(chosen));
            QTRY_COMPARE(stream.state(), State::Stopped); // finite missing file
            const QString error = stream.errorString();
            QVERIFY(!error.isEmpty());
            devices.removeDevice(devices.rowOfHost(first));
            QCoreApplication::processEvents();
            QCOMPARE(stream.camera(), camera(chosen));
            QCOMPARE(stream.errorString(), error);
            // The host exists, but this particular channel does not. Never use
            // rowOfHostChannel's fallback to its first camera.
            stream.setCamera(camera(chosen, 7));
            QCOMPARE(stream.state(), State::Stopped);
            QCOMPARE(stream.errorString(), QString("Camera unavailable"));
        }
        if (bus.isNull())
            qunsetenv("DBUS_SESSION_BUS_ADDRESS");
        else
            qputenv("DBUS_SESSION_BUS_ADDRESS", bus);
    }
};
QTEST_GUILESS_MAIN(TestLiveStream)
#include "test_livestream.moc"
