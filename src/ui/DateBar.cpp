#include "ui/DateBar.h"

#include <algorithm>
#include <cmath>

namespace rl::datebar {

Layout layout(const QStringList &rowDays, const QVector<double> &heights, double headerHeight,
              double spacing)
{
    Layout l;
    double y = 0;
    for (int i = 0; i < rowDays.size() && i < heights.size(); ++i) {
        if (i > 0)
            y += spacing;
        if (i == 0 || rowDays.at(i) != rowDays.at(i - 1)) {
            l.days.append({rowDays.at(i), y});
            y += headerHeight;
        }
        l.rowTop.append(y);
        l.rowHeight.append(heights.at(i));
        y += heights.at(i);
    }
    l.total = y;
    return l;
}

QVector<Mark> placeMarks(const Layout &layout, double barHeight, double minGap,
                         const QSet<QString> &keep)
{
    QVector<Mark> marks;
    if (layout.total <= 0 || barHeight <= 0)
        return marks;
    for (const Day &d : layout.days) {
        const double tick = d.top / layout.total * barHeight;
        marks.append({d.day, tick, tick, false});
    }

    // How many labels fit: the first at 0, the last ending at the bottom.
    const int capacity = barHeight >= minGap ? int(std::floor((barHeight - minGap) / minGap)) + 1 : 0;
    // Labels for the kept days and every `stride`th of the others (stride 0: kept only).
    const auto labels = [&](int stride) {
        QVector<bool> on;
        int other = 0;
        for (const Mark &m : std::as_const(marks))
            on << (keep.contains(m.day) || (stride > 0 && other++ % stride == 0));
        return on;
    };
    QVector<bool> chosen;
    for (int stride = 1; stride <= marks.size(); stride *= 2) {
        const QVector<bool> on = labels(stride);
        if (on.count(true) <= capacity) {
            chosen = on;
            break;
        }
    }
    if (chosen.isEmpty()) {
        chosen = labels(0);
        if (chosen.count(true) > capacity) // not even the kept days fit: ticks only
            chosen.fill(false);
    }
    for (int i = 0; i < marks.size(); ++i)
        marks[i].labelled = chosen.at(i);

    // Stack the labels downward, then push them back up from the bottom.
    double prev = -minGap;
    for (Mark &m : marks)
        if (m.labelled)
            prev = m.y = std::max(m.tickY, prev + minGap);
    double next = barHeight;
    for (auto it = marks.rbegin(); it != marks.rend(); ++it)
        if (it->labelled)
            next = it->y = std::max(0.0, std::min(it->y, next - minGap));
    return marks;
}

// The row whose span contains content offset `y` (the last when past the end).
static int rowAtOffset(const Layout &l, double y)
{
    const auto it = std::upper_bound(l.rowTop.cbegin(), l.rowTop.cend(), y);
    int row = int(it - l.rowTop.cbegin()) - 1;
    if (row < 0)
        return 0;
    // In the spacing or below a row: the row after it, if any.
    if (y >= l.rowTop.at(row) + l.rowHeight.at(row) && row + 1 < l.rowTop.size())
        ++row;
    return row;
}

int rowAtBar(const Layout &layout, double y, double barHeight, double viewHeight)
{
    if (layout.rowTop.isEmpty() || barHeight <= 0)
        return -1;
    const double centre = std::clamp(y, 0.0, barHeight) / barHeight * layout.total;
    const double top = std::clamp(centre - viewHeight / 2, 0.0,
                                  std::max(0.0, layout.total - viewHeight));
    return rowAtOffset(layout, top);
}

double barY(const Layout &layout, int row, double offset, double barHeight)
{
    if (layout.total <= 0 || row < 0)
        return 0;
    if (row >= layout.rowTop.size())
        return barHeight;
    return std::clamp((layout.rowTop.at(row) + offset) / layout.total * barHeight, 0.0, barHeight);
}

} // namespace rl::datebar
