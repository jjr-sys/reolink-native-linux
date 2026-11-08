#include "ui/PlaybackGroup.h"
#include "ui/SiteMap.h"
#include "ui/SiteSelection.h"

#include <QSignalSpy>
#include <QtTest>

using rl::PlaybackGroup;
using rl::SiteMap;
using rl::SiteSelection;

namespace {
QVariantMap host(qint64 id, const QString &name)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}};
}
QVariantList days(std::initializer_list<int> d)
{
    QVariantList out;
    for (int v : d)
        out << v;
    return out;
}
// Farm = hosts 2 and 4, Woorabinda = host 1; Site order Farm, Woorabinda.
struct Fixture {
    SiteMap map;
    SiteSelection selection;
    PlaybackGroup group;
    Fixture()
    {
        map.setHosts({host(1, QStringLiteral("Woorabinda")), host(2, QStringLiteral("Farm")),
                      host(4, QStringLiteral("Farm Hikvision"))});
        selection.setSites(&map);
        group.setSelection(&selection);
        group.setMonth(2026, 11);
    }
};
} // namespace

// Playback's synced panes (wayfinder/site-tabs-redesign, step 4): the calendar marks the
// days any selected camera recorded, and the selected pane is the one clicked last.
class TestPlaybackGroup : public QObject
{
    Q_OBJECT
private slots:
    void calendarIsTheUnionOfSelectedCamerasDays()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0,1:3"));
        QSignalSpy spy(&f.group, &PlaybackGroup::recordingDaysChanged);
        f.group.setDays(QStringLiteral("2:0"), 2026, 11, days({3, 9, 1}));
        f.group.setDays(QStringLiteral("1:3"), 2026, 11, days({9, 12}));
        QCOMPARE(f.group.recordingDays(), days({1, 3, 9, 12}));
        QVERIFY(spy.count() >= 1);
    }

    // Days of another month, or of a camera not selected, never mark the calendar.
    void otherMonthsAndUnselectedCamerasDoNotCount()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0"));
        f.group.setDays(QStringLiteral("2:0"), 2026, 10, days({4}));
        f.group.setDays(QStringLiteral("2:7"), 2026, 11, days({5}));
        QCOMPARE(f.group.recordingDays(), QVariantList());
        f.group.setDays(QStringLiteral("2:0"), 2026, 11, days({6}));
        QCOMPARE(f.group.recordingDays(), days({6}));
        // Deselecting a camera takes its days off; reselecting brings them back.
        f.selection.toggle(QStringLiteral("2:0"));
        QCOMPARE(f.group.recordingDays(), QVariantList());
        f.selection.toggle(QStringLiteral("2:0"));
        QCOMPARE(f.group.recordingDays(), days({6}));
    }

    // Days remembered for a month come back when the calendar returns to it.
    void monthsAreRememberedPerCamera()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0"));
        f.group.setDays(QStringLiteral("2:0"), 2026, 11, days({6}));
        f.group.setMonth(2026, 10);
        QCOMPARE(f.group.recordingDays(), QVariantList());
        f.group.setDays(QStringLiteral("2:0"), 2026, 10, days({30}));
        QCOMPARE(f.group.recordingDays(), days({30}));
        f.group.setMonth(2026, 11);
        QCOMPARE(f.group.recordingDays(), days({6}));
    }

    // The selected pane follows the last click; a click on a camera not shown is ignored.
    void selectedPaneFollowsTheLastClick()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0,1:3,2:5"));
        QCOMPARE(f.group.selectedKey(), QStringLiteral("2:0")); // first in pane order
        QSignalSpy spy(&f.group, &PlaybackGroup::selectedKeyChanged);
        f.group.select(QStringLiteral("1:3"));
        QCOMPARE(f.group.selectedKey(), QStringLiteral("1:3"));
        f.group.select(QStringLiteral("2:5"));
        QCOMPARE(f.group.selectedKey(), QStringLiteral("2:5"));
        f.group.select(QStringLiteral("2:5")); // clicking it again keeps it
        QCOMPARE(f.group.selectedKey(), QStringLiteral("2:5"));
        f.group.select(QStringLiteral("9:9"));
        QCOMPARE(f.group.selectedKey(), QStringLiteral("2:5"));
        QCOMPARE(spy.count(), 2);
    }

    // There is always a selected pane while any pane is shown: the first in pane order
    // when the selected camera is removed, none when the selection empties.
    void selectedPaneFallsBackToTheFirst()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("1:3,2:5"));
        QCOMPARE(f.group.selectedKey(), QStringLiteral("2:5")); // Farm goes first
        f.group.select(QStringLiteral("1:3"));
        f.selection.toggle(QStringLiteral("1:3"));
        QCOMPARE(f.group.selectedKey(), QStringLiteral("2:5"));
        f.selection.clear();
        QCOMPARE(f.group.selectedKey(), QString());
        f.selection.undo();
        QCOMPARE(f.group.selectedKey(), QStringLiteral("2:5"));
        f.selection.toggle(QStringLiteral("1:3"));
        QCOMPARE(f.group.selectedKey(), QStringLiteral("2:5")); // adding keeps the choice
    }

    // One speed for every pane.
    void speedIsOneValueForAllPanes()
    {
        Fixture f;
        QCOMPARE(f.group.speed(), 1.0);
        QSignalSpy spy(&f.group, &PlaybackGroup::speedChanged);
        f.group.setSpeed(4.0);
        QCOMPARE(f.group.speed(), 4.0);
        f.group.setSpeed(3.0); // not on the menu
        QCOMPARE(f.group.speed(), 4.0);
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestPlaybackGroup)
#include "test_playbackgroup.moc"
