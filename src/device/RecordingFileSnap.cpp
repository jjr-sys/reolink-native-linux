#include "RecordingFileSnap.h"

#include <QtGlobal>

namespace rl {

RecordingFileSnap snapToRecordingFile(const QVector<QPair<qint64, qint64>> &files,
                                      qint64 startEpoch)
{
    qint64 fileStart = -1;
    qint64 fileEnd = 0;
    for (const auto &f : files) {
        if (startEpoch >= f.first && startEpoch <= f.second) {
            fileStart = f.first;
            fileEnd = f.second;
            break;
        }
        if (f.first <= startEpoch && f.first > fileStart) { // nearest earlier start
            fileStart = f.first;
            fileEnd = f.second;
        }
    }
    RecordingFileSnap snap;
    if (fileStart < 0)
        return snap; // no recording covers this moment
    snap.found = true;
    snap.fileStart = fileStart;
    snap.seekSecs = static_cast<int>(
        qBound<qint64>(0, startEpoch - fileStart, qMax<qint64>(0, fileEnd - fileStart)));
    return snap;
}

} // namespace rl
