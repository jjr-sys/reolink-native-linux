#include "ui/EventColumn.h"

#include "core/Database.h"
#include "device/EventHistory.h"
#include "device/EventManager.h"

#include <QDateTime>
#include <QTimeZone>

#include <algorithm>

namespace rl {

namespace {
const QString kStatus = QStringLiteral("status");
const QString kMotion = QStringLiteral("motion");

// The stored types a type button stands for.
QStringList storedTypes(const QStringList &types)
{
    QStringList out;
    for (const QString &t : types) {
        if (t == kStatus)
            out << QStringLiteral("online") << QStringLiteral("offline");
        else
            out << t;
    }
    return out;
}
} // namespace

EventColumn::EventColumn(QObject *parent)
    : QAbstractListModel(parent)
{
    m_later.setSingleShot(true);
    m_later.setInterval(300);
    connect(&m_later, &QTimer::timeout, this, &EventColumn::refresh);
}

int EventColumn::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant EventColumn::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case KindRole: return r.gap ? QStringLiteral("gap") : QStringLiteral("event");
    case TypeRole: return r.type;
    case TimestampRole: return r.ts;
    case ThumbnailRole: return r.thumbnail;
    case FromRole: return r.from;
    case ToRole: return r.to;
    default: return {};
    }
}

QHash<int, QByteArray> EventColumn::roleNames() const
{
    return {{KindRole, "kind"},       {TypeRole, "type"}, {TimestampRole, "timestamp"},
            {ThumbnailRole, "thumbnail"}, {FromRole, "from"}, {ToRole, "to"}};
}

EventManager *EventColumn::events() const
{
    return m_events;
}

void EventColumn::setEvents(EventManager *events)
{
    if (m_events == events)
        return;
    if (m_events)
        disconnect(m_events, nullptr, this, nullptr);
    m_events = events;
    m_db = events ? events->database() : nullptr;
    if (events)
        connect(events, &EventManager::storedChanged, this, [this](qint64 hostId) {
            if (hostId < 0 || hostId == m_host)
                m_later.start();
        });
    emit eventsChanged();
    refresh();
}

void EventColumn::setDatabase(Database *db)
{
    m_db = db;
    refresh();
}

void EventColumn::setCameraKey(const QString &key)
{
    if (m_key == key)
        return;
    m_key = key;
    const QStringList parts = key.split(QLatin1Char(':'));
    bool okHost = false, okChannel = false;
    m_host = parts.size() == 2 ? parts[0].toLongLong(&okHost) : -1;
    m_channel = parts.size() == 2 ? parts[1].toInt(&okChannel) : -1;
    if (!okHost || !okChannel)
        m_host = m_channel = -1;
    emit cameraKeyChanged();
    refresh();
}

void EventColumn::setDayStart(qint64 dayStart)
{
    if (m_dayStart == dayStart)
        return;
    m_dayStart = dayStart;
    emit dayStartChanged();
    refresh();
}

void EventColumn::setSupportedTypes(const QStringList &types)
{
    if (m_supported == types)
        return;
    m_supported = types;
    emit supportedTypesChanged();
    setEnabled(m_chosen ? m_wanted : defaultTypes(types));
}

QStringList EventColumn::defaultTypes(const QStringList &supported)
{
    QStringList out;
    for (const QString &t : supported)
        if (t != kMotion && t != kStatus)
            out << t;
    return out;
}

void EventColumn::toggle(const QString &type)
{
    if (!m_supported.contains(type))
        return;
    QStringList next = m_enabled;
    if (!next.removeOne(type))
        next << type;
    m_chosen = true;
    m_wanted = next;
    setEnabled(next);
    emit typesToggled(m_enabled);
}

void EventColumn::restoreTypes(const QStringList &types)
{
    m_chosen = true;
    m_wanted = types;
    setEnabled(types);
}

// Kept to the supported types, in their display order.
void EventColumn::setEnabled(const QStringList &types)
{
    QStringList next;
    for (const QString &t : m_supported)
        if (types.contains(t))
            next << t;
    if (next == m_enabled)
        return;
    m_enabled = next;
    emit enabledTypesChanged();
    refresh();
}

qint64 EventColumn::now() const
{
    return m_now > 0 ? m_now : QDateTime::currentSecsSinceEpoch();
}

void EventColumn::refresh()
{
    m_later.stop();
    QVector<Row> rows;
    QVariantMap counts;
    QString note;
    if (m_db && m_host >= 0 && m_dayStart > 0 && hasTypes()) {
        const qint64 dayEnd = device::dayContaining(m_dayStart, QTimeZone::systemTimeZone()).to;
        const qint64 t = now();

        const QHash<QString, int> stored = m_db->eventCountsBetween(m_host, m_channel, m_dayStart, dayEnd);
        for (const QString &type : m_supported) {
            int n = 0;
            for (const QString &s : storedTypes({type}))
                n += stored.value(s);
            counts.insert(type, n);
        }

        if (!m_enabled.isEmpty())
            for (const EventRecord &e : m_db->eventsBetween(m_host, m_channel, m_dayStart, dayEnd,
                                                            storedTypes(m_enabled)))
                rows.append({false, e.type, e.thumbnail, e.timestamp, 0, 0});
        const auto gaps = device::storedNotRunningGaps(*m_db, m_dayStart, dayEnd, t);
        for (const device::TimeSpan &g : gaps)
            rows.append({true, {}, {}, g.to, g.from, g.to});
        // Newest first; a gap sits by its end, above the events before it.
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Row &a, const Row &b) { return a.ts > b.ts; });

        if (dayEnd <= t - device::kOtherKeepSecs)
            note = tr("Events are kept for 7 days.");
        else if (m_enabled.contains(kMotion) && m_dayStart < t - device::kMotionKeepSecs)
            note = tr("Motion is kept for 24 hours.");
    }
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    m_counts = counts;
    m_keptNote = note;
    emit refreshed();
}

} // namespace rl
