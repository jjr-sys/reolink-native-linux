#include "ClipLoader.h"

#include "device/FrigateApi.h"

#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <algorithm>
#include <limits>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace rl {

namespace {

QString avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// The server may take a while to cut a long clip.
constexpr int kDownloadTimeoutMs = 90000;

} // namespace

qint64 remuxToIndexedMp4(const QString &src, const QString &dst, QString *error)
{
    AVFormatContext *in = nullptr;
    AVFormatContext *out = nullptr;
    AVPacket *pkt = nullptr;
    auto fail = [&](const QString &what, int code = 0) {
        if (error)
            *error = code < 0 ? what + QStringLiteral(": ") + avError(code) : what;
        av_packet_free(&pkt);
        if (out) {
            if (out->pb)
                avio_closep(&out->pb);
            avformat_free_context(out);
        }
        avformat_close_input(&in);
        return qint64(-1);
    };

    int rc = avformat_open_input(&in, src.toUtf8().constData(), nullptr, nullptr);
    if (rc < 0)
        return fail(QStringLiteral("could not open the clip"), rc);
    if ((rc = avformat_find_stream_info(in, nullptr)) < 0)
        return fail(QStringLiteral("could not read the clip"), rc);
    if ((rc = avformat_alloc_output_context2(&out, nullptr, "mp4", dst.toUtf8().constData())) < 0)
        return fail(QStringLiteral("could not create the output"), rc);

    // Copy the video and any audio the MP4 container can hold; skip the rest.
    std::vector<int> map(in->nb_streams, -1);
    bool haveVideo = false;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVCodecParameters *par = in->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (avformat_query_codec(out->oformat, par->codec_id, FF_COMPLIANCE_NORMAL) != 1)
            continue;
        AVStream *os = avformat_new_stream(out, nullptr);
        if (!os || avcodec_parameters_copy(os->codecpar, par) < 0)
            return fail(QStringLiteral("could not copy a stream"));
        os->codecpar->codec_tag = 0;
        os->time_base = in->streams[i]->time_base;
        map[i] = os->index;
        haveVideo = haveVideo || par->codec_type == AVMEDIA_TYPE_VIDEO;
    }
    if (!haveVideo)
        return fail(QStringLiteral("the clip has no video"));

    if ((rc = avio_open(&out->pb, dst.toUtf8().constData(), AVIO_FLAG_WRITE)) < 0)
        return fail(QStringLiteral("could not write the clip"), rc);
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "movflags", "faststart", 0); // index first: seekable from the start
    rc = avformat_write_header(out, &opts);
    av_dict_free(&opts);
    if (rc < 0)
        return fail(QStringLiteral("could not write the clip header"), rc);

    pkt = av_packet_alloc();
    // Shift everything so the clip starts at zero, keeping audio and video in step.
    int64_t base = AV_NOPTS_VALUE;
    int64_t firstUs = std::numeric_limits<int64_t>::max();
    int64_t lastUs = std::numeric_limits<int64_t>::min();
    while (av_read_frame(in, pkt) >= 0) {
        const int oi = map[pkt->stream_index];
        if (oi < 0) {
            av_packet_unref(pkt);
            continue;
        }
        const AVRational itb = in->streams[pkt->stream_index]->time_base;
        const AVRational otb = out->streams[oi]->time_base;
        const int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        if (base == AV_NOPTS_VALUE && ts != AV_NOPTS_VALUE)
            base = av_rescale_q(ts, itb, AV_TIME_BASE_Q);
        const int64_t shift = base == AV_NOPTS_VALUE ? 0 : av_rescale_q(base, AV_TIME_BASE_Q, itb);
        if (pkt->pts != AV_NOPTS_VALUE)
            pkt->pts -= shift;
        if (pkt->dts != AV_NOPTS_VALUE)
            pkt->dts -= shift;

        if (in->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ts != AV_NOPTS_VALUE) {
            firstUs = std::min(firstUs, av_rescale_q(ts - shift, itb, AV_TIME_BASE_Q));
            lastUs = std::max(lastUs, av_rescale_q(ts - shift + pkt->duration, itb, AV_TIME_BASE_Q));
        }

        av_packet_rescale_ts(pkt, itb, otb);
        pkt->stream_index = oi;
        pkt->pos = -1;
        rc = av_interleaved_write_frame(out, pkt);
        av_packet_unref(pkt);
        if (rc < 0)
            return fail(QStringLiteral("could not write the clip"), rc);
    }
    if ((rc = av_write_trailer(out)) < 0)
        return fail(QStringLiteral("could not finish the clip"), rc);

    av_packet_free(&pkt);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    if (lastUs <= firstUs)
        return 0;
    return (lastUs - firstUs) / 1000;
}

