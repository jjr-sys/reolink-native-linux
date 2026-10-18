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
