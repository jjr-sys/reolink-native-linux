#include "ui/PaneBarRules.h"

#include <QtTest>

using namespace rl;
using namespace rl::panebar;

// One bottom bar on every video pane (wayfinder/live-replay tickets 10 and 11, layout A): which
// transport buttons a pane's pill shows, which can act (and why not), which is lit and what the
// fast / slow buttons say; which tools sit on the pill and which go under ⋯; how far a pill
// collapses to fit its pane. The QML only lays these out and calls the actions.
class TestPaneBarRules : public QObject
{
    Q_OBJECT

    static State playing(Page page, int direction, double speed)
    {
        State s;
        s.page = page;
        s.direction = direction;
        s.speed = speed;
        return s;
    }

private slots:
    // ---- Which panes have a transport (and a time line) ---------------------------------
    void recorderCamerasHaveATransportOnEveryPage()
    {
        for (Page p : {Page::Live, Page::Replay, Page::Playback, Page::Events}) {
            State s;
            s.page = p;
            QVERIFY(hasTransport(s));
        }
    }

    void boschAndBareStreamsHaveNone()
    {
        for (Page p : {Page::Live, Page::Playback, Page::Events}) {
            State s;
            s.page = p;
            s.camera = Camera::Stream;
            QVERIFY(!hasTransport(s));
            for (const Button &b : transport(s, Full))
                QVERIFY(!b.shown);
        }
    }

    void frigateHasNoReplayOnLiveButPlaysInPlaybackAndEvents()
    {
        State s;
        s.camera = Camera::Frigate;
        s.page = Page::Live;
        QVERIFY(!hasTransport(s));
        s.page = Page::Playback;
        QVERIFY(hasTransport(s));
        s.page = Page::Events;
        QVERIFY(hasTransport(s));
    }

    // ---- The fast and slow buttons' captions --------------------------------------------
    void fastAndSlowSayFastAndSlowAtRest()
    {
        const auto t = transport(playing(Page::Playback, 1, 1.0), Full);
        QCOMPARE(t[kFastBack].caption, QStringLiteral("fast"));
        QCOMPARE(t[kFastForward].caption, QStringLiteral("fast"));
        QCOMPARE(t[kSlowBack].caption, QStringLiteral("slow"));
        QCOMPARE(t[kSlowForward].caption, QStringLiteral("slow"));
        QVERIFY(t[kForward].lit);
        QVERIFY(!t[kFastForward].lit);
    }

    void theFastButtonShowsItsSpeedOnlyInItsOwnDirection()
    {
        auto t = transport(playing(Page::Playback, 1, 4.0), Full);
        QCOMPARE(t[kFastForward].caption, QStringLiteral("4×"));
        QVERIFY(t[kFastForward].lit);
        QCOMPARE(t[kFastBack].caption, QStringLiteral("fast"));
        QVERIFY(!t[kFastBack].lit);
        QVERIFY(!t[kForward].lit);

        t = transport(playing(Page::Replay, -1, 16.0), Full);
        QCOMPARE(t[kFastBack].caption, QStringLiteral("16×"));
        QVERIFY(t[kFastBack].lit);
        QCOMPARE(t[kFastForward].caption, QStringLiteral("fast"));
    }

    void theSlowButtonShowsHalfAndQuarter()
    {
        auto t = transport(playing(Page::Playback, -1, 0.5), Full);
        QCOMPARE(t[kSlowBack].caption, QStringLiteral("½×"));
        QVERIFY(t[kSlowBack].lit);
        QCOMPARE(t[kSlowForward].caption, QStringLiteral("slow"));
        t = transport(playing(Page::Playback, 1, 0.25), Full);
        QCOMPARE(t[kSlowForward].caption, QStringLiteral("¼×"));
        QVERIFY(t[kSlowForward].lit);
    }

    // A Frigate pane among recorder panes plays at 1× while the group runs fast: its own bar's
    // greyed buttons are never lit (review #2, candidate 5's small fixes).
    void aGreyedButtonIsNeverLit()
    {
        State s = playing(Page::Playback, 1, 4.0);
        s.camera = Camera::Frigate;
        const auto t = transport(s, Full);
        QVERIFY(!t[kFastForward].enabled);
        QVERIFY(!t[kFastForward].lit);
        QCOMPARE(t[kFastForward].caption, QStringLiteral("fast"));
    }

    void backAtOneTimesLightsTheBackButton()
    {
        const auto t = transport(playing(Page::Replay, -1, 1.0), Full);
        QVERIFY(t[kBack].lit);
        QVERIFY(!t[kForward].lit);
    }

    void pausedLightsNothingAndSaysAtRest()
    {
        State s = playing(Page::Playback, 1, 8.0);
        s.paused = true;
        const auto t = transport(s, Full);
        for (const Button &b : t)
            QVERIFY(!b.lit);
        QCOMPARE(t[kFastForward].caption, QStringLiteral("fast"));
    }

