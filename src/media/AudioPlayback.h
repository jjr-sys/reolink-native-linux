#pragma once

#include <QByteArray>
#include <QMediaDevices>
#include <QObject>

#include <memory>

class QAudioSink;
class QIODevice;

namespace rl {

// Plays AudioDecoder output (48 kHz stereo S16) on the default output device.
// GUI-thread object: the QAudioSink lives here, and decoded chunks are handed to
// write() from the worker via a queued call.
//
// This is live audio, so latency matters more than completeness. The sink's buffer
// is small and a chunk that does not fit is dropped rather than queued, which keeps
// sound within a fraction of a second of the picture instead of drifting behind after
// a stall.
class AudioPlayback : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlayback(QObject *parent = nullptr);
    ~AudioPlayback() override;

    // Queue PCM for playback; starts the output on first use.
    void write(const QByteArray &pcm);
    // Stop and release the output device (queued audio is discarded).
    void stop();

    // Bytes currently queued in the sink (0 when stopped).
    qint64 queuedBytes() const;

private:
    bool ensureSink();

    QMediaDevices m_devices; // follows the default output changing (e.g. Bluetooth)
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice *m_io = nullptr;
};

} // namespace rl
