#include "StreamPlayer.h"
#include "core/Backoff.h"
#include "media/FrameTimeSei.h"
#include "media/ReversePlay.h"
#include "media/HttpAuth.h"
#include "media/LiveUrl.h"

#include "AudioDecoder.h"
#include "AudioPlayback.h"
#include "media/VaapiVideoBuffer.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <QRegularExpression>
#include <QSaveFile>
#include <QtConcurrent/QtConcurrentRun>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <atomic>
#include <chrono>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

namespace rl {

namespace {

constexpr int kOpenTimeoutMs = 10000;
constexpr int kReadStallTimeoutMs = 10000; // §5.8 watchdog: no packet in 10s → reconnect
constexpr BackoffPolicy kBackoff{1000, 30000};

using Session = StreamPlayer::Session;
using State = StreamPlayer::State;

bool isLiveUrl(const QString &source) { return isLiveStreamUrl(source); }

template<typename T>
void updateMaximum(std::atomic<T> &maximum, T value)
{
    T previous = maximum.load(std::memory_order_relaxed);
    while (previous < value && !maximum.compare_exchange_weak(
                                    previous, value, std::memory_order_relaxed)) {
    }
}

// Never log credentials — they appear both as userinfo (rtsp://user:pass@host)
// and as query params (the http-flv playback URL: ...&user=X&password=Y).
QString redacted(const QString &source)
{
    QUrl u(source);
    if (!u.isValid())
        return source;
    bool changed = !u.userInfo().isEmpty();
    QUrlQuery q(u);
    for (const QString &key : {QStringLiteral("user"), QStringLiteral("password"),
                               QStringLiteral("token")}) {
        if (q.hasQueryItem(key)) {
            q.removeAllQueryItems(key);
            changed = true;
        }
    }
    if (!changed)
        return source;
    u.setQuery(q);
    return u.toDisplayString(QUrl::RemoveUserInfo);
}

// Determine how the sink should rotate a stream's frames for upright display.
//
// Priority order:
//  1. A display-matrix in the stream side-data (standard rotation signalling;
//     Reolink sends none, but other sources may).
//  2. Declared-vs-decoded swap test [primary]. Some Reolink cameras (Duo 3-class)
//     transmit the main stream transposed — the true image is landscape (GetEnc
//     declares e.g. 7680x2160) but it decodes as 2160x7680 portrait, so the coded
//     width stays within the HEVC level cap. No orientation metadata rides along
//     on RTSP/FLV, so we reconcile the two: if the decoded frame is exactly the
//     declared size transposed, the stream is rotated and we correct it. This is
//     self-calibrating — it leaves already-landscape sub streams and genuine
//     corridor-mode portrait feeds (declared == decoded) untouched, unlike a
//     resolution threshold. `expected` is the GetEnc size for THIS stream.
//  3. Extreme-portrait heuristic [fallback], for raw-URL playback with no NVR
//     session to supply a declared size.
//
// Direction is Clockwise270 (verified on an RLN8-410). Nothing on the wire
// encodes winding direction, so this is a per-model constant, not inferred.
QtVideo::Rotation streamRotation(const AVStream *stream, QSize expected)
{
    // The DISPLAYMATRIX side data moved to codecpar->coded_side_data in FFmpeg 6.1
    // (libavcodec 60.15); older builds (e.g. the Flatpak runtime's FFmpeg) expose
    // it via the now-deprecated av_stream_get_side_data. Support both.
    const int32_t *dm = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 15, 100)
    const AVPacketSideData *sd =
        stream->codecpar->coded_side_data
            ? av_packet_side_data_get(stream->codecpar->coded_side_data,
                                      stream->codecpar->nb_coded_side_data,
                                      AV_PKT_DATA_DISPLAYMATRIX)
            : nullptr;
    if (sd && sd->size >= 9 * static_cast<int>(sizeof(int32_t)))
        dm = reinterpret_cast<const int32_t *>(sd->data);
#else
    size_t dmSize = 0;
    const uint8_t *raw = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &dmSize);
    if (raw && dmSize >= 9 * sizeof(int32_t))
        dm = reinterpret_cast<const int32_t *>(raw);
#endif
    if (dm) {
        // av_display_rotation_get returns the CCW angle; the clockwise display
        // rotation is its negation, normalized to [0,360).
        int cw = static_cast<int>(std::llround(-av_display_rotation_get(dm)));
        cw = ((cw % 360) + 360) % 360;
        switch (cw) {
        case 90:
            return QtVideo::Rotation::Clockwise90;
        case 180:
            return QtVideo::Rotation::Clockwise180;
        case 270:
            return QtVideo::Rotation::Clockwise270;
        default:
            return QtVideo::Rotation::None;
        }
    }

    const int w = stream->codecpar->width;
    const int h = stream->codecpar->height;

    // When the NVR told us the declared size, rotate iff the decoded frame is
    // its exact transpose AND the decoded frame is portrait. Cameras transmit
    // transposed as portrait (a landscape sensor turned sideways — Duo 3), so
    // an upright LANDSCAPE stream whose declared size merely looks transposed
    // is metadata being wrong, not a rotated picture: hub-attached doorbells
    // report exactly that, and rotating them broke their view (issue #3).
    if (expected.isValid()) {
        if (w > 0 && h > w && expected.width() == h && expected.height() == w)
            return QtVideo::Rotation::Clockwise270;
        return QtVideo::Rotation::None;
    }

    // No declared size: fall back to the extreme-portrait signature. The 2x
    // threshold sits above a deliberate 9:16 portrait mount (1.78x).
    if (w > 0 && h > w * 2)
        return QtVideo::Rotation::Clockwise270;
    return QtVideo::Rotation::None;
}

QVideoFrameFormat::PixelFormat mapPixelFormat(int avFormat)
{
    switch (avFormat) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return QVideoFrameFormat::Format_YUV420P;
    case AV_PIX_FMT_NV12:
        return QVideoFrameFormat::Format_NV12;
    default:
        return QVideoFrameFormat::Format_Invalid;
    }
}

// Post a state change to the StreamPlayer on the GUI thread. backMutex guarantees
// the player is alive across the invokeMethod call; invokeMethod(context, fn)
// then cancels itself if the player is destroyed before the event is delivered.
void postState(const std::shared_ptr<Session> &s, State st, const QString &err)
{
    // Log each change INTO the error state (not every retry of the same one), so a pane
    // that shows an error on screen leaves a trace, native (Baichuan) sources included.
    if (st == State::Error && !s->lastWasError.load())
        qCWarning(lcMedia).noquote() << "stream error:" << err;
    s->lastWasError.store(st == State::Error);
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(
        p, [p, s, st, err] {
            p->applyStateFromWorker(s, st, err);
        }, Qt::QueuedConnection);
}

void postFramesTick(const std::shared_ptr<Session> &s)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p, s] {
        p->applyFramesTickFromWorker(s);
    }, Qt::QueuedConnection);
}

void postFrameEpoch(const std::shared_ptr<Session> &s)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p, s] {
        p->applyFrameEpochFromWorker(s);
    }, Qt::QueuedConnection);
}

// Reverse play: decoding can't keep up with the speed (HD drops to SD, as for a slow link).
void postTooSlow(const std::shared_ptr<Session> &s)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p, s] {
        p->applyTooSlowFromWorker(s);
    }, Qt::QueuedConnection);
}

// The recording time a frame was tagged with (media/FrameTimeSei.h), or -1.
qint64 frameEpochOf(const AVFrame *frame)
{
    for (int i = 0; i < frame->nb_side_data; ++i) {
        const AVFrameSideData *sd = frame->side_data[i];
        if (sd->type != AV_FRAME_DATA_SEI_UNREGISTERED)
            continue;
        const qint64 t = rl::frametime::fromUserData(sd->data, int(sd->size));
        if (t >= 0)
            return t;
    }
    return -1;
}

// Hand a decoded frame to the player on the GUI thread, where the QML-owned sink lives
// and is destroyed (finding: cross-thread sink teardown race). The player either shows
// it straight away or holds it on the playout schedule (delayUs > 0).
void deliverFrame(const std::shared_ptr<Session> &s, const QVideoFrame &frame, qint64 ptsUs,
                  qint64 delayUs, qint64 bytes)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    const bool needsWakeup = s->frameHandoff.publish({frame, ptsUs, delayUs, bytes});
    if (!needsWakeup)
        return;
    const bool queued = QMetaObject::invokeMethod(p, [p, s] {
        Session::PendingFrame newest;
        if (s->frameHandoff.take(newest))
            p->applyFrameFromWorker(s, newest.frame, newest.ptsUs, newest.delayUs, newest.bytes);
    }, Qt::QueuedConnection);
    if (!queued)
        s->frameHandoff.clear();
}

