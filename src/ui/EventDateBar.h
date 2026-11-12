#pragma once

#include "ui/DateBar.h"
#include "ui/EventFeed.h"

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>

namespace rl {

// The date bar beside the Events feed (wayfinder/events-feed step 19): keeps the feed's
// geometry (datebar::layout) from its rows' kinds and the heights the page draws them at,
// places the day marks for the bar's height, and maps between the bar and the feed's rows.
class EventDateBar : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::EventFeed *feed READ feed WRITE setFeed NOTIFY inputsChanged)
    Q_PROPERTY(double eventHeight MEMBER m_eventHeight WRITE setEventHeight NOTIFY inputsChanged)
    Q_PROPERTY(double slimHeight MEMBER m_slimHeight WRITE setSlimHeight NOTIFY inputsChanged)
    Q_PROPERTY(double headerHeight MEMBER m_headerHeight WRITE setHeaderHeight NOTIFY inputsChanged)
    Q_PROPERTY(double spacing MEMBER m_spacing WRITE setSpacing NOTIFY inputsChanged)
    Q_PROPERTY(double barHeight MEMBER m_barHeight WRITE setBarHeight NOTIFY inputsChanged)
    Q_PROPERTY(double minGap MEMBER m_minGap WRITE setMinGap NOTIFY inputsChanged)
    // Days that keep their label however full the bar is (Today, Yesterday).
    Q_PROPERTY(QStringList keep MEMBER m_keep WRITE setKeep NOTIFY inputsChanged)
    // [{ day, tickY, y, labelled }], newest first.
    Q_PROPERTY(QVariantList marks READ marks NOTIFY changed)
    Q_PROPERTY(double total READ total NOTIFY changed)

public:
    explicit EventDateBar(QObject *parent = nullptr);

    EventFeed *feed() const;
    void setFeed(EventFeed *feed);
    void setEventHeight(double v) { set(m_eventHeight, v); }
    void setSlimHeight(double v) { set(m_slimHeight, v); }
    void setHeaderHeight(double v) { set(m_headerHeight, v); }
    void setSpacing(double v) { set(m_spacing, v); }
    void setBarHeight(double v) { set(m_barHeight, v); }
    void setMinGap(double v) { set(m_minGap, v); }
    void setKeep(const QStringList &days);

    QVariantList marks() const { return m_marks; }
    double total() const { return m_layout.total; }

    // The row to put at the top for a press at `y` on the bar; -1 when the feed is empty.
    Q_INVOKABLE int rowAtBar(double y, double viewHeight) const;
    // Where `offset` pixels into `row` sits on the bar.
    Q_INVOKABLE double barY(int row, double offset) const;

signals:
    void inputsChanged();
    void changed();

private:
    void set(double &field, double v);
    void scheduleUpdate();
    void update();

    QPointer<EventFeed> m_feed;
    double m_eventHeight = 0, m_slimHeight = 0, m_headerHeight = 0, m_spacing = 0;
    double m_barHeight = 0, m_minGap = 26;
    QStringList m_keep;
    bool m_pending = false;
    datebar::Layout m_layout;
    QVariantList m_marks;
};

} // namespace rl
