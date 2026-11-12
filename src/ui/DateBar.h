#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace rl::datebar {

// The Events feed's date bar (wayfinder/events-feed step 19), as geometry: where each row
// and day sits in the feed, worked out from the rows' own heights (the list view only
// knows the rows it has built), where each day's mark goes on the bar, and how a point on
// the bar maps to a row and back.

struct Day {
    QString day;     // "yyyy-MM-dd"
    double top = 0;  // its header's top in the feed
};

struct Layout {
    QVector<double> rowTop;    // each row's top (below its day's header)
    QVector<double> rowHeight;
    QVector<Day> days;         // in feed order, newest first
    double total = 0;          // the feed's whole height
};

// Rows in feed order with their day and height; a day's header (headerHeight) sits above its
// first row, and `spacing` goes between one row and whatever follows it.
Layout layout(const QStringList &rowDays, const QVector<double> &heights, double headerHeight,
              double spacing);

struct Mark {
    QString day;
    double tickY = 0;      // where the day starts, in bar pixels
    double y = 0;          // where its label goes (tickY when it has none)
    bool labelled = false;
};

// One mark per day. Labels sit at their day's start, stacked downward at least `minGap`
// apart and pushed back up so the last ends inside the bar. When there are more days than
// fit, the days in `keep` stay labelled and every second of the others (then every fourth,
// ...) is labelled; the rest keep only their tick.
QVector<Mark> placeMarks(const Layout &layout, double barHeight, double minGap,
                         const QSet<QString> &keep);

// The row to show at the top when the bar is pressed at `y`: the part of the feed there,
// centred on the pointer and kept within the feed. -1 when the feed is empty.
int rowAtBar(const Layout &layout, double y, double barHeight, double viewHeight);

// Where `offset` pixels into row `row` sits on the bar (the bottom for a row past the end).
double barY(const Layout &layout, int row, double offset, double barHeight);

} // namespace rl::datebar
