#include "Highlights.h"

#include "device/DeviceManager.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSettings>

namespace rl {

using highlight::Kind;

namespace {
const char *kRulesKey = "highlights/rules";
QJsonObject parse(const QString &json) { return QJsonDocument::fromJson(json.toUtf8()).object(); }
} // namespace

qint64 Highlights::nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

Highlights::Highlights(DeviceManager *devices, QObject *parent) : QObject(parent)
{
    m_policy.setRules(highlight::rulesFromJson(
        parse(QSettings().value(QLatin1String(kRulesKey)).toString())));
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (m_policy.expire(nowMs()))
            publish();
    });
    connect(devices, &DeviceManager::detectionEvent, this,
            [this](qint64 hostId, int channel, const QString &type, const QString &) {
                ingest(QStringLiteral("%1:%2").arg(hostId).arg(channel), type);
            });
}

void Highlights::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    m_policy.reset();
    publish();
    emit enabledChanged();
}

QString Highlights::rules() const
{
    return QString::fromUtf8(
        QJsonDocument(highlight::rulesToJson(m_policy.rules())).toJson(QJsonDocument::Compact));
}

void Highlights::setRules(const QString &json)
{
    const QString before = rules();
    m_policy.setRules(highlight::rulesFromJson(parse(json)));
    const QString after = rules();
    if (after == before)
        return;
    QSettings().setValue(QLatin1String(kRulesKey), after);
    publish();
    emit rulesChanged();
}

void Highlights::ingest(const QString &camera, const QString &type)
{
    const Kind k = highlight::kindOf(type);
    if (!m_enabled || k == Kind::None)
        return;
    const QString sound = m_policy.detect(camera, k, nowMs());
    publish();
    if (!sound.isEmpty())
        emit soundRequested(sound, camera, type);
}

void Highlights::publish()
{
    const qint64 now = nowMs();
    QVariantMap v;
    for (const QString &camera : m_policy.highlighted(now)) {
        const highlight::Look l = m_policy.look(camera, now);
        v.insert(camera, QVariantMap{
            {QStringLiteral("kind"), highlight::nameOf(l.kind)},
            {QStringLiteral("colour"), l.colour},
            {QStringLiteral("style"), highlight::nameOf(l.style)},
            {QStringLiteral("since"), l.since},
            {QStringLiteral("flashUntil"), l.flashUntil},
            {QStringLiteral("last"), l.last},
        });
    }
    if (v.isEmpty())
        m_timer.stop();
    else if (!m_timer.isActive())
        m_timer.start();
    if (v != m_looks) {
        m_looks = v;
        emit looksChanged();
    }
}

void Highlights::runScript(const QString &script)
{
    for (const QString &step : script.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList at = step.split(QLatin1Char('@'));
        const QStringList f = at.value(0).split(QLatin1Char(':'));
        if (f.size() != 3)
            continue;
        const QString type = f[0].trimmed();
        const QString camera = f[1].trimmed() + u':' + f[2].trimmed();
        QTimer::singleShot(at.value(1).toDouble() * 1000, this,
                           [this, camera, type] { ingest(camera, type); });
    }
}

} // namespace rl
