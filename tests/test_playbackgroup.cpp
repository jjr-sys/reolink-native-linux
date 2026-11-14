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
                      host(4, QStringLiteral("Farm Hikvision")), host(3, QStringLiteral("Farm Frigate"))});
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

    // Speed and direction (wayfinder/playback-controls ticket 11, layout B4): one value for
    // every pane, stepped by the four speed buttons; ◀ / ▶ play at 1× that way.
    void fastForwardSteps2_4_8_16_then1()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0"));
        QCOMPARE(f.group.speed(), 1.0);
        QCOMPARE(f.group.direction(), 1);
        QList<double> seen;
        for (int i = 0; i < 5; ++i) {
            f.group.stepFast(1);
            seen << f.group.speed();
        }
        QCOMPARE(seen, (QList<double>{2, 4, 8, 16, 1}));
        QCOMPARE(f.group.direction(), 1);
    }

    void slowForwardStepsHalf_quarter_then1()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0"));
        QList<double> seen;
        for (int i = 0; i < 3; ++i) {
            f.group.stepSlow(1);
            seen << f.group.speed();
        }
        QCOMPARE(seen, (QList<double>{0.5, 0.25, 1}));
    }

    // Switching between the fast and slow cycles starts the other cycle afresh.
    void otherCycleStartsAtItsFirstStep()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0"));
        f.group.stepFast(1);
        f.group.stepFast(1); // 4x
        f.group.stepSlow(1);
        QCOMPARE(f.group.speed(), 0.5);
        f.group.stepFast(1);
        QCOMPARE(f.group.speed(), 2.0);
        f.group.playForward();
        QCOMPARE(f.group.speed(), 1.0);
    }

    // Until reverse play is built (ticket 12) the backward buttons do nothing.
    void backwardIsOffUntilReversePlay()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0"));
        QVERIFY(!f.group.reverseAvailable());
        f.group.stepFast(1);
        QSignalSpy spy(&f.group, &PlaybackGroup::speedChanged);
        f.group.stepFast(-1);
        f.group.stepSlow(-1);
        f.group.playBackward();
        QCOMPARE(f.group.direction(), 1);
        QCOMPARE(f.group.speed(), 2.0);
        QCOMPARE(spy.count(), 0);
    }

    // Backwards (once reverse play exists): « steps 2 → 4 → 8 → 16 → 1× like » (one ladder
    // both ways, live-replay ticket 09), slow back ½ → ¼ → 1×, and both end at 1× backwards.
    void backwardCyclesEndAt1xBackwards()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0"));
        f.group.setReverseAvailable(true);
        QList<double> seen;
        for (int i = 0; i < 5; ++i) {
            f.group.stepFast(-1);
            seen << f.group.speed();
            QCOMPARE(f.group.direction(), -1);
        }
        QCOMPARE(seen, (QList<double>{2, 4, 8, 16, 1}));
        f.group.stepFast(-1);
        f.group.stepFast(-1);
        QVERIFY(!f.group.sdAtSpeed());      // HD holds to 4x both ways
        f.group.stepFast(-1);
        QVERIFY(f.group.sdAtSpeed());       // 8x: SD at speed
        f.group.playBackward();
        seen.clear();
        for (int i = 0; i < 3; ++i) {
            f.group.stepSlow(-1);
            seen << f.group.speed();
        }
        QCOMPARE(seen, (QList<double>{0.5, 0.25, 1}));
        QCOMPARE(f.group.direction(), -1);
        // A forward button from backwards starts the forward cycle.
        f.group.stepFast(1);
        QCOMPARE(f.group.speed(), 2.0);
        QCOMPARE(f.group.direction(), 1);
        f.group.playBackward();
        QCOMPARE(f.group.speed(), 1.0);
        QCOMPARE(f.group.direction(), -1);
    }

    // A new day or a change of selected cameras resets to ▶ 1×; selecting another pane
    // or re-ordering the panes keeps the speed.
    void newDayOrCameraSetResetsTo1xForward()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0,2:1"));
        f.group.stepFast(1);
        f.group.select(QStringLiteral("2:1"));
        f.selection.swap(QStringLiteral("2:0"), QStringLiteral("2:1"));
        QCOMPARE(f.group.speed(), 2.0);
        f.selection.toggle(QStringLiteral("1:3"));
        QCOMPARE(f.group.speed(), 1.0);
        f.group.stepFast(1);
        f.group.newDay();
        QCOMPARE(f.group.speed(), 1.0);
        QCOMPARE(f.group.direction(), 1);
    }

    // Frigate plays at 1× forward only: with only Frigate panes selected the speed buttons
    // and ◀ do nothing (the page greys them).
    void onlyFrigatePanesLockTheSpeed()
    {
        Fixture f;
        f.group.setReverseAvailable(true);
        f.selection.setSaved(QStringLiteral("3:0"));
        f.group.setDirectKeys({QStringLiteral("3:0")});
        QVERIFY(f.group.speedLocked());
        f.group.stepFast(1);
        f.group.stepSlow(1);
        f.group.playBackward();
        QCOMPARE(f.group.speed(), 1.0);
        QCOMPARE(f.group.direction(), 1);
        // A Reolink pane alongside unlocks it.
        f.selection.toggle(QStringLiteral("2:0"));
        QVERIFY(!f.group.speedLocked());
        f.group.stepFast(1);
        QCOMPARE(f.group.speed(), 2.0);
        // Nothing selected is not "only Frigate".
        f.selection.clear();
        QVERIFY(!f.group.speedLocked());
    }

    // Off 1× forward the Frigate panes freeze ("1× only").
    void offNormalIsAnythingBut1xForward()
    {
        Fixture f;
        f.selection.setSaved(QStringLiteral("2:0"));
        f.group.setReverseAvailable(true);
        QVERIFY(!f.group.offNormal());
        f.group.stepSlow(1);
        QVERIFY(f.group.offNormal());
        f.group.playBackward();
        QVERIFY(f.group.offNormal());
        f.group.playForward();
        QVERIFY(!f.group.offNormal());
    }

    // The speed as text above the buttons.
    void speedLabel()
    {
        QCOMPARE(PlaybackGroup::speedLabel(1, 1.0, false), QStringLiteral("▶ 1×"));
        QCOMPARE(PlaybackGroup::speedLabel(1, 16.0, false), QStringLiteral("▶ 16×"));
        QCOMPARE(PlaybackGroup::speedLabel(-1, 0.5, false), QStringLiteral("◀ ½×"));
        QCOMPARE(PlaybackGroup::speedLabel(1, 0.25, false), QStringLiteral("▶ ¼×"));
        QCOMPARE(PlaybackGroup::speedLabel(1, 4.0, true), QStringLiteral("Paused · 4×"));
        QCOMPARE(PlaybackGroup::speedLabel(1, 1.0, true), QStringLiteral("Paused"));
    }
};

QTEST_GUILESS_MAIN(TestPlaybackGroup)
#include "test_playbackgroup.moc"