    // ---- What can't act, and why --------------------------------------------------------
    void aLivePaneCanOnlyGoBack()
    {
        const auto t = transport(playing(Page::Live, 1, 1.0), Full);
        for (const QString &id : {kSkipBack, kFastBack, kSlowBack, kBack}) {
            QVERIFY2(t[id].shown && t[id].enabled, qPrintable(id));
            QVERIFY(t[id].reason.isEmpty());
        }
        for (const QString &id : {kPause, kForward, kSlowForward, kFastForward, kSkipForward}) {
            QVERIFY2(t[id].shown && !t[id].enabled, qPrintable(id));
            QVERIFY(t[id].reason.contains(QStringLiteral("ahead of live")));
        }
        // Live is not "playing at 1×": nothing lit.
        for (const Button &b : t)
            QVERIFY(!b.lit);
    }

    void replayingEverythingActs()
    {
        const auto t = transport(playing(Page::Replay, 1, 1.0), Full);
        for (const Button &b : t)
            QVERIFY(b.shown && b.enabled);
    }

    void aFrigatePaneInPlaybackOnlyPlaysForward()
    {
        State s = playing(Page::Playback, 1, 1.0);
        s.camera = Camera::Frigate;
        const auto t = transport(s, Full);
        for (const QString &id : {kSkipBack, kPause, kForward, kSkipForward})
            QVERIFY2(t[id].enabled, qPrintable(id));
        for (const QString &id : {kFastBack, kSlowBack, kBack, kSlowForward, kFastForward}) {
            QVERIFY2(!t[id].enabled, qPrintable(id));
            QCOMPARE(t[id].reason, QStringLiteral("Frigate plays at 1× forward only"));
        }
    }

    void aLockedSpeedGreysTheSpeedsWithFrigatesReason()
    {
        State s = playing(Page::Playback, 1, 1.0);
        s.speedLocked = true;
        const auto t = transport(s, Full);
        QVERIFY(t[kForward].enabled);
        QVERIFY(!t[kFastForward].enabled);
        QVERIFY(!t[kBack].enabled);
        QCOMPARE(t[kSlowForward].reason, QStringLiteral("Frigate plays at 1× forward only"));
    }

    void noReversePlayGreysTheBackButtonsOnly()
    {
        State s = playing(Page::Playback, 1, 1.0);
        s.reverseAvailable = false;
        const auto t = transport(s, Full);
        for (const QString &id : {kFastBack, kSlowBack, kBack}) {
            QVERIFY(!t[id].enabled);
            QVERIFY(!t[id].reason.isEmpty());
        }
        QVERIFY(t[kSkipBack].enabled);
        QVERIFY(t[kFastForward].enabled);
    }

    // Ticket 12: a recorder's event clip has every speed both ways, as Playback.
    void recorderEventClipsHaveEverySpeedBothWays()
    {
        const auto t = transport(playing(Page::Events, -1, 4.0), Full);
        for (const Button &b : t)
            QVERIFY(b.shown && b.enabled);
        QVERIFY(t[kFastBack].lit);
        QCOMPARE(t[kFastBack].caption, QStringLiteral("4×"));
    }

    // A Frigate clip is a downloaded file: every forward speed, no reverse.
    void frigateEventClipsPlayForwardsAtAnySpeed()
    {
        State f = playing(Page::Events, 1, 8.0);
        f.camera = Camera::Frigate;
        const auto t = transport(f, Full);
        for (const QString &id : {kSkipBack, kPause, kForward, kSlowForward, kFastForward, kSkipForward})
            QVERIFY2(t[id].enabled, qPrintable(id));
        QVERIFY(t[kFastForward].lit);
        for (const QString &id : {kFastBack, kSlowBack, kBack}) {
            QVERIFY2(!t[id].enabled, qPrintable(id));
            QCOMPARE(t[id].reason, QStringLiteral("Frigate clips play forwards only"));
        }
    }

    // ---- A press: a button or a key, and whether it acts (wayfinder/architecture ticket 04) --
    void theKeysAreTheirButtons()
    {
        const State s = playing(Page::Replay, 1, 1.0);
        QCOMPARE(press(s, QStringLiteral("J")).id, kFastBack);
        QCOMPARE(press(s, QStringLiteral("K")).id, kPause);
        QCOMPARE(press(s, QStringLiteral("L")).id, kFastForward);
        QCOMPARE(press(s, QStringLiteral("Left")).id, kSkipBack);
        QCOMPARE(press(s, QStringLiteral("Right")).id, kSkipForward);
        for (const QString &k : {"J", "K", "L", "Left", "Right"})
            QVERIFY2(press(s, k).refusal.isEmpty(), qPrintable(k));
    }

