#include "FirstSwitchOffs.h"

#include "EventSwitches.h"

namespace rl::device {

QVector<EventSwitchKey> resolveFirstSwitchOffs(const QStringList &entries,
                                               const QVector<KnownCamera> &cameras)
{
    QVector<EventSwitchKey> out;
    for (const QString &entry : entries) {
        const QStringList parts = entry.split(QLatin1Char('/'));
        if (parts.size() != 3)
            continue;
        const QString recorder = parts.at(0).trimmed();
        const QString camera = parts.at(1).trimmed();
        const QString type = EventSwitches::storedType(parts.at(2).trimmed().toLower());
        if (recorder.isEmpty() || camera.isEmpty() || type.isEmpty()
            || EventSwitches::isStatus(type))
            continue;
        for (const KnownCamera &c : cameras) {
            if (recorder != QLatin1String("*") && c.hostName != recorder)
                continue;
            if (camera != QLatin1String("*")
                && c.camera.compare(camera, Qt::CaseInsensitive) != 0)
                continue;
            if (!c.types.isEmpty() && !c.types.contains(type))
                continue;
            const EventSwitchKey key{c.hostId, c.channel, type};
            if (!out.contains(key))
                out.append(key);
        }
    }
    return out;
}

} // namespace rl::device
