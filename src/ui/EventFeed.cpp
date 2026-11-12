#include "ui/EventFeed.h"

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

EventFeed::EventFeed(QObject *parent)
    : QAbstractListModel(parent)
{
    m_refreshSoon.setSingleShot(true);
    m_refreshSoon.setInterval(300);
    connect(&m_refreshSoon, &QTimer::timeout, this, &EventFeed::refresh);
}

int EventFeed::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant EventFeed::data(const QModelIndex &index, int role) const
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
    case DayRole: return r.day;
    default: return {};
    }
}

QHash<int, QByteArray> EventFeed::roleNames() const
{
    return {{KindRole, "kind"},           {TypeRole, "type"}, {TimestampRole, "timestamp"},
            {ThumbnailRole, "thumbnail"}, {FromRole, "from"}, {ToRole, "to"},
            {DayRole, "day"}};
}

EventManager *EventFeed::events() const
{
    return m_events;
}

void EventFeed::setEvents(EventManager *events)
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
                m_refreshSoon.start();
        });
    emit eventsChanged();
    refresh();
}

void EventFeed::setDatabase(Database *db)
{
    m_db = db;
    refresh();
}

void EventFeed::setCameraKey(const QString &key)
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

void EventFeed::setSupportedTypes(const QStringList &types)
{
    if (m_supported == types)
        return;
    m_supported = types;
    emit supportedTypesChanged();
    setEnabled(m_chosen ? m_wanted : defaultTypes(types));
}

QStringList EventFeed::defaultTypes(const QStringList &supported)
{
    QStringList out;
    for (const QString &t : supported)
        if (t != kMotion && t != kStatus)
            out << t;
    // A camera that can only log Status (Motion aside) would otherwise open empty.
    if (out.isEmpty() && supported.contains(kStatus))
        out << kStatus;
    return out;
}

void EventFeed::toggle(const QString &type)
{
    if (!m_supported.contains(type))
        return;
    // From the chosen set, so a remembered type the camera hasn't reported yet stays.
    QStringList next = m_chosen ? m_wanted : m_enabled;
    if (!next.removeOne(type))
        next << type;
    m_chosen = true;
    m_wanted = next;
    setEnabled(next);
    emit typesToggled(m_wanted);
}

void EventFeed::restoreTypes(const QStringList &types)
{
    m_chosen = true;
    m_wanted = types;
    setEnabled(types);
}

void EventFeed::useDefaultTypes()
{
    m_chosen = false;
    m_wanted.clear();
    setEnabled(defaultTypes(m_supported));
}

// Kept to the supported types, in their display order.
void EventFeed::setEnabled(const QStringList &types)
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

// New events arrive at the top and old ones leave the window at the bottom: when the old
// rows are still a run inside the new ones, insert the new rows above, remove the gone
// ones below, and update thumbnails and gap starts in place, so a scrolled feed keeps its
// place. Anything else (a camera or type change, a purge) resets the list.
void EventFeed::applyRows(const QVector<Row> &rows)
{
    // A gap is known by its end: its start moves as the window slides.
    const auto same = [](const Row &a, const Row &b) {
        return a.gap == b.gap && a.ts == b.ts && a.type == b.type;
    };
    qsizetype added = -1; // where the old first row sits in the new rows
    if (m_rows.isEmpty())
        added = rows.size();
    else
        for (qsizetype i = 0; i < rows.size(); ++i)
            if (same(rows.at(i), m_rows.first())) {
                added = i;
                break;
            }
    const qsizetype kept = added < 0 ? 0 : qMin(m_rows.size(), rows.size() - added);
    const bool tailOnlyGone =
        added >= 0 && added + kept == rows.size()
        && std::equal(m_rows.cbegin(), m_rows.cbegin() + kept, rows.cbegin() + added, same);
    if (!tailOnlyGone) {
        beginResetModel();
        m_rows = rows;
        endResetModel();
        return;
    }
    if (kept < m_rows.size()) {
        beginRemoveRows({}, int(kept), int(m_rows.size()) - 1);
        m_rows.resize(kept);
        endRemoveRows();
    }
    if (added > 0) {
        beginInsertRows({}, 0, int(added) - 1);
        m_rows = rows.mid(0, added) + m_rows;
        endInsertRows();
    }
    for (qsizetype i = added; i < rows.size(); ++i) {
        Row &r = m_rows[i];
        QList<int> roles;
        if (r.thumbnail != rows.at(i).thumbnail) {
            r.thumbnail = rows.at(i).thumbnail;
            roles << ThumbnailRole;
        }
        if (r.from != rows.at(i).from) {
            r.from = rows.at(i).from;
            roles << FromRole;
        }
        if (!roles.isEmpty())
            emit dataChanged(index(int(i)), index(int(i)), roles);
    }
}

qint64 EventFeed::now() const
{
    return m_now > 0 ? m_now : QDateTime::currentSecsSinceEpoch();
}

void EventFeed::refresh()
{
    m_refreshSoon.stop();
    QVector<Row> rows;
    QVariantMap counts;
    if (m_db && m_host >= 0 && hasTypes()) {
        const qint64 t = now();
        const qint64 from = t - device::kOtherKeepSecs;
        const qint64 to = t + 86400; // a recorder's clock may run a little ahead
        const QTimeZone zone = QTimeZone::systemTimeZone();
        const auto dayOf = [&zone](qint64 ts) {
            return QDateTime::fromSecsSinceEpoch(ts, zone).date().toString(Qt::ISODate);
        };

        const QHash<QString, int> stored = m_db->eventCountsBetween(m_host, m_channel, from, to);
        for (const QString &type : m_supported) {
            int n = 0;
            for (const QString &s : storedTypes({type}))
                n += stored.value(s);
            counts.insert(type, n);
        }

        if (!m_enabled.isEmpty())
            for (const EventRecord &e : m_db->eventsBetween(m_host, m_channel, from, to,
                                                            storedTypes(m_enabled)))
                rows.append({false, e.type, e.thumbnail, dayOf(e.timestamp), e.timestamp, 0, 0});
        for (const device::TimeSpan &g : device::storedNotRunningGaps(*m_db, from, t, t))
            rows.append({true, {}, {}, dayOf(g.to - 1), g.to, g.from, g.to});
        // Newest first; a gap sits by its end, above the events before it.
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Row &a, const Row &b) { return a.ts > b.ts; });
    }
    applyRows(rows);
    m_counts = counts;
    emit refreshed();
}

} // namespace rl
