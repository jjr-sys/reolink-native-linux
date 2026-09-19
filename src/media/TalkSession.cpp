#include "TalkSession.h"

#include "TalkCodec.h"
#include "core/Log.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QMediaDevices>
#include <QMutex>
#include <QWaitCondition>

#include <deque>
#include <thread>

namespace rl {

namespace {
// Bound on encoded blocks waiting for the network. Live speech: if the link stalls,
// old audio is worthless, so the oldest is dropped rather than letting delay build.
constexpr int kMaxQueuedBlocks = 24;
} // namespace

struct TalkSession::Shared {
    QMutex mutex;
    QWaitCondition cv;
    std::deque<QByteArray> queue;
    bool abort = false;
    TalkSession *owner = nullptr; // nulled under `mutex` before the session goes away
};

namespace {

using Shared = TalkSession::Shared;

void postReady(const std::shared_ptr<Shared> &s, int rate, int dataBytes)
{
    QMutexLocker lock(&s->mutex);
    if (!s->owner)
        return;
    TalkSession *o = s->owner;
    QMetaObject::invokeMethod(o, [o, rate, dataBytes] { o->applyReady(rate, dataBytes); },
                              Qt::QueuedConnection);
}

void postFailed(const std::shared_ptr<Shared> &s, const QString &why)
{
    QMutexLocker lock(&s->mutex);
    if (!s->owner)
        return;
    TalkSession *o = s->owner;
    QMetaObject::invokeMethod(o, [o, why] { o->applyFailed(why); }, Qt::QueuedConnection);
}

// The network side of a talk session. Owns the connection for its whole life and
// always ends it with a TalkReset, so the camera's speaker is released even when
// the GUI has long since moved on.
void runTalkWorker(std::shared_ptr<Shared> s, BaichuanTalk::Params params)
{
    BaichuanTalk talk(params);
    QString error;
    if (!talk.open(&error)) {
        postFailed(s, error);
        return;
    }
    {
        QMutexLocker lock(&s->mutex);
        if (s->abort)
            return; // stopped while connecting; ~BaichuanTalk resets and closes
    }
    postReady(s, talk.format().sampleRate, talk.format().dataBytes());

    for (;;) {
        QByteArray block;
        {
            QMutexLocker lock(&s->mutex);
            while (s->queue.empty() && !s->abort)
                s->cv.wait(&s->mutex, 250);
            if (s->abort)
                break;
            block = std::move(s->queue.front());
            s->queue.pop_front();
        }
        if (!talk.send(block, &error)) {
            postFailed(s, error);
            break;
        }
    }
    talk.close();
}

MicSampleFormat toMicFormat(QAudioFormat::SampleFormat f, bool *ok)
{
    *ok = true;
    switch (f) {
    case QAudioFormat::UInt8:
        return MicSampleFormat::UInt8;
    case QAudioFormat::Int16:
        return MicSampleFormat::Int16;
    case QAudioFormat::Int32:
        return MicSampleFormat::Int32;
    case QAudioFormat::Float:
        return MicSampleFormat::Float;
    default:
        *ok = false;
        return MicSampleFormat::Int16;
    }
}

} // namespace

TalkSession::TalkSession(QObject *parent) : QObject(parent) {}

TalkSession::~TalkSession()
{
    teardownMic();
    if (m_shared) {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->owner = nullptr;
        m_shared->abort = true;
        m_shared->cv.wakeAll();
    }
}

void TalkSession::setState(State s, const QString &error)
{
    if (m_state == s && m_error == error)
        return;
    m_state = s;
    m_error = error;
    emit stateChanged();
}

void TalkSession::start(const BaichuanTalk::Params &params)
{
    if (active())
        return;
    if (QMediaDevices::defaultAudioInput().isNull()) {
        setState(State::Error, tr("No microphone found"));
        return;
    }

    m_shared = std::make_shared<Shared>();
    m_shared->owner = this;
    setState(State::Connecting);
    std::thread(runTalkWorker, m_shared, params).detach();
}

void TalkSession::stop()
{
    teardownMic();
    if (m_shared) {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->owner = nullptr; // late worker callbacks become no-ops
        m_shared->abort = true;
        m_shared->cv.wakeAll();
        lock.unlock();
        m_shared.reset();
    }
    if (m_state != State::Idle)
        setState(State::Idle);
}

void TalkSession::teardownMic()
{
    if (m_mic)
        m_mic->stop();
    m_mic.reset();
    m_micIo = nullptr;
    m_resampler.reset();
    m_encoder.reset();
}

void TalkSession::applyReady(int sampleRate, int dataBytes)
{
    if (m_state != State::Connecting)
        return; // stopped in the meantime

    const QAudioDevice dev = QMediaDevices::defaultAudioInput();
    const QAudioFormat fmt = dev.preferredFormat();
    bool ok = false;
    const MicSampleFormat micFmt = toMicFormat(fmt.sampleFormat(), &ok);
    m_resampler = std::make_unique<MicResampler>(fmt.sampleRate(), fmt.channelCount(), micFmt,
                                                 sampleRate);
    if (!ok || !m_resampler->isValid()) {
        applyFailed(tr("Microphone format not supported"));
        return;
    }
    m_encoder = std::make_unique<TalkEncoder>(dataBytes);

    m_mic = std::make_unique<QAudioSource>(dev, fmt);
    m_micIo = m_mic->start(); // pull mode: readyRead delivers captured PCM
    if (!m_micIo || m_mic->error() != QAudio::NoError) {
        applyFailed(tr("Could not open the microphone"));
        return;
    }
    connect(m_micIo, &QIODevice::readyRead, this, &TalkSession::onMicData);
    qCInfo(lcMedia) << "talk: microphone" << dev.description() << fmt.sampleRate() << "Hz"
                    << fmt.channelCount() << "ch ->" << sampleRate << "Hz mono ADPCM";
    setState(State::Talking);
}

void TalkSession::onMicData()
{
    if (!m_micIo || !m_resampler || !m_encoder || !m_shared)
        return;
    const QByteArray raw = m_micIo->readAll();
    const QByteArray mono = m_resampler->convert(raw);
    const QList<QByteArray> blocks = m_encoder->push(mono);
    if (blocks.isEmpty())
        return;
    QMutexLocker lock(&m_shared->mutex);
    for (const QByteArray &b : blocks) {
        if (static_cast<int>(m_shared->queue.size()) >= kMaxQueuedBlocks)
            m_shared->queue.pop_front(); // keep it live: drop the stalest audio
        m_shared->queue.push_back(b);
    }
    m_shared->cv.wakeOne();
}

void TalkSession::applyFailed(const QString &error)
{
    if (m_state == State::Idle)
        return;
    teardownMic();
    if (m_shared) {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->owner = nullptr;
        m_shared->abort = true;
        m_shared->cv.wakeAll();
        lock.unlock();
        m_shared.reset();
    }
    setState(State::Error, error);
}

} // namespace rl