qint64 trimToIndexedMp4(const QString &src, const QString &dst, double offsetSecs,
                        double durationSecs, QString *error)
{
    AVFormatContext *in = nullptr;
    AVFormatContext *out = nullptr;
    AVPacket *pkt = nullptr;
    auto fail = [&](const QString &what, int code = 0) {
        if (error)
            *error = code < 0 ? what + QStringLiteral(": ") + avError(code) : what;
        av_packet_free(&pkt);
        if (out) {
            if (out->pb)
                avio_closep(&out->pb);
            avformat_free_context(out);
        }
        avformat_close_input(&in);
        return qint64(-1);
    };

    int rc = avformat_open_input(&in, src.toUtf8().constData(), nullptr, nullptr);
    if (rc < 0)
        return fail(QStringLiteral("could not open the recording"), rc);
    if ((rc = avformat_find_stream_info(in, nullptr)) < 0)
        return fail(QStringLiteral("could not read the recording"), rc);
    if ((rc = avformat_alloc_output_context2(&out, nullptr, "mp4", dst.toUtf8().constData())) < 0)
        return fail(QStringLiteral("could not create the output"), rc);

    std::vector<int> map(in->nb_streams, -1);
    bool haveVideo = false;
    int videoStream = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVCodecParameters *par = in->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (avformat_query_codec(out->oformat, par->codec_id, FF_COMPLIANCE_NORMAL) != 1)
            continue;
        AVStream *os = avformat_new_stream(out, nullptr);
        if (!os || avcodec_parameters_copy(os->codecpar, par) < 0)
            return fail(QStringLiteral("could not copy a stream"));
        os->codecpar->codec_tag = 0;
        os->time_base = in->streams[i]->time_base;
        map[i] = os->index;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            haveVideo = true;
            if (videoStream < 0)
                videoStream = int(i);
        }
    }
    if (!haveVideo)
        return fail(QStringLiteral("the recording has no video"));

    // Seek near the start of the window first — this is what keeps a trim of a large
    // hour-long recording fast; av_read_frame below still walks forward to the exact point.
    const int64_t seekTs = av_rescale_q(static_cast<int64_t>(offsetSecs * AV_TIME_BASE),
                                        AV_TIME_BASE_Q, in->streams[videoStream]->time_base);
    av_seek_frame(in, videoStream, seekTs, AVSEEK_FLAG_BACKWARD);

    if ((rc = avio_open(&out->pb, dst.toUtf8().constData(), AVIO_FLAG_WRITE)) < 0)
        return fail(QStringLiteral("could not write the clip"), rc);
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "movflags", "faststart", 0);
    rc = avformat_write_header(out, &opts);
    av_dict_free(&opts);
    if (rc < 0)
        return fail(QStringLiteral("could not write the clip header"), rc);

    pkt = av_packet_alloc();
    int64_t base = AV_NOPTS_VALUE;         // first kept timestamp, in AV_TIME_BASE units
    const int64_t windowStartUs = static_cast<int64_t>(offsetSecs * AV_TIME_BASE);
    const int64_t windowEndUs = static_cast<int64_t>((offsetSecs + durationSecs) * AV_TIME_BASE);
    int64_t firstUs = std::numeric_limits<int64_t>::max();
    int64_t lastUs = std::numeric_limits<int64_t>::min();
    while (av_read_frame(in, pkt) >= 0) {
        const int oi = map[pkt->stream_index];
        const AVRational itb = in->streams[pkt->stream_index]->time_base;
        const int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        const int64_t tsUs = ts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(ts, itb, AV_TIME_BASE_Q);
        if (oi < 0 || tsUs == AV_NOPTS_VALUE || tsUs < windowStartUs) {
            av_packet_unref(pkt);
            continue;
        }
        if (tsUs >= windowEndUs) {
            av_packet_unref(pkt);
            break;
        }
        if (base == AV_NOPTS_VALUE)
            base = tsUs;
        const int64_t shift = av_rescale_q(base, AV_TIME_BASE_Q, itb);
        if (pkt->pts != AV_NOPTS_VALUE)
            pkt->pts -= shift;
        if (pkt->dts != AV_NOPTS_VALUE)
            pkt->dts -= shift;

        if (in->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            firstUs = std::min(firstUs, tsUs - base);
            lastUs = std::max(lastUs, tsUs - base + av_rescale_q(pkt->duration, itb, AV_TIME_BASE_Q));
        }

        const AVRational otb = out->streams[oi]->time_base;
        av_packet_rescale_ts(pkt, itb, otb);
        pkt->stream_index = oi;
        pkt->pos = -1;
        rc = av_interleaved_write_frame(out, pkt);
        av_packet_unref(pkt);
        if (rc < 0)
            return fail(QStringLiteral("could not write the clip"), rc);
    }
    if (base == AV_NOPTS_VALUE)
        return fail(QStringLiteral("nothing in the recording covers this moment"));
    if ((rc = av_write_trailer(out)) < 0)
        return fail(QStringLiteral("could not finish the clip"), rc);

    av_packet_free(&pkt);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    if (lastUs <= firstUs)
        return 0;
    return (lastUs - firstUs) / 1000;
}

