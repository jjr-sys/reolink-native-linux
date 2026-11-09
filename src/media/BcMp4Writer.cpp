#include "BcMp4Writer.h"

#include <QFile>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace rl {

namespace {

constexpr int kSampleRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                22050, 16000, 12000, 11025, 8000, 7350};
// Wait this long into the video for a first audio frame before deciding there is none.
constexpr qint64 kAudioWaitUs = 2'000'000;
constexpr AVRational kMicros{1, 1000000};

QString avErr(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// Calls `fn(nalStart, nalLen)` for each NAL unit of an Annex-B buffer (start codes excluded).
template<typename Fn>
void forEachNal(const QByteArray &b, Fn fn)
{
    const int n = int(b.size());
    const auto *d = reinterpret_cast<const uchar *>(b.constData());
    auto findStart = [&](int from, int *codeLen) {
        for (int i = from; i + 2 < n; ++i)
            if (d[i] == 0 && d[i + 1] == 0) {
                if (d[i + 2] == 1) { *codeLen = 3; return i; }
                if (i + 3 < n && d[i + 2] == 0 && d[i + 3] == 1) { *codeLen = 4; return i; }
            }
        return -1;
    };
    int code = 0;
    int s = findStart(0, &code);
    while (s >= 0) {
        const int begin = s + code;
        int nextCode = 0;
        const int next = findStart(begin, &nextCode);
        const int end = next < 0 ? n : next;
        if (end > begin)
            fn(begin, end - begin);
        s = next;
        code = nextCode;
    }
}

bool setExtradata(AVCodecParameters *p, const QByteArray &bytes)
{
    p->extradata = static_cast<uint8_t *>(av_mallocz(size_t(bytes.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!p->extradata)
        return false;
    memcpy(p->extradata, bytes.constData(), size_t(bytes.size()));
    p->extradata_size = int(bytes.size());
    return true;
}

} // namespace

std::optional<AdtsHeader> parseAdts(const QByteArray &f)
{
    if (f.size() < 7)
        return std::nullopt;
    const auto *d = reinterpret_cast<const uchar *>(f.constData());
    if (d[0] != 0xFF || (d[1] & 0xF0) != 0xF0)
        return std::nullopt;
    AdtsHeader h;
    h.headerLen = (d[1] & 1) ? 7 : 9;
    h.objectType = ((d[2] >> 6) & 3) + 1;
    h.srIndex = (d[2] >> 2) & 0xF;
    h.channels = ((d[2] & 1) << 2) | ((d[3] >> 6) & 3);
    h.frameLen = ((d[3] & 3) << 11) | (d[4] << 3) | ((d[5] >> 5) & 7);
    if (h.srIndex >= int(std::size(kSampleRates)) || h.frameLen < h.headerLen)
        return std::nullopt;
    h.sampleRate = kSampleRates[h.srIndex];
    return h;
}

QByteArray aacAudioSpecificConfig(const AdtsHeader &h)
{
    const quint16 v = quint16((h.objectType << 11) | (h.srIndex << 7) | (h.channels << 3));
    const char b[2] = {char(v >> 8), char(v & 0xFF)};
    return QByteArray(b, 2);
}

QByteArray annexBParameterSets(const QByteArray &annexB, BcMediaParser::Codec codec)
{
    QByteArray out;
    forEachNal(annexB, [&](int at, int len) {
        const uchar b = uchar(annexB[at]);
        bool ps = false;
        if (codec == BcMediaParser::Codec::H264) {
            const int t = b & 0x1F;
            ps = t == 7 || t == 8;
        } else if (codec == BcMediaParser::Codec::H265) {
            const int t = (b >> 1) & 0x3F;
            ps = t == 32 || t == 33 || t == 34;
        }
        if (ps)
            out += QByteArray("\x00\x00\x00\x01", 4) + annexB.mid(at, len);
    });
    return out;
}

qint64 MicrosClock::push(quint32 us)
{
    if (!m_started) {
        m_started = true;
        m_last = us;
        return 0;
    }
    m_total += qint64(quint32(us - m_last)); // unsigned difference survives the wrap
    m_last = us;
    return m_total;
}

BcMp4Writer::BcMp4Writer(QString path) : m_path(std::move(path)) {}

BcMp4Writer::~BcMp4Writer()
{
    if (!m_finished) {
        closeFile();
        QFile::remove(m_path);
    }
}

void BcMp4Writer::fail(const QString &why)
{
    if (m_error.isEmpty())
        m_error = why;
}

void BcMp4Writer::closeFile()
{
    if (!m_fmt)
        return;
    if (m_fmt->pb)
        avio_closep(&m_fmt->pb);
    avformat_free_context(m_fmt);
    m_fmt = nullptr;
}

void BcMp4Writer::addVideo(const BcMediaParser::VideoFrame &f, int width, int height)
{
    if (failed() || m_finished)
        return;
    if (!m_started) {
        if (!f.keyFrame || f.codec == BcMediaParser::Codec::Unknown)
            return;
        m_videoExtra = annexBParameterSets(f.annexB, f.codec);
        if (m_videoExtra.isEmpty())
            return; // a key frame without its parameter sets cannot open the file
        m_started = true;
        m_codec = f.codec;
        m_width = width;
        m_height = height;
    }
    const qint64 us = m_clock.push(f.microseconds);
    m_lastVideoUs = us;
    Pending p{true, f.keyFrame, us, f.annexB};
    if (m_headerWritten) {
        writeVideo(p);
        return;
    }
    m_pending.push_back(std::move(p));
    if (us >= kAudioWaitUs)
        open(); // no audio so far: write the file video-only
}

void BcMp4Writer::addAudio(const QByteArray &adtsFrame)
{
    if (failed() || m_finished || !m_started)
        return; // audio before the first picture has nothing to line up with
    const std::optional<AdtsHeader> h = parseAdts(adtsFrame);
    if (!h || h->frameLen > adtsFrame.size())
        return;
    if (!m_aac) {
        if (m_headerWritten)
            return; // the file was opened without an audio track
        m_aac = h;
        m_audioStartUs = m_lastVideoUs;
    }
    const QByteArray raw = adtsFrame.mid(h->headerLen, h->frameLen - h->headerLen);
    if (m_headerWritten) {
        writeAudio(raw);
        return;
    }
    m_pending.push_back(Pending{false, false, 0, raw});
    open(); // both streams known now
}

bool BcMp4Writer::open()
{
    if (m_headerWritten)
        return true;
    int e = avformat_alloc_output_context2(&m_fmt, nullptr, "mp4", m_path.toUtf8().constData());
    if (e < 0 || !m_fmt) {
        fail(QStringLiteral("mp4 muxer: %1").arg(avErr(e)));
        return false;
    }
    AVStream *vs = avformat_new_stream(m_fmt, nullptr);
    if (!vs) {
        fail(QStringLiteral("mp4 muxer: no video stream"));
        return false;
    }
    vs->time_base = AVRational{1, 90000};
    AVCodecParameters *vp = vs->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = m_codec == BcMediaParser::Codec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    if (m_codec == BcMediaParser::Codec::H265)
        vp->codec_tag = MKTAG('h', 'v', 'c', '1'); // plays in more players than hev1
    vp->width = m_width;
    vp->height = m_height;
    if (!setExtradata(vp, m_videoExtra)) {
        fail(QStringLiteral("out of memory"));
        return false;
    }
    if (m_aac) {
        AVStream *as = avformat_new_stream(m_fmt, nullptr);
        if (!as) {
            fail(QStringLiteral("mp4 muxer: no audio stream"));
            return false;
        }
        as->time_base = AVRational{1, m_aac->sampleRate};
        AVCodecParameters *ap = as->codecpar;
        ap->codec_type = AVMEDIA_TYPE_AUDIO;
        ap->codec_id = AV_CODEC_ID_AAC;
        ap->sample_rate = m_aac->sampleRate;
        ap->frame_size = 1024;
        av_channel_layout_default(&ap->ch_layout, m_aac->channels);
        if (!setExtradata(ap, aacAudioSpecificConfig(*m_aac))) {
            fail(QStringLiteral("out of memory"));
            return false;
        }
    }
    e = avio_open(&m_fmt->pb, m_path.toUtf8().constData(), AVIO_FLAG_WRITE);
    if (e < 0) {
        fail(QStringLiteral("Can't write %1: %2").arg(m_path, avErr(e)));
        return false;
    }
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0); // index at the front: opens and seeks fast
    e = avformat_write_header(m_fmt, &opts);
    av_dict_free(&opts);
    if (e < 0) {
        fail(QStringLiteral("mp4 header: %1").arg(avErr(e)));
        return false;
    }
    m_headerWritten = true;
    std::vector<Pending> held;
    held.swap(m_pending);
    for (const Pending &p : held) {
        if (p.video)
            writeVideo(p);
        else
            writeAudio(p.data);
    }
    return !failed();
}

void BcMp4Writer::writeVideo(const Pending &p)
{
    if (failed())
        return;
    AVStream *st = m_fmt->streams[0];
    qint64 ts = av_rescale_q(p.us, kMicros, st->time_base);
    if (ts <= m_lastVideoDts)
        ts = m_lastVideoDts + 1; // the muxer needs strictly increasing times
    m_lastVideoDts = ts;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, int(p.data.size())) < 0) {
        av_packet_free(&pkt);
        fail(QStringLiteral("out of memory"));
        return;
    }
    memcpy(pkt->data, p.data.constData(), size_t(p.data.size()));
    pkt->stream_index = 0;
    pkt->pts = pkt->dts = ts;
    if (p.key)
        pkt->flags |= AV_PKT_FLAG_KEY;
    const int e = av_interleaved_write_frame(m_fmt, pkt);
    av_packet_free(&pkt);
    if (e < 0)
        fail(QStringLiteral("writing video: %1").arg(avErr(e)));
    else
        ++m_videoFrames;
}

