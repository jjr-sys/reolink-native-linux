#include "AlertSounds.h"

#include <QDateTime>
#include <QSettings>
#include <QSoundEffect>
#include <QUrl>

namespace rl {

namespace {
const char *kVolumeKey = "alertSounds/volume";
const char *kMutedKey = "alertSounds/mutedUntil";
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
} // namespace

QStringList AlertSounds::builtIn()
{
    return {QStringLiteral("bird-chirp"), QStringLiteral("horn"), QStringLiteral("chime"),
            QStringLiteral("beep"), QStringLiteral("double-beep"), QStringLiteral("triple-beep"),
            QStringLiteral("bell"), QStringLiteral("knock"), QStringLiteral("whistle"),
            QStringLiteral("ping"), QStringLiteral("pop"), QStringLiteral("marimba"),
            QStringLiteral("short-siren"), QStringLiteral("buzzer")};
}

bool AlertSounds::allowed(int priority, int playing, bool muted, bool suppressed)
{
    if (priority <= None)
        return false;
    if (priority == Detection && (muted || suppressed))
        return false;
    return priority > playing;
}

AlertSounds::AlertSounds(QObject *parent) : QObject(parent)
{
    QSettings settings;
    m_volume = qBound(0.0, settings.value(QLatin1String(kVolumeKey), 0.8).toDouble(), 1.0);
    m_muteTimer.setSingleShot(true);
    connect(&m_muteTimer, &QTimer::timeout, this, [this] { setMutedUntil(0); });
    const qint64 saved = settings.value(QLatin1String(kMutedKey), 0).toLongLong();
    if (saved == -1 || saved > nowMs())
        setMutedUntil(saved);
    else if (saved != 0)
        settings.remove(QLatin1String(kMutedKey));

    for (const QString &s : builtIn()) {
        auto *e = new QSoundEffect(this);
        e->setSource(QUrl(QStringLiteral("qrc:/sounds/%1.wav").arg(s)));
        e->setVolume(float(m_volume));
        connect(e, &QSoundEffect::playingChanged, this, [this, e] {
            if (e == m_current && !e->isPlaying()) {
                m_current = nullptr;
                m_currentPriority = None;
            }
        });
        m_effects.insert(s, e);
    }
}

QString AlertSounds::label(const QString &sound) const
{
    static const QHash<QString, QString> names{
        {QStringLiteral("bird-chirp"), tr("Bird chirp")}, {QStringLiteral("horn"), tr("Horn")},
        {QStringLiteral("chime"), tr("Chime")}, {QStringLiteral("beep"), tr("Beep")},
        {QStringLiteral("double-beep"), tr("Double beep")},
        {QStringLiteral("triple-beep"), tr("Triple beep")}, {QStringLiteral("bell"), tr("Bell")},
        {QStringLiteral("knock"), tr("Knock")}, {QStringLiteral("whistle"), tr("Whistle")},
        {QStringLiteral("ping"), tr("Ping")}, {QStringLiteral("pop"), tr("Pop")},
        {QStringLiteral("marimba"), tr("Marimba")},
        {QStringLiteral("short-siren"), tr("Short siren")}, {QStringLiteral("buzzer"), tr("Buzzer")},
    };
    return names.value(sound, sound);
}

void AlertSounds::setVolume(double v)
{
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(v + 1, m_volume + 1))
        return;
    m_volume = v;
    for (QSoundEffect *e : std::as_const(m_effects))
        e->setVolume(float(v));
    QSettings().setValue(QLatin1String(kVolumeKey), v);
    emit volumeChanged();
}

void AlertSounds::setSuppressed(bool on)
{
    if (on == m_suppressed)
        return;
    m_suppressed = on;
    // The doorbell has the room: a detection sound already going stops.
    if (on && m_current && m_currentPriority == Detection)
        m_current->stop();
    emit suppressedChanged();
}

int AlertSounds::playingPriority() const
{
    return m_current && m_current->isPlaying() ? m_currentPriority : None;
}

bool AlertSounds::play(const QString &sound, int priority)
{
    if (!allowed(priority, playingPriority(), muted(), m_suppressed))
        return false;
    return start(sound, priority);
}

void AlertSounds::test(const QString &sound)
{
    if (allowed(Detection, playingPriority(), false, false))
        start(sound, Detection);
}

bool AlertSounds::start(const QString &sound, int priority)
{
    QSoundEffect *e = m_effects.value(sound);
    if (!e)
        return false;
    if (m_current && m_current != e)
        m_current->stop();
    m_current = e;
    m_currentPriority = priority;
    e->play();
    return true;
}

void AlertSounds::muteFor(int minutes)
{
    setMutedUntil(minutes > 0 ? nowMs() + qint64(minutes) * 60'000 : -1);
}

void AlertSounds::unmute() { setMutedUntil(0); }

void AlertSounds::setMutedUntil(qint64 until)
{
    m_muteTimer.stop();
    if (until > 0)
        m_muteTimer.start(int(qMin<qint64>(until - nowMs(), 24LL * 3600 * 1000)));
    if (until == m_mutedUntil)
        return;
    m_mutedUntil = until;
    QSettings settings;
    if (until == 0)
        settings.remove(QLatin1String(kMutedKey));
    else
        settings.setValue(QLatin1String(kMutedKey), until);
    if (until != 0 && m_current && m_currentPriority == Detection)
        m_current->stop();
    emit mutedChanged();
}

} // namespace rl
