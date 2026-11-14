#include "ui/PlaybackGroup.h"

#include <QSet>

#include <algorithm>

namespace rl {

namespace {
int monthId(int year, int month) { return year * 100 + month; }
} // namespace

PlaybackGroup::PlaybackGroup(QObject *parent)
    : QObject(parent)
{
}

void PlaybackGroup::setSelection(SiteSelection *selection)
{
    if (m_selection == selection)
        return;
    if (m_selection)
        disconnect(m_selection, nullptr, this, nullptr);
    m_selection = selection;
    if (m_selection)
        connect(m_selection, &SiteSelection::changed, this, &PlaybackGroup::onSelectionChanged);
    emit selectionChanged();
    onSelectionChanged();
}

void PlaybackGroup::setSpeed(int direction, double speed)
{
    if (direction == m_direction && speed == m_speed)
        return;
    m_direction = direction;
    m_speed = speed;
    emit speedChanged();
}

bool PlaybackGroup::canStep(int direction) const
{
    return !m_locked && (direction == 1 || (direction == -1 && m_reverse));
}

void PlaybackGroup::stepFast(int direction)
{
    if (!canStep(direction))
        return;
    const double top = direction == 1 ? 16.0 : 4.0;
    if (direction != m_direction || m_speed < 2.0)
        setSpeed(direction, 2.0);
    else
        setSpeed(direction, m_speed >= top ? 1.0 : m_speed * 2);
}

void PlaybackGroup::stepSlow(int direction)
{
    if (!canStep(direction))
        return;
    if (direction == m_direction && m_speed == 0.5)
        setSpeed(direction, 0.25);
    else if (direction == m_direction && m_speed == 0.25)
        setSpeed(direction, 1.0);
    else
        setSpeed(direction, 0.5);
}

void PlaybackGroup::playBackward()
{
    if (canStep(-1))
        setSpeed(-1, 1.0);
}

void PlaybackGroup::setReverseAvailable(bool on)
{
    if (on == m_reverse)
        return;
    m_reverse = on;
    emit reverseAvailableChanged();
    if (!on && m_direction == -1)
        setSpeed(1, 1.0);
}

void PlaybackGroup::setDirectKeys(const QStringList &keys)
{
    if (keys == m_direct)
        return;
    m_direct = keys;
    updateLocked();
}

void PlaybackGroup::updateLocked()
{
    bool locked = false;
    if (m_selection && !m_selection->keys().isEmpty()) {
        locked = true;
        for (const QString &k : m_selection->keys())
            if (!m_direct.contains(k)) {
                locked = false;
                break;
            }
    }
    if (locked)
        setSpeed(1, 1.0);
    if (locked == m_locked)
        return;
    m_locked = locked;
    emit speedLockedChanged();
}

QString PlaybackGroup::speedLabel(int direction, double speed, bool paused)
{
    const QString x = speed == 0.25 ? QStringLiteral("¼") : speed == 0.5 ? QStringLiteral("½")
                                                                       : QString::number(speed);
    if (paused)
        return direction == 1 && speed == 1.0 ? QStringLiteral("Paused")
                                              : QStringLiteral("Paused · %1×").arg(x);
    return (direction == 1 ? QStringLiteral("▶ ") : QStringLiteral("◀ ")) + x + QStringLiteral("×");
}

void PlaybackGroup::setMonth(int year, int month)
{
    if (year == m_year && month == m_month)
        return;
    m_year = year;
    m_month = month;
    recompute();
}

void PlaybackGroup::setDays(const QString &key, int year, int month, const QVariantList &days)
{
    QList<int> list;
    for (const QVariant &d : days)
        list << d.toInt();
    m_byKey[key][monthId(year, month)] = list;
    if (year == m_year && month == m_month)
        recompute();
}

void PlaybackGroup::select(const QString &key)
{
    if (m_selection && m_selection->contains(key))
        setSelected(key);
}

void PlaybackGroup::onSelectionChanged()
{
    const QStringList order = m_selection ? m_selection->paneOrder() : QStringList();
    if (!order.contains(m_selected))
        setSelected(order.isEmpty() ? QString() : order.first());
    QStringList keys = m_selection ? m_selection->keys() : QStringList();
    keys.sort();
    if (keys != m_keysSeen) {
        m_keysSeen = keys;
        setSpeed(1, 1.0);
    }
    updateLocked();
    recompute();
}

void PlaybackGroup::recompute()
{
    QSet<int> all;
    if (m_selection) {
        const int id = monthId(m_year, m_month);
        for (const QString &key : m_selection->keys()) {
            const auto months = m_byKey.constFind(key);
            if (months == m_byKey.constEnd())
                continue;
            const auto days = months->constFind(id);
            if (days != months->constEnd())
                for (int d : *days)
                    all.insert(d);
        }
    }
    QList<int> sorted(all.begin(), all.end());
    std::sort(sorted.begin(), sorted.end());
    QVariantList out;
    for (int d : sorted)
        out << d;
    if (out == m_days)
        return;
    m_days = out;
    emit recordingDaysChanged();
}

void PlaybackGroup::setSelected(const QString &key)
{
    if (key == m_selected)
        return;
    m_selected = key;
    emit selectedKeyChanged();
}

} // namespace rl