    void aButtonIdPressesItself()
    {
        const Press p = press(playing(Page::Playback, 1, 1.0), kSlowBack);
        QCOMPARE(p.id, kSlowBack);
        QVERIFY(p.refusal.isEmpty());
    }

    void anyOtherKeyIsNotTheTransports()
    {
        const Press p = press(playing(Page::Replay, 1, 1.0), QStringLiteral("X"));
        QVERIFY(p.id.isEmpty());
        QVERIFY(p.refusal.isEmpty());
    }

    // At live J starts ◀ at 1× (as the pane's ◀ does), ← starts 10 s back; the rest have
    // nothing ahead of them and say so.
    void atLiveJStartsBackAndTheForwardKeysSayWhyNot()
    {
        const State s = playing(Page::Live, 1, 1.0);
        QCOMPARE(press(s, QStringLiteral("J")).id, kBack);
        QCOMPARE(press(s, QStringLiteral("Left")).id, kSkipBack);
        for (const QString &k : {"K", "L", "Right"}) {
            const Press p = press(s, k);
            QVERIFY2(p.id.isEmpty(), qPrintable(k));
            QVERIFY2(p.refusal.contains(QStringLiteral("ahead of live")), qPrintable(k));
        }
    }

    void jOnAFrigateClipSaysForwardsOnly()
    {
        State s = playing(Page::Events, 1, 1.0);
        s.camera = Camera::Frigate;
        const Press j = press(s, QStringLiteral("J"));
        QVERIFY(j.id.isEmpty());
        QCOMPARE(j.refusal, QStringLiteral("Frigate clips play forwards only"));
        QCOMPARE(press(s, QStringLiteral("L")).id, kFastForward);
    }

    void aLockedSpeedRefusesJAndLButNotTheRest()
    {
        State s = playing(Page::Playback, 1, 1.0);
        s.speedLocked = true;
        for (const QString &k : {"J", "L"}) {
            const Press p = press(s, k);
            QVERIFY2(p.id.isEmpty(), qPrintable(k));
            QCOMPARE(p.refusal, QStringLiteral("Frigate plays at 1× forward only"));
        }
        for (const QString &k : {"K", "Left", "Right"})
            QVERIFY2(!press(s, k).id.isEmpty(), qPrintable(k));
    }

    void noReversePlayRefusesJOnly()
    {
        State s = playing(Page::Playback, 1, 1.0);
        s.reverseAvailable = false;
        QVERIFY(press(s, QStringLiteral("J")).id.isEmpty());
        QVERIFY(!press(s, QStringLiteral("J")).refusal.isEmpty());
        QCOMPARE(press(s, QStringLiteral("L")).id, kFastForward);
    }

    // Paused (or a clip that ended, which is paused where it stopped): every key acts; K
    // plays on.
    void aPausedPaneTakesEveryKey()
    {
        State s = playing(Page::Playback, -1, 4.0);
        s.paused = true;
        for (const QString &k : {"J", "K", "L", "Left", "Right"}) {
            const Press p = press(s, k);
            QVERIFY2(!p.id.isEmpty() && p.refusal.isEmpty(), qPrintable(k));
        }
        s.page = Page::Events;
        s.camera = Camera::Frigate;
        QCOMPARE(press(s, QStringLiteral("K")).id, kPause);
        QVERIFY(press(s, QStringLiteral("J")).id.isEmpty());
    }

    // A pane with nothing recorded has no transport: its keys are silently not its own.
    void aPaneWithoutATransportTakesNoPress()
    {
        State s = playing(Page::Live, 1, 1.0);
        s.camera = Camera::Stream;
        for (const QString &k : {"J", "K", "Left"}) {
            const Press p = press(s, k);
            QVERIFY(p.id.isEmpty());
            QVERIFY(p.refusal.isEmpty());
        }
    }

    // A key acts whatever the pill shows: ← works on a pane too small for its ↺ button.
    void aKeyActsWhenItsButtonIsCollapsedAway()
    {
        QVERIFY(!transport(playing(Page::Replay, 1, 1.0), NoSkips)[kSkipBack].shown);
        QCOMPARE(press(playing(Page::Replay, 1, 1.0), QStringLiteral("Left")).id, kSkipBack);
    }

    // ---- Collapsing to fit --------------------------------------------------------------
    void aSmallPaneDropsTheSkipButtons()
    {
        const State s = playing(Page::Replay, 1, 1.0);
        QVERIFY(transport(s, Full)[kSkipBack].shown);
        for (int step = NoSkips; step < kSteps; ++step) {
            const auto t = transport(s, step);
            QVERIFY(!t[kSkipBack].shown);
            QVERIFY(!t[kSkipForward].shown);
            QVERIFY(t[kPause].shown);
        }
    }

