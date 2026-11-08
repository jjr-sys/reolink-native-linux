#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
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
//  - Speed: one value for every pane, from the speed menu (0.25 to 8).
class PlaybackGroup : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::SiteSelection *selection READ selection WRITE setSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedKey READ selectedKey NOTIFY selectedKeyChanged)
    Q_PROPERTY(QVariantList recordingDays READ recordingDays NOTIFY recordingDaysChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)

public:
    explicit PlaybackGroup(QObject *parent = nullptr);

    SiteSelection *selection() const { return m_selection; }
    void setSelection(SiteSelection *selection);
    QString selectedKey() const { return m_selected; }
    QVariantList recordingDays() const { return m_days; }
    double speed() const { return m_speed; }
    void setSpeed(double speed);

    Q_INVOKABLE void setMonth(int year, int month);
    Q_INVOKABLE void setDays(const QString &key, int year, int month, const QVariantList &days);
    Q_INVOKABLE void select(const QString &key);

signals:
    void selectionChanged();
    void selectedKeyChanged();
    void recordingDaysChanged();
    void speedChanged();

private:
    void onSelectionChanged();
    void recompute();
    void setSelected(const QString &key);

    QPointer<SiteSelection> m_selection;
    QString m_selected;
    int m_year = 0, m_month = 0;
    QHash<QString, QHash<int, QList<int>>> m_byKey; // key -> year*100+month -> days
    QVariantList m_days;
    double m_speed = 1.0;
};

} // namespace rl
