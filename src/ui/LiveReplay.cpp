#include "ui/LiveReplay.h"

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

QList<LiveReplay::Span> LiveReplay::spansOn(const QDate &day) const
{
    // The day's own, and the next day's that started on it (a file over midnight).
    const double from = midnight(day), to = midnight(day.addDays(1));
    QList<Span> out = m_days.value(day);
    for (const Span &s : m_days.value(day.addDays(1)))
        if (s.start < to)
            out.append({s.start, qMin(s.end, to)});
    for (const Span &s : m_buffer)   // the live buffer counts as footage (ticket 08b)
        if (s.end >= from && s.start < to)
            out.append({s.start, qMin(s.end, to)});
    for (Span &s : out)
        s.start = qMax(s.start, from);
    std::sort(out.begin(), out.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
    return out;
}

QVariantList LiveReplay::daySegments() const
{
    QVariantList out;
    if (!m_day.isValid())
        return out;
    const double base = dayEpoch();
    for (const Span &s : spansOn(m_day)) {
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
    if (m_oldestFound && m_oldestDay.isValid() && !m_days.value(m_oldestDay).isEmpty())
        return m_days.value(m_oldestDay).first().start;
    double best = kNone;
    for (auto it = m_days.cbegin(); it != m_days.cend(); ++it)
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
    QList<Span> all = m_buffer;
    for (auto it = m_days.cbegin(); it != m_days.cend(); ++it)
        all += it.value();
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
    m_days.clear();
    m_buffer.clear();
    m_landedInBuffer = false;
    m_months.clear();
    m_failures.clear();
    m_queue.clear();
    m_inFlight = QDate();
    m_oldestFound = false;
    m_oldestDay = QDate();
    m_scanMonth = 0;
    m_pending = Pending::None;
    m_todayAnswered = -1;
    m_lastCarry = -1;
    m_lastFrame = -1;
    m_lastMoveWall = -1;
    setPaused(false);
    m_group->playForward();
}

void LiveReplay::start(const QString &cameraKey)
{
    if (cameraKey.isEmpty() || cameraKey == m_key)
        return;
    if (m_key.isEmpty())
        reset();
    else
        stop();   // another pane was replaying: it goes back to live first (stop resets)
    m_key = cameraKey;
    const QDate today = dayOf(m_now);
    m_scanMonth = monthId(today);
    setPlayhead(m_now, today);
    qCInfo(lcUi).noquote() << "live replay" << m_key << "from now";
    emit replayingChanged();
    emit recordingsChanged();
    want(today, true);
}

void LiveReplay::stop()
{
    if (m_key.isEmpty())
        return;
    qCInfo(lcUi).noquote() << "live replay" << m_key << "back to live";
    m_key.clear();
    // The pane lets its recording go before the group turns back to forwards (reset): a turn
    // while it still replays would open a forward recorder stream as it closes (ticket 08d).
    emit replayingChanged();
    reset();
    emit recordingsChanged();
}

void LiveReplay::want(const QDate &day, bool urgent)
{
    if (!day.isValid() || known(day) || day == m_inFlight)
        return;
    const qsizetype at = m_queue.indexOf(day);
    if (at >= 0) {
        if (urgent && at > 0)
            m_queue.move(at, 0);
        return;
    }
    if (urgent)
        m_queue.prepend(day);
    else
        m_queue.append(day);
    pump();
}

void LiveReplay::pump()
{
    if (m_inFlight.isValid() || m_queue.isEmpty() || !replaying())
        return;
    m_inFlight = m_queue.takeFirst();
    emit searchWanted(m_inFlight.year(), m_inFlight.month(), m_inFlight.day());
}

void LiveReplay::dayFound(int year, int month, int day, const QVariantList &segments,
                          const QVariantList &monthDays)
{
    const QDate d(year, month, day);
    if (!replaying() || !d.isValid())
        return;
    if (d == m_inFlight)
        m_inFlight = QDate();
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
    m_months.insert(monthId(d), ds);
    storeDay(d, spans);
}

void LiveReplay::storeDay(const QDate &d, const QList<Span> &spans)
{
    QList<Span> sorted = spans;
    std::sort(sorted.begin(), sorted.end(), [](const Span &a, const Span &b) { return a.start < b.start; });
    m_days.insert(d, sorted);
    if (d == dayOf(m_now))
        m_todayAnswered = m_now;
    advanceScan();
    if (d == m_day || d == m_day.addDays(1))
        emit dayChanged();
    emit recordingsChanged();
    retryPending();
    pump();
}

void LiveReplay::dayFailed(int year, int month, int day)
{
    const QDate d(year, month, day);
    if (!replaying() || !d.isValid())
        return;
    if (d == m_inFlight)
        m_inFlight = QDate();
    // Asked once more; a second failure counts as no recordings that day, so replay moves on
    // (what its month is known to have stays).
    if (++m_failures[d] < 2) {
        m_queue.append(d);
        pump();
        return;
    }
    qCWarning(lcUi).noquote() << "live replay: the search for" << d.toString(Qt::ISODate)
                              << "failed twice; taken as no recordings";
    storeDay(d, {});
}

// The oldest recording: month by month backwards from today's (each day's search answers its
// month's days) until a month with none; the oldest is then the earliest day of the earliest
// month with any, searched for its first recording.
void LiveReplay::advanceScan()
{
    if (m_oldestFound || m_scanMonth == 0)
        return;
    if (m_oldestDay.isValid()) {
        m_oldestFound = known(m_oldestDay);
        return;
    }
    const QDate today = dayOf(m_now);
    for (int guard = 0; guard < 120 && m_months.contains(m_scanMonth); ++guard) {
        const QDate first(m_scanMonth / 100, m_scanMonth % 100, 1);
        if (m_months.value(m_scanMonth).isEmpty()) {
            const QDate earliest = first.addMonths(1);
            if (monthId(earliest) > monthId(today) || m_months.value(monthId(earliest)).isEmpty()) {
                // No month with recordings: the oldest known day stands in.
                m_oldestFound = true;
                const double o = oldest();
                m_oldestDay = o >= 0 ? dayOf(o) : QDate();
                return;
            }
            const QList<int> ds = m_months.value(monthId(earliest));
            m_oldestDay = QDate(earliest.year(), earliest.month(), *std::min_element(ds.begin(), ds.end()));
            m_oldestFound = known(m_oldestDay);
            want(m_oldestDay, false);
            return;
        }
        m_scanMonth = monthId(first.addMonths(-1));
    }
    if (!m_months.contains(m_scanMonth))
        want(QDate(m_scanMonth / 100, m_scanMonth % 100, 1), false);
}

bool LiveReplay::mayHave(const QDate &day) const
{
    if (day > dayOf(m_now))
        return false;
    if (m_oldestFound && m_oldestDay.isValid() && day < m_oldestDay)
        return false;
    const auto it = m_months.constFind(monthId(day));
    return it == m_months.cend() || it.value().contains(day.day());
}

double LiveReplay::previousEnd(double epoch, QDate *day)
{
    // The live buffer's runs are footage too (ticket 08b): an older run before a break.
    double buffered = kNone;
    for (const Span &s : m_buffer)
        if (s.end < epoch - 0.5 && s.end > buffered)
            buffered = s.end;
    QDate d = dayOf(epoch);
    for (int guard = 0; guard < 4000 && d.isValid(); ++guard, d = d.addDays(-1)) {
        const bool bufferedToday = buffered >= 0 && dayOf(buffered) == d;
        if (bufferedToday && !known(d)) {   // no need to wait for the day's search
            *day = d;
            return buffered;
        }
        if (m_oldestFound && (!m_oldestDay.isValid() || d < m_oldestDay))
            return kNone;
        if (known(d)) {
            double best = bufferedToday ? buffered : kNone;
            for (const Span &s : m_days.value(d))
                if (s.end < epoch - 0.5 && s.end > best)
                    best = s.end;
            if (best >= 0) {
                *day = dayOf(best - 0.001);   // an end at midnight is its own day's
                return best;
            }
        } else if (mayHave(d)) {
            want(d, true);
            return kPending;
        }
    }
    return kNone;
}

double LiveReplay::nextStart(double epoch, QDate *day)
{
    const QDate today = dayOf(m_now);
    for (QDate d = dayOf(epoch); d.isValid() && d <= today; d = d.addDays(1)) {
        if (known(d)) {
            double best = kNone;
            for (const Span &s : m_days.value(d))
                if (s.start > epoch + 0.5 && s.start < m_now && (best < 0 || s.start < best))
                    best = s.start;
            for (const Span &s : m_buffer)   // the live buffer is footage too (ticket 08b)
                if (dayOf(s.start) == d && s.start > epoch + 0.5 && (best < 0 || s.start < best))
                    best = s.start;
            if (best >= 0) {
                *day = dayOf(best);           // a file over midnight starts the day before
                return best;
            }
        } else if (mayHave(d)) {
            want(d, true);
            return kPending;
        }
    }
    return kNone;
}

void LiveReplay::setBufferSpans(const QVariantList &spans)
{
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
    const bool same = next.size() == m_buffer.size()
        && std::equal(next.begin(), next.end(), m_buffer.begin(), [](const Span &a, const Span &b) {
               return std::abs(a.start - b.start) < 0.5 && std::abs(a.end - b.end) < 5;
           });
    m_buffer = next;
    if (same)
        return;
    emit dayChanged();          // the day's segments
    emit recordingsChanged();   // the strip
}

bool LiveReplay::inBuffer(double epoch) const
{
    for (const Span &s : m_buffer)
        if (epoch >= s.start && epoch <= s.end)
            return true;
    return false;
}

bool LiveReplay::covered(double epoch) const
{
    for (const Span &s : spansOn(dayOf(epoch)))
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
    // Backwards from now: the buffer's newest frame (the camera's clock runs up to a second or
    // so off this machine's), then the buffer, without waiting for any search (ticket 08b).
    if (direction < 0)
        for (const Span &s : m_buffer)
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
    if (!known(d) && mayHave(d)) {
        want(d, true);
        wait();
        return;
    }
    if (covered(epoch)) {
        land(epoch, d);
        return;
    }
    QDate at;
    double r = direction > 0 ? nextStart(epoch, &at) : previousEnd(epoch, &at);
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
        r = previousEnd(epoch, &at);
        if (r == kPending) {
            wait();
            return;
        }
        for (const Span &s : spansOn(at))
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
    const QDate today = dayOf(m_now);
    if (m_todayAnswered >= 0 && m_now - m_todayAnswered > 20) {
        m_days.remove(today);
        m_todayAnswered = -1;
        m_pending = retry;
        want(today, true);
        return false;
    }
    stop();                     // the newest footage: back to live
    return true;
}

void LiveReplay::carryOn(int direction)
{
    if (!replaying())
        return;
    m_pending = Pending::None;
    // Forwards at the live buffer's newest frame: that is now, so back to live (ticket 08b) —
    // not a reopen at each newest second while live keeps filling it.
    if (direction > 0 && !m_buffer.isEmpty()) {
        const Span newest = *std::max_element(m_buffer.begin(), m_buffer.end(),
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
    const double r = direction > 0 ? nextStart(m_playhead, &at) : previousEnd(m_playhead, &at);
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
        carryOn(m_pendingDirection);
}

void LiveReplay::land(double epoch, const QDate &day)
{
    // A recording that runs to midnight ends on the next day's first second: play it from its
    // own day's last one (a backwards run stops at the start of the day it opens on).
    const double nextMidnight = midnight(day.addDays(1));
    if (epoch >= nextMidnight)
        epoch = nextMidnight - 1;
    m_pending = Pending::None;
    m_landedInBuffer = inBuffer(epoch);
    opened();
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
        emit dayChanged();
    }
}

void LiveReplay::frameAt(double epoch, double wallMs)
{
    if (!replaying() || epoch <= 0)
        return;
    // Forwards in the recorder's footage, reaching the live buffer: play on from the buffer
    // (ticket 08b), not on to the recorder's stall at its newest footage.
    if (m_group->direction() > 0 && !m_landedInBuffer && inBuffer(epoch)) {
        land(epoch, dayOf(epoch));
        return;
    }
    if (epoch != m_lastFrame) {
        m_lastFrame = epoch;
        m_lastMoveWall = wallMs;
    }
    // A frame at exactly midnight going backwards still belongs to the day before.
    const QDate d = m_group->direction() < 0 && epoch == midnight(dayOf(epoch)) ? dayOf(epoch - 1)
                                                                                 : dayOf(epoch);
    setPlayhead(epoch, d);
}

void LiveReplay::opened()
{
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
