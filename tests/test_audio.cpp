#include "media/AudioDecoder.h"

#include <QtTest>

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace {

// Encode `seconds` of a 440 Hz tone to a single ADTS AAC byte stream (16 kHz mono,
// like a Reolink camera), using libavformat's adts muxer into memory.
QByteArray makeAdtsAac(double seconds)
{
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!enc)
        return {};
    AVCodecContext *ctx = avcodec_alloc_context3(enc);
    ctx->sample_rate = 16000;
    av_channel_layout_default(&ctx->ch_layout, 1);
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->bit_rate = 32000;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx, enc, nullptr) < 0)
        return {};

    AVFormatContext *oc = nullptr;
    avformat_alloc_output_context2(&oc, nullptr, "adts", nullptr);
    AVStream *st = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(st->codecpar, ctx);
    avio_open_dyn_buf(&oc->pb);
    avformat_write_header(oc, nullptr);

    AVFrame *fr = av_frame_alloc();
    fr->nb_samples = ctx->frame_size;
    fr->format = ctx->sample_fmt;
    av_channel_layout_copy(&fr->ch_layout, &ctx->ch_layout);
    av_frame_get_buffer(fr, 0);
    AVPacket *pkt = av_packet_alloc();

    auto drain = [&] {
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            pkt->stream_index = 0;
            av_write_frame(oc, pkt);
            av_packet_unref(pkt);
        }
    };
    const int total = static_cast<int>(seconds * ctx->sample_rate);
    for (int done = 0, n = 0; done + ctx->frame_size <= total; done += ctx->frame_size, ++n) {
        av_frame_make_writable(fr);
        auto *d = reinterpret_cast<float *>(fr->data[0]);
        for (int i = 0; i < ctx->frame_size; ++i)
            d[i] = 0.5f * std::sin(2.0 * M_PI * 440.0 * (done + i) / ctx->sample_rate);
        fr->pts = done;
        avcodec_send_frame(ctx, fr);
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_write_trailer(oc);

    uint8_t *buf = nullptr;
    const int size = avio_close_dyn_buf(oc->pb, &buf);
    QByteArray out(reinterpret_cast<const char *>(buf), size);
    av_free(buf);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avformat_free_context(oc);
    avcodec_free_context(&ctx);
    return out;
}

// Split an ADTS stream into frames using the 13-bit frame length in each header.
QList<QByteArray> splitAdts(const QByteArray &s)
{
    QList<QByteArray> frames;
    int i = 0;
    while (i + 7 <= s.size()) {
        const auto *b = reinterpret_cast<const uchar *>(s.constData() + i);
        if (b[0] != 0xff || (b[1] & 0xf0) != 0xf0)
            break;
        const int len = ((b[3] & 0x03) << 11) | (b[4] << 3) | (b[5] >> 5);
        if (len < 7 || i + len > s.size())
            break;
        frames.append(s.mid(i, len));
        i += len;
    }
    return frames;
}

double rms(const QByteArray &pcm)
{
    const auto *p = reinterpret_cast<const qint16 *>(pcm.constData());
    const int n = pcm.size() / 2;
    double acc = 0;
    for (int i = 0; i < n; ++i)
        acc += double(p[i]) * p[i];
    return n ? std::sqrt(acc / n) : 0;
}

} // namespace

class TestAudio : public QObject
{
    Q_OBJECT
private slots:
    // The Baichuan path: bare ADTS frames, one per call, 16 kHz mono in -> 48 kHz
    // stereo S16 out, with real signal energy and about the right duration.
    void adtsAacFrames()
    {
        const QByteArray stream = makeAdtsAac(1.0);
        QVERIFY2(!stream.isEmpty(), "libavcodec has no AAC encoder");
        const QList<QByteArray> frames = splitAdts(stream);
        QVERIFY(frames.size() > 10);

        rl::AudioDecoder dec;
        QVERIFY(dec.openAdtsAac());
        QByteArray pcm;
        for (const QByteArray &f : frames)
            pcm.append(dec.decode(f));

        QCOMPARE(pcm.size() % rl::AudioDecoder::kBytesPerFrame, 0);
        const double secs = double(pcm.size() / rl::AudioDecoder::kBytesPerFrame) /
                            rl::AudioDecoder::kOutRate;
        QVERIFY2(secs > 0.85 && secs < 1.15, qPrintable(QString::number(secs)));
        // A 0.5-amplitude sine is ~11585 RMS in S16; allow for codec + resample loss.
        QVERIFY2(rms(pcm) > 6000, qPrintable(QString::number(rms(pcm))));

        // Mono is upmixed: left and right samples match.
        const auto *p = reinterpret_cast<const qint16 *>(pcm.constData());
        const int mid = pcm.size() / 4 / 2 * 2;
        QCOMPARE(p[mid], p[mid + 1]);
    }

    // The RTSP/FLV path: codec parameters come from the demuxer. G.711 a-law at 8 kHz
    // (older Reolink firmware) must resample to 48 kHz: 1 s in -> 1 s out.
    void g711FromCodecParameters()
    {
        AVCodecParameters *par = avcodec_parameters_alloc();
        par->codec_type = AVMEDIA_TYPE_AUDIO;
        par->codec_id = AV_CODEC_ID_PCM_ALAW;
        par->sample_rate = 8000;
        av_channel_layout_default(&par->ch_layout, 1);

        QVERIFY(rl::AudioDecoder::canDecode(par));
        rl::AudioDecoder dec;
        QVERIFY(dec.open(par));

        const QByteArray alaw(8000, char(0xD5)); // a-law digital silence
        AVPacket *pkt = av_packet_alloc();
        av_new_packet(pkt, alaw.size());
        memcpy(pkt->data, alaw.constData(), alaw.size());
        const QByteArray pcm = dec.decode(pkt);
        av_packet_free(&pkt);
        avcodec_parameters_free(&par);

        const int frames = pcm.size() / rl::AudioDecoder::kBytesPerFrame;
        QVERIFY2(qAbs(frames - rl::AudioDecoder::kOutRate) < 200, qPrintable(QString::number(frames)));
    }

    void refusesWhatItCannotDecode()
    {
        AVCodecParameters *video = avcodec_parameters_alloc();
        video->codec_type = AVMEDIA_TYPE_VIDEO;
        video->codec_id = AV_CODEC_ID_H264;
        QVERIFY(!rl::AudioDecoder::canDecode(video));
        rl::AudioDecoder dec;
        QVERIFY(!dec.open(video));
        avcodec_parameters_free(&video);

        // Garbage into an open decoder is dropped, not fatal.
        rl::AudioDecoder aac;
        QVERIFY(aac.openAdtsAac());
        QVERIFY(aac.decode(QByteArray(64, char(0x55))).isEmpty());
        QVERIFY(aac.decode(QByteArray()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestAudio)
#include "test_audio.moc"
