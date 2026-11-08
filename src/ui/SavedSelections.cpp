#include "ui/SavedSelections.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace rl {

namespace {
QStringList uniqueKeys(const QStringList &keys)
{
    QStringList out;
    for (const QString &k : keys) {
        const QString t = k.trimmed();
        if (t.contains(QLatin1Char(':')) && !out.contains(t))
            out << t;
    }
    return out;
}
QJsonArray parseArray(const QString &text)
{
    return QJsonDocument::fromJson(text.toUtf8()).array();
}
QString newId()
{
    static int n = 0;
    return QStringLiteral("s%1%2").arg(QDateTime::currentMSecsSinceEpoch(), 0, 36).arg(++n);
}
} // namespace

SavedSelections::SavedSelections(QObject *parent)
    : QObject(parent)
{
}

void SavedSelections::setSaved(const QString &text)
{
    m_saved = text;
    m_list.clear();
    for (const QJsonValue &v : parseArray(text)) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        const QString name = o.value(QStringLiteral("name")).toString().trimmed();
        QStringList cams;
        for (const QJsonValue &c : o.value(QStringLiteral("cameras")).toArray())
            cams << c.toString();
        cams = uniqueKeys(cams);
        if (id.isEmpty() || name.isEmpty() || cams.isEmpty())
            continue;
        m_list << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), name},
                              {QStringLiteral("cameras"), cams}};
    }
    emit changed();
}

void SavedSelections::store()
{
    m_saved = QString::fromUtf8(
        QJsonDocument(QJsonArray::fromVariantList(m_list)).toJson(QJsonDocument::Compact));
    emit changed();
    emit saveRequested(m_saved);
}

int SavedSelections::indexOf(const QString &id) const
{
    for (int i = 0; i < m_list.size(); ++i)
        if (m_list.at(i).toMap().value(QStringLiteral("id")).toString() == id)
            return i;
    return -1;
}

QString SavedSelections::add(const QString &name, const QStringList &cameras)
{
    const QString n = name.trimmed();
    const QStringList cams = uniqueKeys(cameras);
    if (n.isEmpty() || cams.isEmpty())
        return {};
    const QString id = newId();
    m_list << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), n},
                          {QStringLiteral("cameras"), cams}};
    store();
    return id;
}

void SavedSelections::rename(const QString &id, const QString &name)
{
    const int i = indexOf(id);
    const QString n = name.trimmed();
    if (i < 0 || n.isEmpty())
        return;
    QVariantMap e = m_list.at(i).toMap();
    if (e.value(QStringLiteral("name")).toString() == n)
        return;
    e.insert(QStringLiteral("name"), n);
    m_list[i] = e;
    store();
}

void SavedSelections::remove(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    m_list.removeAt(i);
    store();
}

QVariantMap SavedSelections::find(const QString &id) const
{
    const int i = indexOf(id);
    return i < 0 ? QVariantMap() : m_list.at(i).toMap();
}

QStringList SavedSelections::camerasOf(const QString &id) const
{
    return find(id).value(QStringLiteral("cameras")).toStringList();
}

QString SavedSelections::nextName(const QString &base) const
{
    for (int n = 1;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        bool taken = false;
        for (const QVariant &v : m_list)
            taken = taken || v.toMap().value(QStringLiteral("name")).toString() == name;
        if (!taken)
            return name;
    }
}

QVariantList SavedSelections::fromMosaics(const QString &mosaicsJson)
{
    QVariantList out;
    for (const QJsonValue &v : parseArray(mosaicsJson)) {
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString().trimmed();
        QStringList cams;
        for (const QJsonValue &c : o.value(QStringLiteral("cameras")).toArray())
            cams << c.toString();
        cams = uniqueKeys(cams);
        if (name.isEmpty() || cams.isEmpty())
            continue;
        out << QVariantMap{{QStringLiteral("id"), newId()}, {QStringLiteral("name"), name},
                           {QStringLiteral("cameras"), cams}};
    }
    return out;
}

QStringList SavedSelections::mosaicKeys(const QString &mosaicsJson, const QString &field,
                                        const QString &gridKeys)
{
    QStringList keys = gridKeys.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QJsonValue &v : parseArray(mosaicsJson))
        for (const QJsonValue &k : v.toObject().value(field).toArray())
            keys << k.toString();
    return uniqueKeys(keys);
}

} // namespace rl
