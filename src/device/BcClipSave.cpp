#include "BcClipSave.h"

#include "media/BcMediaParser.h"
#include "media/BcMp4Writer.h"

#include <QFileInfo>
#include <QStorageInfo>

namespace rl {

BcClipSaveResult saveBcClip(BcDownloadPort &port, const BcDownloadRequest &req, const QString &path,
                            const std::atomic<bool> *cancel,
                            const std::function<void(int percent)> &progress)
{
    BcClipSaveResult r;
    BcMp4Writer writer(path);
    BcMediaParser parser;
    const qint64 from = req.start.toSecsSinceEpoch();
    const qint64 span = qMax<qint64>(1, req.end.toSecsSinceEpoch() - from);
    int lastPercent = -1;
    parser.onVideo = [&](const BcMediaParser::VideoFrame &f) {
        writer.addVideo(f, parser.declaredWidth(), parser.declaredHeight());
        if (!progress || f.time == 0)
            return;
        const int pct = int(qBound(qint64(0), (qint64(f.time) - from) * 100 / span, qint64(99)));
        if (pct > lastPercent) {
            lastPercent = pct;
            progress(pct);
        }
    };
    parser.onAudio = [&](const QByteArray &adts) { writer.addAudio(adts); };

    BcDownloadOptions opts;
    opts.cancel = cancel;
    opts.onAccepted = [&](qint64 announced) {
        if (announced < 0)
            return true; // unknown: a full disk shows up as a write error instead
        const qint64 free = QStorageInfo(QFileInfo(path).absolutePath()).bytesAvailable();
        constexpr qint64 kMargin = 256ll << 20; // the announced size runs a little under
        r.noSpace = free >= 0 && free < announced + kMargin;
        return !r.noSpace;
    };
    r.download = runBcDownload(port, req, [&](const QByteArray &b) { parser.append(b); }, opts);
    if (!r.download.ok())
        return r; // the writer's destructor removes the partial file
    if (!writer.finish(&r.writeError))
        return r;
    r.durationMs = writer.durationMs();
    return r;
}

} // namespace rl