ClipLoader::ClipLoader(QObject *parent) : QObject(parent) {}

ClipLoader::~ClipLoader()
{
    ++m_generation; // an in-flight worker only touches its own local files
}

void ClipLoader::setState(State s)
{
    m_state = s;
    emit stateChanged();
}

void ClipLoader::cancel()
{
    ++m_generation;
    m_localUrl.clear();
    m_durationMs = 0;
    m_error.clear();
    setState(State::Idle);
}

void ClipLoader::load(const QString &url)
{
    const int gen = ++m_generation;
    m_localUrl.clear();
    m_durationMs = 0;
    m_error.clear();
    if (!m_dir.isValid()) {
        m_error = tr("no temporary folder");
        setState(State::Error);
        return;
    }
    setState(State::Loading);

    const QString raw = m_dir.filePath(QStringLiteral("raw-%1.mp4").arg(gen));
    const QString done = m_dir.filePath(QStringLiteral("clip-%1.mp4").arg(gen));

    // Already a local, indexed clip (an NVR footage fetch already ran trimToIndexedMp4):
    // play it as-is. Remuxing it again here was a second full copy for no benefit, and
    // picking a fresh zero-point in that second pass is what was pulling audio and video
    // out of step with each other.
    if (url.startsWith(QLatin1String("file:"))) {
        m_localUrl = QUrl(url);
        m_durationMs = 0; // MediaPlayer reports its own duration once it opens the file
        setState(State::Ready);
        return;
    }

    struct Result { qint64 durationMs = -1; QString error; };
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, gen, raw, done] {
        const Result r = watcher->result();
        watcher->deleteLater();
        QFile::remove(raw);
        if (gen != m_generation) { // superseded by another load() or cancel()
            QFile::remove(done);
            return;
        }
        if (r.durationMs < 0) {
            m_error = r.error;
            setState(State::Error);
            return;
        }
        m_durationMs = r.durationMs;
        m_localUrl = QUrl::fromLocalFile(done);
        setState(State::Ready);
    });
    watcher->setFuture(QtConcurrent::run([url, raw, done]() -> Result {
        Result r;
        QString err;
        const QByteArray body = frigate::httpGet(url, kDownloadTimeoutMs, &err);
        if (body.isEmpty()) {
            r.error = err.isEmpty() ? QObject::tr("the server sent an empty clip") : err;
            return r;
        }
        QFile f(raw);
        if (!f.open(QIODevice::WriteOnly) || f.write(body) != body.size()) {
            r.error = QObject::tr("could not save the clip");
            return r;
        }
        f.close();
        r.durationMs = remuxToIndexedMp4(raw, done, &err);
        if (r.durationMs < 0)
            r.error = err;
        return r;
    }));
}

} // namespace rl
