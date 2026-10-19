#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

class QSoundEffect;

namespace rl {

// The Alert sound player (wayfinder/detection-highlights): the built-in clips under
// qrc:/sounds/, one at a time. A sound only interrupts one of lower priority
// (doorbell > offline alert > detection). Detection sounds are dropped while muted from
// the Highlights menu or while the doorbell pop-up is up. Its own volume, separate from
// camera audio, which it never touches.
class AlertSounds : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList sounds READ sounds CONSTANT)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool suppressed READ suppressed WRITE setSuppressed NOTIFY suppressedChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    // Epoch ms the mute ends; -1 until unmuted; 0 not muted.
    Q_PROPERTY(qint64 mutedUntil READ mutedUntil NOTIFY mutedChanged)

public:
    enum Priority { None = 0, Detection = 1, Offline = 2, Doorbell = 3 };
    Q_ENUM(Priority)

    explicit AlertSounds(QObject *parent = nullptr);

    static QStringList builtIn();
    // Whether a sound of `priority` may start now (pure; tested).
    static bool allowed(int priority, int playing, bool muted, bool suppressed);

    QStringList sounds() const { return builtIn(); }
    Q_INVOKABLE QString label(const QString &sound) const;
    double volume() const { return m_volume; }
    void setVolume(double v);
    bool suppressed() const { return m_suppressed; }
    void setSuppressed(bool on);
    bool muted() const { return m_mutedUntil != 0; }
    qint64 mutedUntil() const { return m_mutedUntil; }

    // Plays `sound` if the rules allow it; returns whether it started.
    Q_INVOKABLE bool play(const QString &sound, int priority = Detection);
    // ▶ beside a sound picker: ignores mute and suppression, still yields to higher priority.
    Q_INVOKABLE void test(const QString &sound);
    // Mute detection sounds; 0 minutes = until unmuted.
    Q_INVOKABLE void muteFor(int minutes);
    Q_INVOKABLE void unmute();

signals:
    void volumeChanged();
    void suppressedChanged();
    void mutedChanged();

private:
    bool start(const QString &sound, int priority);
    int playingPriority() const;
    void setMutedUntil(qint64 until);

    QHash<QString, QSoundEffect *> m_effects;
    QSoundEffect *m_current = nullptr;
    int m_currentPriority = None;
    double m_volume = 0.8;
    bool m_suppressed = false;
    qint64 m_mutedUntil = 0;
    QTimer m_muteTimer;
};

} // namespace rl
