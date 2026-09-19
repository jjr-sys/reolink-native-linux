#include "AudioPlayback.h"

#include "AudioDecoder.h"
#include "core/Log.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

namespace rl {

namespace {
// ~300 ms of buffer: enough to ride out scheduling jitter on a live stream, small
// enough that a full buffer (and so a dropped chunk) is never far behind the video.
constexpr int kBufferMs = 300;
constexpr int kBufferBytes = AudioDecoder::kOutRate * kBufferMs / 1000 * AudioDecoder::kBytesPerFrame;
} // namespace

AudioPlayback::AudioPlayback(QObject *parent) : QObject(parent)
{
    // The default device moved (headphones plugged in, Bluetooth connected): drop the
    // sink so the next chunk reopens on the new default.
    connect(&m_devices, &QMediaDevices::audioOutputsChanged, this, [this] { stop(); });
}

AudioPlayback::~AudioPlayback() = default;

bool AudioPlayback::ensureSink()
{
    if (m_sink && m_sink->state() != QAudio::StoppedState)
        return true;

    m_sink.reset();
    m_io = nullptr;

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        qCWarning(lcMedia) << "audio: no output device";
        return false;
    }
    QAudioFormat format;
    format.setSampleRate(AudioDecoder::kOutRate);
    format.setChannelCount(AudioDecoder::kOutChannels);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        qCWarning(lcMedia) << "audio: output device rejects 48 kHz stereo S16:"
                           << device.description();
        return false;
    }

    m_sink = std::make_unique<QAudioSink>(device, format);
    m_sink->setBufferSize(kBufferBytes);
    m_io = m_sink->start(); // push mode: we write PCM into the returned device
    if (!m_io || m_sink->error() != QAudio::NoError) {
        qCWarning(lcMedia) << "audio: could not start output on" << device.description();
        m_sink.reset();
        m_io = nullptr;
        return false;
    }
    qCInfo(lcMedia) << "audio: playing on" << device.description();
    return true;
}

void AudioPlayback::write(const QByteArray &pcm)
{
    if (pcm.isEmpty() || !ensureSink())
        return;
    // Drop what does not fit; never block the GUI thread or let latency build up.
    if (m_sink->bytesFree() < pcm.size())
        return;
    m_io->write(pcm);
}

void AudioPlayback::stop()
{
    if (m_sink)
        m_sink->stop();
    m_sink.reset();
    m_io = nullptr;
}

qint64 AudioPlayback::queuedBytes() const
{
    return m_sink ? m_sink->bufferSize() - m_sink->bytesFree() : 0;
}

} // namespace rl