// Decoded PCM to the GUI thread, where the audio sink lives. A stale session's
// audio (after stop/replace) must also be checked when the queued call is delivered.
void postAudio(const std::shared_ptr<Session> &s, const QByteArray &pcm)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p, s, pcm] {
        p->applyAudioFromWorker(s, pcm);
    }, Qt::QueuedConnection);
}

void postAudioFlush(const std::shared_ptr<Session> &s)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p, s] {
        p->applyAudioFlush(s);
    }, Qt::QueuedConnection);
}

void postHasAudio(const std::shared_ptr<Session> &s, bool has)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p, s, has] {
        p->applyHasAudioFromWorker(s, has);
    }, Qt::QueuedConnection);
}

void postRecording(const std::shared_ptr<Session> &s, bool recording, const QString &path,
                   const QString &error)
{
    const auto target = s->recordingTarget.lock();
    if (!target)
        return;
    QMutexLocker lock(&target->mutex);
    if (!target->player)
        return;
    StreamPlayer *p = target->player;
    QMetaObject::invokeMethod(
        p, [p, s, recording, path, error] {
            p->applyRecordingState(s, recording, path, error);
        },
        Qt::QueuedConnection);
}

// Stream-copy recorder: taps the live demux, muxing video packets into an MP4
// without re-encoding (DESIGN §5.5). Starts on the next keyframe and rebases
// timestamps so the file begins at t=0.
struct Recorder {
    AVFormatContext *oc = nullptr;
    int outIndex = -1;
    int inIndex = -1;
    AVRational inTb{};
    bool waitingKey = true;
    qint64 startTs = AV_NOPTS_VALUE;
    QString path;

    bool active() const { return oc != nullptr; }

    bool open(const QString &outPath, const AVStream *in)
    {
        path = outPath;
        inIndex = in->index;
        inTb = in->time_base;
        const QByteArray p = outPath.toUtf8();
        if (avformat_alloc_output_context2(&oc, nullptr, "mp4", p.constData()) < 0 || !oc)
            return false;
        AVStream *out = avformat_new_stream(oc, nullptr);
        if (!out || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0)
            return false;
        out->codecpar->codec_tag = 0; // let the muxer choose (avc1/hvc1)
        outIndex = out->index;
        if (!(oc->oformat->flags & AVFMT_NOFILE) &&
            avio_open(&oc->pb, p.constData(), AVIO_FLAG_WRITE) < 0)
            return false;
        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "movflags", "+faststart", 0);
        const int rc = avformat_write_header(oc, &opts);
        av_dict_free(&opts);
        if (rc < 0)
            return false;
        waitingKey = true;
        startTs = AV_NOPTS_VALUE;
        return true;
    }

    void write(const AVPacket *src)
    {
        if (src->stream_index != inIndex)
            return;
        if (waitingKey) {
            if (!(src->flags & AV_PKT_FLAG_KEY))
                return; // start on a keyframe so the file is decodable
            waitingKey = false;
            startTs = src->pts != AV_NOPTS_VALUE ? src->pts : src->dts;
        }
        AVPacket *pkt = av_packet_clone(src); // original continues to the decoder
        if (!pkt)
            return;
        pkt->stream_index = outIndex;
        const AVRational outTb = oc->streams[outIndex]->time_base;
        const qint64 off = startTs == AV_NOPTS_VALUE ? 0 : startTs;
        const auto rnd = static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        if (pkt->pts != AV_NOPTS_VALUE)
            pkt->pts = av_rescale_q_rnd(pkt->pts - off, inTb, outTb, rnd);
        if (pkt->dts != AV_NOPTS_VALUE)
            pkt->dts = av_rescale_q_rnd(pkt->dts - off, inTb, outTb, rnd);
        pkt->duration = av_rescale_q(pkt->duration, inTb, outTb);
        pkt->pos = -1;
        av_interleaved_write_frame(oc, pkt); // takes ownership of pkt's contents
        av_packet_free(&pkt);
    }

    // Writes the trailer and closes; returns the path (empty on nothing recorded).
    QString finalize()
    {
        if (!oc)
            return {};
        QString result;
        if (!waitingKey) { // at least one frame was written
            av_write_trailer(oc);
            result = path;
        }
        if (!(oc->oformat->flags & AVFMT_NOFILE) && oc->pb)
            avio_closep(&oc->pb);
        avformat_free_context(oc);
        oc = nullptr;
        return result;
    }
};

struct InterruptContext {
    Session *session = nullptr;
    QDeadlineTimer deadline;

    static int callback(void *opaque)
    {
        auto *ctx = static_cast<InterruptContext *>(opaque);
        if (ctx->session->abort.load(std::memory_order_relaxed))
            return 1;
        return ctx->deadline.hasExpired() ? 1 : 0;
    }
};

void abortableSleepUs(Session *s, qint64 waitUs)
{
    for (qint64 remaining = waitUs;
         remaining > 0 && !s->abort.load(std::memory_order_relaxed); remaining -= 10000)
        av_usleep(static_cast<unsigned>(qMin<qint64>(remaining, 10000)));
}

// How far behind arrival smoothed live video runs. Long enough to ride out the stalls seen
// on the routed links (up to about 0.8 s), and the same delay is applied to sound.
constexpr qint64 kSmoothDelayUs = 1000000;
constexpr qint64 kMinSmoothDelayUs = 250000;
// Decoded frames waiting to be shown are held in memory. The picture delay is sized from a
// budget shared by all live players (so a 16-tile grid can't hold 16 x 96 MB). The queue's own
// byte cap stays at the per-stream ceiling: it must never be lower than what the delay
// implies, or frames are discarded before they are due and the picture goes blank.
constexpr qint64 kPlayoutTotalBytes = 256ll * 1024 * 1024;
constexpr qint64 kPlayoutMaxPerStream = 96ll * 1024 * 1024;
constexpr qint64 kPlayoutMinPerStream = 16ll * 1024 * 1024;
std::atomic<int> gLivePlayers{0};

qint64 playoutBudgetBytes()
{
    const int n = qMax(1, gLivePlayers.load(std::memory_order_relaxed));
    return qBound(kPlayoutMinPerStream, kPlayoutTotalBytes / n, kPlayoutMaxPerStream);
}

// The picture delay for a stream: one second, or less if that many decoded frames would
// not fit the memory budget (a big main stream). 0 when smoothing does not apply.
qint64 smoothDelayFor(int width, int height, double fps)
{
    if (width <= 0 || height <= 0)
        return 0;
    if (fps < 1 || fps > 120)
        fps = 15;
    const double frameBytes = double(width) * height * 1.5;
    const qint64 fits = qint64(double(playoutBudgetBytes()) / frameBytes / fps * 1e6);
    return qBound(kMinSmoothDelayUs, qMin(kSmoothDelayUs, fits), kSmoothDelayUs);
}

// Record and hand a prepared QVideoFrame to the sink. CPU-backed and imported
// VAAPI frames share the same timestamp smoothing and latest-value handoff.
bool publishVideoFrame(const std::shared_ptr<Session> &s, QVideoFrame videoFrame,
                       QtVideo::Rotation rotation, qint64 ptsUs, qint64 delayUs,
                       qint64 bytes)
{
    if (!videoFrame.isValid())
        return false;
    if (rotation != QtVideo::Rotation::None)
        videoFrame.setRotation(rotation);
    {
        QMutexLocker lock(&s->lastFrameMutex);
        s->lastFrame = videoFrame; // shared, not copied
    }
    deliverFrame(s, videoFrame, ptsUs, delayUs, bytes);
    return true;
}

// A software AVFrame copied into a QVideoFrame (invalid on a mapping failure).
QVideoFrame copyToVideoFrame(AVFrame *out, QVideoFrameFormat::PixelFormat outFormat)
{
    QVideoFrameFormat format(QSize(out->width, out->height), outFormat);
    // Full-range (JPEG) YUV must be tagged or the sink renders it as limited range.
    if (out->color_range == AVCOL_RANGE_JPEG || out->format == AV_PIX_FMT_YUVJ420P)
        format.setColorRange(QVideoFrameFormat::ColorRange_Full);

    QVideoFrame videoFrame(format);
    if (!videoFrame.map(QVideoFrame::WriteOnly))
        return {};
    for (int plane = 0; plane < videoFrame.planeCount(); ++plane) {
        const int planeHeight = plane == 0 ? out->height : (out->height + 1) / 2;
        // linesize may be negative for bottom-up frames; qAbs stops the memcpy
        // size from wrapping to ~2^64.
        const int rowBytes = qMin(videoFrame.bytesPerLine(plane), qAbs(out->linesize[plane]));
        for (int y = 0; y < planeHeight; ++y)
            memcpy(videoFrame.bits(plane) + y * videoFrame.bytesPerLine(plane),
                   out->data[plane] + y * out->linesize[plane], static_cast<size_t>(rowBytes));
    }
    videoFrame.unmap();
    return videoFrame;
}

