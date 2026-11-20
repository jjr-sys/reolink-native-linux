#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

#include "ui/SiteBudget.h"
#include "ui/SiteMap.h"
#include "ui/SiteSelection.h"

class QAbstractItemModel;

namespace rl {

// One page's camera-selection session (architecture review, selection session): everything
// Live View and Playback both do with the cameras a user picked on the Site tabs, so a page
// keeps only its transport. It owns, and exposes as `selection` and `budget`, the page's
// SiteSelection and SiteBudget, and adds:
//
//  - the stored selection: restored on start() (from `storeGroup`, or, on the first launch, from
//    the old grid at `gridGroup`/`gridKey`), saved on every user change, migrated once;
//  - the device rows: rowOf(key) / keyOfRow(row), cached and kept right as the model changes;
//  - pruning: keys of removed recorders and of cameras gone from a Frigate server are dropped
//    once the device list has been still for `kPruneMs`; pruned() lets a page add its own rule;
//  - the camera's shape: remembered between launches in the shared [mosaicShapes] store
//    (noteFrame), one rule for the shape a pane or a drawer card is drawn at (ratioOf);
//  - the mosaic panes, in pane order, with their shape and Large flag (`panes`), re-packed
//    when the selection, the rows or a shape change, gathered `settleMs` so that cameras
//    connecting at launch cause one re-pack, not 18;
//  - the budget refusals' wording (hdRefusal).
//
// Per-page differences are parameters, set before start():
//   waitMs             Live waits this long at launch for the selected cameras to be reported
//                      before building the first mosaic (`waitOver` says when); 0 = no wait.
//   settleMs           the gather time above (Live 500, Playback 300).
//   settleOnSelection  a selection change re-packs after the gather time, not at once (Playback).
//   seedCount          with nothing to migrate, select this many cameras once they arrive (Live 4).
//   gridPresetKey      the old grid's panes-per-page key; "" = 4 panes a page.
//
// Inputs from QML: `sites`, `devices` (a QAbstractItemModel with roles hostId, channel,
// rotationOverride, isFisheye, viewMode; a `listedCameras()` invokable, if it has one, says which
// Frigate cameras exist), the budget inputs, `large`, `overBudgetAdvice`.
//
// Time: the module holds no clock of its own. It stamps deadlines from `clock` (wall
// milliseconds; tests set their own) and acts on them in tick(); the QTimer inside is only the
// alarm that calls tick() at the next deadline (as EventManager arms OfflineAlerts).
class SelectionSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(rl::SiteMap *sites READ sites WRITE setSites NOTIFY sitesChanged)
    Q_PROPERTY(QAbstractItemModel *devices READ devices WRITE setDevices NOTIFY devicesChanged)
    Q_PROPERTY(rl::SiteSelection *selection READ selection CONSTANT)
    Q_PROPERTY(rl::SiteBudget *budget READ budget CONSTANT)
    Q_PROPERTY(QVariantMap budgets READ budgets WRITE setBudgets)
    Q_PROPERTY(double defaultBudget READ defaultBudget WRITE setDefaultBudget)
    Q_PROPERTY(QVariantMap costs READ costs WRITE setCosts)
    Q_PROPERTY(QStringList hd READ hd WRITE setHd)
    Q_PROPERTY(QStringList large READ large WRITE setLarge)
    Q_PROPERTY(QString overBudgetAdvice READ overBudgetAdvice WRITE setOverBudgetAdvice)

    Q_PROPERTY(QString storeGroup READ storeGroup WRITE setStoreGroup)
    Q_PROPERTY(QString gridGroup READ gridGroup WRITE setGridGroup)
    Q_PROPERTY(QString gridKey READ gridKey WRITE setGridKey)
    Q_PROPERTY(QString gridPresetKey READ gridPresetKey WRITE setGridPresetKey)
    Q_PROPERTY(int seedCount READ seedCount WRITE setSeedCount)
    Q_PROPERTY(int waitMs READ waitMs WRITE setWaitMs)
    Q_PROPERTY(int settleMs READ settleMs WRITE setSettleMs)
    Q_PROPERTY(bool settleOnSelection READ settleOnSelection WRITE setSettleOnSelection)

    Q_PROPERTY(QVariantList panes READ panes NOTIFY panesChanged)
    Q_PROPERTY(bool waitOver READ waitOver NOTIFY waitOverChanged)

public:
    static constexpr int kPruneMs = 8000;

    explicit SelectionSession(QObject *parent = nullptr);

