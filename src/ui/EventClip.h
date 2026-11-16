#pragma once

#include <QObject>
#include <QPointer>
#include <functional>

#include "ui/PlaybackGroup.h"

namespace rl {

class DeviceManager;

// The Events page's clip player (wayfinder/events-feed step 22): which footage an event
// plays and over what window. A Reolink recorder plays its sub stream from kPreSecs before
// the event; a camera whose footage is on Frigate plays Frigate's own clip from
// kFrigatePreSecs before (Frigate's events reach the app up to 10 s late). Both run to
// kPostSecs after, where the player stops with Keep playing / Open in Playback.
// Positions are wall-clock epoch seconds.
//
// It is also the player's state machine (architecture review #2, candidate 2): EventClipPlayer
// tells it what happened — the user's presses, the engines' reports (a first picture, a stream
// that stopped or reconnects, a file that ended or failed to load) and a tick of the wall clock
// (milliseconds, any clock) — and does what it asks (open, stop, pause or seek the file, retry
// HD, show a notice). It holds the phase, the playhead, the give-up times and the speed group.
class EventClip : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::DeviceManager *devices READ devices WRITE setDevices NOTIFY devicesChanged)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(qint64 hostId READ hostId NOTIFY changed)
    Q_PROPERTY(int channel READ channel NOTIFY changed)
    Q_PROPERTY(qint64 timestamp READ timestamp NOTIFY changed)
    Q_PROPERTY(bool frigate READ frigate NOTIFY changed)
    Q_PROPERTY(QString clipUrl READ clipUrl NOTIFY changed)
    Q_PROPERTY(qint64 from READ from NOTIFY changed)
    Q_PROPERTY(qint64 to READ to NOTIFY changed)
    Q_PROPERTY(bool openEnded READ openEnded NOTIFY changed)
    Q_PROPERTY(Phase phase READ phase NOTIFY phaseChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY phaseChanged)          // paused or ended
    Q_PROPERTY(bool hadPicture READ hadPicture NOTIFY phaseChanged)  // this event showed a picture
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    // Where the bar shows the playhead: the paused moment while paused.
    Q_PROPERTY(double shownPosition READ shownPosition NOTIFY positionChanged)
    // The frame on screen's camera time (StreamPlayer::frameEpochMs) and the Frigate file's
    // position (MediaPlayer::position): bound by the player.
    Q_PROPERTY(double frameEpochMs READ frameEpochMs WRITE setFrameEpochMs NOTIFY positionChanged)
    Q_PROPERTY(double mediaPositionMs READ mediaPositionMs WRITE setMediaPositionMs NOTIFY positionChanged)
    // The ticks are only needed while starting (give-up) or playing a recorder (the clock).
    Q_PROPERTY(bool ticking READ ticking NOTIFY phaseChanged)
    Q_PROPERTY(rl::PlaybackGroup *group READ group CONSTANT)

