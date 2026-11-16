#pragma once

#include <QHash>
#include <QDate>
#include <QObject>
#include <QStringList>
#include <QVariantList>

#include "ui/PaneSync.h"

namespace rl {

// Playback's day (architecture review #2, candidate 1): one day of the selected cameras'
// footage and the rules that play their panes as one — which camera leads, where a play lands,
// where a stream that ran out carries on, the playhead's clock and the day's searches. Live
// replay has the same rules across days (LiveReplay); both use PaneSync for whose frames move
// the playhead and when a pane starts again at it. Positions are seconds into the day.
// The page tells it the panes' order, the selected pane and the Frigate (direct) cameras.
class PlaybackDay : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList paneOrder READ paneOrder WRITE setPaneOrder NOTIFY changed)
    Q_PROPERTY(QString selectedKey READ selectedKey WRITE setSelectedKey NOTIFY changed)
    Q_PROPERTY(QStringList directKeys READ directKeys WRITE setDirectKeys NOTIFY changed)
    Q_PROPERTY(double searchGapMs READ searchGapMs CONSTANT)   // for the page's retry timer

public:
    static constexpr double kDayEnd = 86399;
    // carryOn's answers besides a time: no recording that way that day; not the lead's stream.
    enum CarryOn { Nothing = -1, Ignore = -2 };
    Q_ENUM(CarryOn)
    static constexpr int kSearchTries = 3;
    static constexpr double kSearchGapMs = 10000;

    explicit PlaybackDay(QObject *parent = nullptr);
    double searchGapMs() const { return kSearchGapMs; }

    QStringList paneOrder() const { return m_order; }
    void setPaneOrder(const QStringList &keys);
    QString selectedKey() const { return m_selected; }
    void setSelectedKey(const QString &key);
    QStringList directKeys() const { return m_direct; }
    void setDirectKeys(const QStringList &keys);

    // A camera's recordings that day ([{start, end, …}], seconds into the day), as its
    // recorder answers; clear() for a new day (forgets the searches too).
    Q_INVOKABLE void setSegments(const QString &key, const QVariantList &segments);
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool known(const QString &key) const { return m_segments.contains(key); }
    // The day shown; takes() is true only for an answer about one of the panes' cameras and
    // that day (a search may answer after the day changed).
    Q_INVOKABLE void setDate(int year, int month, int day) { m_date = QDate(year, month, day); }
    Q_INVOKABLE bool takes(const QString &key, int year, int month, int day) const
    {
        return m_order.contains(key) && m_date == QDate(year, month, day);
    }

    // The camera whose recordings decide where a play lands and carries on: the selected one,
    // or with a Frigate one selected (forward 1× only), the first recorder in pane order.
    Q_INVOKABLE QString leadKey() const;
    Q_INVOKABLE bool covered(const QString &key, double sec) const;
    // The end of the last recording finishing before `sec` / the start of the first one after
    // it (more than half a second away); -1 none that day.
    Q_INVOKABLE double previousEnd(const QString &key, double sec) const;
    Q_INVOKABLE double nextStart(const QString &key, double sec) const;
    // Where a play from `sec` going `direction` starts: backwards from where the lead has
    // nothing, at the end of its previous recording, or -1 (nothing earlier that day).
    Q_INVOKABLE double landing(double sec, int direction) const;
    // `key`'s stream ran out going `direction` at `playhead`: where every pane goes on (the
    // previous recording's end / the next one's start), Nothing, or Ignore (not the lead's).
    Q_INVOKABLE double carryOn(const QString &key, int direction, double playhead) const;
    // Whether to ask for `key`'s day now (and note that it was): never twice within 10 s; an
    // unanswered day until it answers; an answered day only while it came back empty, at most
    // three times; `force` regardless.
    Q_INVOKABLE bool wantSearch(const QString &key, double nowMs, bool force);
    // The last search for `key` couldn't be asked (device not ready): not a try, and the
    // camera may be asked again at once.
    Q_INVOKABLE void notReady(const QString &key);

    // The playhead after `dt` seconds at `speed` going `direction`, inside the day; off 1×, the
    // driving pane's frame time when there is one (driverSecs >= 0).
    Q_INVOKABLE static double advance(double playhead, double dt, double speed, int direction,
                                      double driverSecs);
    // The panes as the page sees them (PaneSync::paneFrom maps, pane order): whose frames move the playhead (PaneSync: the lead camera's
    // pane, else the first that can; Frigate panes are locked off 1×), and its frame's time
    // (-1: none, or no frame yet).
    Q_INVOKABLE QString frameDriver(const QVariantList &panes, double playhead, bool offNormal) const;
    Q_INVOKABLE double frameDriverSecs(const QVariantList &panes, double playhead, bool offNormal) const;
    // Whether this pane starts again at the playhead (PaneSync); Frigate panes are locked off 1×.
    Q_INVOKABLE bool needsRestart(const QVariantMap &pane, double playhead, double speed,
                                  bool offNormal, const QString &driverKey, double nowMs) const;

signals:
    void changed();

private:
    struct Span { double start; double end; };
    struct Searches { double lastAskedMs = -1; int count = 0; };
    panesync::Pane paneFrom(const QVariantMap &m, double playhead, bool offNormal) const;
    QList<panesync::Pane> panesFrom(const QVariantList &panes, double playhead, bool offNormal) const;

    QDate m_date;
    QStringList m_order;
    QString m_selected;
    QStringList m_direct;
    QHash<QString, QList<Span>> m_segments;
    QHash<QString, Searches> m_searches;
};

} // namespace rl
