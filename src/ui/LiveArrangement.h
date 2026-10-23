#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace rl {

// The Live grid's arrangement for QML: which camera sits in which pane, which page of the
// grid is showing, which camera is maximized or selected, and the saved layout. Cameras are
// identified by their key, "hostId:channel", never by a model row, so a device being added,
// removed or reordered cannot point anything at the wrong camera. QML renders `slots`,
// `pageKeys` and the focus keys, and only reports what the user did.
//
//  - Give it the cameras that exist now (setCameras) whenever the device model changes; it
//    repairs the arrangement itself: cameras gone are dropped, duplicates removed, gaps
//    filled with cameras not yet placed, focus on a vanished camera cleared.
//  - Give it the stored text first (setSaved). The first non-empty camera list restores it.
//    A saved camera that has no match yet (its host answers late) keeps its pane waiting
//    and nothing is written meanwhile; it gets its pane when it arrives, or the wait is
//    given up after giveUpMs and the stand-ins are saved.
//  - saveRequested(text) says the arrangement changed after restoring and is worth storing.
//
// Stored text: one entry per pane joined by ",": "hostId:channel", or "-" for an empty pane.
// This is the format the QML page always wrote; shorter values and unknown or repeated
// entries load fine.
class LiveArrangement : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString saved READ saved WRITE setSaved)
    Q_PROPERTY(QStringList cameras READ cameras WRITE setCameras NOTIFY camerasChanged)
    // Camera key per pane, kMaxPanes long, "" = empty pane.
    Q_PROPERTY(QStringList paneKeys READ paneKeys NOTIFY slotsChanged)
    Q_PROPERTY(QString serialized READ serialized NOTIFY slotsChanged)
    Q_PROPERTY(bool restored READ restored NOTIFY restoredChanged)
    Q_PROPERTY(bool hasPending READ hasPending NOTIFY pendingChanged)
    // Paging: the grid shows `preset` panes of the arrangement at a time.
    Q_PROPERTY(int preset READ preset WRITE setPreset NOTIFY presetChanged)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pagingChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pagingChanged)
    Q_PROPERTY(int curPage READ curPage NOTIFY pagingChanged)
    Q_PROPERTY(int pageOffset READ pageOffset NOTIFY pagingChanged)
    // The cameras on the current page in pane order, empty panes left out.
    Q_PROPERTY(QStringList pageKeys READ pageKeys NOTIFY pageKeysChanged)
    Q_PROPERTY(QString maximized READ maximized WRITE setMaximized NOTIFY maximizedChanged)
    Q_PROPERTY(QString selected READ selected WRITE setSelected NOTIFY selectedChanged)

public:
    // Longer than the biggest grid so an arrangement made in the 16-grid survives a trip
    // through the 4-grid, and cameras beyond 25 can be reached by paging.
    static constexpr int kMaxPanes = 32;

    explicit LiveArrangement(QObject *parent = nullptr);

    QString saved() const { return m_saved; }
    void setSaved(const QString &text);
    QStringList cameras() const { return m_cameras; }
    void setCameras(const QStringList &keys);
    QStringList paneKeys() const { return m_slots; }
    QString serialized() const;
    bool restored() const { return m_restored; }
    bool hasPending() const { return !m_pending.isEmpty(); }

    int preset() const { return m_preset; }
    void setPreset(int n);
    int page() const { return m_page; }
    void setPage(int p);
    int pageCount() const;
    int curPage() const;
    int pageOffset() const { return curPage() * m_preset; }
    QStringList pageKeys() const;

    QString maximized() const { return m_maximized; }
    void setMaximized(const QString &key);
    QString selected() const { return m_selected; }
    void setSelected(const QString &key);

    // The model was reset: forget the arrangement and restore it again from the last save
    // on the next setCameras.
    Q_INVOKABLE void reset();
    // Put `key` in `pane`; a camera already placed swaps with whatever was there, so a drag
    // can never drop a camera off the grid. Unknown cameras and panes are ignored.
    Q_INVOKABLE bool assign(int pane, const QString &key);
    Q_INVOKABLE QString keyAt(int pane) const;
    Q_INVOKABLE int paneOf(const QString &key) const;
    // Previous/next page of the grid, wrapping; nothing with a single page.
    Q_INVOKABLE void turnPage(int dir);
    // The camera `dir` steps from `key`, wrapping, in `order` (default: pane order); ""
    // when there is nowhere to go.
    Q_INVOKABLE QString neighbour(const QString &key, int dir, const QStringList &order = {}) const;
    // The cameras of a stored arrangement text in pane order, empty panes left out.
    Q_INVOKABLE static QStringList savedKeys(const QString &text);
    // A named mosaic's own order after a drop: a camera not in it is added before `target`,
    // one already in it swaps places with `target`.
    Q_INVOKABLE static QStringList dropInto(const QStringList &order, const QString &target,
                                            const QString &source);

    int giveUpMs() const { return m_giveUp.interval(); }
    void setGiveUpMs(int ms) { m_giveUp.setInterval(ms); }

signals:
    void camerasChanged();
    void slotsChanged();
    void restoredChanged();
    void pendingChanged();
    void pagingChanged();
    void presetChanged();
    void pageChanged(); // the page being shown changed (a turn, or a new grid size)
    void pageKeysChanged();
    void maximizedChanged();
    void selectedChanged();
    void saveRequested(const QString &text);

private:
    void restore();
    void applyPending();
    void repair();
    void setSlots(const QStringList &s);
    void settle();
    void giveUp();

    QString m_saved, m_lastSaved;
    QStringList m_cameras;
    QStringList m_slots;
    QHash<int, QString> m_pending; // pane -> saved camera with no match yet
    bool m_restored = false;
    int m_preset = 4, m_page = 0;
    QString m_maximized, m_selected;
    QTimer m_giveUp;
};

} // namespace rl
