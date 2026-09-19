#pragma once

#include <QList>
#include <QString>

namespace rl {

// "v0.3.0", "0.3.0-jjr" and "0.3.0" all mean 0.3.0: any "-suffix" (the fork tag, -beta)
// is dropped, so the fork's own "0.3.0-jjr" build is not offered its own "v0.3.0" tag as
// an update, while a later tag such as "v0.3.1" is.
inline QList<int> versionParts(QString v)
{
    v.remove(QLatin1Char('v')).remove(QLatin1Char(' '));
    QList<int> out;
    const auto segs = v.split(QLatin1Char('.'));
    for (const QString &p : segs)
        out << p.split(QLatin1Char('-')).first().toInt();
    return out;
}

inline bool isNewer(const QString &latest, const QString &current)
{
    QList<int> a = versionParts(latest), b = versionParts(current);
    while (a.size() < b.size()) a << 0;
    while (b.size() < a.size()) b << 0;
    for (int i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

} // namespace rl