void BcMp4Writer::writeAudio(const QByteArray &raw)
{
    if (failed() || m_fmt->nb_streams < 2)
        return;
    AVStream *st = m_fmt->streams[1];
    const qint64 ts = av_rescale_q(m_audioStartUs, kMicros, AVRational{1, m_aac->sampleRate})
                      + m_audioSamples;
    m_audioSamples += 1024;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, int(raw.size())) < 0) {
        av_packet_free(&pkt);
        fail(QStringLiteral("out of memory"));
        return;
    }
    memcpy(pkt->data, raw.constData(), size_t(raw.size()));
    pkt->stream_index = 1;
    pkt->pts = pkt->dts = av_rescale_q(ts, AVRational{1, m_aac->sampleRate}, st->time_base);
    pkt->duration = av_rescale_q(1024, AVRational{1, m_aac->sampleRate}, st->time_base);
    pkt->flags |= AV_PKT_FLAG_KEY;
    const int e = av_interleaved_write_frame(m_fmt, pkt);
    av_packet_free(&pkt);
    if (e < 0)
        fail(QStringLiteral("writing audio: %1").arg(avErr(e)));
    else
        ++m_audioFrames;
}

bool BcMp4Writer::finish(QString *error)
{
    if (!failed() && !m_started)
        fail(QStringLiteral("No video in the recording"));
    if (!failed() && !m_headerWritten)
        open(); // a clip shorter than the audio wait
    if (!failed()) {
        const int e = av_write_trailer(m_fmt);
        if (e < 0)
            fail(QStringLiteral("finishing the file: %1").arg(avErr(e)));
    }
    closeFile();
    if (failed()) {
        QFile::remove(m_path);
        if (error)
            *error = m_error;
        m_finished = true; // nothing left to clean up
        return false;
    }
    m_finished = true;
    return true;
}

} // namespace rl
