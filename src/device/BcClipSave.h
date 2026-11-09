#pragma once

#include "protocol/BaichuanDownload.h"

#include <QString>

#include <atomic>
#include <functional>

namespace rl {

// Download a range over Baichuan and save it as an MP4 at `path`: the download (cmd 143)
// feeds the media parser, which feeds the MP4 writer. Progress is how far the footage has
// got through the asked range (0..99), from each frame's wall-clock time. If the size the
// recorder announces (plus a margin) does not fit on the disk, it stops before taking any
// footage. On any failure or cancel the partial file is removed.
struct BcClipSaveResult {
    BcDownloadResult download;
    QString writeError;   // the file could not be written (or held no video)
    bool noSpace = false; // stopped early: the announced size does not fit on the disk
    qint64 durationMs = 0;
    bool ok() const { return download.ok() && writeError.isEmpty(); }
};

BcClipSaveResult saveBcClip(BcDownloadPort &port, const BcDownloadRequest &req, const QString &path,
                            const std::atomic<bool> *cancel,
                            const std::function<void(int percent)> &progress = {});

} // namespace rl
