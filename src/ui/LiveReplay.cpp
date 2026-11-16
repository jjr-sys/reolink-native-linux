#include "ui/LiveReplay.h"

#include "ui/PaneSync.h"

#include "core/Log.h"

#include <QDateTime>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace rl {

namespace {
int monthId(const QDate &d) { return d.year() * 100 + d.month(); }

// The slider's marks up to the oldest (exclusive), then the oldest.
QList<double> marksTo(double oldestBehind)
{
    QList<double> marks;
    for (const QVariant &m : LiveReplay::sliderMarks())
        if (m.toDouble() < oldestBehind)
            marks << m.toDouble();
    marks << oldestBehind;
    return marks;
}
} // namespace

LiveReplay::LiveReplay(QObject *parent)
    : QObject(parent)
    , m_group(new PlaybackGroup(this))
{
    m_group->setReverseAvailable(true);
}

void LiveReplay::setNow(double epochSecs)
{
    if (epochSecs == m_now)
        return;
    const bool stripMoves = std::abs(epochSecs - m_stripNow) >= 5;
    m_now = epochSecs;
    emit nowChanged();
    emit behindChanged();
    if (stripMoves) {
        m_stripNow = m_now;
        emit recordingsChanged();   // the strip and the slider's scale follow now
    }
}

void LiveReplay::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    // Paused (the button, the doorbell): a play still waiting for the recorder doesn't start
    // the video when its answer comes; resuming plays from the playhead (ticket 14).
    if (paused)
        m_pending = Pending::None;
    emit pausedChanged();
}

QDate LiveReplay::dayOf(double epoch) const
{
    return QDateTime::fromSecsSinceEpoch(qint64(std::floor(epoch))).date();
}

double LiveReplay::midnight(const QDate &day) const
{
    return double(QDateTime(day, QTime(0, 0)).toSecsSinceEpoch());
}

double LiveReplay::dayEpoch() const
{
    return m_day.isValid() ? midnight(m_day) : 0;
}

