#include "Log.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>

extern "C" {
#include <libavutil/log.h>
}

#include <cstdarg>
#include <cstdio>

Q_LOGGING_CATEGORY(lcCore, "rl.core")
Q_LOGGING_CATEGORY(lcProto, "rl.proto")
Q_LOGGING_CATEGORY(lcMedia, "rl.media")
Q_LOGGING_CATEGORY(lcUi, "rl.ui")

namespace rl {

namespace {

// FFmpeg writes straight to stderr, and a dying stream repeats the same handful of
// lines hundreds of times a second. Collapse identical consecutive lines into one plus
// a "repeated N times" note, and send the rest through Qt's log so it is timestamped.
void ffmpegLog(void *, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, vl);
    QByteArray line = QByteArray(buf).trimmed();
    if (line.isEmpty())
        return;

    static QMutex mutex;
    static QByteArray last;
    static int repeats = 0;
    static QElapsedTimer sinceLast;
    QMutexLocker lock(&mutex);
    // Object addresses ("[h264 @ 0x...]") differ per stream; compare on the message only.
    const int at = line.indexOf("] ");
    const QByteArray key = at >= 0 ? line.mid(at + 2) : line;
    const bool sameRecently = key == last && sinceLast.isValid() && sinceLast.elapsed() < 5000;
    if (sameRecently) {
        ++repeats;
        return;
    }
    if (repeats > 0)
        qCWarning(lcMedia).noquote() << "ffmpeg: previous message repeated" << repeats << "times";
    repeats = 0;
    last = key;
    sinceLast.start();
    if (level <= AV_LOG_ERROR)
        qCWarning(lcMedia).noquote() << "ffmpeg:" << line;
    else
        qCInfo(lcMedia).noquote() << "ffmpeg:" << line;
}

} // namespace

void installLogging()
{
    av_log_set_callback(ffmpegLog);
    qSetMessagePattern(
        QStringLiteral("%{time hh:mm:ss.zzz} %{category} "
                       "%{if-debug}D%{endif}%{if-info}I%{endif}%{if-warning}W%{endif}"
                       "%{if-critical}C%{endif}%{if-fatal}F%{endif} %{message}"));
}

} // namespace rl
