#include "device/Inbox.h"

#include "device/Detections.h"
#include "device/EventSwitches.h"
#include "device/FrigateApi.h"

#include <QDateTime>
#include <QFile>

#include <algorithm>
#include <numeric>

namespace rl {

Inbox::Inbox(Database *db, EventSwitches *switches, std::function<qint64()> now,
             int capPerRecorder, QObject *parent)
    : QObject(parent), m_db(db), m_switches(switches), m_now(std::move(now)), m_cap(capPerRecorder)
{
}

qint64 Inbox::now() const
{
    return m_now ? m_now() : QDateTime::currentSecsSinceEpoch();
}

Inbox::Filing Inbox::file(const Candidate &c, Source source)
{
    Filing f;
    if (m_switches && m_switches->isOff(c.hostId, c.channel, c.type)) {
        f.verdict = Verdict::SwitchedOff;
        return f;
    }
    // When the source saw it, never ahead of our clock (a server clock that runs fast).
    const qint64 clock = now();
    const qint64 at = c.at > 0 ? qMin(qint64(c.at), clock) : clock;
    const bool motion = EventSwitches::storedType(c.type) == QLatin1String("motion");
    if (at < clock - (motion ? device::kMotionKeepSecs : device::kOtherKeepSecs)) {
        f.verdict = Verdict::TooOld;
        return f;
    }
    // At the source's own time a stored row may already be this event: history rerun, or
    // a live report of what history filed (ticket 02). Detections at "now" (Reolink) have
    // no history to meet and file as they come.
    if (source == Source::History || c.at > 0) {
        const auto near = m_db->eventNear(c.hostId, c.channel, c.type, at,
                                          qint64(frigate::kBurstGapSecs));
        if (near && device::sameBurstWindow(near->timestamp, at)) {
            f.verdict = Verdict::SameEvent;
            f.row = *near;
            f.needsPicture = near->thumbnail.isEmpty();
            return f;
        }
    }
    EventRecord rec;
    rec.hostId = c.hostId;
    rec.channel = c.channel;
    rec.timestamp = at;
    rec.type = c.type;
    rec.camera = c.camera;
    f = store(rec, source == Source::Live);
    f.needsPicture = f.verdict == Verdict::Stored;
    return f;
}

QVector<Inbox::Filing> Inbox::fileHistory(const QVector<Candidate> &batch)
{
    // Oldest first, so the burst gate sees them in the order they happened.
    QVector<int> order(batch.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&batch](int a, int b) { return batch.at(a).at < batch.at(b).at; });
    device::BurstGate burst;   // the live poller's rule: a lingering person is one event
    QVector<Filing> out(batch.size());
    for (int i : order) {
        const Candidate &c = batch.at(i);
        if (m_switches && m_switches->isOff(c.hostId, c.channel, c.type)) {
            out[i].verdict = Verdict::SwitchedOff;
            continue;
        }
        const QString camera = QString::number(c.hostId) + u':' + QString::number(c.channel);
        if (!burst.admit(camera, c.type, c.at)) {
            out[i].verdict = Verdict::SameBurst;
            continue;
        }
        out[i] = file(c, Source::History);
    }
    return out;
}

QVector<Inbox::Filing> Inbox::fileStatus(qint64 hostId, int channel, const QString &name,
                                         bool online, const QVector<device::CameraRef> &changed)
{
    // A dead camera can't announce itself, so the client does: once per change.
    QVector<Filing> out;
    for (const EventRecord &rec : device::statusRows(hostId, channel, name, online, changed, now()))
        out.append(store(rec, out.isEmpty()));
    if (out.isEmpty()) {
        Filing f;
        f.announce = true;
        out.append(f);
    }
    return out;
}

Inbox::Filing Inbox::store(EventRecord rec, bool announce)
{
    Filing f;
    rec.id = m_db->addEvent(rec);
    f.row = rec;
    if (rec.id <= 0) {
        f.verdict = Verdict::NotStored;   // not stored: nothing to picture or count
        f.announce = announce && EventSwitches::isStatus(rec.type);
        return f;
    }
    f.verdict = Verdict::Stored;
    f.announce = announce;
    enforceCap(rec.hostId, rec.type);
    return f;
}

void Inbox::enforceCap(qint64 hostId, const QString &type)
{
    if (EventSwitches::storedType(type) == QLatin1String("motion"))
        return;
    auto it = m_cappedCount.find(hostId);
    if (it == m_cappedCount.end())
        it = m_cappedCount.insert(hostId, m_db->countCappedEvents(hostId));   // includes this one
    else
        ++it.value();
    if (it.value() <= m_cap)
        return;
    const QStringList pictures = m_db->trimHostEvents(hostId, m_cap);
    it.value() = m_db->countCappedEvents(hostId);
    dropPictures(pictures);
    emit trimmed();
}

void Inbox::enforceRetention()
{
    const QStringList pictures = m_db->purgeExpiredEvents(now(), device::kMotionKeepSecs,
                                                          device::kOtherKeepSecs);
    m_cappedCount.clear();   // recounted on each recorder's next event
    dropPictures(pictures);
    emit trimmed();
}

void Inbox::recount(qint64 hostId)
{
    if (hostId < 0)
        m_cappedCount.clear();
    else
        m_cappedCount.remove(hostId);
}

void Inbox::dropPictures(const QStringList &paths)
{
    for (const QString &path : paths)
        QFile::remove(path);
}

} // namespace rl
