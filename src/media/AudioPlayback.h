#pragma once

#include "AudioJitterBuffer.h"

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
// Camera audio reaches us in bursts (the demuxer reads in blocks, and video decode or a
// busy GUI thread can hold audio back), while the sound card wants a steady stream.
// Chunks therefore go into an AudioJitterBuffer that waits for a short head start before
// sound comes out and re-buffers after any dry spell. The cost is a fixed delay of about
// half a second, in exchange for continuous sound instead of gaps.
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

    // Bytes waiting in the jitter buffer (0 when stopped).
    qint64 queuedBytes() const;
    // Times sound ran out after it had started (each is an audible gap).
    int underruns() const;

private:
    bool ensureSink();
    void logSummary() const;

    QMediaDevices m_devices; // follows the default output changing (e.g. Bluetooth)
    AudioJitterBuffer m_buf;
    std::unique_ptr<QIODevice> m_feed; // the sound card pulls from this
    std::unique_ptr<QAudioSink> m_sink; // declared last: destroyed first, before the feed
};

} // namespace rl
