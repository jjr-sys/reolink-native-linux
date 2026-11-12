#include "ui/EventDateBar.h"

#include "ui/EventFeed.h"

#include <QMetaObject>

namespace rl {

EventDateBar::EventDateBar(QObject *parent) : QObject(parent) {}

EventFeed *EventDateBar::feed() const
{
    return m_feed;
}

void EventDateBar::setFeed(EventFeed *feed)
{
    if (m_feed == feed)
        return;
    if (m_feed)
        disconnect(m_feed, nullptr, this, nullptr);
    m_feed = feed;
    if (feed) {
        // Rows only come and go (a gap's start moves in place); recount on any of those.
        for (auto sig : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
            connect(feed, sig, this, &EventDateBar::scheduleUpdate);
        connect(feed, &QAbstractItemModel::modelReset, this, &EventDateBar::scheduleUpdate);
    }
    emit inputsChanged();
    scheduleUpdate();
}

void EventDateBar::setKeep(const QStringList &days)
{
    if (m_keep == days)
        return;
    m_keep = days;
    emit inputsChanged();
    scheduleUpdate();
}

void EventDateBar::set(double &field, double v)
{
    if (field == v)
        return;
    field = v;
    emit inputsChanged();
    scheduleUpdate();
}

// Inputs arrive in bursts (a resize changes several heights at once): work it out once.
void EventDateBar::scheduleUpdate()
{
    if (m_pending)
        return;
    m_pending = true;
    QMetaObject::invokeMethod(this, &EventDateBar::update, Qt::QueuedConnection);
}

void EventDateBar::update()
{
    m_pending = false;
    QStringList days;
    QVector<double> heights;
    if (m_feed) {
        const int n = m_feed->rowCount();
        days.reserve(n);
        heights.reserve(n);
        for (int i = 0; i < n; ++i) {
            const QModelIndex ix = m_feed->index(i);
            const QString type = m_feed->data(ix, EventFeed::TypeRole).toString();
            const bool slim = m_feed->data(ix, EventFeed::KindRole).toString() == QLatin1String("gap")
                              || type == QLatin1String("online") || type == QLatin1String("offline");
            days << m_feed->data(ix, EventFeed::DayRole).toString();
            heights << (slim ? m_slimHeight : m_eventHeight);
        }
    }
    m_layout = datebar::layout(days, heights, m_headerHeight, m_spacing);
    m_marks.clear();
    const QSet<QString> keep(m_keep.cbegin(), m_keep.cend());
    for (const datebar::Mark &m : datebar::placeMarks(m_layout, m_barHeight, m_minGap, keep))
        m_marks.append(QVariantMap{{QStringLiteral("day"), m.day},
                                   {QStringLiteral("tickY"), m.tickY},
                                   {QStringLiteral("y"), m.y},
                                   {QStringLiteral("labelled"), m.labelled}});
    emit changed();
}

int EventDateBar::rowAtBar(double y, double viewHeight) const
{
    return datebar::rowAtBar(m_layout, y, m_barHeight, viewHeight);
}

double EventDateBar::barY(int row, double offset) const
{
    return datebar::barY(m_layout, row, offset, m_barHeight);
}

} // namespace rl