public:
    // "idle", "starting" (opened, no picture yet), "playing", "paused", "ended" (stopped at the
    // window's end), "failed".
    enum Phase { Idle, Starting, Playing, Paused, Ended, Failed };
    Q_ENUM(Phase)
    enum Notice { StartOfClip, PartMissing };
    Q_ENUM(Notice)
    // Give up on a picture after this long starting (a Frigate clip is cut on request; a
    // recorder's reverse or fast windows take longer to arrive than its 1× stream).
    static constexpr int kGiveUpMs = 10000;
    static constexpr int kGiveUpOffNormalMs = 30000;
    static constexpr int kGiveUpFrigateMs = 20000;

    static constexpr int kPreSecs = 5;
    static constexpr int kFrigatePreSecs = 15;
    static constexpr int kPostSecs = 30;
    static constexpr int kFrigateMoreSecs = 60; // Keep playing on Frigate: the next minute
    // (host, channel, timestamp, preSecs, postSecs) → a Frigate clip URL, "" for a recorder.
    using ClipResolver = std::function<QString(qint64, int, qint64, int, int)>;

    explicit EventClip(QObject *parent = nullptr);

    DeviceManager *devices() const;
    void setDevices(DeviceManager *devices);
    // Tests: stands in for DeviceManager::eventClipUrl.
    void setClipResolver(ClipResolver resolver);

    bool active() const { return m_active; }
    qint64 hostId() const { return m_host; }
    int channel() const { return m_channel; }
    qint64 timestamp() const { return m_ts; }
    bool frigate() const { return !m_url.isEmpty(); }
    QString clipUrl() const { return m_url; }
    qint64 from() const { return m_from; }
    qint64 to() const { return m_to; }
    bool openEnded() const { return m_openEnded; }

    Phase phase() const { return m_phase; }
    bool paused() const { return m_phase == Paused || m_phase == Ended; }
    bool hadPicture() const { return m_hadPicture; }
    double position() const;
    double shownPosition() const { return m_phase == Paused ? m_pausedAt : position(); }
    double frameEpochMs() const { return m_frameEpochMs; }
    void setFrameEpochMs(double ms);
    double mediaPositionMs() const { return m_mediaPositionMs; }
    void setMediaPositionMs(double ms);
    bool ticking() const { return m_phase == Starting || (m_phase == Playing && !frigate()); }
    PlaybackGroup *group() const { return m_group; }

    // The event's window, without playing it (tests and the window maths).
    Q_INVOKABLE void setEvent(qint64 hostId, int channel, qint64 timestamp);
    Q_INVOKABLE void clear();

    // ---- The user ----
    // Play an event from its window's start, at 1× forwards, trying HD again.
    Q_INVOKABLE void play(qint64 hostId, int channel, qint64 timestamp);
    Q_INVOKABLE void close();
    // Keep playing past `to`, with no end (openEnded): a recorder's stream plays on from the
    // picture; a Frigate clip can't, so each call moves the window to the next
    // kFrigateMoreSecs as a new clip (and again as each one ends while it is in the past).
    Q_INVOKABLE void keepPlaying();
    // ❚❚ closes a recorder's stream (freeing the recorder; the last picture stays) or pauses the
    // file; ▶ plays on from there; at the end, from the window's start.
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void seekTo(double at);
    Q_INVOKABLE void skip(double secs);
    // The group's speed or direction was changed by a speed button: a paused clip plays on from
    // where it is (an ended one backwards from its end, forwards from its start).
    Q_INVOKABLE void speedChanged();
    // HD switched: a running recorder stream reopens (moved in place it would stay SD).
    Q_INVOKABLE void hdChanged();
    // Open in Playback: the moment to hand over (the event's own before a picture).
    Q_INVOKABLE double handOffAt() const;

    // ---- The engines ----
    // The first picture of an open (true when that started the playhead).
    Q_INVOKABLE bool firstPicture(double wallMs);
    // The recorder dropped the stream and the player retries it from where it was opened.
    Q_INVOKABLE void streamReconnecting();
    // The recorder's stream stopped by itself (`stillStopped`: the player is still stopped when
    // this is handled — an ordered stop posts Stopped too and has moved on by then).
    Q_INVOKABLE void streamStopped(bool stillStopped);
    // The Frigate file played to its end (`nowSecs`: the wall clock, epoch seconds).
    Q_INVOKABLE void mediaEnded(double nowSecs);
    Q_INVOKABLE void loadFailed();   // the Frigate clip couldn't be fetched
    Q_INVOKABLE void openFailed();   // the recorder has nothing to play here
    // The session's route changed while open (speed, direction): reopen at the picture.
    Q_INVOKABLE void reopenNeeded();
    // The wall clock: moves a recorder's playhead, and gives up on a picture that doesn't come.
    Q_INVOKABLE void tick(double wallMs);

    // Where `position` sits in [from, to], 0..1; and whether it has reached `to`.
    Q_INVOKABLE double progress(double position) const;
    Q_INVOKABLE bool atEnd(double position) const;
    // The pane bar (live-replay ticket 11): the moment at `fraction` (0..1) of the window, and
    // `secs` on from `position` kept inside it (no end once open-ended). 0 with no event.
    Q_INVOKABLE double timeAt(double fraction) const;
    Q_INVOKABLE double skipFrom(double position, double secs) const;

signals:
    void devicesChanged();
    void changed();
    void phaseChanged();
    void positionChanged();
    // What the player does.
    void openWanted(double at);    // open the stream at `at` (Frigate: load clipUrl)
    void stopAllWanted();          // stop every engine (the download, the file, the stream)
    void stopStreamWanted();       // stop the recorder's stream, keeping the last picture
    void pauseMediaWanted();
    void seekMediaWanted(double ms);   // the file to `ms` and play
    void retryHdWanted();          // a play the user asked for tries HD again
    void notice(int which);        // a Notice

private:
    QString resolve(qint64 hostId, int channel, qint64 timestamp, int pre, int post) const;
    // Ticket 12: whether playing `direction` (1 / -1) from `position` has reached the window's
    // edge — its start backwards (within a quarter second), its end forwards (atEnd).
    bool edgeReached(double position, int direction) const;
    // A recorder stream going `direction` stopped at `position`: where to open it again (a
    // second backwards from the window's start / forwards from its end is the edge), or -1 —
    // the edge, or it already reopened there (`lastReopenAt`, -1 for none) and ended again.
    double reopenAfterEnd(double position, int direction, double lastReopenAt) const;
    void extendWindow();
    void setPhase(Phase phase);
    void open(double at);
    void stopAtStart();
    void reachEnd();
    void giveUp();
    void checkEdge();
    int direction() const { return m_group->direction(); }

    PlaybackGroup *m_group;
    Phase m_phase = Idle;
    bool m_hadPicture = false;
    double m_clock = 0;              // a recorder's playhead
    double m_openedAt = 0;
    double m_pausedAt = 0;
    double m_lastReopenAt = -1;
    double m_frameEpochMs = 0;
    double m_epochAtOpenMs = 0;      // the frame on screen when the stream was opened
    double m_mediaPositionMs = 0;
    double m_lastTickWall = -1;      // playing: the wall time the clock last moved
    double m_startingSince = -1;     // starting: the first tick's wall time

    QPointer<DeviceManager> m_devices;
    ClipResolver m_resolver;
    bool m_active = false;
    qint64 m_host = -1;
    int m_channel = 0;
    qint64 m_ts = 0;
    QString m_url;
    qint64 m_from = 0, m_to = 0;
    bool m_openEnded = false;
};

} // namespace rl
