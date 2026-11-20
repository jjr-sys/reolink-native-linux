#pragma once

#include "device/FrigateApi.h"
#include "device/Inbox.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace rl::device {

// Whether a filed row should have its picture fetched now: it has none yet (the Inbox says
// so), and Frigate's own snapshot for it isn't already on its way (`snapshotsDue`, so a
// camera frame can't overwrite the right picture, nor a rerun fetch it twice). A history
// sync and a live report both ask this.
bool fetchPictureFor(const Inbox::Filing &filed, const QSet<qint64> &snapshotsDue);

// One Frigate snapshot to fetch: the stored row's id and Frigate's own event id.
struct SnapshotFetch
{
    qint64 rowId = -1;
    QString frigateEventId;
    friend bool operator==(const SnapshotFetch &, const SnapshotFetch &) = default;
};

// A Frigate host's event history (its /api/events reply), planned for the Inbox and the
// pictures. Pure: the caller files batch() through Inbox::fileHistory, then fetches what
// snapshots() names and marks those rows due until each lands.
class FrigateHistoryPlan
{
public:
    // `channelOf`: the host's cameras by Frigate's raw camera name. Events on another camera,
    // or with a label the app doesn't model, are left out.
    FrigateHistoryPlan(qint64 hostId, const QVector<frigate::Event> &events,
                       const QHash<QString, int> &channelOf);

    // What to file, in the events' order.
    const QVector<Inbox::Candidate> &batch() const { return m_batch; }
    // After filing batch() (`filed`: one filing per candidate, in batch order): the rows
    // whose picture is Frigate's snapshot, as fetchPictureFor decides.
    QVector<SnapshotFetch> snapshots(const QVector<Inbox::Filing> &filed,
                                     const QSet<qint64> &snapshotsDue) const;

private:
    QVector<Inbox::Candidate> m_batch;
    QVector<QString> m_frigateIds;   // beside each candidate: Frigate's own event id
};

} // namespace rl::device
