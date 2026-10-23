#pragma once

#include "device/FrigateApi.h"
#include "protocol/ReolinkApi.h"

#include <QHash>
#include <QString>
#include <QVector>

// What a host's detections look like to the rest of the app, however they arrived.
// Four sources feed one host: the Reolink poll (GetMdState/GetAiState), Baichuan push,
// Frigate's events poll and Frigate's websocket. This module owns the rules that make
// them one stream: edge detection, push taking over from the poll, Frigate's watermark
// and its burst gate. DeviceManager owns the sockets and timers and hands each message
// here; what comes back is a list of Detection to emit, nothing more.
namespace rl::device {

struct Detection
{
    int channel = 0;
    QString type;   // "motion" | "person" | "vehicle" | "pet" | "visitor"
    QString camera; // as the source named it (Frigate: the raw config key)
};

// Frigate opens a new event for each object it tracks, so one person crossing the yard
// can be several. One detection per camera and type per burst (frigate::startsNewBurst).
class BurstGate
{
public:
    // True (and remembered) when `start` begins a new burst for this camera and type.
    bool admit(const QString &camera, const QString &type, double start);

private:
    QHash<QString, double> m_last;
};

// An existing history row (a DB event) already covers a moment this close to it, so the
// catch-up must not file it again. Same window as the burst gap.
bool sameBurstWindow(qint64 existingTimestamp, qint64 candidateTimestamp);

// Where the next Frigate poll starts, captured when the poll is sent.
struct FrigatePoll
{
    bool initialized = false; // false until the first poll has set a watermark
    double after = 0.0;       // newest event start seen (server clock)
};

class HostDetections
{
public:
    // ---- Reolink ----
    // Baichuan push has subscribed (or dropped). While it is live, the poll stops
    // reporting detections for the host (it keeps checking connectivity).
    void setPushLive(bool on) { m_pushLive = on; }
    bool pushLive() const { return m_pushLive; }
    // Whether the poll should ask for motion/AI state. A multi-channel host covered by
    // push asks only for channel status; a single camera's poll is its health check and
    // still asks.
    bool pollQueriesStates(int channelCount) const { return !(m_pushLive && channelCount > 1); }

    // A pushed state for one channel.
    QVector<Detection> onPush(int channel, api::DetectionState state, const QString &camera);
    // A polled state for one channel. `pushLiveWhenSent` is the push flag when the poll
    // went out: a poll sent while push was live, or answered while it is, is ignored.
    // A press of the doorbell is invisible to the poll, so the last pushed `visitor` is
    // carried over.
    QVector<Detection> onPoll(int channel, api::DetectionState state, const QString &camera,
                              bool pushLiveWhenSent);

    // ---- Frigate ----
    FrigatePoll frigatePlan() const { return {m_frigateInitialized, m_frigateWatermark}; }
    // The reply to the poll sent with `plan`. The first poll raises nothing: it only sets
    // the watermark (the newest event, else `nowSecs`; or `nowSecs - backfillSecs` when a
    // backfill is asked for, for testing). Later polls raise, oldest first, each event
    // after the watermark that is a modelled label on one of `channelOf`'s cameras and
    // starts a new burst; the watermark then moves to the newest start and never back.
    QVector<Detection> onFrigatePoll(const FrigatePoll &plan,
                                     const QVector<frigate::Event> &events,
                                     const QHash<QString, int> &channelOf, qint64 nowSecs,
                                     int backfillSecs);
    // An event from the websocket. Same label, camera and burst rules as the poll, so an
    // event seen on both is raised once.
    QVector<Detection> onFrigateLive(const QString &camera, const QString &label, double start,
                                     const QHash<QString, int> &channelOf);

private:
    QVector<Detection> edges(int channel, const api::DetectionState &now,
                             const QString &camera);
    bool raiseBurst(const QString &camera, const QString &type, double start);

    bool m_pushLive = false;
    QHash<int, api::DetectionState> m_last; // per channel, for 0->1 edges
    bool m_frigateInitialized = false;
    double m_frigateWatermark = 0.0;
    BurstGate m_burst;
};

} // namespace rl::device
