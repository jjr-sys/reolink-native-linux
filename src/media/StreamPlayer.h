#pragma once

#include "VideoPlayoutQueue.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

#include <QByteArray>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace rl {

class AudioPlayback;

// One live/playback stream: FFmpeg demux + decode on a dedicated worker thread,
// frames delivered to a QML VideoOutput's QVideoSink (NV12/YUV420P upload path —
// DESIGN.md §5: the universal fallback; zero-copy VAAPI arrives with the GPU spike).
//
// Lifetime: the worker shares a heap-allocated Session (below). stop()/destruction
// signal abort and detach — they NEVER join on the GUI thread, so a stalled network
// read or DNS lookup can never freeze the UI. The Session outlives the StreamPlayer
// until the worker drops its reference; a mutex-guarded back-pointer makes stale
// frame/state callbacks safe no-ops after the StreamPlayer is gone.
//
// Audio: the same demux session carries the camera's audio track. It is decoded only
// while unmuted (`muted` defaults to true), so a wall of muted tiles pays nothing for
// it; the page decides which single pane is audible (DESIGN §5.3). Decoded PCM is
// handed to an AudioPlayback on the GUI thread. Recording remains video-only.
//
// Sources: rtsp:// (live, TCP, low-latency flags), or any libavformat-openable
// URL/file (used by tests and the "direct stream" device kind).
class StreamPlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    // GetEnc-declared display size of the stream being opened (optional). When the
    // decoded frame is this size transposed, the stream was transmitted rotated and
    // is corrected for display. Set alongside `source`; unset ⇒ heuristic fallback.
    Q_PROPERTY(QSize expectedSize READ expectedSize WRITE setExpectedSize NOTIFY expectedSizeChanged)
    Q_PROPERTY(QVideoSink *videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(qint64 framesDecoded READ framesDecoded NOTIFY framesDecodedChanged)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)
    Q_PROPERTY(bool retryOnError READ retryOnError WRITE setRetryOnError NOTIFY retryOnErrorChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    // Camera audio is muted unless a pane is explicitly made audible.
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    // True once the stream is known to carry an audio track we can decode.
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY hasAudioChanged)
    // Live smoothing: hold the picture back about a second and show frames on their own
    // timestamps, so a network stall or burst does not freeze and then rush the video.
    // Only affects live RTSP; playback and native Baichuan are shown as they arrive.
    // Sound is delayed by the same amount so lips stay in sync.
    Q_PROPERTY(bool smoothing READ smoothing WRITE setSmoothing NOTIFY smoothingChanged)
    // Recorded playback: sound gets a short, fixed cushion so it stays in step with the
    // (already paced) picture, instead of live view's longer, self-adjusting one.
    // Loudness, 0..1 on a perceptual scale (a volume slider's value). Survives stop().
    // Recorded-playback speed: 1 = real time; 0.25 to 8 are offered. Above 1 the picture
    // can only go as fast as the link delivers it, and sound is muted at any speed but 1.
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool playback READ playback WRITE setPlayback NOTIFY playbackChanged)