    void captionsGoAtRestOnTheNarrowestStepsButASpeedStays()
    {
        auto t = transport(playing(Page::Replay, 1, 4.0), NoCaptions);
        QCOMPARE(t[kFastBack].caption, QString());
        QCOMPARE(t[kSlowForward].caption, QString());
        QCOMPARE(t[kFastForward].caption, QStringLiteral("4×"));
    }

    void fitStepIsTheFirstThatFits()
    {
        const QList<double> widths{500, 440, 380, 330, 290};
        QCOMPARE(fitStep(600, widths), 0);
        QCOMPARE(fitStep(500, widths), 0);
        QCOMPARE(fitStep(450, widths), 1);
        QCOMPARE(fitStep(331, widths), 3);
        QCOMPARE(fitStep(100, widths), 4);   // nothing fits: the narrowest
        QCOMPARE(fitStep(100, {}), 0);
    }

    // ---- Tools on the pill and under ⋯ --------------------------------------------------
    void liveShowsTheCommonToolsAndMenusTheRest()
    {
        const QStringList have{"large", "snapshot", "record", "rotate", "zoom", "ptz", "talk",
                               "siren", "floodlight", "popout", "maximise"};
        const ToolSplit s = tools(Page::Live, have, Full);
        QCOMPARE(s.shown, (QStringList{"snapshot", "record", "ptz", "talk"}));
        QCOMPARE(s.menu, (QStringList{"large", "rotate", "zoom", "siren", "floodlight",
                                      "popout", "maximise"}));
    }

    void eachPageHasItsOwnCommonTools()
    {
        const QStringList replay{"snapshot", "rotate", "zoom", "popout", "maximise"};
        QCOMPARE(tools(Page::Replay, replay, Full).shown, QStringList{"snapshot"});
        const QStringList playback{"snapshot", "rotate", "zoom", "range", "maximise"};
        QCOMPARE(tools(Page::Playback, playback, Full).shown, QStringList{"snapshot"});
        QCOMPARE(tools(Page::Playback, playback, Full).menu,
                 (QStringList{"rotate", "zoom", "range", "maximise"}));
        const QStringList events{"newer", "older", "open", "zoom"};
        QCOMPARE(tools(Page::Events, events, Full).shown, (QStringList{"newer", "older"}));
        QCOMPARE(tools(Page::Events, events, Full).menu, (QStringList{"open", "zoom"}));
    }

    void boschPickersShowWhenRoomyElseUnderTheMenu()
    {
        const QStringList have{"positions", "scene", "wiper", "tour", "snapshot", "record", "ptz",
                               "maximise"};
        QCOMPARE(tools(Page::Live, have, Full).shown,
                 (QStringList{"positions", "scene", "wiper", "tour", "snapshot", "record", "ptz"}));
        const ToolSplit few = tools(Page::Live, have, FewTools);
        QCOMPARE(few.shown, QStringList{"positions"});
        QCOMPARE(few.menu, (QStringList{"scene", "wiper", "tour", "snapshot", "record", "ptz",
                                        "maximise"}));
    }

    void aToolNotAvailableIsLeftOut()
    {
        const ToolSplit s = tools(Page::Live, {"snapshot", "maximise"}, Full);
        QCOMPARE(s.shown, QStringList{"snapshot"});
        QCOMPARE(s.menu, QStringList{"maximise"});
    }

    void theNarrowestStepPutsEveryToolUnderTheMenu()
    {
        const ToolSplit s = tools(Page::Events, {"newer", "older", "open"}, AllToolsInMenu);
        QVERIFY(s.shown.isEmpty());
        QCOMPARE(s.menu, (QStringList{"newer", "older", "open"}));
    }

    // ---- The QML face -------------------------------------------------------------------
    void theQmlFaceReadsMaps()
    {
        PaneBarRules r;
        const QVariantMap t = r.transport({{"page", "live"}, {"camera", "recorder"}}, 0);
        QCOMPARE(t.value(kBack).toMap().value("enabled").toBool(), true);
        QCOMPARE(t.value(kForward).toMap().value("enabled").toBool(), false);
        QVERIFY(!r.hasTransport({{"page", "live"}, {"camera", "frigate"}}));
        const QVariantMap s = r.tools("events", {"newer", "older", "open"}, 0);
        QCOMPARE(s.value("shown").toStringList(), (QStringList{"newer", "older"}));
        QCOMPARE(r.fitStep(450, {500, 440}), 1);
        const QVariantMap p = r.press({{"page", "events"}, {"camera", "frigate"}}, "J");
        QCOMPARE(p.value("id").toString(), QString());
        QCOMPARE(p.value("refusal").toString(), QStringLiteral("Frigate clips play forwards only"));
        QCOMPARE(r.press({{"page", "live"}, {"camera", "recorder"}}, "J").value("id").toString(), kBack);
    }
};

QTEST_GUILESS_MAIN(TestPaneBarRules)
#include "test_panebarrules.moc"