// Convert one software AVFrame to a QVideoFrame and hand it to the sink. Returns
// false on an unrecoverable mapping failure (frame dropped).
bool emitFrame(const std::shared_ptr<Session> &s, AVFrame *out,
               QVideoFrameFormat::PixelFormat outFormat, QtVideo::Rotation rotation,
               qint64 ptsUs, qint64 delayUs)
{
    // Rotation is honored at the frame level by the sink/renderer, including
    // imported textures, so portrait camera streams remain upright.
    // 4:2:0 is 1.5 bytes per pixel, which also budgets retained VAAPI surfaces.
    return publishVideoFrame(s, copyToVideoFrame(out, outFormat), rotation, ptsUs, delayUs,
                             qint64(out->width) * out->height * 3 / 2);
}

// ---- Hardware decode ------------------------------------------------------
// Offload H.264/H.265 decode to the GPU (VAAPI/CUDA/VDPAU). Intel/AMD VAAPI
// NV12 surfaces are handed to Qt Quick as DMA-BUF textures; other formats,
// render backends, and import failures use the existing CPU upload path.
// Any decoder setup failure falls back to software decode, per-stream.
bool hwDecodeEnabled()
{
    return qgetenv("RL_HWDECODE") != "0"; // default on
}

AVPixelFormat getHwFormat(AVCodecContext *ctx, const AVPixelFormat *fmts)
{
    const auto want = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == want)
            return *p;
    // Wanted GPU surface not offered (or hwaccel init failed and ffmpeg is
    // re-asking): pick the first SOFTWARE format, not fmts[0] — the list leads
    // with hw formats, and returning a refused one loops the failure (e.g. VAAPI
    // rejecting 2160x7680, which exceeds its 4352 max height).
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    }
    return fmts[0];
}

// Configures dec for hardware decode; returns the hw pixel format (or NONE).
AVPixelFormat setupHwDecode(AVCodecContext *dec, const AVCodec *codec, AVBufferRef **hwCtx)
{
    static const AVHWDeviceType order[] = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA,
                                           AV_HWDEVICE_TYPE_VDPAU};
    for (AVHWDeviceType type : order) {
        AVPixelFormat pixfmt = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == type) {
                pixfmt = cfg->pix_fmt;
                break;
            }
        }
        if (pixfmt == AV_PIX_FMT_NONE)
            continue;
        AVBufferRef *ctx = nullptr;
        if (av_hwdevice_ctx_create(&ctx, type, nullptr, nullptr, 0) < 0)
            continue;
        *hwCtx = ctx;
        dec->hw_device_ctx = av_buffer_ref(ctx);
        dec->opaque = reinterpret_cast<void *>(static_cast<intptr_t>(pixfmt));
        dec->get_format = getHwFormat;
        qCInfo(lcMedia) << "hw decode via" << av_hwdevice_get_type_name(type);
        return pixfmt;
    }
    return AV_PIX_FMT_NONE;
}

// AVIOContext read callback for a custom packet source (native Baichuan): pull
// elementary-stream bytes from the session's blocking reader.
int avioReadThunk(void *opaque, uint8_t *buf, int size)
{
    auto *s = static_cast<Session *>(opaque);
    if (s->abort.load())
        return AVERROR_EOF;
    return s->readPacket(buf, size);
}

