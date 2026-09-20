#include "ActivityGrid.h"

#include "device/DeviceManager.h"

#include <QDateTime>
#include <QTimer>

#include <utility>

namespace rl {

using activity::Kind;

namespace {
// The poller sees a detection up to one poll interval (10 s) after it began.
constexpr qint64 kPollLagMs = 10'000;

Kind kindOf(const QString &type)
{
    if (type == QLatin1String("person")) return Kind::Person;
    if (type == QLatin1String("vehicle")) return Kind::Vehicle;
    if (type == QLatin1String("pet") || type == QLatin1String("dog_cat")) return Kind::Pet;
    if (type == QLatin1String("visitor")) return Kind::Visitor;
    if (type == QLatin1String("motion")) return Kind::Motion;
    return Kind::None;
}
QString nameOf(Kind k)
{
    switch (k) {
    case Kind::Motion: return QStringLiteral("motion");
    case Kind::Person: return QStringLiteral("person");
    case Kind::Vehicle: return QStringLiteral("vehicle");
    case Kind::Pet: return QStringLiteral("pet");
    case Kind::Visitor: return QStringLiteral("visitor");
    default: return QString();
    }
}
} // namespace

qint64 ActivityGrid::nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

ActivityGrid::ActivityGrid(DeviceManager *devices, QObject *parent)
    : QObject(parent), m_devices(devices)
{
    m_policy.setHostLookup([this](int row) -> qint64 {
        return m_devices->cameraInfo(row).value(QStringLiteral("hostId")).toLongLong();
    });
    setTrackedTypes(m_tracked);
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &ActivityGrid::onTick);
    connect(m_devices, &DeviceManager::detectionEvent, this,
            [this](qint64 hostId, int channel, const QString &type, const QString &) {
                onDetection(hostId, channel, type);
            });
}

void ActivityGrid::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    if (on) {
        m_timer.start();
        if (!m_pendingScript.isEmpty())
            startScript(std::exchange(m_pendingScript, QString()));
    } else {
        m_timer.stop();
        m_policy.reset();
        m_replayFrom.fill(0);
        m_trigger.fill(0);
    }
    publish();
    emit enabledChanged();
}

void ActivityGrid::setTrackedTypes(const QStringList &types)
{
    QSet<Kind> kinds;
    QStringList clean;
    for (const QString &t : types) {
        const Kind k = kindOf(t);
        if (k != Kind::None && !kinds.contains(k)) {
            kinds.insert(k);
            clean.append(t);
        }
    }
    m_policy.setTracked(kinds);
    if (clean != m_tracked || types != m_tracked) {
        m_tracked = clean;
        emit trackedTypesChanged();
    }
}

void ActivityGrid::setBaseline(const QVariantList &rows)
{
    QVector<int> r;
    for (const QVariant &v : rows)
        r.append(v.toInt());
    m_policy.setBaseline(r);
    m_replayFrom.resize(r.size());
    m_trigger.resize(r.size());
    publish();
}

void ActivityGrid::onDetection(qint64 hostId, int channel, const QString &type)
{
    if (!m_enabled)
        return;
    ingest(m_devices->rowOfHostChannel(hostId, channel), type);
}

void ActivityGrid::ingest(int row, const QString &type)
{
    const Kind k = kindOf(type);
    if (row < 0 || k == Kind::None)
        return;
    const qint64 now = nowMs();
    m_policy.detect(row, k, now, now - kPollLagMs);
    onTick();
}

void ActivityGrid::onTick()
{
    if (!m_enabled)
        return;
    const auto changes = m_policy.tick(nowMs());
    for (const activity::Change &c : changes) {
        if (c.tile < 0 || c.tile >= m_replayFrom.size())
            continue;
        m_replayFrom[c.tile] = c.activity ? c.replayFromMs : 0;
        m_trigger[c.tile] = c.activity ? c.triggerMs : 0;
    }
    publish();
}

void ActivityGrid::publish()
{
    QVariantList v;
    for (int i = 0; i < m_policy.tileCount(); ++i) {
        const activity::TileState &t = m_policy.tile(i);
        const bool onActivity = m_enabled && t.active;
        v.append(QVariantMap{
            {QStringLiteral("row"), m_enabled ? t.shown : t.baseline},
            {QStringLiteral("active"), onActivity},
            {QStringLiteral("kind"), onActivity ? nameOf(t.kind) : QString()},
            {QStringLiteral("replayFrom"), onActivity && i < m_replayFrom.size() ? m_replayFrom[i] / 1000 : 0},
            {QStringLiteral("trigger"), onActivity && i < m_trigger.size() && m_trigger[i] > 0 ? m_trigger[i] / 1000 : 0},
        });
    }
    if (v != m_view) {
        m_view = v;
        emit tilesChanged();
    }
}

void ActivityGrid::runScript(const QString &script) { m_pendingScript = script; }

void ActivityGrid::startScript(const QString &script)
{
    for (const QString &step : script.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList at = step.split(QLatin1Char('@'));
        const QStringList f = at.value(0).split(QLatin1Char(':'));
        if (f.size() != 3)
            continue;
        const QString type = f[0].trimmed();
        const qint64 host = f[1].toLongLong();
        const int channel = f[2].toInt();
        QTimer::singleShot(at.value(1).toDouble() * 1000, this, [this, host, channel, type] {
            onDetection(host, channel, type);
        });
    }
}

} // namespace rl
