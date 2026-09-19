#pragma once

#include <QByteArray>

struct AVCodecContext;
struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace rl {

// Decodes one compressed camera audio stream (AAC, G.711 a-law/mu-law, PCM, ...) to a
// single fixed playback format: 48 kHz, stereo, interleaved signed 16-bit. Fixing the
// output means the audio sink never has to be renegotiated when a camera's native
// rate or channel count differs (Reolink cameras send 16 kHz mono AAC; others 8 kHz
// G.711), and a mono source is simply upmixed.
//
// Not thread-safe: one decoder belongs to one worker.
class AudioDecoder
{
public:
    static constexpr int kOutRate = 48000;
    static constexpr int kOutChannels = 2;
    static constexpr int kBytesPerFrame = kOutChannels * 2; // S16

    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    // True when libavcodec has a decoder for this stream's codec.
    static bool canDecode(const AVCodecParameters *par);

    // Open for a demuxed stream (RTSP/FLV). Codec parameters, including any
    // out-of-band AAC config from the SDP, come from `par`.
    bool open(const AVCodecParameters *par);

    // Open for a bare ADTS AAC stream (Baichuan): every frame carries its own header,
    // so no out-of-band configuration exists or is needed.
    bool openAdtsAac();

    bool isOpen() const { return m_ctx != nullptr; }

    // Decode one compressed packet; returns 0 or more output frames of PCM.
    // Undecodable input yields an empty result rather than an error: a live stream
    // tolerates a dropped frame.
    QByteArray decode(const AVPacket *packet);
    QByteArray decode(const QByteArray &frame);

private:
    bool openContext(const AVCodecParameters *par, int codecId);
    QByteArray convert(const AVFrame *frame);
    void freeResampler();

    AVCodecContext *m_ctx = nullptr;
    AVFrame *m_frame = nullptr;
    SwrContext *m_swr = nullptr;
    // Input shape the resampler was configured for; a change reconfigures it.
    int m_inFormat = -1;
    int m_inRate = 0;
    int m_inChannels = 0;
};

} // namespace rl