// One open→decode session. Returns true when the caller should retry (reconnect).
bool runSession(const std::shared_ptr<Session> &s, bool *streamingOut)
{
    const QString source = s->source;
    const QByteArray url = source.toUtf8();
    // A custom packet source (native Baichuan) is treated as live: real-time paced
    // by the device, no URL to open.
    const bool packetSource = static_cast<bool>(s->readPacket);
    const bool live = packetSource || isLiveUrl(source);
    const bool httpUrl = !packetSource && (url.startsWith("http://") || url.startsWith("https://"));
    // A server that signs in with a session cookie (Frigate on 8971) needs it on the playlist
    // and on every segment; FFmpeg's HLS reader passes `headers` on to the segments. Fetched
    // before the open deadline starts, since getting it may mean signing in first.
    const QByteArray authHeaders = httpUrl ? httpauth::headersFor(QUrl(source)) : QByteArray();

    InterruptContext interrupt{s.get(), QDeadlineTimer(kOpenTimeoutMs)};

    AVFormatContext *fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = &InterruptContext::callback;
    fmt->interrupt_callback.opaque = &interrupt;

    // Custom-IO cleanup must outlive fmt close (avformat_close_input leaves a
    // caller-supplied AVIOContext alone), so this guard is declared first.
    AVIOContext *avioCtx = nullptr;
    struct AvioGuard {
        AVIOContext **pb;
        ~AvioGuard()
        {
            if (*pb) {
                av_freep(&(*pb)->buffer);
                avio_context_free(pb);
            }
        }
    } avioGuard{&avioCtx};

    AVDictionary *opts = nullptr;
    const AVInputFormat *ifmt = nullptr;
    if (packetSource) {
        constexpr int kAvioBuf = 1 << 16;
        avioCtx = avio_alloc_context(static_cast<unsigned char *>(av_malloc(kAvioBuf)), kAvioBuf,
                                     0, s.get(), &avioReadThunk, nullptr, nullptr);
        fmt->pb = avioCtx;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        ifmt = av_find_input_format(s->forcedFormat.toUtf8().constData());
        // Raw elementary stream over non-seekable IO: a big probesize lets the
        // parser find the SPS/VPS in the first I-frame (can be ~600 KB) so the size
        // isn't "unspecified". Keep analyzeduration SHORT — the codec params are all
        // in the first IDR, and waiting to estimate the framerate over many frames
        // adds seconds of startup latency on a realtime live stream (playback is
        // unaffected since its frames arrive faster than realtime).
        av_dict_set(&opts, "probesize", "4000000", 0);
        av_dict_set(&opts, "analyzeduration", "300000", 0);
    } else if (live) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        // Don't spend seconds estimating the framerate — the codec params arrive in
        // the SDP/first IDR, so cap the probe to show live video sooner. Do NOT set
        // fflags=nobuffer here: it discards the keyframe find_stream_info already
        // read, forcing the decode loop to wait a whole extra GOP (~4s on the sub
        // stream) for the next IDR before the first frame appears.
        av_dict_set(&opts, "analyzeduration", "500000", 0);
        av_dict_set(&opts, "probesize", "500000", 0);
    }
    // Reolink devices serve HTTPS (playback FLV, snapshots) with a SELF-SIGNED
    // certificate — there is no CA to validate against on a LAN appliance, and
    // the HTTP-CGI transport has always accepted it (CURLOPT_SSL_VERIFYPEER 0).
    // FFmpeg 8.0 flipped tls_verify's default from 0 to 1, so the media path
    // started rejecting the same certificate the rest of the app accepts:
    // playback died with "Peer certificate failed verification" and retried
    // into a black screen. State the project's existing posture explicitly
    // rather than depending on a library default that moved under us.
    if (!packetSource && (url.startsWith("https://") || url.startsWith("rtsps://")))
        av_dict_set(&opts, "tls_verify", "0", 0);
    if (!authHeaders.isEmpty())
        av_dict_set(&opts, "headers", authHeaders.constData(), 0);
    int rc = avformat_open_input(&fmt, packetSource ? nullptr : url.constData(), ifmt, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        if (rc == AVERROR_HTTP_UNAUTHORIZED && !authHeaders.isEmpty())
            httpauth::forgetHeadersFor(QUrl(source)); // a stale session: the retry signs in afresh
        char buf[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(rc, buf, sizeof(buf));
        // A recorder answering "not found" for a camera's stream means it has no video
        // from that camera (unreachable, or not a stream it can relay), not that the
        // app is stuck: say so.
        // A server that wants a login (a Frigate restream, an IP camera) answers 401: say so, since
        // "Server returned 401 Unauthorized" reads like the app failing rather than the login.
        const QString reason = rc == AVERROR_HTTP_NOT_FOUND
            ? QStringLiteral("Stream not found: the recorder has no video from this camera")
            : rc == AVERROR_HTTP_UNAUTHORIZED
                ? QStringLiteral("Sign-in refused (401): this stream needs a login, or the saved one is wrong")
                : QString::fromUtf8(buf);
        if (!packetSource) {
            // Log what was asked for, minus secrets, so a server-side refusal (a Reolink
            // NVR just drops the connection: "End of file") can be diagnosed from the log.
            QString shown = QString::fromUtf8(url);
            static const QRegularExpression queryLogin(
                QStringLiteral("((?:token|password|user)=)[^&]*"));
            static const QRegularExpression urlLogin(QStringLiteral("(://[^:/@]+:)[^@/]+@"));
            shown.replace(queryLogin, QStringLiteral("\\1***"));
            shown.replace(urlLogin, QStringLiteral("\\1***@"));
            qCWarning(lcMedia).noquote() << "open failed:" << QString::fromUtf8(buf) << "for" << shown;
        }
        postState(s, State::Error, reason);
        return !packetSource && (live || s->retryOnError.load());
    }

    struct FmtGuard {
        AVFormatContext *ctx;
        ~FmtGuard() { avformat_close_input(&ctx); }
    } fmtGuard{fmt};

    interrupt.deadline = QDeadlineTimer(kOpenTimeoutMs);
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        postState(s, State::Error, QStringLiteral("could not read stream info"));
        return !packetSource && (live || s->retryOnError.load());
    }

    const int videoIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        postState(s, State::Error, QStringLiteral("no video stream"));
        return false;
    }
    AVStream *stream = fmt->streams[videoIndex];

    // The camera's audio track, if any (RTSP/FLV demux it alongside the video; the
    // native Baichuan raw stream has none — its audio arrives via AudioTap instead).
    // Decoding is deferred until the pane is unmuted.
    const int audioIndex =
        packetSource ? -1 : av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
    std::unique_ptr<AudioDecoder> audioDec;
    if (audioIndex >= 0 && AudioDecoder::canDecode(fmt->streams[audioIndex]->codecpar))
        postHasAudio(s, true);
    else if (audioIndex >= 0)
        qCInfo(lcMedia) << redacted(source) << "audio codec"
                        << avcodec_get_name(fmt->streams[audioIndex]->codecpar->codec_id)
                        << "not decodable — staying silent";

    const QtVideo::Rotation rotation = streamRotation(stream, s->expectedSize);

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        postState(s, State::Error, QStringLiteral("unsupported codec"));
        return false;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    struct DecGuard {
        AVCodecContext *ctx;
        ~DecGuard() { avcodec_free_context(&ctx); }
    } decGuard{dec};

    avcodec_parameters_to_context(dec, stream->codecpar);
    // Frame threads each hold a frame in flight and a malloc arena. Hardware decode needs
    // one; software gets a few for big pictures and two for sub streams, not one per core.
    {
        const int pixels = stream->codecpar->width * stream->codecpar->height;
        dec->thread_count = pixels > 1920 * 1088 ? 4 : 2;
    }

    AVBufferRef *hwCtx = nullptr;
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
    // Reverse play decodes in software: it holds CPU copies anyway, and pulling 4K–8K frames
    // back from the GPU is slower than decoding them (wayfinder/live-replay ticket 06).
    if (hwDecodeEnabled() && !s->reverse)
        hwPixFmt = setupHwDecode(dec, codec, &hwCtx);
    if (hwPixFmt != AV_PIX_FMT_NONE)
        dec->thread_count = 1;
    struct HwGuard {
        AVBufferRef **ctx;
        ~HwGuard() { av_buffer_unref(ctx); }
    } hwGuard{&hwCtx};

    if (avcodec_open2(dec, codec, nullptr) < 0) {
        // Hardware path can fail at open on some driver/profile combos; retry
        // software before giving up.
        if (hwPixFmt != AV_PIX_FMT_NONE) {
            qCInfo(lcMedia) << redacted(source) << "hw decoder open failed — software fallback";
            av_buffer_unref(&dec->hw_device_ctx);
            av_buffer_unref(&hwCtx);
            dec->get_format = nullptr;
            dec->opaque = nullptr;
            hwPixFmt = AV_PIX_FMT_NONE;
            dec->thread_count = stream->codecpar->width * stream->codecpar->height > 1920 * 1088 ? 4 : 2;
            if (avcodec_open2(dec, codec, nullptr) < 0) {
                postState(s, State::Error, QStringLiteral("could not open decoder"));
                return false;
            }
        } else {
            postState(s, State::Error, QStringLiteral("could not open decoder"));
            return false;
        }
    }

    qCInfo(lcMedia) << redacted(source) << "opened:" << codec->name << stream->codecpar->width
                    << "x" << stream->codecpar->height
                    << (hwPixFmt != AV_PIX_FMT_NONE ? "[hw]" : "[sw]");

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *hwTransfer = av_frame_alloc();
    AVFrame *converted = av_frame_alloc();
    SwsContext *sws = nullptr;
    struct LoopGuard {
        AVPacket *p;
        AVFrame *f1, *f2, *f3;
        SwsContext **s;
        ~LoopGuard()
        {
            av_packet_free(&p);
            av_frame_free(&f1);
            av_frame_free(&f2);
            av_frame_free(&f3);
            sws_freeContext(*s);
        }
    } loopGuard{packet, frame, hwTransfer, converted, &sws};

    bool streaming = *streamingOut;
    qint64 playbackStartUs = 0;
    double pacedSpeed = 1.0; // the speed the current pacing anchor was set at
    qint64 firstPtsUs = AV_NOPTS_VALUE;
    qint64 smoothDelayUs = 0; // chosen at the first smoothed frame
    const bool perfDiagnosticsEnabled = lcMedia().isDebugEnabled();
    auto perfWindowStart = std::chrono::steady_clock::now();
    qint64 perfPipelineUs = 0;
    qint64 perfFrames = 0;
    qint64 perfVaapiFrames = 0;
    qint64 perfHwTransferFrames = 0;
    qint64 perfSoftwareFrames = 0;

    qint64 frameEpochPostedWall = 0; // when the frame time was last posted to the GUI
    // Bookkeeping for a frame handed to the screen: counts, its recording time, Streaming.
    auto delivered = [&](qint64 frameEpoch, bool vaapiOutput, bool transferred) {
        if (perfDiagnosticsEnabled) {
            ++perfFrames;
            if (vaapiOutput)
                ++perfVaapiFrames;
            else if (transferred)
                ++perfHwTransferFrames;
            else
                ++perfSoftwareFrames;
        }
        const qint64 n = s->framesDecoded.fetch_add(1) + 1;
        if (frameEpoch >= 0) {
            const qint64 was = s->frameEpochMs.exchange(frameEpoch);
            // At most 4 a second, and at once on a jump (a seek).
            if (qAbs(frameEpoch - was) > 2000 || av_gettime_relative() - frameEpochPostedWall >= 250000) {
                frameEpochPostedWall = av_gettime_relative();
                postFrameEpoch(s);
            }
        }
        if (!streaming) {
            streaming = true;
            *streamingOut = true;
            postState(s, State::Streaming, QString());
            qCInfo(lcMedia) << redacted(source) << "first frame delivered";
        }
        if (n % 100 == 0)
            postFramesTick(s);
    };
    // Reverse play: decoded groups come back reversed, shown at their recording-time gaps.
    rl::reverse::GopReverser<QVideoFrame> reverser;
    QSize reverseHold;                       // the size this group is held at
    SwsContext *reverseSws = nullptr;
    AVFrame *reverseScaled = av_frame_alloc();
    struct ReverseGuard {
        AVFrame *f;
        SwsContext **s;
        ~ReverseGuard()
        {
            av_frame_free(&f);
            sws_freeContext(*s);
        }
    } reverseGuard{reverseScaled, &reverseSws};
    // Reversed frames wait in a show queue, shown at their recording-time gaps ÷ speed. The
    // loop below decodes the next group only once what is left to show is about as long as a
    // group takes to decode, so decoding overlaps showing (no pause between groups) while
    // holding about one group and a bit (wayfinder/live-replay ticket 06).
    QList<rl::reverse::GopReverser<QVideoFrame>::Item> showQueue;
    qint64 reverseShownMs = -1;
    qint64 reverseShownWallUs = 0;
    qint64 decodeUsPerFrame = 0;   // decode + scale + copy, averaged
    int lastGroupFrames = 0;
    // Groups that take longer to decode than to show (8K at 4×): too slow, like a slow link.
    rl::reverse::LinkPace decodePace;
    bool decodeSlowSaid = false;
    auto queueReversed = [&](QList<rl::reverse::GopReverser<QVideoFrame>::Item> items) {
        if (items.isEmpty())
            return;
        lastGroupFrames = int(items.size());
        const double speed = qMax(0.25, s->speed->load(std::memory_order_relaxed));
        decodePace.window(double(decodeUsPerFrame) * lastGroupFrames / 1e6,
                          double(items.first().epochMs - items.last().epochMs) / 1000.0 / speed);
        if (decodePace.tooSlow() && !decodeSlowSaid) {
            decodeSlowSaid = true;
            qCInfo(lcMedia) << redacted(source) << "reverse: decoding can't keep up at this speed";
            postTooSlow(s);
        }
        showQueue.append(std::move(items));
    };
    auto queuedUs = [&]() -> qint64 {
        if (showQueue.isEmpty())
            return 0;
        const double speed = qMax(0.25, s->speed->load(std::memory_order_relaxed));
        return qint64(double(showQueue.first().epochMs - showQueue.last().epochMs) * 1000.0 / speed);
    };
    // Show the next queued frame if it is due; with `wait`, sleep until it is. True if shown.
    auto showNext = [&](bool wait) -> bool {
        if (showQueue.isEmpty() || s->abort.load())
            return false;
        auto &item = showQueue.first();
        // The time spent since the last frame (decoding) counts towards the wait.
        const qint64 waitUs = rl::reverse::waitUs(reverseShownMs, item.epochMs,
                                                  s->speed->load(std::memory_order_relaxed))
                            - (av_gettime_relative() - reverseShownWallUs);
        if (waitUs > 0) {
            if (!wait)
                return false;
            abortableSleepUs(s.get(), waitUs);
        }
        if (s->abort.load())
            return false;
        auto shown = showQueue.takeFirst();
        reverseShownMs = shown.epochMs;
        reverseShownWallUs = av_gettime_relative();
        const qint64 bytes = qint64(shown.frame.width()) * shown.frame.height() * 3 / 2;
        if (publishVideoFrame(s, std::move(shown.frame), rotation, AV_NOPTS_VALUE, 0, bytes))
            delivered(shown.epochMs, false, false);
        return true;
    };
    // Drain one decoded frame at a time; shared by the normal and flush paths.
    auto receiveFrames = [&]() {
        while (avcodec_receive_frame(dec, frame) == 0 && !s->abort.load()) {
            // Don't render frames the decoder knows are damaged (missing slices or
            // references — e.g. Reolink's flawed RTSP output for 8K duo mains).
            // A brief freeze beats a band of macroblock garbage.
            if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags) {
                av_frame_unref(frame);
                continue;
            }
            const qint64 frameEpoch = frameEpochOf(frame); // before a transfer drops side data
            // Keep compatible VAAPI NV12 surfaces on the GPU. If export is not
            // available for this stream, use the existing transfer/upload path.
            QVideoFrame vaapiVideoFrame;
            AVFrame *decoded = frame;
            if (hwPixFmt != AV_PIX_FMT_NONE && frame->format == hwPixFmt) {
                // Reverse play holds a whole group of frames: copies, not decoder surfaces.
                if (hwPixFmt == AV_PIX_FMT_VAAPI && !s->reverse
                    && !s->vaapiInterop->unavailable.load(std::memory_order_relaxed)) {
                    vaapiVideoFrame = makeVaapiVideoFrame(frame, s->vaapiInterop);
                }
                if (!vaapiVideoFrame.isValid()) {
                    if (av_hwframe_transfer_data(hwTransfer, frame, 0) < 0) {
                        av_frame_unref(frame);
                        continue;
                    }
                    hwTransfer->pts = frame->pts; // transfer drops timing metadata
                    decoded = hwTransfer;
                }
            }

            const QVideoFrameFormat::PixelFormat qtFormat = vaapiVideoFrame.isValid()
                ? QVideoFrameFormat::Format_NV12 : mapPixelFormat(decoded->format);
            AVFrame *out = decoded;
            QVideoFrameFormat::PixelFormat outFormat = qtFormat;
            if (qtFormat == QVideoFrameFormat::Format_Invalid) {
                sws = sws_getCachedContext(sws, decoded->width, decoded->height,
                                           static_cast<AVPixelFormat>(decoded->format),
                                           decoded->width, decoded->height, AV_PIX_FMT_YUV420P,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws) {
                    if (decoded == hwTransfer)
                        av_frame_unref(hwTransfer);
                    av_frame_unref(frame);
                    continue; // swscale can't ingest this pixel format; drop
                }
                converted->width = decoded->width;
                converted->height = decoded->height;
                converted->format = AV_PIX_FMT_YUV420P;
                if (av_frame_get_buffer(converted, 0) < 0) {
                    if (decoded == hwTransfer)
                        av_frame_unref(hwTransfer);
                    av_frame_unref(frame);
                    continue;
                }
                sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height,
                          converted->data, converted->linesize);
                out = converted;
                outFormat = QVideoFrameFormat::Format_YUV420P;
            }

            // File pacing: map the stream clock onto the wall clock (live is paced
            // by the network).
            if (!live && decoded->pts != AV_NOPTS_VALUE) {
                const qint64 ptsUs =
                    av_rescale_q(decoded->pts, stream->time_base, AVRational{1, 1000000});
                const double speedNow = s->speed->load(std::memory_order_relaxed);
                if (firstPtsUs == static_cast<qint64>(AV_NOPTS_VALUE) || ptsUs < firstPtsUs
                    || speedNow != pacedSpeed) {
                    // (Re)anchor here: first frame, a jump back, or a speed change.
                    firstPtsUs = ptsUs;
                    playbackStartUs = av_gettime_relative();
                    pacedSpeed = speedNow;
                }
                const qint64 waitUs =
                    (playbackStartUs + qint64(double(ptsUs - firstPtsUs) / pacedSpeed)) - av_gettime_relative();
                if (waitUs > 0 && waitUs < 2000000)
                    abortableSleepUs(s.get(), waitUs);
            }

            // Live RTSP with smoothing on: give the frame its camera timestamp and the
            // delay to hold it for. Anything else is shown the moment it arrives.
            qint64 framePtsUs = AV_NOPTS_VALUE;
            qint64 frameDelayUs = 0;
            if (live && !packetSource && decoded->pts != AV_NOPTS_VALUE &&
                s->smoothing.load(std::memory_order_relaxed)) {
                framePtsUs = av_rescale_q(decoded->pts, stream->time_base, AVRational{1, 1000000});
                if (smoothDelayUs == 0)
                    smoothDelayUs = smoothDelayFor(decoded->width, decoded->height,
                                                   av_q2d(stream->avg_frame_rate));
                frameDelayUs = smoothDelayUs;
            }
            const bool vaapiOutput = vaapiVideoFrame.isValid();
            if (s->reverse) {
                // Reverse play: hold the group, show it backwards when the next one starts.
                // AV_FRAME_FLAG_KEY is FFmpeg 6.1+ (libavutil 58.29); the Flatpak runtime's
                // older FFmpeg has key_frame.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 29, 100)
                const bool key = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
#else
                const bool key = frame->key_frame != 0;
#endif
                // Held at the pane's size (a fisheye's up to 1920²), chosen at each group's start.
                if (key || reverseHold.isEmpty()) {
                    const QSize was = reverseHold;
                    reverseHold = rl::reverse::holdSize(
                        QSize(out->width, out->height),
                        QSize(s->holdW.load(), s->holdH.load()), s->holdFisheye.load());
                    if (reverseHold != was)
                        qCInfo(lcMedia) << redacted(source) << "reverse: holding" << out->width << "x"
                                        << out->height << "groups at" << reverseHold;
                }
                AVFrame *held = out;
                QVideoFrameFormat::PixelFormat heldFormat = outFormat;
                if (reverseHold != QSize(out->width, out->height)) {
                    reverseSws = sws_getCachedContext(
                        reverseSws, out->width, out->height, static_cast<AVPixelFormat>(out->format),
                        reverseHold.width(), reverseHold.height(), AV_PIX_FMT_YUV420P,
                        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    reverseScaled->width = reverseHold.width();
                    reverseScaled->height = reverseHold.height();
                    reverseScaled->format = AV_PIX_FMT_YUV420P;
                    if (reverseSws && av_frame_get_buffer(reverseScaled, 0) >= 0) {
                        sws_scale(reverseSws, out->data, out->linesize, 0, out->height,
                                  reverseScaled->data, reverseScaled->linesize);
                        // swscale turns full-range (J) input into limited range; keep the rest.
                        reverseScaled->color_range = out->format == AV_PIX_FMT_YUVJ420P
                                                         ? AVCOL_RANGE_MPEG : out->color_range;
                        reverseScaled->colorspace = out->colorspace;
                        reverseScaled->color_primaries = out->color_primaries;
                        reverseScaled->color_trc = out->color_trc;
                        held = reverseScaled;
                        heldFormat = QVideoFrameFormat::Format_YUV420P;
                    }
                }
                QVideoFrame copy = copyToVideoFrame(held, heldFormat);
                if (held == reverseScaled)
                    av_frame_unref(reverseScaled);
                if (copy.isValid())
                    queueReversed(reverser.push(std::move(copy), key, frameEpoch));
            } else {
                const bool emitted = vaapiOutput
                    ? publishVideoFrame(s, std::move(vaapiVideoFrame), rotation, framePtsUs,
                                        frameDelayUs, qint64(decoded->width) * decoded->height * 3 / 2)
                    : emitFrame(s, out, outFormat, rotation, framePtsUs, frameDelayUs);
                if (emitted)
                    delivered(frameEpoch, vaapiOutput, decoded == hwTransfer);
            }
            if (out == converted)
                av_frame_unref(converted);
            if (decoded == hwTransfer)
                av_frame_unref(hwTransfer);
            av_frame_unref(frame);
        }
    };

    Recorder recorder;
    // Opens/closes the recorder to match the requested state. Called each packet.
    auto syncRecording = [&]() {
        if (s->recordRequested.load() && !recorder.active()) {
            QString path;
            {
                QMutexLocker lock(&s->recMutex);
                path = s->recPath;
            }
            if (recorder.open(path, stream)) {
                s->recording.store(true);
                postRecording(s, true, path, QString());
                qCInfo(lcMedia) << redacted(source) << "recording to" << path;
            } else {
                recorder.finalize();
                s->recordRequested.store(false);
                postRecording(s, false, QString(), QStringLiteral("could not open recording file"));
            }
        } else if (!s->recordRequested.load() && recorder.active()) {
            const QString saved = recorder.finalize();
            s->recording.store(false);
            postRecording(s, false, saved, saved.isEmpty()
                                               ? QStringLiteral("no frames captured")
                                               : QString());
        }
    };
    // Finalize on any session exit (EOF, reconnect, stop). Recording does not
    // survive a reconnect — it saves what it has and disarms.
    auto finalizeRecording = [&]() {
        if (!recorder.active())
            return;
        const QString saved = recorder.finalize();
        s->recording.store(false);
        s->recordRequested.store(false);
        postRecording(s, false, saved,
                      saved.isEmpty() ? QStringLiteral("no frames captured") : QString());
    };

    while (!s->abort.load()) {
        if (s->reverse) {
            while (showNext(false)) {}
            // Decode ahead only when the queue is about to run out (1.5× a group's decode time).
            const qint64 aheadUs = qMax<qint64>(150000, decodeUsPerFrame * qMax(lastGroupFrames, 10) * 3 / 2);
            if (queuedUs() > aheadUs) {
                showNext(true);
                continue;
            }
        }
        interrupt.deadline = QDeadlineTimer(kReadStallTimeoutMs);
        rc = av_read_frame(fmt, packet);
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(dec, nullptr); // flush: drain buffered frames
            receiveFrames();
            queueReversed(reverser.flush()); // reverse play: the last group
            while (showNext(true)) {}
            if (!live && s->loop.load()) {
                if (av_seek_frame(fmt, videoIndex, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                    avcodec_flush_buffers(dec);
                    firstPtsUs = AV_NOPTS_VALUE;
                    continue;
                }
                // Non-seekable input can't loop; fall through to a reconnect.
            }
            finalizeRecording();
            return live && !packetSource; // a custom source is one-shot: don't retry
        }
        if (rc < 0) {
            postState(s, State::Error, QStringLiteral("read error / stalled"));
            finalizeRecording();
            return !packetSource && (live || s->retryOnError.load());
        }
        if (packet->stream_index == audioIndex) {
            if (s->audioWanted.load(std::memory_order_relaxed)) {
                if (!audioDec) {
                    audioDec = std::make_unique<AudioDecoder>();
                    if (!audioDec->open(fmt->streams[audioIndex]->codecpar)) {
                        audioDec.reset();
                        s->audioWanted.store(false); // give up rather than retry per packet
                    }
                }
                if (audioDec) {
                    const QByteArray pcm = audioDec->decode(packet);
                    if (!pcm.isEmpty())
                        postAudio(s, pcm);
                }
            }
            av_packet_unref(packet);
            continue;
        }
        if (packet->stream_index != videoIndex) {
            av_packet_unref(packet);
            continue;
        }
        syncRecording();
        if (recorder.active())
            recorder.write(packet); // stream-copy before the packet is consumed
        const auto pipelineStart = perfDiagnosticsEnabled
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const qint64 decodeStartUs = av_gettime_relative();
        rc = avcodec_send_packet(dec, packet);
        av_packet_unref(packet);
        if (rc < 0 && rc != AVERROR(EAGAIN))
            continue; // tolerate bitstream hiccups on live sources
        receiveFrames();
        if (s->reverse) {   // one frame a packet: the average cost of a frame
            const qint64 tookUs = av_gettime_relative() - decodeStartUs;
            decodeUsPerFrame = decodeUsPerFrame == 0 ? tookUs : (decodeUsPerFrame * 7 + tookUs) / 8;
        }
        if (perfDiagnosticsEnabled) {
            perfPipelineUs += std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - pipelineStart)
                                  .count();
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - perfWindowStart).count();
            if (seconds >= 5.0) {
                const auto pixFmtName = hwPixFmt == AV_PIX_FMT_NONE
                    ? QByteArrayLiteral("software")
                    : QByteArray(av_get_pix_fmt_name(hwPixFmt));
                qCDebug(lcMedia).noquote()
                    << redacted(source) << "perf: fps=" << QString::number(perfFrames / seconds, 'f', 1)
                    << "pipeline_ms/s=" << QString::number(perfPipelineUs / 1000.0 / seconds, 'f', 1)
                    << "decode=" << pixFmtName
                    << "qt_hw_frames=" << perfVaapiFrames
                    << "hw_download_frames=" << perfHwTransferFrames
                    << "software_frames=" << perfSoftwareFrames
                    << "interop_disabled=" << s->vaapiInterop->unavailable.load()
                    << "playout_frames=" << s->playoutFrames.load()
                    << "playout_peak_frames=" << s->playoutPeakFrames.load()
                    << "surface_mib_est=" << QString::number(s->playoutBytes.load() / 1048576.0, 'f', 1)
                    << "surface_peak_mib_est=" << QString::number(s->playoutPeakBytes.load() / 1048576.0, 'f', 1)
                    << "late_drop_total=" << s->playoutLateDrops.load()
                    << "overflow_drop_total=" << s->playoutOverflowDrops.load()
                    << "mailbox_overwrite_total=" << s->frameHandoff.overwrittenCount();
                perfWindowStart = now;
                perfPipelineUs = 0;
                perfFrames = 0;
                perfVaapiFrames = 0;
                perfHwTransferFrames = 0;
                perfSoftwareFrames = 0;
            }
        }
    }
    finalizeRecording(); // clean stop
    return false;
}

