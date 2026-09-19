#include "AudioDecoder.h"

#include "core/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace rl {

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder()
{
    freeResampler();
    av_frame_free(&m_frame);
    avcodec_free_context(&m_ctx);
}

bool AudioDecoder::canDecode(const AVCodecParameters *par)
{
    return par && par->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(par->codec_id);
}

bool AudioDecoder::openContext(const AVCodecParameters *par, int codecId)
{
    const AVCodec *codec = avcodec_find_decoder(static_cast<AVCodecID>(codecId));
    if (!codec)
        return false;
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx)
        return false;
    if (par && avcodec_parameters_to_context(m_ctx, par) < 0)
        return false;
    if (avcodec_open2(m_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&m_ctx);
        return false;
    }
    m_frame = av_frame_alloc();
    return m_frame != nullptr;
}

bool AudioDecoder::open(const AVCodecParameters *par)
{
    if (m_ctx || !canDecode(par))
        return false;
    return openContext(par, par->codec_id);
}

bool AudioDecoder::openAdtsAac()
{
    if (m_ctx)
        return false;
    return openContext(nullptr, AV_CODEC_ID_AAC);
}

void AudioDecoder::freeResampler()
{
    swr_free(&m_swr);
    m_inFormat = -1;
    m_inRate = 0;
    m_inChannels = 0;
}

QByteArray AudioDecoder::convert(const AVFrame *frame)
{
    const int channels = frame->ch_layout.nb_channels;
    if (channels <= 0 || frame->sample_rate <= 0)
        return {};

    // (Re)configure when the input shape is new or changed — a camera can renegotiate
    // its audio mid-stream after a reconnect.
    if (!m_swr || m_inFormat != frame->format || m_inRate != frame->sample_rate ||
        m_inChannels != channels) {
        freeResampler();

        AVChannelLayout in{};
        if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&in, channels); // decoders may leave it unspecified
        else
            av_channel_layout_copy(&in, &frame->ch_layout);
        AVChannelLayout out{};
        av_channel_layout_default(&out, kOutChannels);

        const int rc = swr_alloc_set_opts2(&m_swr, &out, AV_SAMPLE_FMT_S16, kOutRate, &in,
                                           static_cast<AVSampleFormat>(frame->format),
                                           frame->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
        if (rc < 0 || swr_init(m_swr) < 0) {
            qCWarning(lcMedia) << "audio: cannot configure resampler for" << frame->sample_rate
                               << "Hz," << channels << "ch";
            freeResampler();
            return {};
        }
        m_inFormat = frame->format;
        m_inRate = frame->sample_rate;
        m_inChannels = channels;
    }

    const int maxOut = swr_get_out_samples(m_swr, frame->nb_samples);
    if (maxOut <= 0)
        return {};
    QByteArray pcm(maxOut * kBytesPerFrame, Qt::Uninitialized);
    auto *dst = reinterpret_cast<uint8_t *>(pcm.data());
    const int got = swr_convert(m_swr, &dst, maxOut,
                                const_cast<const uint8_t **>(frame->extended_data),
                                frame->nb_samples);
    if (got <= 0)
        return {};
    pcm.resize(got * kBytesPerFrame);
    return pcm;
}

QByteArray AudioDecoder::decode(const AVPacket *packet)
{
    QByteArray out;
    if (!m_ctx || avcodec_send_packet(m_ctx, packet) < 0)
        return out;
    while (avcodec_receive_frame(m_ctx, m_frame) == 0) {
        out.append(convert(m_frame));
        av_frame_unref(m_frame);
    }
    return out;
}

QByteArray AudioDecoder::decode(const QByteArray &frame)
{
    if (!m_ctx || frame.isEmpty())
        return {};
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return {};
    // A packet without a refcounted buffer is copied (and padded) by libavcodec, so
    // pointing it at the caller's bytes is safe.
    pkt->data = reinterpret_cast<uint8_t *>(const_cast<char *>(frame.constData()));
    pkt->size = frame.size();
    const QByteArray out = decode(pkt);
    av_packet_free(&pkt);
    return out;
}

} // namespace rl
