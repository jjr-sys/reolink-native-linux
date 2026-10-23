#pragma once

#include <QPair>
#include <QVector>
#include <QtGlobal>

namespace rl {

struct RecordingFileSnap {
    bool found = false;
    qint64 fileStart = 0; // wall-clock epoch of the recording file to open
    int seekSecs = 0;     // offset into that file for the requested moment
};

// The NVR's FLV endpoint only accepts a start on a recording-file boundary and
// seeks within the file by an offset. Given the day's files as (start,end) epochs,
// pick the file containing `startEpoch` (or the nearest earlier one across a gap).
RecordingFileSnap snapToRecordingFile(const QVector<QPair<qint64, qint64>> &files,
                                      qint64 startEpoch);

} // namespace rl