    SiteMap *sites() const { return m_sites; }
    void setSites(SiteMap *sites);
    QAbstractItemModel *devices() const { return m_devices; }
    void setDevices(QAbstractItemModel *devices);
    SiteSelection *selection() const { return m_selection; }
    SiteBudget *budget() const { return m_budget; }
    QVariantMap budgets() const { return m_budget->budgets(); }
    void setBudgets(const QVariantMap &v) { m_budget->setBudgets(v); }
    double defaultBudget() const { return m_budget->defaultBudget(); }
    void setDefaultBudget(double v) { m_budget->setDefaultBudget(v); }
    QVariantMap costs() const { return m_budget->costs(); }
    void setCosts(const QVariantMap &v) { m_budget->setCosts(v); }
    QStringList hd() const { return m_budget->hd(); }
    void setHd(const QStringList &v) { m_budget->setHd(v); }
    QStringList large() const { return m_large; }
    void setLarge(const QStringList &keys);
    QString overBudgetAdvice() const { return m_advice; }
    void setOverBudgetAdvice(const QString &text) { m_advice = text; }

    QString storeGroup() const { return m_storeGroup; }
    void setStoreGroup(const QString &g) { m_storeGroup = g; }
    QString gridGroup() const { return m_gridGroup; }
    void setGridGroup(const QString &g) { m_gridGroup = g; }
    QString gridKey() const { return m_gridKey; }
    void setGridKey(const QString &k) { m_gridKey = k; }
    QString gridPresetKey() const { return m_gridPresetKey; }
    void setGridPresetKey(const QString &k) { m_gridPresetKey = k; }
    int seedCount() const { return m_seedCount; }
    void setSeedCount(int n) { m_seedCount = n; }
    int waitMs() const { return m_waitMs; }
    void setWaitMs(int ms) { m_waitMs = ms; }
    int settleMs() const { return m_settleMs; }
    void setSettleMs(int ms) { m_settleMs = ms; }
    bool settleOnSelection() const { return m_settleOnSelection; }
    void setSettleOnSelection(bool on) { m_settleOnSelection = on; }

    QVariantList panes() const { return m_panes; }
    bool waitOver() const { return m_waitOver; }

    // Wall-clock milliseconds; the default is the system clock.
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }

    // Restore the stored selection (migrating once), start the launch wait and the prune
    // countdown. Call once the parameters and inputs are set.
    Q_INVOKABLE void start();
    // Act on every deadline that has passed.
    Q_INVOKABLE void tick();

    // ---- Device rows by camera key ("hostId:channel") ----
    Q_INVOKABLE int rowOf(const QString &key) const;   // -1: its recorder has not reported it
    Q_INVOKABLE QString keyOfRow(int row) const;
    // The camera `dir` panes from `key` in pane order, wrapping, skipping any selected camera
    // whose recorder has not reported it; "" with nowhere to go.
    Q_INVOKABLE QString stepFrom(const QString &key, int dir) const;

    // ---- Shapes ----
    // A pane measured its decoded picture (width / height): remember it, and re-pack if the
    // pane is drawn differently now.
    Q_INVOKABLE void noteFrame(const QString &key, double ratio);
    // The shape this camera's pane or drawer card is drawn at: its fisheye view's, else its
    // remembered picture turned by its rotation, else 16:9.
    Q_INVOKABLE double ratioOf(const QString &key) const;
    Q_INVOKABLE double rememberedFrame(const QString &key) const;

    // ---- Budget wording ----
    // Why switching this pane to HD would go over its Site's budget; "" when it fits.
    Q_INVOKABLE QString hdRefusal(const QString &key) const;

    // Re-pack now.
    Q_INVOKABLE void rebuild();

signals:
    void sitesChanged();
    void devicesChanged();
    void panesChanged();
    void waitOverChanged();
    // After the prune rules ran with a device list to judge by: a page adds its own.
    void pruned();

private:
    void indexRows() const;
    void forgetRows() { m_rowsValid = false; }
    void onRowsChanged(bool inserted);
    void onDataChanged(int first, int last);
    void onSelectionChanged();
    void scheduleSettle();
    void refreshRatio(const QString &key);
    double computeRatio(const QString &key) const;
    void prune();
    void seedIfNeeded();
    void restore();
    bool selectionMissing() const;
    void rearm();
    qint64 now() const { return m_clock ? m_clock() : 0; }

    QPointer<SiteMap> m_sites;
    QPointer<QAbstractItemModel> m_devices;
    SiteSelection *m_selection;
    SiteBudget *m_budget;
    QStringList m_large;
    QString m_advice;
    QString m_storeGroup, m_gridGroup, m_gridKey, m_gridPresetKey;
    int m_seedCount = 0, m_waitMs = 0, m_settleMs = 500;
    bool m_settleOnSelection = false;

    std::function<qint64()> m_clock;
    QTimer m_alarm;
    qint64 m_waitDue = 0, m_pruneDue = 0, m_settleDue = 0;   // 0: not pending
    bool m_waitOver = true, m_seedPending = false;

    mutable bool m_rowsValid = false;
    mutable QStringList m_keyByRow;
    mutable QHash<QString, int> m_rowByKey;
    int m_roleHost = -1, m_roleChannel = -1, m_roleRotation = -1, m_roleFisheye = -1,
        m_roleView = -1;
    QHash<QString, double> m_ratios;   // the shape each camera was last laid out at
    QVariantList m_panes;
};

} // namespace rl
