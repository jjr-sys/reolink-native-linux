#include "device/FrigateHistoryPlan.h"

namespace rl::device {

bool fetchPictureFor(const Inbox::Filing &filed, const QSet<qint64> &snapshotsDue)
{
    return filed.needsThumbnail && !snapshotsDue.contains(filed.row.id);
}

FrigateHistoryPlan::FrigateHistoryPlan(qint64 hostId, const QVector<frigate::Event> &events,
                                       const QHash<QString, int> &channelOf)
{
    for (const frigate::Event &ev : events) {
        const QString type = frigate::typeForLabel(ev.label);
        if (type.isEmpty() || !channelOf.contains(ev.camera))
            continue;
        m_batch.append({hostId, channelOf.value(ev.camera), type,
                        frigate::displayCameraName(ev.camera), ev.start});
        m_frigateIds.append(ev.id);
    }
}

QVector<SnapshotFetch> FrigateHistoryPlan::snapshots(const QVector<Inbox::Filing> &filed,
                                                     const QSet<qint64> &snapshotsDue) const
{
    Q_ASSERT(filed.size() == m_frigateIds.size());
    QVector<SnapshotFetch> out;
    for (int i = 0; i < filed.size(); ++i) {
        if (fetchPictureFor(filed.at(i), snapshotsDue))
            out.append({filed.at(i).row.id, m_frigateIds.at(i)});
    }
    return out;
}

} // namespace rl::device
