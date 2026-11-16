#pragma once

#include "core/Database.h"
#include "device/EventHistory.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

namespace rl {

class EventSwitches;

// The one way an event reaches the store (wayfinder/architecture ticket 03). Every source
// (live detections, a recorder's own history, connectivity) hands it a candidate and gets
// back what became of it: stored, or why not, and whether to announce it. It owns the
// rules a filing obeys: the switch-offs (EventSwitches::isOff, read here), how long events
// are kept, a history batch's burst gate, the same-event check against what's stored, and
// the per-recorder cap. The list model (EventManager) shows the rows; pictures and
// desktop notices stay with their callers.
class Inbox : public QObject
{
    Q_OBJECT

public:
    enum class Source {
        Live,      // seen now (a detection; `at` is the source's time when it has one)
        History,   // a recorder's own record, caught up later: never announced
        Status,    // a connectivity change (StatusRows): can't be switched off
    };
    enum class Verdict {
        Stored,
        SwitchedOff,   // the pair is switched off: not stored, pictured or counted
        TooOld,        // older than the store keeps its type
        SameEvent,     // a stored row already covers it (its burst window)
        SameBurst,     // a history batch already filed one of this burst
        NotStored,     // the store refused it
    };

    struct Candidate {
        qint64 hostId = -1;
        int channel = 0;
        QString type;     // as the source calls it ("dog_cat" is stored as "pet")
        QString camera;   // the name to file under
        double at = 0;    // when the source saw it, epoch seconds; 0 = now
    };

    struct Filing {
        Verdict verdict = Verdict::NotStored;
        // The stored row; for SameEvent the row that covers it.
        EventRecord row;
        // Count it unread and tell the person (a live event or a status change, stored).
        bool announce = false;
        // A row without a picture yet: the new one, or a SameEvent row whose picture never
        // came. The caller fetches one (live: a camera frame; Frigate history: its snapshot).
        bool needsPicture = false;
    };

    // `now` gives the clock (epoch seconds); `capPerRecorder` is the safety-net cap.
    Inbox(Database *db, EventSwitches *switches, std::function<qint64()> now = {},
          int capPerRecorder = device::kCapPerRecorder, QObject *parent = nullptr);

    Filing file(const Candidate &c, Source source);
    // A history batch, in any order: filed oldest first, one per camera, type and burst.
    // The filings come back in the batch's order (one per candidate).
    QVector<Filing> fileHistory(const QVector<Candidate> &batch);
    // A connectivity change: one Status row per camera named (device::statusRows). Announced
    // once, by the first filing (also when no row was written: a host whose cameras were
    // all in that state already).
    QVector<Filing> fileStatus(qint64 hostId, int channel, const QString &name, bool online,
                               const QVector<device::CameraRef> &changed);

    // The age purge (motion 24 h, the rest 30 days), with the removed rows' pictures.
    void enforceRetention();
    // Rows went another way (a clear, a switch-off): count the recorder (-1: every one)
    // afresh for the cap.
    void recount(qint64 hostId = -1);
    // Deletes removed rows' picture files.
    static void dropPictures(const QStringList &paths);

signals:
    // Stored rows went: a retention purge or a cap trim. The list reloads.
    void trimmed();

private:
    qint64 now() const;
    Filing store(EventRecord rec, bool announce);
    void enforceCap(qint64 hostId, const QString &type);

    Database *m_db;
    EventSwitches *m_switches;
    std::function<qint64()> m_now;
    int m_cap;
    QHash<qint64, int> m_cappedCount;   // per recorder, counted on its first event
};

} // namespace rl
