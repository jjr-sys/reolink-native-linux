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

void PlaybackGroup::setSpeed(double speed)
{
    static const double kSpeeds[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
    if (speed == m_speed || std::find(std::begin(kSpeeds), std::end(kSpeeds), speed) == std::end(kSpeeds))
        return;
    m_speed = speed;
    emit speedChanged();
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