void runWorker(std::shared_ptr<Session> s)
{
    Backoff backoff(kBackoff);
    bool streaming = false;
    // Live sources reconnect indefinitely; a non-live retry (playback FLV on a
    // busy NVR) is capped so it can't loop forever.
    const bool live = isLiveUrl(s->source);
    int retriesLeft = 6;
    while (!s->abort.load()) {
        const qint64 framesBefore = s->framesDecoded.load();
        const bool retry = runSession(s, &streaming);
        if (s->abort.load() || !retry)
            break;
        if (s->framesDecoded.load() - framesBefore > 100) {
            backoff.reset();
            retriesLeft = 6; // made progress; reset the budget
        }
        if (!live && --retriesLeft <= 0)
            break;
        // After a failed attempt keep showing why it failed while we retry, rather than
        // flipping straight back to a "connecting" spinner that never explains itself.
        if (!s->lastWasError.load())
            postState(s, State::Connecting, QStringLiteral("reconnecting"));
        QDeadlineTimer wait(backoff.fail());
        while (!wait.hasExpired() && !s->abort.load())
            QThread::msleep(50);
    }
    if (!s->abort.load()) // terminal non-retryable end (EOF/no-video)
        postState(s, State::Stopped, QString());
}

} // namespace

// Entry point for Baichuan audio. The BaichuanClient's network thread calls push()
// with one ADTS AAC frame at a time; it lives independently of the StreamPlayer (the
// client outlives neither unpredictably) and reaches the player only through the
// Session's back-pointer, so a late frame after teardown is a no-op.
struct StreamPlayer::AudioTap {
    QMutex mutex;                   // guards `session`
    std::weak_ptr<Session> session; // the currently running session, if any
    QMutex decMutex;                // guards `decoder`
    std::unique_ptr<AudioDecoder> decoder;

