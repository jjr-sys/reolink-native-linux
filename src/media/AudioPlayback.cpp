#include "AudioPlayback.h"

#include "AudioDecoder.h"
#include "core/Log.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

namespace rl {

namespace {
// Head start before sound begins, and again after any dry spell. Big enough to ride out a
// video-decode stall or a burst-and-gap arrival pattern; small enough to feel live.
constexpr int kPrebufferMs = 400;
// If sound still runs dry, the head start grows a little each time, up to this.
constexpr int kMaxPrebufferMs = 900;
// Ceiling on queued audio. Past it the oldest is dropped so a long stall followed by a
// burst cannot leave the sound seconds behind the picture.
constexpr int kMaxBufferMs = 1500;
// The sound card's own buffer: kept short because the jitter buffer does the absorbing.
constexpr int kSinkBufferMs = 100;

constexpr qint64 msToBytes(int ms)
{
    return qint64(AudioDecoder::kOutRate) * ms / 1000 * AudioDecoder::kBytesPerFrame;
}

// What the sound card reads from. It always returns the full request (silence while the
// jitter buffer is filling), so the card never starves and never has to restart.
class Feed : public QIODevice
{
public:
    explicit Feed(AudioJitterBuffer &buf) : m_buf(buf) { open(QIODevice::ReadOnly); }
    qint64 bytesAvailable() const override { return kAlwaysReady + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 maxlen) override
    {
        m_buf.read(data, maxlen);
        return maxlen;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    static constexpr qint64 kAlwaysReady = 1 << 20;
    AudioJitterBuffer &m_buf;
};
} // namespace

AudioPlayback::AudioPlayback(QObject *parent)
    : QObject(parent),
      m_buf({msToBytes(kPrebufferMs), msToBytes(kMaxBufferMs), AudioDecoder::kBytesPerFrame,
             msToBytes(kMaxPrebufferMs)})
{
    // The default device moved (headphones plugged in, Bluetooth connected): drop the
    // sink so the next chunk reopens on the new default.
    connect(&m_devices, &QMediaDevices::audioOutputsChanged, this, [this] { stop(); });
}

AudioPlayback::~AudioPlayback() { logSummary(); }

bool AudioPlayback::ensureSink()
{
    if (m_sink && m_sink->state() != QAudio::StoppedState)
        return true;

    m_sink.reset();

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

    m_feed = std::make_unique<Feed>(m_buf);
    m_sink = std::make_unique<QAudioSink>(device, format);
    m_sink->setBufferSize(int(msToBytes(kSinkBufferMs)));
    m_sink->start(m_feed.get()); // pull mode: the card reads from the jitter buffer
    if (m_sink->error() != QAudio::NoError) {
        qCWarning(lcMedia) << "audio: could not start output on" << device.description();
        m_sink.reset();
        m_feed.reset();
        return false;
    }
    qCInfo(lcMedia) << "audio: playing on" << device.description();
    return true;
}

void AudioPlayback::write(const QByteArray &pcm)
{
    if (pcm.isEmpty() || !ensureSink())
        return;
    m_buf.write(pcm.constData(), pcm.size());
}

void AudioPlayback::stop()
{
    logSummary();
    if (m_sink)
        m_sink->stop();
    m_sink.reset();
    m_feed.reset();
    m_buf.reset();
}

void AudioPlayback::logSummary() const
{
    if (m_buf.underruns() > 0 || m_buf.droppedBytes() > 0)
        qCInfo(lcMedia) << "audio: session ended with" << m_buf.underruns() << "gaps ("
                        << m_buf.silenceBytes() * 1000 / msToBytes(1000) << "ms of silence ) and"
                        << m_buf.droppedBytes() * 1000 / msToBytes(1000) << "ms dropped";
}

void AudioPlayback::setDelayMs(int ms)
{
    if (ms <= 0) {
        m_buf.configure({msToBytes(kPrebufferMs), msToBytes(kMaxBufferMs), AudioDecoder::kBytesPerFrame,
                         msToBytes(kMaxPrebufferMs)});
        return;
    }
    // The sound card's own buffer adds latency, so start that much earlier; then no learning.
    const qint64 pre = msToBytes(qMax(0, ms - kSinkBufferMs));
    m_buf.configure({pre, pre + msToBytes(1500), AudioDecoder::kBytesPerFrame, 0});
}

qint64 AudioPlayback::queuedBytes() const
{
    return m_buf.queuedBytes();
}

int AudioPlayback::underruns() const
{
    return m_buf.underruns();
}

} // namespace rl
