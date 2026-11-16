#pragma once

#include <QDate>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

#include "ui/PlaybackGroup.h"

namespace rl {

// Live replay (wayfinder/live-replay ticket 07): a Live View pane steps back from now into its
// recorder camera's footage and plays it with Playback's controls, across days, as far back as
// storage holds; "● Live" (or reaching the newest footage going forwards) returns it to live.
// Up to four panes replay in step (ticket 15): one playhead, one speed, led by one camera, as
// Playback's panes are. Each pane plays through Playback's pane; this holds what is worth
// testing apart from it:
//
//  - Which cameras replay (start / join / leave / stop), which one leads (setLead: its footage
//    decides where a play lands and carries on, its frames move the playhead — or, where it has
//    none, the next camera's), and the speed and direction (group: Playback's ladder).
//  - The playhead (epoch seconds; frame times move it) and its day: the day's recordings in
//    seconds into the day, as Playback's pane takes them.
//  - Each camera's recordings, searched a day at a time as needed (searchWanted → dayFound, one
//    at a time per camera), and its oldest recording, found month by month backwards from today
//    until a month with none (each day's search also answers which days of its month have
//    footage).
//  - Where a play lands (playFrom), and where a stream that ended carries on (carryOn):
//    across days; nothing earlier than the oldest; forwards with nothing ahead, back to live.
//  - The "time behind live" slider's stretched scale and the strip of known recordings on it.
//  - Back to live where recorder-paced forward play stalls at the newest footage.
//  - The pane's live buffer (ticket 08b): its spans count as footage, so ◀ from now starts in
//    it at once and carries on into the recorder's footage from its oldest frame.
class LiveReplay : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool replaying READ replaying NOTIFY replayingChanged)
    Q_PROPERTY(QString cameraKey READ cameraKey NOTIFY replayingChanged)    // the lead
    Q_PROPERTY(QStringList cameraKeys READ cameraKeys NOTIFY replayingChanged)
    Q_PROPERTY(double now READ now WRITE setNow NOTIFY nowChanged)
    Q_PROPERTY(double playhead READ playhead NOTIFY playheadChanged)
    Q_PROPERTY(double behind READ behind NOTIFY behindChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(double dayEpoch READ dayEpoch NOTIFY dayChanged)
    Q_PROPERTY(QVariantList daySegments READ daySegments NOTIFY dayChanged)
    Q_PROPERTY(double oldest READ oldest NOTIFY recordingsChanged)
    Q_PROPERTY(bool oldestFound READ oldestFound NOTIFY recordingsChanged)
    Q_PROPERTY(QVariantList strip READ strip NOTIFY recordingsChanged)
    Q_PROPERTY(rl::PlaybackGroup *group READ group CONSTANT)

public:
    explicit LiveReplay(QObject *parent = nullptr);

    bool replaying() const { return !m_lead.isEmpty(); }
    QString cameraKey() const { return m_lead; }
    QStringList cameraKeys() const { return m_keys; }
    static constexpr int kMaxCameras = 4;
    double now() const { return m_now; }
    void setNow(double epochSecs);
    double playhead() const { return m_playhead; }
    double behind() const { return qMax(0.0, m_now - m_playhead); }
    bool paused() const { return m_paused; }
    void setPaused(bool paused);
    double dayEpoch() const;
    QVariantList daySegments() const;
    // The oldest recording's start any camera has (epoch seconds): found, or the oldest known so
    // far; -1 none.
    double oldest() const;
    bool oldestFound() const;   // every camera's oldest found
    // Known recordings as slider spans {from, to} (0 = now … 1 = the oldest), oldest last.
    QVariantList strip() const;
    PlaybackGroup *group() const { return m_group; }

    // This camera replays (any other goes back to live): a fresh day book, 1× forward, and
    // today asked for first.
    Q_INVOKABLE void start(const QString &cameraKey);
    // Back to live (every replaying pane).
    Q_INVOKABLE void stop();
    // Several panes in step (ticket 15): another camera joins the running replay at the
    // playhead (false when nothing replays, it's in already, or four are); it searches its own
    // recorder. leave: that pane back to live (the lead's leaving hands the lead to the next;
    // the last's ends replay). setLead: the camera whose footage and frames lead (Playback's
    // selected pane); false when it isn't replaying.
    Q_INVOKABLE bool join(const QString &cameraKey);
    Q_INVOKABLE void leave(const QString &cameraKey);
    Q_INVOKABLE bool setLead(const QString &cameraKey);

    // Play from `epoch`, going `direction` (1 forwards, -1 backwards): there if a recording
    // covers it, else forwards to the next recording's start (or, with nothing ahead before
    // now, the newest footage's last 10 s), backwards to the previous one's end. Waits for the
    // days it needs; then playWanted(epoch), or nothingEarlier.
    Q_INVOKABLE void playFrom(double epoch, int direction);
    // The stream ran out going `direction` (a recording's start or end, a day's end): carry on
    // from the playhead to the previous recording's end or the next one's start; backwards
    // past the oldest, nothingEarlier; forwards with nothing ahead before now, back to live.
    // Only the lead camera's stream moves the playhead on (Playback's rule, ticket 15a).
    Q_INVOKABLE void carryOn(const QString &key, int direction);
    // ↺ / ↻: `secs` from the playhead (never past now): there if footage covers it, else back to
    // the previous recording's end, or forwards to the next one's start — with nothing ahead,
    // back to live.
    Q_INVOKABLE void skipBy(double secs);
    Q_INVOKABLE bool covered(double epoch) const;              // the lead camera's footage
    Q_INVOKABLE bool covers(const QString &key, double epoch) const;
    // That day's recordings for one camera, as daySegments (its pane plays them).
    Q_INVOKABLE QVariantList daySegmentsFor(const QString &key) const;
    // A replaying camera's live buffer: its runs as {start, end} epoch seconds (the page
    // updates it each second); counted as footage. inBuffer: whether the lead's holds `epoch`.
    Q_INVOKABLE void setBufferSpans(const QString &key, const QVariantList &spans);
    Q_INVOKABLE bool inBuffer(double epoch) const;

    // A day's search answered: its recordings (seconds into the day) and the days of its month
    // with recordings. A failed search may be asked again.
    Q_INVOKABLE void dayFound(const QString &key, int year, int month, int day,
                              const QVariantList &segments, const QVariantList &monthDays);
    Q_INVOKABLE void dayFailed(const QString &key, int year, int month, int day);

    // The pane shows a frame recorded at `epoch` (wallMs: a monotonic clock); a new stream opened.
    // Only the lead camera's frames move the playhead.
    Q_INVOKABLE void frameAt(const QString &key, double epoch, double wallMs);
    Q_INVOKABLE void opened(const QString &key);
    // Forwards, the picture's time hasn't moved for 8 s within 5 min of now: the recorder's
    // newest footage (recorder-paced replay stalls there). Not paused, not before a picture.
    Q_INVOKABLE bool stalledAtEdge(double wallMs) const;

    // The slider: marks at now · 1 min · 10 min · 1 h · 1 day · 1 week · oldest, evenly spaced,
    // linear between them; marks at or past the oldest dropped. Positions 0 … 1.
    Q_INVOKABLE static double posForBehind(double behindSecs, double oldestBehindSecs);
    Q_INVOKABLE static double behindForPos(double pos, double oldestBehindSecs);
    Q_INVOKABLE static QVariantList sliderMarks();   // the marks before the oldest, seconds behind
    // "42 s", "1 min 40 s", "2 h 1 min", "1 d 1 h".
    Q_INVOKABLE static QString behindText(double secs);
    Q_INVOKABLE double posFor(double epoch) const;
    Q_INVOKABLE double epochForPos(double pos) const;

signals:
    void replayingChanged();
    void nowChanged();
    void playheadChanged();
    void behindChanged();
    void pausedChanged();
    void dayChanged();
    void recordingsChanged();
    // Ask the camera's device for this day's recordings (answer with dayFound / dayFailed).
    void searchWanted(const QString &key, int year, int month, int day);
    // Open every replaying pane's stream at `epoch` (the playhead and its day are already there).
    void playWanted(double epoch);
    // Open one pane's stream at `epoch`: forwards, its recorder footage reached its live buffer.
    void paneWanted(const QString &cameraKey, double epoch);
    // Backwards reached the oldest recording: pause there.
    void nothingEarlier();

private:
    struct Span {
        double start = 0;
        double end = 0;
    };
    enum class Pending { None, Play, CarryOn };
    // Where a play with nothing ahead before now goes: the newest footage's last 10 s (a play
    // from live or a drag), or back to live (a skip).
    enum class Ahead { NewestFootage, Live };
    static constexpr double kPending = -2;
    static constexpr double kNone = -1;
    // The live buffer (ticket 08b): ◀ "from now" starts at its newest frame when that is
    // within this of now (frames reach this machine up to ≈8 s after their camera time,
    // ticket 08a); forwards this close to its newest frame is now (back to live).
    static constexpr double kBufferNowSlackSecs = 15;
    static constexpr double kLiveEdgeSecs = 3;

    // One camera's recordings as known so far, its searches and its live buffer (ticket 15a).
    struct Book {
        QHash<QDate, QList<Span>> days;  // searched days: recordings, epoch seconds
        QList<Span> buffer;              // the live buffer's runs, epoch seconds
        QHash<int, QList<int>> months;   // year*100+month -> days with recordings
        QHash<QDate, int> failures;      // failed searches per day
        QList<QDate> queue;
        QDate inFlight;
        bool oldestFound = false;
        QDate oldestDay;                 // once found
        int scanMonth = 0;               // year*100+month the oldest scan asked for last (0 = idle)
        double todayAnswered = -1;       // now when today's search was last answered
        bool landedInBuffer = false;     // its pane's last play opened in its buffer
        bool known(const QDate &day) const { return days.contains(day); }
    };
    // A replaying camera's book (never makes one: callers hold a camera that replays).
    Book &book(const QString &key)
    {
        Q_ASSERT(m_books.contains(key));
        return m_books[key];
    }
    const Book &leadBook() const
    {
        static const Book none;
        const auto it = m_books.constFind(m_lead);
        return it == m_books.cend() ? none : it.value();
    }
    static double oldestOf(const Book &b);
    static bool inBufferOf(const Book &b, double epoch);
    void leadChanged();
    QString frameDriver() const;

    void reset();
    void want(const QString &key, const QDate &day, bool urgent);
    void pump(const QString &key);
    void advanceScan(const QString &key);
    void retryPending();
    void land(double epoch, const QDate &day);
    void resolve(double epoch, int direction, Ahead ahead);
    // Nothing ahead before now: ask for today again first when its answer is old (a long
    // replay may have more footage ahead now), else back to live. False while asking.
    bool liveOrRefresh(Pending retry);
    void storeDay(const QString &key, const QDate &day, const QList<Span> &spans);
    void setPlayhead(double epoch, const QDate &day);
    QList<Span> spansOn(const Book &b, const QDate &day) const;
    // Whether `day` may have recordings: unknown, or its month says so; never before the oldest.
    bool mayHave(const Book &b, const QDate &day) const;
    QDate dayOf(double epoch) const;
    double midnight(const QDate &day) const;
    // The previous recording's end before `epoch` / the next one's start after it, with the
    // day it belongs to; kPending while a day it needs is asked for, kNone when there is none.
    double previousEnd(const QString &key, double epoch, QDate *day);
    double nextStart(const QString &key, double epoch, QDate *day);
    double oldestBehind() const;

    PlaybackGroup *m_group;
    QString m_lead;
    double m_now = 0;
    double m_stripNow = 0;             // now when the strip was last announced
    double m_playhead = 0;
    // the playhead's day (a recording's end at midnight is its day's)
    QDate m_day;
    bool m_paused = false;

    QHash<QString, Book> m_books;      // the replaying cameras'
    QStringList m_keys;                // the replaying cameras, in the order they joined

    Pending m_pending = Pending::None;
    double m_pendingEpoch = 0;
    int m_pendingDirection = 1;
    Ahead m_pendingAhead = Ahead::NewestFootage;

    double m_lastCarry = -1;           // where carryOn last landed, and which way
    int m_lastCarryDirection = 0;
    double m_lastFrame = -1;
    double m_lastMoveWall = -1;
};

} // namespace rl