    void bind(const std::shared_ptr<Session> &s)
    {
        {
            QMutexLocker lock(&mutex);
            session = s;
        }
        QMutexLocker lock(&decMutex);
        decoder.reset(); // new session, fresh decoder state
    }

    void push(const QByteArray &adts)
    {
        std::shared_ptr<Session> s;
        {
            QMutexLocker lock(&mutex);
            s = session.lock();
        }
        if (!s)
            return;
        if (adts.isEmpty()) { // seek: the decoder and queued sound belong to the old position
            {
                QMutexLocker lock(&decMutex);
                decoder.reset();
            }
            postAudioFlush(s);
            return;
        }
        // Existence of an audio track is worth reporting even while muted, so the UI
        // can offer an unmute control; decoding is what muting saves.
        if (!s->announcedAudio.exchange(true))
            postHasAudio(s, true);
        if (!s->audioWanted.load(std::memory_order_relaxed))
            return;
        QByteArray pcm;
        {
            QMutexLocker lock(&decMutex);
            if (!decoder) {
                auto d = std::make_unique<AudioDecoder>();
                if (!d->openAdtsAac()) {
                    s->audioWanted.store(false);
                    return;
                }
                decoder = std::move(d);
            }
            pcm = decoder->decode(adts);
        }
        if (!pcm.isEmpty())
            postAudio(s, pcm);
    }
};

