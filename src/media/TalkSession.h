#pragma once

#include "protocol/BaichuanTalk.h"

#include <QObject>
#include <QString>

#include <memory>

class QAudioSource;
class QIODevice;

namespace rl {

class MicResampler;
class TalkEncoder;

// Push-to-talk to one camera: microphone -> ADPCM -> Baichuan talk session.
//
//   GUI thread   owns the QAudioSource, resamples + encodes what it captures
//   worker       one detached thread owns the network session (connect, ability,
//                config, send, reset); it is never joined from the GUI thread, so a
//                slow or dead camera cannot freeze the UI
//
// The microphone is only opened once the camera has accepted the talk config, so a
// refused session never leaves the mic hot. Half-duplex is the caller's job: mute the
// camera's own playback while active, or the speaker feeds back into the mic.
class TalkSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)

public:
    enum class State { Idle, Connecting, Talking, Error };
    Q_ENUM(State)

    // State shared with the worker thread; defined in the .cpp.
    struct Shared;

    explicit TalkSession(QObject *parent = nullptr);
    ~TalkSession() override;

    State state() const { return m_state; }
    bool active() const { return m_state == State::Connecting || m_state == State::Talking; }
    QString errorString() const { return m_error; }

    // Begin talking to `params.channel` of the device. Not a QML API — DeviceManager
    // supplies the credentials (Devices.startTalk). No-op while already active.
    void start(const BaichuanTalk::Params &params);
    Q_INVOKABLE void stop();

    // Called on the GUI thread by the worker (queued). Not part of the QML API.
    void applyReady(int sampleRate, int dataBytes);
    void applyFailed(const QString &error);

signals:
    void stateChanged();

private:
    void setState(State s, const QString &error = QString());
    void onMicData();
    void teardownMic();

    State m_state = State::Idle;
    QString m_error;
    std::shared_ptr<Shared> m_shared;
    std::unique_ptr<QAudioSource> m_mic;
    QIODevice *m_micIo = nullptr;
    std::unique_ptr<MicResampler> m_resampler;
    std::unique_ptr<TalkEncoder> m_encoder;
};

} // namespace rl
