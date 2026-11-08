#include "Paths.h"

#include <QDir>
#include <QStandardPaths>

namespace rl {

static QString ensured(const QString &path)
{
    QDir().mkpath(path);
    return path;
}

QString Paths::dataDir()
{
    return ensured(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}

QString Paths::configDir()
{
    return ensured(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
}

QString Paths::databaseFile()
{
    return dataDir() + QStringLiteral("/reolink.db");
}

QString Paths::thumbnailsDir()
{
    return ensured(dataDir() + QStringLiteral("/thumbnails"));
}

QString Paths::lastFrameFile(qint64 hostId, int channel)
{
    return ensured(dataDir() + QStringLiteral("/last-frames"))
           + QStringLiteral("/%1_%2.jpg").arg(hostId).arg(channel);
}

QString Paths::recordingsDir()
{
    return ensured(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
                   + QStringLiteral("/Reolink"));
}

} // namespace rl