public:
    enum class State { Idle, Connecting, Streaming, Error, Stopped };
    Q_ENUM(State)

    // Shared between the StreamPlayer and its detached worker thread.
    struct Session {
        std::atomic<bool> abort{false};
        std::atomic<bool> loop{false};
        std::atomic<qint64> framesDecoded{0};
        std::atomic<bool> lastWasError{false}; // the last state posted was Error
        // Playback speed (1 = real time), shared with the player and the native client.
        std::shared_ptr<std::atomic<double>> speed = std::make_shared<std::atomic<double>>(1.0);
        // The most recent decoded picture, for saving a snapshot of what is on screen.
        QMutex lastFrameMutex;
        QVideoFrame lastFrame;
        QString source;
        QSize expectedSize; // declared size for rotation detection (see property)

        // Custom packet source (native Baichuan): when set, the demuxer reads the
        // raw elementary stream via this blocking callback instead of opening a URL.
        // forcedFormat is the libavformat demuxer name ("h264"/"hevc").
        std::function<int(unsigned char *, int)> readPacket;
        QString forcedFormat;

        // Retry on connection error even for non-live sources (playback FLV on a
        // connection-limited NVR often needs a couple of attempts).
        std::atomic<bool> retryOnError{false};

        // Audio is decoded only while wanted (= !muted). announcedAudio makes the
        // Baichuan path report the track's existence once, from its first frame.
        std::atomic<bool> audioWanted{false};
        std::atomic<bool> announcedAudio{false};

        // Live smoothing requested (see the property). Read by the worker per session.
        std::atomic<bool> smoothing{false};

        // Recording taps this same demux session (DESIGN §5.5): no second stream.
        std::atomic<bool> recordRequested{false};
        std::atomic<bool> recording{false};
        QMutex recMutex;
        QString recPath;

        // Frame/state callbacks marshal to the GUI thread only while the player
        // is alive. backMutex guards `player`; the StreamPlayer destructor nulls
        // it under the lock before detaching, closing the lifetime race.
        QMutex backMutex;
        QPointer<StreamPlayer> player;

        // The sink is owned by QML; delivery hops to the GUI thread where it lives.
        QMutex sinkMutex;
        QPointer<QVideoSink> sink;
    };

    explicit StreamPlayer(QObject *parent = nullptr);
    ~StreamPlayer() override;

    QString source() const { return m_source; }
    void setSource(const QString &source);

    QSize expectedSize() const { return m_expectedSize; }
    void setExpectedSize(const QSize &size);

    // Drive playback from a blocking packet callback (native Baichuan) instead of a
    // URL. `reader` fills a buffer with elementary-stream bytes (returns count, 0/EOF
    // to end); `format` is the demuxer ("h264"/"hevc"); `onStop` is invoked on
    // stop()/destruction to unblock and tear down the source. Set before start();
    // cleared by setSource(). Not a QML API.
    void setPacketSource(std::function<int(unsigned char *, int)> reader, const QString &format,
                         std::function<void()> onStop);

    QVideoSink *videoSink() const;
    void setVideoSink(QVideoSink *sink);

    State state() const { return m_state; }
    QString errorString() const { return m_errorString; }
    qint64 framesDecoded() const;

    bool loop() const { return m_loop; }
    void setLoop(bool loop);

    bool retryOnError() const { return m_retryOnError; }
    void setRetryOnError(bool v);

    bool muted() const { return m_muted; }
    void setMuted(bool muted);
    bool hasAudio() const { return m_hasAudio; }

    bool smoothing() const { return m_smoothing; }
    void setSmoothing(bool on);

    qreal speed() const { return m_speed->load(); }
    void setSpeed(qreal speed);
    // Shared with BaichuanClient so the native playback pacing follows the same setting.
    std::shared_ptr<std::atomic<double>> speedCell() const { return m_speed; }

    // Save the picture currently on screen as an image file. False if there is none.
    Q_INVOKABLE bool saveSnapshot(const QString &path);

    qreal volume() const { return m_volume; }
    void setVolume(qreal volume);

    bool playback() const { return m_playback; }
    void setPlayback(bool on);

    // Native Baichuan carries audio in the same byte stream as video, but the video
    // path is a raw elementary stream with no room for it. The client feeds ADTS AAC
    // frames to this callback instead; an empty array means "seek: drop what is queued"
    // (thread-safe; valid after this player is gone,
    // where it does nothing). Hand it to BaichuanClient::setAudioHandler.
    std::function<void(const QByteArray &)> audioFeed();

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

    bool recording() const;
    // Begin/stop stream-copy recording of the live session to an MP4. An empty
    // path auto-names a file in the recordings dir. Returns false if not streaming.
    Q_INVOKABLE bool startRecording(const QString &path = QString());
    Q_INVOKABLE void stopRecording();

    // Called on the GUI thread by the worker (via QMetaObject::invokeMethod).
    // Public so the worker helpers can reach it; not part of the QML API.
    void applyStateFromWorker(State state, const QString &error);
    void applyRecordingState(bool recording, const QString &path, const QString &error);
    void applyAudioFromWorker(const QByteArray &pcm);
    void applyAudioFlush();
    // A decoded frame with its camera timestamp. delayUs > 0: hold it on the playout
    // schedule; 0: show it now. `s` identifies the session so a stopped one is ignored.
    void applyFrameFromWorker(const std::shared_ptr<Session> &s, const QVideoFrame &frame,
                              qint64 ptsUs, qint64 delayUs, qint64 bytes);
    void applyHasAudio(bool hasAudio);

    struct AudioTap; // Baichuan audio entry point; defined in the .cpp

signals:
    void sourceChanged();
    void expectedSizeChanged();
    void videoSinkChanged();
    void stateChanged();
    void errorStringChanged();
    void framesDecodedChanged();
    void loopChanged();
    void retryOnErrorChanged();
    void recordingChanged();
    void mutedChanged();
    void hasAudioChanged();
    void smoothingChanged();
    void playbackChanged();
    void volumeChanged();
    void speedChanged();
    void recordingSaved(const QString &path);
    void recordingFailed(const QString &error);

private:
    void applyState(State state, const QString &error);
    void presentFrame(const QVideoFrame &frame);
    void servicePlayout();
    void resetPlayout();
    qint64 nowUs() const { return m_clock.nsecsElapsed() / 1000; }

    QString m_source;
    QSize m_expectedSize;
    std::function<int(unsigned char *, int)> m_pendingReader;
    QString m_pendingFormat;
    std::function<void()> m_pendingStop; // teardown for the not-yet-started source
    std::function<void()> m_activeStop;  // teardown for the running session's source
    bool m_loop = false;

    State m_state = State::Idle;
    QString m_errorString;
    bool m_recording = false;
    bool m_retryOnError = false;
    bool m_muted = true;
    bool m_hasAudio = false;
    bool m_smoothing = false;
    bool m_playback = false;
    qreal m_volume = 1.0;
    std::shared_ptr<std::atomic<double>> m_speed = std::make_shared<std::atomic<double>>(1.0);
    int m_liveDelayMs = 0; // the picture delay in force; 0 = none. Sound follows it.
    QElapsedTimer m_clock;
    QTimer m_playoutTimer;
    VideoPlayoutQueue<QVideoFrame> m_playout;
    std::unique_ptr<AudioPlayback> m_audioOut; // created on the first audible chunk
    std::shared_ptr<AudioTap> m_tap;           // lazily created by audioFeed()

    // Set via QML before start(); copied into each new Session. Survives stop().
    QPointer<QVideoSink> m_pendingSink;

    // The current session; replaced on each start(). Held so we can signal abort
    // and read framesDecoded. The worker holds its own shared_ptr copy.
    std::shared_ptr<Session> m_session;
};

} // namespace rl
