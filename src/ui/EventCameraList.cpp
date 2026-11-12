#include "ui/EventCameraList.h"

#include "core/Database.h"
#include "device/EventManager.h"
#include "device/EventSwitches.h"

#include <algorithm>

namespace rl {

EventCameraList::EventCameraList(QObject *parent) : QAbstractListModel(parent)
{
    m_refreshSoon.setSingleShot(true);
    m_refreshSoon.setInterval(500);
    connect(&m_refreshSoon, &QTimer::timeout, this, &EventCameraList::refresh);
}

int EventCameraList::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant EventCameraList::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case KeyRole: return r.key;
    case NameRole: return r.name;
    case LastRole: return r.last;
    case StatusOnlyRole: return r.statusOnly;
    default: return {};
    }
}

QHash<int, QByteArray> EventCameraList::roleNames() const
{
    return {{KeyRole, "key"}, {NameRole, "name"}, {LastRole, "last"},
            {StatusOnlyRole, "statusOnly"}};
}

EventManager *EventCameraList::events() const
{
    return m_events;
}

void EventCameraList::setEvents(EventManager *events)
{
    if (m_events == events)
        return;
    if (m_events)
        disconnect(m_events, nullptr, this, nullptr);
    m_events = events;
    m_db = events ? events->database() : nullptr;
    if (events)
        connect(events, &EventManager::storedChanged, this, [this] { m_refreshSoon.start(); });
    setSwitches(events ? events->switches() : nullptr); // refreshes
    emit eventsChanged();
}

void EventCameraList::setDatabase(Database *db)
{
    m_db = db;
    refresh();
}

void EventCameraList::setSwitches(EventSwitches *switches)
{
    if (m_switches == switches)
        return;
    if (m_switches)
        disconnect(m_switches, nullptr, this, nullptr);
    m_switches = switches;
    if (switches)
        connect(switches, &EventSwitches::changed, this, [this] { m_refreshSoon.start(); });
    refresh();
}

void EventCameraList::setCameras(const QVariantList &cameras)
{
    if (m_cameras == cameras)
        return;
    m_cameras = cameras;
    emit camerasChanged();
    refresh();
}

QString EventCameraList::first() const
{
    return m_rows.isEmpty() ? QString() : m_rows.first().key;
}

bool EventCameraList::contains(const QString &key) const
{
    return std::any_of(m_rows.cbegin(), m_rows.cend(), [&key](const Row &r) { return r.key == key; });
}

void EventCameraList::refresh()
{
    m_refreshSoon.stop();
    const QHash<QPair<qint64, int>, qint64> last =
        m_db ? m_db->lastEventTimes() : QHash<QPair<qint64, int>, qint64>();
    QVector<Row> rows;
    for (const QVariant &v : std::as_const(m_cameras)) {
        const QVariantMap m = v.toMap();
        Row r;
        r.key = m.value(QStringLiteral("key")).toString();
        r.name = m.value(QStringLiteral("name")).toString();
        const QStringList parts = r.key.split(QLatin1Char(':'));
        if (parts.size() != 2)
            continue;
        const qint64 host = parts.at(0).toLongLong();
        const int channel = parts.at(1).toInt();
        r.last = last.value({host, channel});
        const QStringList types = m.value(QStringLiteral("types")).toStringList();
        r.statusOnly = !types.isEmpty(); // not known yet: not marked
        for (const QString &t : types) {
            const bool off = m_switches && m_switches->isOff(host, channel, t);
            if (t != QLatin1String("status") && !off) {
                r.statusOnly = false;
                break;
            }
        }
        rows.append(r);
    }
    // Loggable before Status only; then newest activity first; ties keep the given order.
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.statusOnly != b.statusOnly)
            return !a.statusOnly;
        return !a.statusOnly && a.last > b.last;
    });

    const bool sameKeys = rows.size() == m_rows.size()
                          && std::equal(rows.cbegin(), rows.cend(), m_rows.cbegin(),
                                        [](const Row &a, const Row &b) { return a.key == b.key; });
    if (sameKeys) {
        for (int i = 0; i < rows.size(); ++i) {
            const Row &r = rows.at(i);
            Row &o = m_rows[i];
            if (o.name == r.name && o.last == r.last && o.statusOnly == r.statusOnly)
                continue;
            o = r;
            emit dataChanged(index(i), index(i), {NameRole, LastRole, StatusOnlyRole});
        }
    } else {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    }
    emit refreshed();
}

} // namespace rl
