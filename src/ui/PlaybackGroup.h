#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>

#include "ui/SiteSelection.h"

namespace rl {

// Playback's synced panes (wayfinder/site-tabs-redesign, step 4): the state shared by every
// pane on one playhead that is worth testing apart from the page.
//
//  - The calendar: setDays() records the days a camera has recordings in a month (as the
//    recorder answers); recordingDays is the union over the selected cameras for the month
//    shown (setMonth), ascending. Each camera's months are remembered.
//  - The selected pane: the camera clicked last (select). While any camera is selected
//    there is always one: the first in pane order to start with, or when it is removed.
//    HD, sound, Export and Download range act on it.
//  - Speed and direction (wayfinder/playback-controls ticket 11, layout B4): one value for
//    every pane. » steps 2 → 4 → 8 → 16 → 1× forwards (2 → 4 → 1× backwards), the slow
//    buttons ½ → ¼ → 1×; a button of the other cycle or direction starts its cycle afresh.
//    ◀ / ▶ play at 1× that way. Backwards needs reverseAvailable (reverse play, ticket 12).
//    A new day or a change of selected cameras resets to 1× forward. With only Frigate
//    panes selected (directKeys) the speed is locked at 1× forward.
class PlaybackGroup : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::SiteSelection *selection READ selection WRITE setSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedKey READ selectedKey NOTIFY selectedKeyChanged)
    Q_PROPERTY(QVariantList recordingDays READ recordingDays NOTIFY recordingDaysChanged)
    Q_PROPERTY(double speed READ speed NOTIFY speedChanged)
    Q_PROPERTY(int direction READ direction NOTIFY speedChanged)          // 1 forward, -1 backward
    Q_PROPERTY(bool offNormal READ offNormal NOTIFY speedChanged)         // anything but 1× forward
    Q_PROPERTY(bool reverseAvailable READ reverseAvailable WRITE setReverseAvailable NOTIFY reverseAvailableChanged)
    Q_PROPERTY(QStringList directKeys READ directKeys WRITE setDirectKeys NOTIFY directKeysChanged)
    Q_PROPERTY(bool speedLocked READ speedLocked NOTIFY speedLockedChanged)

public:
    explicit PlaybackGroup(QObject *parent = nullptr);

    SiteSelection *selection() const { return m_selection; }
    void setSelection(SiteSelection *selection);
    QString selectedKey() const { return m_selected; }
    QVariantList recordingDays() const { return m_days; }
    double speed() const { return m_speed; }
    int direction() const { return m_direction; }
    bool offNormal() const { return m_direction != 1 || m_speed != 1.0; }
    bool reverseAvailable() const { return m_reverse; }
    void setReverseAvailable(bool on);
    QStringList directKeys() const { return m_direct; }
    void setDirectKeys(const QStringList &keys);
    bool speedLocked() const { return m_locked; }

    Q_INVOKABLE void stepFast(int direction);
    Q_INVOKABLE void stepSlow(int direction);
    Q_INVOKABLE void playForward() { setSpeed(1, 1.0); }
    Q_INVOKABLE void playBackward();
    Q_INVOKABLE void newDay() { setSpeed(1, 1.0); }
    // "▶ 4×", "◀ ½×", "Paused", "Paused · 4×".
    static QString speedLabel(int direction, double speed, bool paused);
    Q_INVOKABLE QString label(bool paused) const { return speedLabel(m_direction, m_speed, paused); }

    Q_INVOKABLE void setMonth(int year, int month);
    Q_INVOKABLE void setDays(const QString &key, int year, int month, const QVariantList &days);
    Q_INVOKABLE void select(const QString &key);

signals:
    void selectionChanged();
    void selectedKeyChanged();
    void recordingDaysChanged();
    void speedChanged();
    void reverseAvailableChanged();
    void speedLockedChanged();
    void directKeysChanged();

private:
    void onSelectionChanged();
    void recompute();
    void setSelected(const QString &key);
    void setSpeed(int direction, double speed);
    bool canStep(int direction) const;
    void updateLocked();

    QPointer<SiteSelection> m_selection;
    QString m_selected;
    int m_year = 0, m_month = 0;
    QHash<QString, QHash<int, QList<int>>> m_byKey; // key -> year*100+month -> days
    QVariantList m_days;
    double m_speed = 1.0;
    int m_direction = 1;
    bool m_reverse = false;
    bool m_locked = false;
    QStringList m_direct;
    QStringList m_keysSeen;   // the selected cameras, sorted: a change resets the speed
};

} // namespace rl