std::function<void(const QByteArray &)> StreamPlayer::audioFeed()
{
    // Each packet source needs its own tap. Rebinding a shared tap on replacement
    // would let the previous Baichuan client's late audio enter the new session.
    m_tap = std::make_shared<AudioTap>();
    // Capture the tap, never `this`: the client keeps this callback for as long as it
    // runs, which can be past this player's destruction.
    return [tap = m_tap](const QByteArray &adts) { tap->push(adts); };
}

void StreamPlayer::setMuted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;
    if (m_session)
        m_session->audioWanted.store(!muted && speed() == 1.0);
    if (muted)
        m_audioOut.reset(); // release the device and drop queued sound at once
    emit mutedChanged();
}

void StreamPlayer::setSmoothing(bool on)
{
    if (m_smoothing == on)
        return;
    m_smoothing = on;
    if (m_session)
        m_session->smoothing.store(on); // takes effect on the next frame
    emit smoothingChanged();
}

// Show one frame on the sink (GUI thread).
void StreamPlayer::presentFrame(const QVideoFrame &frame)
{
    if (!m_session)
        return;
    QMutexLocker sinkLock(&m_session->sinkMutex);
    if (m_session->sink)
        m_session->sink->setVideoFrame(frame);
}

void StreamPlayer::applyFrameFromWorker(const std::shared_ptr<Session> &s, const QVideoFrame &frame,
                                        qint64 ptsUs, qint64 delayUs, qint64 bytes)
{
    if (s != m_session)
        return; // from a stream that has since been stopped or replaced
    if (delayUs <= 0 || ptsUs == static_cast<qint64>(AV_NOPTS_VALUE)) {
        presentFrame(frame);
        return;
    }
    const int delayMs = int(delayUs / 1000);
    if (delayMs != m_liveDelayMs) {
        auto cfg = m_playout.config();
        cfg.delayUs = delayUs;
        cfg.maxDepthUs = delayUs * 5 / 2;
        cfg.maxBytes = kPlayoutMaxPerStream; // hard ceiling; the delay is what sizes the queue
        m_playout.setConfig(cfg);
        m_liveDelayMs = delayMs;
        if (m_audioOut)
            m_audioOut->setDelayMs(delayMs);
    }
    m_playout.push(frame, ptsUs, nowUs(), bytes);
    servicePlayout();
}

// Show whatever is due, then wake up when the next frame is.
void StreamPlayer::servicePlayout()
{
    QVideoFrame frame;
    const qint64 now = nowUs();
    if (m_playout.poll(now, frame))
        presentFrame(frame);
    updatePlayoutMetrics();
    const qint64 next = m_playout.nextDueUs();
    if (next >= 0)
        m_playoutTimer.start(int(qMax<qint64>(1, (next - now + 999) / 1000)));
}

void StreamPlayer::updatePlayoutMetrics()
{
    if (!m_session || !lcMedia().isDebugEnabled())
        return;
    const auto session = m_session;
    const int queuedFrames = m_playout.size();
    const qint64 queuedBytes = m_playout.queuedBytes();
    session->playoutFrames.store(queuedFrames, std::memory_order_relaxed);
    session->playoutBytes.store(queuedBytes, std::memory_order_relaxed);
    updateMaximum(session->playoutPeakFrames, queuedFrames);
    updateMaximum(session->playoutPeakBytes, queuedBytes);
    const auto &stats = m_playout.stats();
    session->playoutLateDrops.store(stats.droppedLate, std::memory_order_relaxed);
    session->playoutOverflowDrops.store(stats.droppedOverflow, std::memory_order_relaxed);
}

void StreamPlayer::resetPlayout()
{
    m_playoutTimer.stop();
    m_playout.clear();
    updatePlayoutMetrics();
    if (m_liveDelayMs != 0) {
        m_liveDelayMs = 0;
        if (m_audioOut)
            m_audioOut->setDelayMs(0);
    }
}

void StreamPlayer::setSpeed(qreal speed)
{
    speed = qBound<qreal>(0.25, speed, 16.0); // 16x = the recorder's keyframe mode (cmd 381)
    if (qFuzzyCompare(m_speed->load(), speed))
        return;
    m_speed->store(speed); // read per frame by both pacing loops
    // Sound only makes sense at normal speed; drop what is queued when leaving it.
    if (m_session)
        m_session->audioWanted.store(!m_muted && speed == 1.0);
    if (speed != 1.0)
        m_audioOut.reset();
    emit speedChanged();
}

bool StreamPlayer::saveSnapshot(const QString &path)
{
    if (!m_session)
        return false;
    QVideoFrame frame;
    {
        QMutexLocker lock(&m_session->lastFrameMutex);
        frame = m_session->lastFrame;
    }
    if (!frame.isValid())
        return false;
    const QImage img = frame.toImage();
    return !img.isNull() && img.save(path);
}

bool StreamPlayer::saveThumbnail(const QString &path, int maxWidth)
{
    if (!m_session)
        return false;
    QVideoFrame frame;
    {
        QMutexLocker lock(&m_session->lastFrameMutex);
        frame = m_session->lastFrame;
    }
    if (!frame.isValid())
        return false;
    QImage img = frame.toImage();
    if (img.isNull())
        return false;
    (void)QtConcurrent::run([img = std::move(img), path, maxWidth]() {
        const QImage small = img.width() > maxWidth
            ? img.scaledToWidth(maxWidth, Qt::SmoothTransformation) : img;
        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly) && small.save(&f, "JPG", 80))
            f.commit();
    });
    return true;
}

void StreamPlayer::setVolume(qreal volume)
{
    volume = qBound<qreal>(0.0, volume, 1.0);
    if (qFuzzyCompare(m_volume, volume))
        return;
    m_volume = volume;
    if (m_audioOut)
        m_audioOut->setVolume(volume);
    emit volumeChanged();
}

void StreamPlayer::setPlayback(bool on)
{
    if (m_playback == on)
        return;
    m_playback = on;
    if (m_audioOut)
        m_audioOut->setPlaybackMode(on);
    emit playbackChanged();
}

void StreamPlayer::applyAudioFlush(const std::shared_ptr<Session> &s)
{
    if (s == m_session && m_audioOut)
        m_audioOut->flush();
}

void StreamPlayer::applyHasAudioFromWorker(const std::shared_ptr<Session> &s, bool hasAudio)
{
    if (s == m_session)
        applyHasAudio(hasAudio);
}

void StreamPlayer::applyHasAudio(bool hasAudio)
{
    if (m_hasAudio == hasAudio)
        return;
    m_hasAudio = hasAudio;
    emit hasAudioChanged();
}

void StreamPlayer::applyAudioFromWorker(const std::shared_ptr<Session> &s, const QByteArray &pcm)
{
    if (s != m_session || m_muted)
        return; // muted after this chunk was queued
    if (!m_audioOut) {
        m_audioOut = std::make_unique<AudioPlayback>();
        m_audioOut->setPlaybackMode(m_playback);
        m_audioOut->setVolume(m_volume);
        m_audioOut->setDelayMs(m_liveDelayMs); // match the picture delay, if any
    }
    m_audioOut->write(pcm);
}

