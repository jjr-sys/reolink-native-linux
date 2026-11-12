#include "ui/DateBar.h"

#include <QtTest>

using namespace rl::datebar;

// The Events feed's date bar (wayfinder/events-feed step 19): day marks lined up with where
// each day starts in the feed, stacked so labels never overlap, and the mapping between a
// point on the bar and a row of the feed.
class TestDateBar : public QObject
{
    Q_OBJECT

    // Rows of equal height 100, no spacing or headers, unless a test says otherwise.
    static Layout rows(const QStringList &days, double height = 100)
    {
        return layout(days, QVector<double>(days.size(), height), 0, 0);
    }
    static QStringList labelled(const QVector<Mark> &marks)
    {
        QStringList out;
        for (const Mark &m : marks)
            if (m.labelled)
                out << m.day;
        return out;
    }

private slots:
    void layoutAddsHeadersAndSpacing()
    {
        // Two days: a header (30) above each day's first row, 10 between rows.
        const Layout l = layout({"d2", "d2", "d1"}, {100, 28, 100}, 30, 10);
        QCOMPARE(l.rowTop, QVector<double>({30, 140, 208}));
        QCOMPARE(l.total, 308.0);
        QCOMPARE(l.days.size(), 2);
        QCOMPARE(l.days.at(0).day, QStringLiteral("d2"));
        QCOMPARE(l.days.at(0).top, 0.0);    // the header's top
        QCOMPARE(l.days.at(1).top, 178.0);  // d1's header: 10 below the row above, 30 above its row
    }

    void marksSitWhereTheirDayStarts()
    {
        // Four days of 1, 1, 1 and 1 rows over a 400 px bar: marks at 0, 100, 200, 300.
        const Layout l = rows({"d4", "d3", "d2", "d1"});
        const QVector<Mark> marks = placeMarks(l, 400, 20, {});
        QCOMPARE(marks.size(), 4);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(marks.at(i).y, i * 100.0);
            QVERIFY(marks.at(i).labelled);
        }
    }

    // A busy day followed by quiet ones: the quiet days' labels stack below at the minimum
    // gap instead of overlapping, while each tick stays where its day starts.
    void tinyDaysNextToABigOneStack()
    {
        QStringList days;
        for (int i = 0; i < 96; ++i)
            days << "big";
        days << "q3" << "q2" << "q1" << "rest";
        const Layout l = rows(days); // total 10,000; the quiet days start at 9600, 9700, 9800, 9900
        const QVector<Mark> marks = placeMarks(l, 500, 20, {});
        QCOMPARE(marks.at(0).y, 0.0);
        // Ideal 480, 485, 490, 495 on a 500 px bar: stacked 20 apart and kept inside the bar
        // (the last label ends at the bottom), pushed back up from the bottom.
        QCOMPARE(marks.at(4).y, 480.0);
        QCOMPARE(marks.at(3).y, 460.0);
        QCOMPARE(marks.at(2).y, 440.0);
        QCOMPARE(marks.at(1).y, 420.0);
        QCOMPARE(marks.at(1).tickY, 480.0); // the tick stays where the day starts
        QCOMPARE(labelled(marks), QStringList({"big", "q3", "q2", "q1", "rest"}));
    }

    // Labels pushed up from the bottom still never go above the one before them, and
    // stay inside the bar.
    void stackingNeverOverlapsAtEitherEnd()
    {
        const Layout l = rows({"a", "b", "c", "d", "e", "f"});
        const QVector<Mark> marks = placeMarks(l, 100, 20, {});
        double prev = -1e9;
        for (const Mark &m : marks) {
            if (!m.labelled)
                continue;
            QVERIFY(m.y - prev >= 20 - 1e-9);
            QVERIFY(m.y >= 0);
            QVERIFY(m.y <= 100 - 20);
            prev = m.y;
        }
    }

    // A month that can't fit keeps a tick for every day, labels Today and Yesterday, and
    // labels every second day of the rest.
    void aFullMonthIsThinned()
    {
        QStringList days;
        for (int d = 30; d >= 1; --d)
            days << QStringLiteral("2026-11-%1").arg(d, 2, 10, QLatin1Char('0'));
        const Layout l = rows(days);
        const QVector<Mark> marks =
            placeMarks(l, 500, 26, {QStringLiteral("2026-11-30"), QStringLiteral("2026-11-29")});
        QCOMPARE(marks.size(), 30);  // a tick for every day
        const QStringList shown = labelled(marks);
        QVERIFY(shown.contains("2026-11-30"));
        QVERIFY(shown.contains("2026-11-29"));
        QCOMPARE(shown.size(), 16); // Today, Yesterday and every second of the other 28
        // Every second of the rest, not a run of them.
        QVERIFY(shown.contains("2026-11-28") != shown.contains("2026-11-27"));
        for (int i = 1; i < marks.size(); ++i)
            if (marks.at(i).labelled)
                for (int j = i - 1; j >= 0; --j)
                    if (marks.at(j).labelled) {
                        QVERIFY(marks.at(i).y - marks.at(j).y >= 26 - 1e-9);
                        break;
                    }
    }

    // A point on the bar shows that part of the feed, centred on the pointer.
    void barPointMapsToTheRowThere()
    {
        const Layout l = rows({"d10", "d9", "d8", "d7", "d6", "d5", "d4", "d3", "d2", "d1"}); // 1,000
        // Bar 100 high, view 200 high: the pointer at 50 centres content 500, so the view
        // starts at 400: row 4.
        QCOMPARE(rowAtBar(l, 50, 100, 200), 4);
        QCOMPARE(rowAtBar(l, 0, 100, 200), 0);     // the top
        QCOMPARE(rowAtBar(l, -10, 100, 200), 0);   // above the bar
        QCOMPARE(rowAtBar(l, 100, 100, 200), 8);   // the bottom: the last full view
        QCOMPARE(rowAtBar(l, 500, 100, 200), 8);   // below the bar
        QCOMPARE(rowAtBar(Layout{}, 50, 100, 200), -1);
    }

    void aRowMapsToItsPlaceOnTheBar()
    {
        const Layout l = rows({"a", "a", "b", "b"}); // 400
        QCOMPARE(barY(l, 0, 0, 200), 0.0);
        QCOMPARE(barY(l, 2, 0, 200), 100.0);
        QCOMPARE(barY(l, 2, 50, 200), 125.0); // half way into row 2
        QCOMPARE(barY(l, 9, 0, 200), 200.0);  // past the end: the bottom
        QCOMPARE(barY(Layout{}, 0, 0, 200), 0.0);
    }
};

QTEST_GUILESS_MAIN(TestDateBar)
#include "test_datebar.moc"
