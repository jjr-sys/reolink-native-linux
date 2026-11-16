#include "Detections.h"

namespace rl::device {

bool BurstGate::admit(const QString &camera, const QString &type, double start)
{
    double &last = m_last[camera + u'/' + type];
    if (!frigate::startsNewBurst(last, start))
        return false;
    last = start;
    return true;
}

bool sameBurstWindow(qint64 existingTimestamp, qint64 candidateTimestamp)
{
    return qAbs(existingTimestamp - candidateTimestamp) < qint64(frigate::kBurstGapSecs);
}

QVector<Detection> HostDetections::edges(int channel, const api::DetectionState &now,
                                         const QString &camera)
{
    QVector<Detection> out;
    for (const QString &type : api::risingDetections(m_last.value(channel), now))
        out.append({channel, type, camera});
    m_last[channel] = now;
    return out;
}

QVector<Detection> HostDetections::onPush(int channel, api::DetectionState state,
                                          const QString &camera)
{
    return edges(channel, state, camera);
}

QVector<Detection> HostDetections::onPoll(int channel, api::DetectionState state,
                                          const QString &camera, bool pushLiveWhenSent)
{
    if (pushLiveWhenSent || m_pushLive)
        return {}; // push took over (or still holds) this host's detections
    state.visitor = m_last.value(channel).visitor;
    return edges(channel, state, camera);
}

bool HostDetections::raiseBurst(const QString &camera, const QString &type, double start)
{
    return m_burst.admit(camera, type, start);
}

QVector<Detection> HostDetections::onFrigatePoll(const FrigatePoll &plan,
                                                 const QVector<frigate::Event> &events,
                                                 const QHash<QString, int> &channelOf,
                                                 qint64 nowSecs, int backfillSecs)
{
    if (!plan.initialized) {
        // Newest event's start; with none yet, the server's clock is ours.
        m_frigateWatermark = frigate::newWatermark(events.isEmpty() ? double(nowSecs) : 0.0, events);
        if (backfillSecs > 0)
            m_frigateWatermark = double(nowSecs - backfillSecs);
        m_frigateInitialized = true;
        return {};
    }
    QVector<Detection> out;
    // Newest first from Frigate; raise oldest first.
    for (int i = events.size() - 1; i >= 0; --i) {
        const frigate::Event &ev = events.at(i);
        const QString type = frigate::typeForLabel(ev.label);
        if (ev.start <= plan.after || type.isEmpty() || !channelOf.contains(ev.camera))
            continue;
        if (raiseBurst(ev.camera, type, ev.start))
            out.append({channelOf.value(ev.camera), type, ev.camera, qint64(ev.start)});
    }
    m_frigateWatermark = frigate::newWatermark(plan.after, events);
    return out;
}

QVector<Detection> HostDetections::onFrigateLive(const QString &camera, const QString &label,
                                                 double start,
                                                 const QHash<QString, int> &channelOf)
{
    const QString type = frigate::typeForLabel(label);
    if (type.isEmpty() || !channelOf.contains(camera))
        return {};
    if (!raiseBurst(camera, type, start))
        return {};
    return {{channelOf.value(camera), type, camera, qint64(start)}};
}

} // namespace rl::device
