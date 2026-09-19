#pragma once

#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cstring>

namespace rl {

// Sits between the decoder (which delivers audio in bursts, on the GUI thread) and the
// sound card (which consumes it at a steady rate, on its own thread).
//
// Playback waits until `prebufferBytes` are queued before any sound comes out, and after
// the queue runs dry it waits again. That trades a fixed delay for not stuttering: a late
// or bunched-up chunk is absorbed instead of leaving a gap. read() always fills the whole
// request, with silence while (re)buffering, so the sound card never starves or restarts.
// If the queue grows past `maxBytes` (a long stall, then a burst) the oldest audio is
// dropped so the delay cannot build up without limit.
//
// Thread-safe: write() and read() may run on different threads.
class AudioJitterBuffer
{
public:
    struct Config {
        qint64 prebufferBytes = 0;    // queued audio needed before playback (re)starts
        qint64 maxBytes = 0;          // hard ceiling on queued audio
        int frameBytes = 4;           // drops stay aligned to whole sample frames
        qint64 maxPrebufferBytes = 0; // if > prebufferBytes: each underrun raises the head start
                                      // by half of prebufferBytes, up to this; 0 keeps it fixed
    };

    explicit AudioJitterBuffer(Config config) : m_cfg(config), m_prebuffer(config.prebufferBytes) {}

    void write(const char *data, qint64 len)
    {
        if (len <= 0)
            return;
        QMutexLocker lock(&m_mutex);
        m_buf.append(data, len);
        const qint64 avail = m_buf.size() - m_pos;
        if (avail > m_cfg.maxBytes) {
            // Keep two thirds of the ceiling so a sustained overrun drops in a few large
            // steps rather than clipping every chunk.
            qint64 drop = avail - m_cfg.maxBytes * 2 / 3;
            drop -= drop % m_cfg.frameBytes;
            m_pos += drop;
            m_droppedBytes += drop;
        }
        if (m_pos > kCompactAt) {
            m_buf.remove(0, int(m_pos));
            m_pos = 0;
        }
    }

    // Fill exactly `len` bytes for the sound card.
    void read(char *dst, qint64 len)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_playing && m_buf.size() - m_pos >= m_prebuffer && m_buf.size() > m_pos) {
            m_playing = true;
            m_started = true;
        }
        qint64 n = 0;
        if (m_playing) {
            n = std::min<qint64>(len, m_buf.size() - m_pos);
            std::memcpy(dst, m_buf.constData() + m_pos, size_t(n));
            m_pos += n;
            if (n < len) { // ran dry: fall silent and re-buffer rather than dribble
                m_playing = false;
                ++m_underruns;
                // It ran dry, so this connection needs a bigger cushion: ask for more next time.
                m_prebuffer = std::min(std::max(m_prebuffer, m_cfg.maxPrebufferBytes),
                                       m_prebuffer + m_cfg.prebufferBytes / 2);
            }
        }
        if (n < len) {
            std::memset(dst + n, 0, size_t(len - n));
            if (m_started)
                m_silenceBytes += len - n;
        }
    }

    // Change the cushion sizes (a new stream mode). Keeps queued audio; the head start
    // restarts from the new value.
    void configure(Config config)
    {
        QMutexLocker lock(&m_mutex);
        m_cfg = config;
        m_prebuffer = config.prebufferBytes;
    }

    void reset()
    {
        QMutexLocker lock(&m_mutex);
        m_buf.clear();
        m_pos = 0;
        m_playing = false;
        m_started = false;
        m_underruns = 0; // counters describe one session; the learned head start is kept
        m_droppedBytes = 0;
        m_silenceBytes = 0;
    }

    // Current head start (grows after underruns when adaptive).
    qint64 prebufferBytes() const { QMutexLocker l(&m_mutex); return m_prebuffer; }
    qint64 queuedBytes() const { QMutexLocker l(&m_mutex); return m_buf.size() - m_pos; }
    bool playing() const { QMutexLocker l(&m_mutex); return m_playing; }
    // Times the queue ran dry after playback had begun (each one is an audible gap).
    int underruns() const { QMutexLocker l(&m_mutex); return m_underruns; }
    // Audio discarded because the queue passed its ceiling.
    qint64 droppedBytes() const { QMutexLocker l(&m_mutex); return m_droppedBytes; }
    // Silence played after playback had begun (re-buffering time).
    qint64 silenceBytes() const { QMutexLocker l(&m_mutex); return m_silenceBytes; }

private:
    static constexpr qint64 kCompactAt = 256 * 1024;

    Config m_cfg;
    qint64 m_prebuffer;
    mutable QMutex m_mutex;
    QByteArray m_buf;
    qint64 m_pos = 0;
    bool m_playing = false;
    bool m_started = false;
    int m_underruns = 0;
    qint64 m_droppedBytes = 0;
    qint64 m_silenceBytes = 0;
};

} // namespace rl