// applyStateFromWorker runs on the GUI thread (posted via invokeMethod).
void StreamPlayer::applyStateFromWorker(const std::shared_ptr<Session> &s, State state,
                                       const QString &error)
{
    if (s == m_session)
        applyState(state, error);
}

void StreamPlayer::applyFramesTickFromWorker(const std::shared_ptr<Session> &s)
{
    if (s == m_session)
        emit framesDecodedChanged();
}

void StreamPlayer::applyFrameEpochFromWorker(const std::shared_ptr<Session> &s)
{
    if (s == m_session)
        emit frameEpochMsChanged();
}

void StreamPlayer::applyTooSlowFromWorker(const std::shared_ptr<Session> &s)
{
    if (s == m_session)
        setLinkTooSlow(true);
}

void StreamPlayer::applyRecordingState(const std::shared_ptr<Session> &s, bool recording,
                                      const QString &path, const QString &error)
{
    // A finalized old file still deserves its completion notification, but
    // cannot clear or set the replacement session's recording status.
    if (s == m_session && m_recording != recording) {
        m_recording = recording;
        emit recordingChanged();
    }
    if (!recording) {
        if (!path.isEmpty())
            emit recordingSaved(path);
        else if (!error.isEmpty())
            emit recordingFailed(error);
    }
}

bool StreamPlayer::recording() const
{
    return m_recording;
}

bool StreamPlayer::startRecording(const QString &path)
{
    if (!m_session || m_state != State::Streaming)
        return false;
    QString out = path;
    if (out.isEmpty()) {
        const QString dir = Paths::recordingsDir();
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
        out = dir + QStringLiteral("/clip_") + stamp + QStringLiteral(".mp4");
    }
    {
        QMutexLocker lock(&m_session->recMutex);
        m_session->recPath = out;
    }
    m_session->recordRequested.store(true);
    return true;
}

void StreamPlayer::stopRecording()
{
    if (m_session)
        m_session->recordRequested.store(false);
}

StreamPlayer::StreamPlayer(QObject *parent) : QObject(parent)
{
    m_recordingTarget = std::make_shared<RecordingTarget>();
    m_recordingTarget->player = this;
    gLivePlayers.fetch_add(1, std::memory_order_relaxed);
    m_clock.start();
    m_playoutTimer.setSingleShot(true);
    m_playoutTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_playoutTimer, &QTimer::timeout, this, &StreamPlayer::servicePlayout);
}

StreamPlayer::~StreamPlayer()
{
    {
        QMutexLocker lock(&m_recordingTarget->mutex);
        m_recordingTarget->player = nullptr;
    }
    gLivePlayers.fetch_sub(1, std::memory_order_relaxed);
    if (m_activeStop)
        m_activeStop();
    if (m_pendingStop)
        m_pendingStop();
    if (m_session) {
        // Null the back-pointer under the lock so no queued callback touches us
        // after this returns, then signal abort. The detached worker drops the
        // last Session reference on its own; we never join.
        {
            QMutexLocker lock(&m_session->backMutex);
            m_session->player = nullptr;
        }
        m_session->frameHandoff.clear();
        m_session->abort.store(true);
    }
}

void StreamPlayer::setSource(const QString &source)
{
    if (m_source == source && !m_pendingReader)
        return;
    stop();
    m_pendingReader = nullptr; // URL source supersedes any packet source
    m_pendingFormat.clear();
    m_pendingStop = nullptr;
    m_pendingPresentationDelayMs = 0;
    m_source = source;
    emit sourceChanged();
}

void StreamPlayer::setExpectedSize(const QSize &size)
{
    if (m_expectedSize == size)
        return;
    m_expectedSize = size;
    emit expectedSizeChanged();
}

void StreamPlayer::setPacketSource(std::function<int(unsigned char *, int)> reader,
                                   const QString &format, std::function<void()> onStop,
                                   int presentationDelayMs)
{
    stop();
    m_pendingReader = std::move(reader);
    m_pendingFormat = format;
    m_pendingStop = std::move(onStop);
    m_pendingPresentationDelayMs = qMax(0, presentationDelayMs);
    m_source.clear();
    emit sourceChanged();
}

QVideoSink *StreamPlayer::videoSink() const
{
    return m_session ? m_session->sink.data() : m_pendingSink.data();
}

void StreamPlayer::setVideoSink(QVideoSink *sink)
{
    if (m_pendingSink == sink)
        return;
    m_pendingSink = sink;
    if (m_session) {
        QMutexLocker lock(&m_session->sinkMutex);
        m_session->sink = sink;
    }
    emit videoSinkChanged();
}

void StreamPlayer::setHoldPaneSize(const QSize &size)
{
    if (size == m_holdPane)
        return;
    m_holdPane = size;
    if (m_session) {
        m_session->holdW.store(size.width());
        m_session->holdH.store(size.height());
    }
    emit holdChanged();
}

void StreamPlayer::setHoldFisheye(bool on)
{
    if (on == m_holdFisheye)
        return;
    m_holdFisheye = on;
    if (m_session)
        m_session->holdFisheye.store(on);
    emit holdChanged();
}

void StreamPlayer::setLinkTooSlow(bool slow)
{
    if (slow == m_linkTooSlow)
        return;
    m_linkTooSlow = slow;
    emit linkTooSlowChanged();
}

qint64 StreamPlayer::frameEpochMs() const
{
    return m_session ? m_session->frameEpochMs.load() : 0;
}

qint64 StreamPlayer::framesDecoded() const
{
    return m_session ? m_session->framesDecoded.load() : 0;
}

void StreamPlayer::setLoop(bool loop)
{
    if (m_loop == loop)
        return;
    m_loop = loop;
    if (m_session)
        m_session->loop.store(loop);
    emit loopChanged();
}

void StreamPlayer::setRetryOnError(bool v)
{
    if (m_retryOnError == v)
        return;
    m_retryOnError = v;
    if (m_session)
        m_session->retryOnError.store(v);
    emit retryOnErrorChanged();
}

void StreamPlayer::start()
{
    if (m_source.isEmpty() && !m_pendingReader)
        return;
    stop();
    resetPlayout();
    m_liveDelayMs = m_pendingPresentationDelayMs;

    auto s = std::make_shared<Session>();
    s->player = this;
    s->recordingTarget = m_recordingTarget;
    s->source = m_source;
    s->readPacket = m_pendingReader;
    s->forcedFormat = m_pendingFormat;
    s->expectedSize = m_expectedSize;
    s->loop.store(m_loop);
    s->retryOnError.store(m_retryOnError);
    s->speed = m_speed;
    s->audioWanted.store(!m_muted && speed() == 1.0);
    s->smoothing.store(m_smoothing);
    s->reverse = m_reverse;
    ++m_sessionId;
    s->holdW.store(m_holdPane.width());
    s->holdH.store(m_holdPane.height());
    s->holdFisheye.store(m_holdFisheye);
    setLinkTooSlow(false);
    if (m_tap)
        m_tap->bind(s);
    applyHasAudio(false); // the new session reports its own track
    {
        QMutexLocker lock(&s->sinkMutex);
        s->sink = m_pendingSink;
    }
    m_session = s;
    // Adopt the pending source's teardown as the running session's teardown, so the
    // stop() above (which tears down the previous session) doesn't kill this source.
    m_activeStop = std::move(m_pendingStop);
    m_pendingStop = nullptr;
    applyState(State::Connecting, QString());
    std::thread(runWorker, s).detach();
}

void StreamPlayer::stop()
{
    // Unblock and tear down the running session's custom packet source (native
    // Baichuan). Its read() may be blocking the decode worker; this wakes it so
    // avformat unwinds. The pending (not-yet-started) source is left untouched.
    if (m_activeStop) {
        m_activeStop();
        m_activeStop = nullptr;
    }
    if (!m_session)
        return;
    {
        QMutexLocker lock(&m_session->backMutex);
        m_session->player = nullptr; // stale callbacks become no-ops
    }
    m_session->frameHandoff.clear();
    m_session->abort.store(true);
    m_session.reset();
    resetPlayout();
    m_audioOut.reset(); // don't let buffered sound play on after the picture stopped
    applyHasAudio(false);
    if (m_recording) {
        m_recording = false;
        emit recordingChanged();
    }
    if (m_state != State::Idle)
        applyState(State::Stopped, QString());
}

void StreamPlayer::applyState(State state, const QString &error)
{
    if (m_errorString != error) {
        m_errorString = error;
        emit errorStringChanged();
    }
    if (m_state != state) {
        m_state = state;
        emit stateChanged();
    }
}

} // namespace rl
