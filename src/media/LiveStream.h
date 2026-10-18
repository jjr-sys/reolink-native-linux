#pragma once

#include "LiveStreamTransport.h"
#include "device/DeviceManager.h"

#include <QQmlParserStatus>
#include <memory>

namespace rl {

// One view's live stream. QML supplies intent; transport/recovery/teardown stay here.
class LiveStream : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(rl::DeviceManager *devices READ devices WRITE setDevices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantMap camera READ camera WRITE setCamera NOTIFY cameraChanged)
    Q_PROPERTY(bool mainStream READ mainStream WRITE setMainStream NOTIFY mainStreamChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool viewVisible READ viewVisible WRITE setViewVisible NOTIFY viewVisibleChanged)
    Q_PROPERTY(QVideoSink *videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(rl::StreamPlayer::State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
    Q_PROPERTY(bool degraded READ degraded NOTIFY degradedChanged)
    Q_PROPERTY(bool recovering READ recovering NOTIFY recoveringChanged)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY hasAudioChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)

public:
    explicit LiveStream(QObject *parent = nullptr);
    // C++ only: internal transport and time adapters, never exposed as QML settings.
    LiveStream(std::unique_ptr<LiveStreamTransport> transport,
               std::unique_ptr<LiveStreamScheduler> scheduler, QObject *parent = nullptr);
    ~LiveStream() override;
    void classBegin() override { m_complete = false; }
    void componentComplete() override
    {
        m_complete = true;
        refresh();
    }

    DeviceManager *devices() const { return m_devices; }
    void setDevices(DeviceManager *devices);
    QVariantMap camera() const { return m_camera; }
    void setCamera(const QVariantMap &camera);
    bool mainStream() const { return m_main; }
    void setMainStream(bool main);
    bool active() const { return m_active; }
    void setActive(bool active);
    bool viewVisible() const { return m_visible; }
    void setViewVisible(bool visible);
    QVideoSink *videoSink() const { return m_transport->player.videoSink(); }
    void setVideoSink(QVideoSink *sink) { m_transport->player.setVideoSink(sink); }
    bool muted() const { return m_transport->player.muted(); }
    void setMuted(bool muted);
    StreamPlayer::State state() const { return m_state; }
    QString errorString() const { return m_error; }
    bool degraded() const { return m_degraded; }
    bool recovering() const { return m_recovering; }
    bool hasAudio() const { return m_transport->player.hasAudio(); }
    bool recording() const { return m_transport->player.recording(); }

    Q_INVOKABLE void release();
    Q_INVOKABLE bool startRecording() { return m_transport->player.startRecording(); }
    Q_INVOKABLE void stopRecording() { m_transport->player.stopRecording(); }

signals:
    void devicesChanged();
    void cameraChanged();
    void mainStreamChanged();
    void activeChanged();
    void viewVisibleChanged();
    void videoSinkChanged();
    void mutedChanged();
    void stateChanged();
    void degradedChanged();
    void recoveringChanged();
    void hasAudioChanged();
    void recordingChanged();
    void recordingSaved(const QString &path);
    void recordingFailed(const QString &error);

private:
    void refresh();
    void stopConnection();
    void resetPolicy();
    void updateAudio();
    void onState(quint64 attempt, StreamPlayer::State state, const QString &error);
    void setState(StreamPlayer::State state, const QString &error = {});
    void setDegraded(bool degraded);
    void setRecovering(bool recovering);
    void retry(int delayMs);

    std::unique_ptr<LiveStreamTransport> m_transport;
    std::unique_ptr<LiveStreamScheduler> m_scheduler;
    QPointer<DeviceManager> m_devices;
    QVariantMap m_camera;
    bool m_main = false;
    bool m_complete = true;
    bool m_active = true;
    bool m_visible = true;
    bool m_muted = true;
    bool m_released = false;
    bool m_open = false;
    bool m_changing = false;
    bool m_retryPending = false;
    bool m_hidePending = false;
    bool m_degraded = false;
    bool m_recovering = false;
    bool m_bcSub = false;
    bool m_terminal = false;
    int m_retries = 0;
    int m_backoffMs = 1000;
    quint64 m_attempt = 0;
    quint64 m_hideEpoch = 0;
    LiveStreamTransport::Mode m_mode = LiveStreamTransport::Mode::Url;
    QString m_key;
    StreamPlayer::State m_state = StreamPlayer::State::Idle;
    QString m_error;
};
} // namespace rl
