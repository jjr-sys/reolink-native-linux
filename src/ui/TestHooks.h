#pragma once

#include <QObject>
#include <QString>

namespace rl {

// The hooks on-screen checks drive the app with, as QML reads them (the TestHooks singleton;
// wayfinder/architecture ticket 06). Each is read once from its RL_* variable; none set means
// every hook is off. Hooks read in C++ (RL_SCREENSHOT, RL_NO_PUSH, RL_MOCK_EVENTS, ...) stay
// where they're used.
class TestHooks : public QObject
{
    Q_OBJECT
    // RL_INITIAL_PAGE: open on a page (0 Live, 1 Playback, 2 Events, 3 Settings).
    Q_PROPERTY(int initialPage READ initialPage CONSTANT)
    // RL_SETTINGS_CATEGORY=<key>: open Settings on a section ("highlights", "mosaics").
    Q_PROPERTY(QString initialSettingsCategory READ initialSettingsCategory CONSTANT)
    // RL_EVENTS_AUTOSCROLL=1: scroll the Events feed to its end, log the longest stall
    // between scroll steps, and quit (checks the feed scrolls smoothly).
    Q_PROPERTY(bool eventsAutoScroll READ eventsAutoScroll CONSTANT)
    // RL_EVENTS_PLAY=1: play the shown camera's newest event in the clip player once the
    // feed has loaded.
    Q_PROPERTY(bool eventsAutoPlay READ eventsAutoPlay CONSTANT)
    // RL_EVENTS_KEYS="J,J,K": with RL_EVENTS_PLAY, steps for the clip player 4 s after its
    // first picture (live-replay ticket 12). Comma-separated: a key (J K L Left Right), "SD"
    // (HD off), "KEEP" (Keep playing, only once the clip has ended, as on screen) or "+N"
    // (wait N seconds). From the first step on, the clip's phase, speed and shown time are
    // logged each second.
    Q_PROPERTY(QString eventsKeys READ eventsKeys CONSTANT)
    // RL_MOCK_RECORDINGS=1: Playback shows made-up recordings.
    Q_PROPERTY(bool mockRecordings READ mockRecordings CONSTANT)
    // RL_MOCK_DOORBELL=<hostId>:<channel>[@<secs>]: a mock doorbell press on that camera
    // (that many seconds after start).
    Q_PROPERTY(bool mockDoorbell READ mockDoorbell CONSTANT)
    Q_PROPERTY(QString mockDoorbellTarget READ mockDoorbellTarget CONSTANT)
    // RL_OPEN_CLIP=<hostId>:<channel>: open the detection clip for "30 s ago" at start.
    Q_PROPERTY(QString openClipTarget READ openClipTarget CONSTANT)
    // RL_PLAYBACK_AUTOPLAY=1: Playback plays its selection once the day is in. With it:
    // RL_PLAYBACK_AT=<secs into the day> plays from there (-1: unset); RL_PLAYBACK_STOP_MS
    // presses Stop that long after it starts; RL_PLAYBACK_HD=1 switches the selected pane
    // to HD first; RL_PLAYBACK_BACK=<n> plays backwards (1 = ◀ 1×, n > 1 = « n-1 times);
    // RL_PLAYBACK_FAST=<n> presses » n times once it plays.
    Q_PROPERTY(bool playbackAutoplay READ playbackAutoplay CONSTANT)
    Q_PROPERTY(int playbackAtSecs READ playbackAtSecs CONSTANT)
    Q_PROPERTY(int playbackStopMs READ playbackStopMs CONSTANT)
    Q_PROPERTY(bool playbackHd READ playbackHd CONSTANT)
    Q_PROPERTY(int playbackBackSteps READ playbackBackSteps CONSTANT)
    Q_PROPERTY(int playbackFastSteps READ playbackFastSteps CONSTANT)
    // RL_PANE_BARS=1: every video pane's bottom bar shows without hover (screenshots have
    // no pointer; live-replay ticket 11).
    Q_PROPERTY(bool paneBarsShown READ paneBarsShown CONSTANT)
    // RL_LIVE_REPLAY="<hostId:channel>;<action>": Live View maximises that camera and, 8 s
    // later, starts Live replay: action "J" (◀ from now) or "<secs behind now>:<1|-1>"
    // (play from there forwards or backwards), optionally ":<» presses>" (live-replay 07);
    // optionally ";<secs>": ● Live that many seconds after replay starts (live-replay 08d);
    // optionally ";<key>,<key>": with the grid not maximised, those panes press J 2 s after
    // replay starts, joining it (live-replay 15b; "<secs>" may be empty); optionally
    // ";<key>@<secs>": maximise that camera then, and back to the grid 5 s later.
    Q_PROPERTY(QString liveReplayTest READ liveReplayTest CONSTANT)

public:
    explicit TestHooks(QObject *parent = nullptr);

    int initialPage() const { return m_initialPage; }
    QString initialSettingsCategory() const { return m_initialSettingsCategory; }
    bool eventsAutoScroll() const { return m_eventsAutoScroll; }
    bool eventsAutoPlay() const { return m_eventsAutoPlay; }
    QString eventsKeys() const { return m_eventsKeys; }
    bool mockRecordings() const { return m_mockRecordings; }
    bool mockDoorbell() const { return m_mockDoorbell; }
    QString mockDoorbellTarget() const { return m_mockDoorbellTarget; }
    QString openClipTarget() const { return m_openClipTarget; }
    bool playbackAutoplay() const { return m_playbackAutoplay; }
    int playbackAtSecs() const { return m_playbackAtSecs; }
    int playbackStopMs() const { return m_playbackStopMs; }
    bool playbackHd() const { return m_playbackHd; }
    int playbackBackSteps() const { return m_playbackBackSteps; }
    int playbackFastSteps() const { return m_playbackFastSteps; }
    bool paneBarsShown() const { return m_paneBarsShown; }
    QString liveReplayTest() const { return m_liveReplayTest; }

private:
    int m_initialPage;
    QString m_initialSettingsCategory;
    bool m_eventsAutoScroll;
    bool m_eventsAutoPlay;
    QString m_eventsKeys;
    bool m_mockRecordings;
    bool m_mockDoorbell;
    QString m_mockDoorbellTarget;
    QString m_openClipTarget;
    bool m_playbackAutoplay;
    int m_playbackAtSecs;
    int m_playbackStopMs;
    bool m_playbackHd;
    int m_playbackBackSteps;
    int m_playbackFastSteps;
    bool m_paneBarsShown;
    QString m_liveReplayTest;
};

} // namespace rl
