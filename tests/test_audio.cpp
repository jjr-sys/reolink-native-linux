#include "media/AudioDecoder.h"
#include "media/AudioJitterBuffer.h"

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

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

    // ---- AudioJitterBuffer: the choppy-audio regression. Decoded audio arrives in bursts
    // while the sound card consumes at a fixed rate; without a buffer in between, every
    // late or bunched chunk leaves a gap. These run on a virtual clock (no audio device):
    // the consumer reads 10 ms every 10 ms, chunks arrive at the times each case gives.

    // Feed `arrivalsMs` (64 ms chunks: what a 16 kHz AAC frame becomes at 48 kHz stereo)
    // and read 10 ms every 10 ms for `runMs`. Returns the buffer for inspection.
    static void simulate(rl::AudioJitterBuffer &buf, const std::vector<int> &arrivalsMs, int runMs)
    {
        const QByteArray chunk(3072 * 4, char(0x11));
        std::vector<char> out(1920);
        size_t next = 0;
        for (int t = 0; t < runMs; t += 10) {
            while (next < arrivalsMs.size() && arrivalsMs[next] <= t)
                buf.write(chunk.constData(), chunk.size()), ++next;
            buf.read(out.data(), qint64(out.size()));
        }
    }
    static rl::AudioJitterBuffer::Config bufferConfig()
    {
        return {/*prebufferBytes*/ 48000 * 400 / 1000 * 4, /*maxBytes*/ 48000 * 1200 / 1000 * 4,
                /*frameBytes*/ 4};
    }

    void jitterBufferStaysSilentUntilPrebufferIsFull()
    {
        rl::AudioJitterBuffer buf(bufferConfig());
        const QByteArray chunk(3072 * 4, char(0x7F));
        std::vector<char> out(1920, char(0x55));
        for (int i = 0; i < 3; ++i) // 192 ms: under the 400 ms target
            buf.write(chunk.constData(), chunk.size());
        buf.read(out.data(), 1920);
        QVERIFY(!buf.playing());
        QCOMPARE(out[0], char(0)); // silence, and the request was filled in full
        QCOMPARE(out[1919], char(0));
        for (int i = 0; i < 4; ++i) // now 448 ms
            buf.write(chunk.constData(), chunk.size());
        buf.read(out.data(), 1920);
        QVERIFY(buf.playing());
        QCOMPARE(out[0], char(0x7F));
    }

    void jitterBufferAbsorbsBurstsAndJitter()
    {
        const int n = 200; // ~12.8 s
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> jit(0, 60);

        std::vector<int> jitter, stall, burst;
        for (int i = 0; i < n; ++i)
            jitter.push_back(100 + i * 64 + jit(rng));
        std::sort(jitter.begin(), jitter.end());
        for (int i = 0; i < n; ++i) { // a 200 ms GUI stall every 2 s: chunks pile up, then land together
            int t = 100 + i * 64, ph = t % 2000;
            stall.push_back(ph < 200 ? t - ph + 200 : t);
        }
        for (int i = 0; i < n; ++i) // four chunks at a time, as a demuxer reading in blocks delivers them
            burst.push_back(100 + (i / 4) * 256);

        const std::vector<int> *cases[] = {&jitter, &stall, &burst};
        const char *names[] = {"jitter", "stall", "burst"};
        for (int c = 0; c < 3; ++c) {
            rl::AudioJitterBuffer buf(bufferConfig());
            simulate(buf, *cases[c], (*cases[c]).back()); // stop before the feed ends
            QVERIFY2(buf.underruns() == 0, names[c]);
            QVERIFY2(buf.droppedBytes() == 0, names[c]);
            QVERIFY2(buf.silenceBytes() == 0, names[c]);
        }
    }

    void jitterBufferRebuffersAfterStarvation()
    {
        rl::AudioJitterBuffer buf(bufferConfig());
        std::vector<int> a;
        for (int i = 0; i < 20; ++i) // 1.3 s of steady audio, then nothing for a second
            a.push_back(i * 64);
        simulate(buf, a, 2600);
        QCOMPARE(buf.underruns(), 1);
        QVERIFY(!buf.playing());   // waiting to refill, not dribbling out fragments
        QVERIFY(buf.silenceBytes() > 0);
    }

    // A stall longer than the head start costs one gap; the buffer then holds more in
    // reserve, so the same stall again is absorbed.
    void jitterBufferLearnsFromAStall()
    {
        rl::AudioJitterBuffer::Config cfg = bufferConfig();
        cfg.maxPrebufferBytes = 48000 * 900 / 1000 * 4;
        rl::AudioJitterBuffer buf(cfg);
        const qint64 before = buf.prebufferBytes();
        std::vector<int> a; // 500 ms stalls at 3 s and 6 s: chunks due meanwhile land together after
        for (int i = 0; i < 150; ++i) {
            int t = 100 + i * 64, ph = t % 3000;
            a.push_back(ph < 500 && t >= 3000 ? t - ph + 500 : t);
        }
        simulate(buf, a, 5500); // through the first stall only
        QCOMPARE(buf.underruns(), 1);
        QVERIFY(buf.prebufferBytes() > before);
        simulate(buf, a, 8000); // the second stall: no new gap
        QCOMPARE(buf.underruns(), 1);
    }

    void jitterBufferDropsOldestPastTheCeiling()
    {
        rl::AudioJitterBuffer buf(bufferConfig());
        QByteArray big(48000 * 3 * 4, '\0'); // 3 s at once, each 4-byte frame numbered
        auto *f = reinterpret_cast<quint32 *>(big.data());
        const quint32 frames = quint32(big.size() / 4);
        for (quint32 i = 0; i < frames; ++i)
            f[i] = i;
        buf.write(big.constData(), big.size());

        QVERIFY(buf.queuedBytes() <= 48000 * 1200 / 1000 * 4);
        QVERIFY(buf.droppedBytes() > 0);
        QCOMPARE(buf.droppedBytes() % 4, qint64(0)); // whole frames only
        std::vector<char> out(4);
        buf.read(out.data(), 4);
        quint32 first;
        std::memcpy(&first, out.data(), 4);
        QVERIFY2(first > 0, "the oldest audio should be the part that was dropped");
        // What remains is the newest run, contiguous up to the last frame written.
        const quint32 expectFirst = frames - quint32(buf.queuedBytes() / 4) - 1;
        QCOMPARE(first, expectFirst);
    }

    // Recorded playback: a short fixed cushion. Paced (real-time) arrival plays without
    // gaps and sound starts within ~130 ms; a seek/start burst is capped near the picture.
    static rl::AudioJitterBuffer::Config playbackConfig()
    {
        return {48000 * 100 / 1000 * 4, 48000 * 600 / 1000 * 4, 4, 0};
    }

    void playbackModeIsSteadyAndLowLatency()
    {
        rl::AudioJitterBuffer buf(playbackConfig());
        std::vector<int> a;
        for (int i = 0; i < 100; ++i)
            a.push_back(i * 64);
        simulate(buf, a, 6000);
        QCOMPARE(buf.underruns(), 0);
        QCOMPARE(buf.droppedBytes(), qint64(0));
        QVERIFY(buf.queuedBytes() <= 48000 * 200 / 1000 * 4); // stays within ~200 ms of the feed

        rl::AudioJitterBuffer first(playbackConfig());
        int startMs = -1;
        const QByteArray chunk(3072 * 4, char(0x11));
        std::vector<char> out(1920);
        for (int t = 0; t < 500 && startMs < 0; t += 10) {
            if (t % 64 < 10)
                first.write(chunk.constData(), chunk.size());
            first.read(out.data(), 1920);
            if (first.playing())
                startMs = t;
        }
        QVERIFY2(startMs >= 0 && startMs <= 140, "sound should start within about 140 ms");
    }

    void playbackModeCapsASeekBurst()
    {
        rl::AudioJitterBuffer buf(playbackConfig());
        const QByteArray big(48000 * 3 * 4, char(0x22)); // 3 s at once
        buf.write(big.constData(), big.size());
        QVERIFY(buf.queuedBytes() <= 48000 * 600 / 1000 * 4);
        buf.reset(); // flush on seek
        QCOMPARE(buf.queuedBytes(), qint64(0));
        QVERIFY(!buf.playing());
    }
};

QTEST_GUILESS_MAIN(TestAudio)
#include "test_audio.moc"