QList<LiveReplay::Span> LiveReplay::spansOn(const Book &b, const QDate &day) const
{
    // The day's own, and the next day's that started on it (a file over midnight).
    const double from = midnight(day), to = midnight(day.addDays(1));
    QList<Span> out = b.days.value(day);
    for (const Span &s : b.days.value(day.addDays(1)))
        if (s.start < to)
            out.append({s.start, qMin(s.end, to)});
    for (const Span &s : b.buffer)   // the live buffer counts as footage (ticket 08b)
        if (s.end >= from && s.start < to)
            out.append({s.start, qMin(s.end, to)});
    for (Span &s : out)
        s.start = qMax(s.start, from);
    std::sort(out.begin(), out.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
    return out;
}

QVariantList LiveReplay::daySegments() const
{
    return daySegmentsFor(m_lead);
}

QVariantList LiveReplay::daySegmentsFor(const QString &key) const
{
    QVariantList out;
    const auto it = m_books.constFind(key);
    if (!m_day.isValid() || it == m_books.cend())
        return out;
    const Book &b = it.value();
    const double base = dayEpoch();
    for (const Span &s : spansOn(b, m_day)) {
        QVariantMap m;
        m["start"] = s.start - base;
        m["end"] = s.end - base;
        m["type"] = QStringLiteral("timer");
        out << m;
    }
    return out;
}

double LiveReplay::oldest() const
{
    // The oldest any of the cameras can reach (ticket 15).
    double best = kNone;
    for (const Book &b : m_books) {
        const double o = oldestOf(b);
        if (o >= 0 && (best < 0 || o < best))
            best = o;
    }
    return best;
}

bool LiveReplay::oldestFound() const
{
    if (m_books.isEmpty())
        return false;
    for (const Book &b : m_books)
        if (!b.oldestFound)
            return false;
    return true;
}

double LiveReplay::oldestOf(const Book &b)
{
    if (b.oldestFound && b.oldestDay.isValid() && !b.days.value(b.oldestDay).isEmpty())
        return b.days.value(b.oldestDay).first().start;
    double best = kNone;
    for (auto it = b.days.cbegin(); it != b.days.cend(); ++it)
        if (!it.value().isEmpty() && (best < 0 || it.value().first().start < best))
            best = it.value().first().start;
    return best;
}

double LiveReplay::oldestBehind() const
{
    const double o = oldest();
    return o < 0 ? 0 : qMax(0.0, m_now - o);
}

QVariantList LiveReplay::strip() const
{
    QList<std::pair<double, double>> spans;
    const double ob = oldestBehind();
    QList<Span> all;   // every camera's footage (ticket 15)
    for (const Book &b : m_books) {
        all += b.buffer;
        for (auto it = b.days.cbegin(); it != b.days.cend(); ++it)
            all += it.value();
    }
    for (const Span &s : all)
        if (s.start < m_now)
            spans.append({posForBehind(m_now - qMin(s.end, m_now), ob),
                          posForBehind(m_now - s.start, ob)});
    std::sort(spans.begin(), spans.end());
    QVariantList out;
    for (const auto &[from, to] : spans) {
        QVariantMap m;
        m["from"] = from;
        m["to"] = to;
        out << m;
    }
    return out;
}

void LiveReplay::reset()
{
    m_books.clear();
    m_pending = Pending::None;
    m_lastCarry = -1;
    m_lastFrame = -1;
    m_lastMoveWall = -1;
    setPaused(false);
    m_group->playForward();
}

void LiveReplay::start(const QString &cameraKey)
{
    if (cameraKey.isEmpty() || cameraKey == m_lead)
        return;
    if (m_books.contains(cameraKey)) {   // already in step with the others: it leads now
        setLead(cameraKey);
        return;
    }
    if (m_lead.isEmpty())
        reset();
    else
        stop();   // another pane was replaying: it goes back to live first (stop resets)
    m_lead = cameraKey;
    m_keys = {cameraKey};
    const QDate today = dayOf(m_now);
    m_books[m_lead].scanMonth = monthId(today);   // its book, made here
    setPlayhead(m_now, today);
    qCInfo(lcUi).noquote() << "live replay" << m_lead << "from now";
    emit replayingChanged();
    emit recordingsChanged();
    want(m_lead, today, true);
}

void LiveReplay::stop()
{
    if (m_lead.isEmpty())
        return;
    qCInfo(lcUi).noquote() << "live replay" << m_keys.join(QLatin1Char(' ')) << "back to live";
    m_lead.clear();
    m_keys.clear();
    // The pane lets its recording go before the group turns back to forwards (reset): a turn
    // while it still replays would open a forward recorder stream as it closes (ticket 08d).
    emit replayingChanged();
    reset();
    emit recordingsChanged();
}

bool LiveReplay::join(const QString &cameraKey)
{
    if (!replaying() || cameraKey.isEmpty() || m_books.contains(cameraKey)
        || m_keys.size() >= kMaxCameras)
        return false;
    m_keys << cameraKey;
    const QDate today = dayOf(m_now);
    Book &b = m_books[cameraKey];
    b.scanMonth = monthId(today);
    qCInfo(lcUi).noquote() << "live replay" << cameraKey << "joins" << m_lead;
    emit replayingChanged();
    emit recordingsChanged();
    want(cameraKey, m_day.isValid() ? m_day : today, true);   // the playhead's day first
    want(cameraKey, today, true);
    return true;
}

void LiveReplay::leave(const QString &cameraKey)
{
    if (!m_books.contains(cameraKey))
        return;
    if (m_keys.size() == 1) {
        stop();
        return;
    }
    qCInfo(lcUi).noquote() << "live replay" << cameraKey << "back to live";
    m_keys.removeAll(cameraKey);
    m_books.remove(cameraKey);
    const bool leadLeft = cameraKey == m_lead;
    if (leadLeft)
        m_lead = m_keys.first();
    emit replayingChanged();
    emit dayChanged();
    emit recordingsChanged();
    if (leadLeft)
        leadChanged();
}

bool LiveReplay::setLead(const QString &cameraKey)
{
    if (!m_books.contains(cameraKey))
        return false;
    if (cameraKey != m_lead) {
        m_lead = cameraKey;
        emit replayingChanged();
        emit dayChanged();
        leadChanged();
    }
    return true;
}

// A new lead: its footage and frames decide from here. A play or carry-on still waiting is
// asked again under it (a handover, not a drop); the stall clock and carry-on memory start
// afresh.
void LiveReplay::leadChanged()
{
    m_lastCarry = -1;
    opened(m_lead);
    retryPending();
}

// Whose frames move the playhead: the lead's, or where it has no footage at the playhead,
// the first replaying camera that has (Playback's frameDriverSecs).
QString LiveReplay::frameDriver() const
{
    // PaneSync's rule (shared with Playback); with none playing, the lead's.
    QList<panesync::Pane> panes;
    for (const QString &k : m_keys) {
        panesync::Pane p;
        p.key = k;
        p.shown = m_books.value(k).shown;
        p.covered = covers(k, m_playhead);
        panes.append(p);
    }
    const QString key = panesync::frameDriver(m_lead, panes);
    return key.isEmpty() ? m_lead : key;
}

bool LiveReplay::needsRestart(const QVariantMap &pane, double nowMs) const
{
    panesync::Pane p = panesync::paneFrom(pane);
    if (!replaying() || m_paused || !m_books.contains(p.key))
        return false;
    p.shown = m_books.value(p.key).shown;
    p.covered = covers(p.key, m_playhead);
    return panesync::needsRestart(p, m_playhead, m_group->speed(), frameDriver() == p.key, nowMs);
}

void LiveReplay::setShown(const QString &cameraKey, bool shown)
{
    const auto it = m_books.find(cameraKey);
    if (it != m_books.end())
        it.value().shown = shown;
}

bool LiveReplay::anyShown() const
{
    for (const Book &b : m_books)
        if (b.shown)
            return true;
    return false;
}

void LiveReplay::want(const QString &key, const QDate &day, bool urgent)
{
    const auto it = m_books.find(key);
    if (it == m_books.end())
        return;
    Book &b = it.value();
    if (!day.isValid() || b.known(day) || day == b.inFlight)
        return;
    const qsizetype at = b.queue.indexOf(day);
    if (at >= 0) {
        if (urgent && at > 0)
            b.queue.move(at, 0);
        return;
    }
    if (urgent)
        b.queue.prepend(day);
    else
        b.queue.append(day);
    pump(key);
}

// One search in flight per camera (different recorders answer in parallel).
void LiveReplay::pump(const QString &key)
{
    const auto it = m_books.find(key);
    if (it == m_books.end())
        return;
    Book &b = it.value();
    if (b.inFlight.isValid() || b.queue.isEmpty())
        return;
    b.inFlight = b.queue.takeFirst();
    emit searchWanted(key, b.inFlight.year(), b.inFlight.month(), b.inFlight.day());
}

void LiveReplay::dayFound(const QString &key, int year, int month, int day,
                          const QVariantList &segments, const QVariantList &monthDays)
{
    const QDate d(year, month, day);
    const auto it = m_books.find(key);
    if (it == m_books.end() || !d.isValid())
        return;
    Book &b = it.value();
    if (d == b.inFlight)
        b.inFlight = QDate();
    const double base = midnight(d);
    QList<Span> spans;
    for (const QVariant &v : segments) {
        const QVariantMap m = v.toMap();
        // A file that runs over midnight is listed on its last day clipped to 00:00; its real
        // start comes with it, so the day before shows it as footage too.
        double start = base + m.value("start").toDouble();
        if (m.contains("startEpoch"))
            start = qMin(start, m.value("startEpoch").toDouble());
        spans.append({start, base + m.value("end").toDouble()});
    }
    QList<int> ds;
    for (const QVariant &v : monthDays)
        ds << v.toInt();
    b.months.insert(monthId(d), ds);
    storeDay(key, d, spans);
}

void LiveReplay::storeDay(const QString &key, const QDate &d, const QList<Span> &spans)
{
    Book &b = book(key);
    QList<Span> sorted = spans;
    std::sort(sorted.begin(), sorted.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
    b.days.insert(d, sorted);
    if (d == dayOf(m_now))
        b.todayAnswered = m_now;
    advanceScan(key);
    if (d == m_day || d == m_day.addDays(1))
        emit dayChanged();
    emit recordingsChanged();
    retryPending();
    pump(key);
}

void LiveReplay::dayFailed(const QString &key, int year, int month, int day)
{
    const QDate d(year, month, day);
    const auto it = m_books.find(key);
    if (it == m_books.end() || !d.isValid())
        return;
    Book &b = it.value();
    if (d == b.inFlight)
        b.inFlight = QDate();
    // Asked once more; a second failure counts as no recordings that day, so replay moves on
    // (what its month is known to have stays).
    if (++b.failures[d] < 2) {
        b.queue.append(d);
        pump(key);
        return;
    }
    qCWarning(lcUi).noquote() << "live replay: the search for" << d.toString(Qt::ISODate)
                              << "failed twice; taken as no recordings";
    storeDay(key, d, {});
}

// The oldest recording: month by month backwards from today's (each day's search answers its
// month's days) until a month with none; the oldest is then the earliest day of the earliest
// month with any, searched for its first recording.
void LiveReplay::advanceScan(const QString &key)
{
    Book &b = book(key);
    if (b.oldestFound || b.scanMonth == 0)
        return;
    if (b.oldestDay.isValid()) {
        b.oldestFound = b.known(b.oldestDay);
        return;
    }
    const QDate today = dayOf(m_now);
    for (int guard = 0; guard < 120 && b.months.contains(b.scanMonth); ++guard) {
        const QDate first(b.scanMonth / 100, b.scanMonth % 100, 1);
        if (b.months.value(b.scanMonth).isEmpty()) {
            const QDate earliest = first.addMonths(1);
            if (monthId(earliest) > monthId(today) || b.months.value(monthId(earliest)).isEmpty()) {
                // No month with recordings: the oldest known day stands in.
                b.oldestFound = true;
                const double o = oldestOf(b);
                b.oldestDay = o >= 0 ? dayOf(o) : QDate();
                return;
            }
            const QList<int> ds = b.months.value(monthId(earliest));
            b.oldestDay = QDate(earliest.year(), earliest.month(), *std::min_element(ds.begin(), ds.end()));
            b.oldestFound = b.known(b.oldestDay);
            want(key, b.oldestDay, false);
            return;
        }
        b.scanMonth = monthId(first.addMonths(-1));
    }
    if (!b.months.contains(b.scanMonth))
        want(key, QDate(b.scanMonth / 100, b.scanMonth % 100, 1), false);
}

bool LiveReplay::mayHave(const Book &b, const QDate &day) const
{
    if (day > dayOf(m_now))
        return false;
    if (b.oldestFound && b.oldestDay.isValid() && day < b.oldestDay)
        return false;
    const auto it = b.months.constFind(monthId(day));
    return it == b.months.cend() || it.value().contains(day.day());
}

double LiveReplay::previousEnd(const QString &key, double epoch, QDate *day)
{
    const Book &b = book(key);
    // The live buffer's runs are footage too (ticket 08b): an older run before a break.
    double buffered = kNone;
    for (const Span &s : b.buffer)
        if (s.end < epoch - 0.5 && s.end > buffered)
            buffered = s.end;
    QDate d = dayOf(epoch);
    for (int guard = 0; guard < 4000 && d.isValid(); ++guard, d = d.addDays(-1)) {
        const bool bufferedToday = buffered >= 0 && dayOf(buffered) == d;
        if (bufferedToday && !b.known(d)) {   // no need to wait for the day's search
            *day = d;
            return buffered;
        }
        if (b.oldestFound && (!b.oldestDay.isValid() || d < b.oldestDay))
            return kNone;
        if (b.known(d)) {
            double best = bufferedToday ? buffered : kNone;
            for (const Span &s : b.days.value(d))
                if (s.end < epoch - 0.5 && s.end > best)
                    best = s.end;
            if (best >= 0) {
                *day = dayOf(best - 0.001);   // an end at midnight is its own day's
                return best;
            }
        } else if (mayHave(b, d)) {
            want(key, d, true);
            return kPending;
        }
    }
    return kNone;
}

double LiveReplay::nextStart(const QString &key, double epoch, QDate *day)
{
    const Book &b = book(key);
    const QDate today = dayOf(m_now);
    for (QDate d = dayOf(epoch); d.isValid() && d <= today; d = d.addDays(1)) {
        if (b.known(d)) {
            double best = kNone;
            for (const Span &s : b.days.value(d))
                if (s.start > epoch + 0.5 && s.start < m_now && (best < 0 || s.start < best))
                    best = s.start;
            for (const Span &s : b.buffer)   // the live buffer is footage too (ticket 08b)
                if (dayOf(s.start) == d && s.start > epoch + 0.5 && (best < 0 || s.start < best))
                    best = s.start;
            if (best >= 0) {
                *day = dayOf(best);           // a file over midnight starts the day before
                return best;
            }
        } else if (mayHave(b, d)) {
            want(key, d, true);
            return kPending;
        }
    }
    return kNone;
}

void LiveReplay::setBufferSpans(const QString &key, const QVariantList &spans)
{
    const auto it = m_books.find(key);
    if (it == m_books.end())
        return;
    Book &b = it.value();
    QList<Span> next;
    for (const QVariant &v : spans) {
        const QVariantMap m = v.toMap();
        const double start = m.value(QStringLiteral("start")).toDouble();
        const double end = m.value(QStringLiteral("end")).toDouble();
        if (end > start)
            next.append({start, end});
    }
    // The newest end moves every second while live fills the buffer: the segments and the
    // strip are announced when a run comes, goes or starts elsewhere, or an end moves 5 s.
    const bool same = next.size() == b.buffer.size()
        && std::equal(next.begin(), next.end(), b.buffer.begin(), [](const Span &a, const Span &b) {
               return std::abs(a.start - b.start) < 0.5 && std::abs(a.end - b.end) < 5;
           });
    b.buffer = next;
    if (same)
        return;
    emit dayChanged();          // the day's segments
    emit recordingsChanged();   // the strip
}

bool LiveReplay::inBuffer(double epoch) const
{
    return inBufferOf(leadBook(), epoch);
}

bool LiveReplay::inBufferOf(const Book &b, double epoch)
{
    for (const Span &s : b.buffer)
        if (epoch >= s.start && epoch <= s.end)
            return true;
    return false;
}

bool LiveReplay::covered(double epoch) const
{
    return covers(m_lead, epoch);
}

bool LiveReplay::covers(const QString &key, double epoch) const
{
    const auto it = m_books.constFind(key);
    if (it == m_books.cend())
        return false;
    const Book &b = it.value();
    for (const Span &s : spansOn(b, dayOf(epoch)))
        if (epoch >= s.start && epoch <= s.end)
            return true;
    return false;
}

void LiveReplay::playFrom(double epoch, int direction)
{
    if (!replaying())
        return;
    m_lastCarry = -1;
    resolve(epoch, direction, Ahead::NewestFootage);
}

void LiveReplay::skipBy(double secs)
{
    if (!replaying() || secs == 0)
        return;
    m_lastCarry = -1;
    resolve(qMin(m_now, m_playhead + secs), secs > 0 ? 1 : -1, Ahead::Live);
}

void LiveReplay::resolve(double epoch, int direction, Ahead ahead)
{
    m_pending = Pending::None;
    const Book &b = leadBook();   // the lead camera's footage decides (ticket 15a)
    // Backwards from now: the buffer's newest frame (the camera's clock runs up to a second or
    // so off this machine's), then the buffer, without waiting for any search (ticket 08b).
    if (direction < 0)
        for (const Span &s : b.buffer)
            if (epoch > s.end && epoch - s.end < kBufferNowSlackSecs)
                epoch = s.end;
    if (inBuffer(epoch)) {
        land(epoch, dayOf(epoch));
        return;
    }
    const QDate d = dayOf(epoch);
    auto wait = [&] {
        m_pending = Pending::Play;
        m_pendingEpoch = epoch;
        m_pendingDirection = direction;
        m_pendingAhead = ahead;
    };
    if (!b.known(d) && mayHave(b, d)) {
        want(m_lead, d, true);
        wait();
        return;
    }
    if (covered(epoch)) {
        land(epoch, d);
        return;
    }
    QDate at;
    double r = direction > 0 ? nextStart(m_lead, epoch, &at) : previousEnd(m_lead, epoch, &at);
    if (r == kPending) {
        wait();
        return;
    }
    if (r < 0 && direction > 0) {
        if (ahead == Ahead::Live) {
            if (!liveOrRefresh(Pending::Play))
                wait();
            return;
        }
        // Nothing ahead before now: the newest footage's last 10 s.
        r = previousEnd(m_lead, epoch, &at);
        if (r == kPending) {
            wait();
            return;
        }
        for (const Span &s : spansOn(b, at))
            if (r >= 0 && s.end == r)
                r = qMax(s.start, r - 10);
    }
    if (r < 0) {
        emit nothingEarlier();
        return;
    }
    land(r, at);
}

bool LiveReplay::liveOrRefresh(Pending retry)
{
    Book &b = book(m_lead);
    const QDate today = dayOf(m_now);
    if (b.todayAnswered >= 0 && m_now - b.todayAnswered > 20) {
        b.days.remove(today);
        b.todayAnswered = -1;
        m_pending = retry;
        want(m_lead, today, true);
        return false;
    }
    stop();                     // the newest footage: back to live
    return true;
}

void LiveReplay::carryOn(const QString &key, int direction)
{
    // The lead camera's stream running out moves every pane; another's waits for the
    // playhead to reach its footage again (ticket 15a, as Playback).
    if (!replaying() || key != m_lead)
        return;
    const Book &b = leadBook();
    m_pending = Pending::None;
    // Forwards at the live buffer's newest frame: that is now, so back to live (ticket 08b) —
    // not a reopen at each newest second while live keeps filling it.
    if (direction > 0 && !b.buffer.isEmpty()) {
        const Span newest = *std::max_element(b.buffer.begin(), b.buffer.end(),
            [](const Span &a, const Span &b) { return a.end < b.end; });
        if (m_playhead >= newest.start && newest.end - m_playhead < kLiveEdgeSecs) {
            stop();
            return;
        }
    }
    // Footage goes on past where the stream stopped (a file over midnight; a run that stopped
    // at the start of its day): carry straight on from the next second. Not twice at the same
    // place the same way (a run the recorder refused there): then the next recording.
    const double next = direction > 0 ? std::floor(m_playhead) + 1 : std::floor(m_playhead) - 1;
    const bool again = m_lastCarry >= 0 && m_lastCarryDirection == direction
                    && std::abs(next - m_lastCarry) < 2;
    if (!again && covered(next)) {
        land(next, dayOf(next));
        m_lastCarry = m_playhead;
        m_lastCarryDirection = direction;
        return;
    }
    QDate at;
    const double r = direction > 0 ? nextStart(m_lead, m_playhead, &at)
                                   : previousEnd(m_lead, m_playhead, &at);
    if (r == kPending) {
        m_pending = Pending::CarryOn;
        m_pendingDirection = direction;
        return;
    }
    if (r >= 0) {
        land(r, at);
        m_lastCarry = m_playhead;
        m_lastCarryDirection = direction;
        return;
    }
    if (direction > 0) {
        m_pendingDirection = direction;
        liveOrRefresh(Pending::CarryOn);
    } else {
        emit nothingEarlier();
    }
}

void LiveReplay::retryPending()
{
    const Pending p = m_pending;
    m_pending = Pending::None;
    if (p == Pending::Play)
        resolve(m_pendingEpoch, m_pendingDirection, m_pendingAhead);
    else if (p == Pending::CarryOn)
        carryOn(m_lead, m_pendingDirection);
}

void LiveReplay::land(double epoch, const QDate &day)
{
    // A recording that runs to midnight ends on the next day's first second: play it from its
    // own day's last one (a backwards run stops at the start of the day it opens on).
    const double nextMidnight = midnight(day.addDays(1));
    if (epoch >= nextMidnight)
        epoch = nextMidnight - 1;
    m_pending = Pending::None;
    for (Book &b : m_books)   // every pane opens there: in its buffer or its recorder
        b.landedInBuffer = inBufferOf(b, epoch);
    opened(m_lead);
    setPlayhead(epoch, day);
    emit playWanted(epoch);
}

void LiveReplay::setPlayhead(double epoch, const QDate &day)
{
    if (epoch != m_playhead) {
        m_playhead = epoch;
        emit playheadChanged();
        emit behindChanged();
    }
    if (day != m_day) {
        m_day = day;
        // Every replaying camera learns its footage on the playhead's day (its pane shows it
        // or "No recording here"); the lead's own search is asked for where it plays.
        for (const QString &k : std::as_const(m_keys))
            if (k != m_lead)
                want(k, day, true);
        emit dayChanged();
    }
}

void LiveReplay::frameAt(const QString &key, double epoch, double wallMs)
{
    const auto it = m_books.find(key);
    if (!replaying() || it == m_books.end() || epoch <= 0)
        return;
    // Forwards in the recorder's footage, reaching this camera's live buffer: its pane plays
    // on from the buffer (ticket 08b), not on to the recorder's stall at its newest footage —
    // that pane only (ticket 15a).
    Book &b = it.value();
    if (m_group->direction() > 0 && !b.landedInBuffer && inBufferOf(b, epoch)) {
        b.landedInBuffer = true;
        qCInfo(lcUi).noquote() << "live replay" << key << "into its live buffer";
        if (key == m_lead) {
            opened(m_lead);
            setPlayhead(epoch, dayOf(epoch));
        }
        emit paneWanted(key, epoch);
        return;
    }
    if (key != frameDriver())
        return;
    if (epoch != m_lastFrame) {
        m_lastFrame = epoch;
        m_lastMoveWall = wallMs;
    }
    // A frame at exactly midnight going backwards still belongs to the day before.
    const QDate d = m_group->direction() < 0 && epoch == midnight(dayOf(epoch)) ? dayOf(epoch - 1)
                                                                                 : dayOf(epoch);
    setPlayhead(epoch, d);
}

void LiveReplay::opened(const QString &key)
{
    if (key != m_lead)
        return;
    m_lastFrame = -1;
    m_lastMoveWall = -1;
}

bool LiveReplay::stalledAtEdge(double wallMs) const
{
    return replaying() && !m_paused && m_group->direction() > 0 && m_lastMoveWall >= 0
        && wallMs - m_lastMoveWall >= 8000 && m_now - m_playhead < 300;
}

double LiveReplay::posForBehind(double behindSecs, double oldestBehindSecs)
{
    if (oldestBehindSecs <= 0)
        return 0;
    const QList<double> marks = marksTo(oldestBehindSecs);
    const int n = int(marks.size()) - 1;
    const double b = qBound(0.0, behindSecs, oldestBehindSecs);
    for (int i = 0; i < n; ++i)
        if (b <= marks[i + 1])
            return (i + (b - marks[i]) / (marks[i + 1] - marks[i])) / n;
    return 1.0;
}

double LiveReplay::behindForPos(double pos, double oldestBehindSecs)
{
    if (oldestBehindSecs <= 0)
        return 0;
    const QList<double> marks = marksTo(oldestBehindSecs);
    const int n = int(marks.size()) - 1;
    const double x = qBound(0.0, pos, 1.0) * n;
    const int i = qMin(int(std::floor(x)), n - 1);
    return marks[i] + (x - i) * (marks[i + 1] - marks[i]);
}

QVariantList LiveReplay::sliderMarks()
{
    return {0.0, 60.0, 600.0, 3600.0, 86400.0, 604800.0};
}

QString LiveReplay::behindText(double secs)
{
    const qint64 s = qMax<qint64>(0, qRound64(secs));
    if (s < 60)
        return tr("%1 s").arg(s);
    if (s < 3600)
        return tr("%1 min %2 s").arg(s / 60).arg(s % 60);
    if (s < 86400)
        return tr("%1 h %2 min").arg(s / 3600).arg(s % 3600 / 60);
    return tr("%1 d %2 h").arg(s / 86400).arg(s % 86400 / 3600);
}

double LiveReplay::posFor(double epoch) const
{
    return posForBehind(m_now - epoch, oldestBehind());
}

double LiveReplay::epochForPos(double pos) const
{
    return m_now - behindForPos(pos, oldestBehind());
}

} // namespace rl
