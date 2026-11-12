#include "device/UnseenCounts.h"

#include "core/Database.h"
#include "core/SeenStore.h"
#include "device/EventManager.h"
#include "device/EventSwitches.h"

#include <QDateTime>

namespace rl {

namespace {

// "hostId:channel" → (host, channel); false for anything else.
bool parseKey(const QString &key, qint64 &host, int &channel)
{
    const QStringList parts = key.split(QLatin1Char(':'));
    if (parts.size() != 2)
        return false;
    bool okHost = false, okChannel = false;
    host = parts.at(0).toLongLong(&okHost);
    channel = parts.at(1).toInt(&okChannel);
    return okHost && okChannel;
}

} // namespace

UnseenCounts::UnseenCounts(QObject *parent) : QObject(parent)
{
    m_refreshSoon.setSingleShot(true);
    m_refreshSoon.setInterval(500);
    connect(&m_refreshSoon, &QTimer::timeout, this, &UnseenCounts::refresh);
}

QStringList UnseenCounts::countedTypes()
{
    return {QStringLiteral("person"), QStringLiteral("vehicle"), QStringLiteral("pet"),
            QStringLiteral("visitor")};
}

void UnseenCounts::setEvents(EventManager *events)
{
    if (m_events == events)
        return;
    if (m_events)
        disconnect(m_events, nullptr, this, nullptr);
    m_events = events;
    m_db = events ? events->database() : nullptr;
    if (events)
        connect(events, &EventManager::storedChanged, this, [this] { m_refreshSoon.start(); });
    setSwitches(events ? events->switches() : nullptr);
    refresh();
}

void UnseenCounts::setDatabase(Database *db)
{
    m_db = db;
}

void UnseenCounts::setSwitches(EventSwitches *switches)
{
    if (m_switches == switches)
        return;
    if (m_switches)
        disconnect(m_switches, nullptr, this, nullptr);
    m_switches = switches;
    if (switches)
        connect(switches, &EventSwitches::changed, this, [this] { m_refreshSoon.start(); });
}

void UnseenCounts::setStore(SeenStore *store)
{
    if (m_store == store)
        return;
    if (m_store)
        disconnect(m_store, nullptr, this, nullptr);
    m_store = store;
    if (store)
        connect(store, &SeenStore::changed, this, [this] { m_refreshSoon.start(); });
}

void UnseenCounts::setOpenCamera(const QString &key)
{
    if (m_openCamera == key)
        return;
    // The camera being left is seen up to this moment, not just up to the last refresh.
    const QHash<QPair<qint64, int>, qint64> last =
        m_db ? m_db->lastEventTimes() : QHash<QPair<qint64, int>, qint64>();
    markOpenSeen(last);
    m_openCamera = key;
    qint64 host = -1;
    int channel = 0;
    m_seenLine = -1;
    if (m_store && parseKey(key, host, channel)) {
        m_seenLine = m_store->seenAt(host, channel);
        markOpenSeen(last);
    }
    emit openCameraChanged();
    refresh();
}

// The open camera is seen up to now, or up to its newest event when a recorder's clock
// runs ahead of this desktop's.
void UnseenCounts::markOpenSeen(const QHash<QPair<qint64, int>, qint64> &lastEventTimes)
{
    qint64 host = -1;
    int channel = 0;
    if (!m_store || !parseKey(m_openCamera, host, channel))
        return;
    const qint64 at = qMax(now(), lastEventTimes.value({host, channel}));
    if (at > m_store->seenAt(host, channel))
        m_store->markSeen(host, channel, at);
}

QVariantMap UnseenCounts::countsFor(const QString &key) const
{
    qint64 host = -1;
    int channel = 0;
    return parseKey(key, host, channel) ? m_counts.value({host, channel}) : QVariantMap();
}

int UnseenCounts::totalForHosts(const QVariantList &hostIds) const
{
    QSet<qint64> hosts;
    for (const QVariant &v : hostIds)
        hosts.insert(v.toLongLong());
    int n = 0;
    for (auto it = m_counts.cbegin(); it != m_counts.cend(); ++it)
        if (hosts.contains(it.key().first))
            for (const QVariant &c : it.value())
                n += c.toInt();
    return n;
}

void UnseenCounts::refresh()
{
    m_refreshSoon.stop();
    const QHash<QPair<qint64, int>, qint64> last =
        m_db ? m_db->lastEventTimes() : QHash<QPair<qint64, int>, qint64>();
    markOpenSeen(last);
    QHash<QPair<qint64, int>, QVariantMap> counts;
    int total = 0;
    {
        // Only a camera with something logged can have anything unseen.
        for (auto it = last.cbegin(); it != last.cend(); ++it) {
            const auto [host, channel] = it.key();
            const qint64 seen = m_store ? m_store->seenAt(host, channel) : 0;
            if (it.value() <= seen)
                continue;
            QStringList types;
            for (const QString &t : countedTypes())
                if (!m_switches || !m_switches->isOff(host, channel, t))
                    types << t;
            const QHash<QString, int> n = m_db->eventCountsAfter(host, channel, seen, types);
            QVariantMap m;
            for (auto c = n.cbegin(); c != n.cend(); ++c)
                if (c.value() > 0) {
                    m.insert(c.key(), c.value());
                    total += c.value();
                }
            if (!m.isEmpty())
                counts.insert(it.key(), m);
        }
    }
    if (counts == m_counts && total == m_total)
        return;
    m_counts = counts;
    m_total = total;
    ++m_revision;
    emit changed();
}

qint64 UnseenCounts::now() const
{
    return m_now > 0 ? m_now : QDateTime::currentSecsSinceEpoch();
}

} // namespace rl
