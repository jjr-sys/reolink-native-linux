#include "ui/CameraNoise.h"

#include "core/Database.h"
#include "device/EventHistory.h"
#include "device/EventManager.h"
#include "device/EventSwitches.h"

#include <QDateTime>

#include <algorithm>

namespace rl {

namespace {

// Display order of the types a camera can switch off; anything else goes after.
int typeOrder(const QString &type)
{
    static const QStringList order = {QStringLiteral("person"), QStringLiteral("vehicle"),
                                      QStringLiteral("pet"), QStringLiteral("visitor"),
                                      QStringLiteral("motion")};
    const int i = int(order.indexOf(type));
    return i < 0 ? int(order.size()) : i;
}

void sortTypes(QStringList &types)
{
    std::stable_sort(types.begin(), types.end(), [](const QString &a, const QString &b) {
        return typeOrder(a) < typeOrder(b);
    });
}

} // namespace

CameraNoise::CameraNoise(QObject *parent) : QObject(parent)
{
    m_refreshSoon.setSingleShot(true);
    m_refreshSoon.setInterval(500);
    connect(&m_refreshSoon, &QTimer::timeout, this, &CameraNoise::refresh);
}

EventManager *CameraNoise::events() const
{
    return m_events;
}

void CameraNoise::setEvents(EventManager *events)
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
    emit eventsChanged();
    refresh();
}

void CameraNoise::setDatabase(Database *db)
{
    m_db = db;
}

void CameraNoise::setSwitches(EventSwitches *switches)
{
    if (m_switches == switches)
        return;
    if (m_switches)
        disconnect(m_switches, nullptr, this, nullptr);
    m_switches = switches;
    // A switch change re-reads at once: the dashed line follows Turn on / Switch off.
    if (switches)
        connect(switches, &EventSwitches::changed, this, &CameraNoise::refresh);
}

void CameraNoise::setCameraKey(const QString &key)
{
    if (m_key == key)
        return;
    m_key = key;
    const QStringList parts = key.split(QLatin1Char(':'));
    m_host = parts.size() == 2 ? parts.at(0).toLongLong() : -1;
    m_channel = parts.size() == 2 ? parts.at(1).toInt() : -1;
    emit cameraKeyChanged();
    refresh();
}

void CameraNoise::keepLogging(const QString &type)
{
    if (m_db && m_host >= 0 && m_db->setNoiseKept(m_host, m_channel, EventSwitches::storedType(type)))
        refresh();
}

void CameraNoise::refresh()
{
    m_refreshSoon.stop();
    QVariantList cards;
    QStringList off;
    if (m_host >= 0 && m_switches)
        for (const EventSwitchKey &k : m_switches->offPairs())
            if (k.hostId == m_host && k.channel == m_channel)
                off.append(k.type);
    sortTypes(off);

    if (m_db && m_host >= 0) {
        const qint64 running = runningSecs();
        const QHash<QString, int> counts = dayCounts();
        const QStringList kept = m_db->noiseKeptTypes(m_host, m_channel);
        QStringList noisy;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            const QString &type = it.key();
            if (EventSwitches::isStatus(type) || off.contains(type) || kept.contains(type))
                continue;
            if (rate(it.value(), running) >= kNoisyPerDay)
                noisy.append(type);
        }
        sortTypes(noisy);
        for (const QString &type : std::as_const(noisy)) {
            const int count = counts.value(type);
            const int perDay = rate(count, running);
            cards.append(QVariantMap{
                {QStringLiteral("type"), type},
                {QStringLiteral("count"), count},
                {QStringLiteral("perDay"), perDay},
                {QStringLiteral("everySecs"), int(kDaySecs / qMax(1, perDay))},
                {QStringLiteral("thumbnails"),
                 m_db->recentThumbnails(m_host, m_channel, type, kCardSnapshots)},
            });
        }
    }
    m_cards = cards;
    m_off = off;
    emit refreshed();
}

// How long the app ran over the last 24 h. Time before its first recorded run (a fresh
// install) doesn't count: the gaps only start from that run.
qint64 CameraNoise::runningSecs() const
{
    if (!m_db)
        return 0;
    const qint64 t = now();
    const qint64 first = m_db->firstAppRunStart();
    const qint64 from = first > 0 ? qMax(t - kDaySecs, first) : t - kDaySecs;
    qint64 running = qMax<qint64>(0, t - from);
    for (const device::TimeSpan &g : device::storedNotRunningGaps(*m_db, from, t, t))
        running -= g.to - g.from;
    return running;
}

QHash<QString, int> CameraNoise::dayCounts() const
{
    const qint64 t = now(); // a recorder's clock may run a little ahead
    return m_db && m_host >= 0
               ? m_db->eventCountsBetween(m_host, m_channel, t - kDaySecs, t + kDaySecs)
               : QHash<QString, int>();
}

int CameraNoise::rate(int count, qint64 running)
{
    return running < kMinRunningSecs ? -1 : int(qint64(count) * kDaySecs / running);
}

int CameraNoise::perDay(const QString &type) const
{
    return rate(dayCounts().value(EventSwitches::storedType(type)), runningSecs());
}

qint64 CameraNoise::now() const
{
    return m_now > 0 ? m_now : QDateTime::currentSecsSinceEpoch();
}

} // namespace rl
