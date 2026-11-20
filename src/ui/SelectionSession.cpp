#include "ui/SelectionSession.h"

#include "ui/FrameShape.h"

#include <QAbstractItemModel>
#include <QDateTime>
#include <QLocale>
#include <QMetaObject>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace rl {

namespace {
constexpr char kShapesGroup[] = "mosaicShapes";   // "hostId:channel" = decoded width/height

// Mbit/s as people write it: one decimal, none when whole ("4.6", "5").
QString mbps(double v)
{
    const double r = std::round(v * 10) / 10;
    return QLocale().toString(r, 'f', std::fmod(r, 1.0) == 0.0 ? 0 : 1);
}
} // namespace

SelectionSession::SelectionSession(QObject *parent)
    : QObject(parent)
    , m_selection(new SiteSelection(this))
    , m_budget(new SiteBudget(this))
    , m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
{
    m_budget->setSelection(m_selection);
    m_alarm.setSingleShot(true);
    connect(&m_alarm, &QTimer::timeout, this, &SelectionSession::tick);
    connect(m_selection, &SiteSelection::changed, this, &SelectionSession::onSelectionChanged);
    connect(m_selection, &SiteSelection::saveRequested, this, [this](const QString &text) {
        QSettings s;
        s.beginGroup(m_storeGroup);
        s.setValue(QStringLiteral("keys"), text);
    });
}

void SelectionSession::setSites(SiteMap *sites)
{
    if (m_sites == sites)
        return;
    m_sites = sites;
    m_selection->setSites(sites);
    m_budget->setSites(sites);
    emit sitesChanged();
}

void SelectionSession::setDevices(QAbstractItemModel *devices)
{
    if (m_devices == devices)
        return;
    if (m_devices)
        disconnect(m_devices, nullptr, this, nullptr);
    m_devices = devices;
    forgetRows();
    m_roleHost = m_roleChannel = m_roleRotation = m_roleFisheye = m_roleView = -1;
    if (m_devices) {
        const auto roles = m_devices->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if (it.value() == "hostId") m_roleHost = it.key();
            else if (it.value() == "channel") m_roleChannel = it.key();
            else if (it.value() == "rotationOverride") m_roleRotation = it.key();
            else if (it.value() == "isFisheye") m_roleFisheye = it.key();
            else if (it.value() == "viewMode") m_roleView = it.key();
        }
        // Forgotten as soon as the model is about to change, so a page's own slot on the same
        // signal, running before ours, never reads a stale index.
        for (auto sig : {&QAbstractItemModel::rowsAboutToBeInserted, &QAbstractItemModel::rowsAboutToBeRemoved})
            connect(m_devices, sig, this, [this] { forgetRows(); });
        connect(m_devices, &QAbstractItemModel::modelAboutToBeReset, this, [this] { forgetRows(); });
        connect(m_devices, &QAbstractItemModel::layoutAboutToBeChanged, this, [this] { forgetRows(); });
        connect(m_devices, &QAbstractItemModel::rowsInserted, this, [this] { onRowsChanged(true); });
        connect(m_devices, &QAbstractItemModel::rowsRemoved, this, [this] { onRowsChanged(false); });
        connect(m_devices, &QAbstractItemModel::modelReset, this, [this] { onRowsChanged(false); });
        connect(m_devices, &QAbstractItemModel::layoutChanged, this, [this] { onRowsChanged(false); });
        connect(m_devices, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex &a, const QModelIndex &b) { onDataChanged(a.row(), b.row()); });
    }
    emit devicesChanged();
}

void SelectionSession::setLarge(const QStringList &keys)
{
    if (m_large == keys)
        return;
    m_large = keys;
    rebuild();   // a pane turning Large is the user's own act: re-pack at once
}

// ---- start and time -----------------------------------------------------------------------

void SelectionSession::start()
{
    restore();
    const qint64 t = now();
    m_pruneDue = t + kPruneMs;
    m_waitOver = m_waitMs <= 0;
    m_waitDue = m_waitOver ? 0 : t + m_waitMs;
    emit waitOverChanged();
    rearm();
}

void SelectionSession::tick()
{
    const qint64 t = now();
    if (m_waitDue && t >= m_waitDue) {
        // The launch wait is over: build with whatever has arrived.
        m_waitDue = 0;
        m_waitOver = true;
        emit waitOverChanged();
        seedIfNeeded();
        prune();
        rebuild();
    }
    if (m_pruneDue && t >= m_pruneDue) {
        m_pruneDue = 0;
        prune();
    }
    if (m_settleDue && t >= m_settleDue) {
        m_settleDue = 0;
        rebuild();
    }
    rearm();
}

void SelectionSession::rearm()
{
    qint64 next = 0;
    for (qint64 due : {m_waitDue, m_pruneDue, m_settleDue})
        if (due && (!next || due < next))
            next = due;
    if (!next) {
        m_alarm.stop();
        return;
    }
    m_alarm.start(int(std::max<qint64>(0, next - now())));
}

void SelectionSession::scheduleSettle()
{
    if (m_settleDue)
        return;   // already gathering: not pushed back
    m_settleDue = now() + m_settleMs;
    rearm();
}

// ---- the stored selection -----------------------------------------------------------------

void SelectionSession::restore()
{
    QSettings s;
    s.beginGroup(m_storeGroup);
    if (s.value(QStringLiteral("migrated"), false).toBool()) {
        m_selection->setSaved(s.value(QStringLiteral("keys")).toString());
        return;
    }
    // First launch: the old grid's first page (left in place for one release).
    QSettings g;
    g.beginGroup(m_gridGroup);
    int preset = 4;
    if (!m_gridPresetKey.isEmpty()) {
        bool ok = false;
        const int p = g.value(m_gridPresetKey, 4).toInt(&ok);
        if (ok)
            preset = p;
    }
    const QStringList keys =
        SiteSelection::fromGrid(g.value(m_gridKey).toString(), preset);
    const QString text = keys.join(QLatin1Char(','));
    m_selection->setSaved(text);
    s.setValue(QStringLiteral("keys"), text);
    s.setValue(QStringLiteral("migrated"), true);
    m_seedPending = keys.isEmpty() && m_seedCount > 0;
}

void SelectionSession::seedIfNeeded()
{
    if (!m_seedPending || !m_devices || m_devices->rowCount() == 0)
        return;
    m_seedPending = false;
    if (m_selection->count() > 0)
        return;
    const int rows = m_devices->rowCount();
    for (int r = 0; r < rows && m_selection->count() < m_seedCount; ++r) {
        const QString k = keyOfRow(r);
        if (!k.isEmpty() && m_budget->canAdd(k))
            m_selection->toggle(k);
    }
}

// ---- device rows --------------------------------------------------------------------------

void SelectionSession::indexRows() const
{
    m_keyByRow.clear();
    m_rowByKey.clear();
    if (m_devices && m_roleHost >= 0 && m_roleChannel >= 0) {
        const int n = m_devices->rowCount();
        for (int r = 0; r < n; ++r) {
            const QModelIndex i = m_devices->index(r, 0);
            const QVariant h = m_devices->data(i, m_roleHost);
            const QString key = h.isValid()
                ? QStringLiteral("%1:%2").arg(h.toLongLong()).arg(m_devices->data(i, m_roleChannel).toInt())
                : QString();
            m_keyByRow << key;
            if (!key.isEmpty())
                m_rowByKey.insert(key, r);   // a duplicate key: the last row, as the pages did
        }
    }
    m_rowsValid = true;
}

int SelectionSession::rowOf(const QString &key) const
{
    if (!m_rowsValid || (m_devices && m_keyByRow.size() != m_devices->rowCount()))
        indexRows();
    return m_rowByKey.value(key, -1);
}

QString SelectionSession::keyOfRow(int row) const
{
    if (!m_rowsValid || (m_devices && m_keyByRow.size() != m_devices->rowCount()))
        indexRows();
    return row >= 0 && row < m_keyByRow.size() ? m_keyByRow.at(row) : QString();
}

QString SelectionSession::stepFrom(const QString &key, int dir) const
{
    QString k = key;
    for (int i = 0; i < m_selection->count(); ++i) {
        k = m_selection->neighbour(k, dir);
        if (k.isEmpty() || k == key)
            return {};
        if (rowOf(k) >= 0)
            return k;
    }
    return {};
}

bool SelectionSession::selectionMissing() const
{
    const QStringList order = m_selection->paneOrder();
    return std::any_of(order.begin(), order.end(), [this](const QString &k) { return rowOf(k) < 0; });
}

void SelectionSession::onRowsChanged(bool inserted)
{
    forgetRows();
    scheduleSettle();
    m_pruneDue = now() + kPruneMs;   // the list is still moving (an NVR swaps its placeholder for its channels)
    if (inserted && m_waitOver)
        seedIfNeeded();   // no cameras had arrived when the wait ended
    rearm();
}

void SelectionSession::onDataChanged(int first, int last)
{
    if (!m_devices)
        return;
    if (m_rowsValid) {
        // A row that turned into another camera makes the index stale.
        for (int r = first; r <= last && r < m_keyByRow.size(); ++r) {
            const QModelIndex i = m_devices->index(r, 0);
            const QVariant h = m_devices->data(i, m_roleHost);
            const QString key = h.isValid()
                ? QStringLiteral("%1:%2").arg(h.toLongLong()).arg(m_devices->data(i, m_roleChannel).toInt())
                : QString();
            if (key != m_keyByRow.at(r)) {
                forgetRows();
                break;
            }
        }
    }
    for (int r = first; r <= last; ++r) {
        const QString key = keyOfRow(r);
        if (!key.isEmpty())
            refreshRatio(key);
    }
}

void SelectionSession::onSelectionChanged()
{
    if (m_settleOnSelection)
        scheduleSettle();
    else
        rebuild();
}

// ---- pruning ------------------------------------------------------------------------------

// Keys of a recorder that has been removed are dropped once the device list settles (an NVR
// swaps its placeholder row for its channels while it connects), and keys of a camera removed
// from a Frigate server.
void SelectionSession::prune()
{
    if (!m_devices || m_devices->rowCount() == 0)
        return;
    QVariantList hosts;
    QSet<qint64> seen;
    const int n = m_devices->rowCount();
    for (int r = 0; r < n; ++r) {
        const qint64 h = m_devices->data(m_devices->index(r, 0), m_roleHost).toLongLong();
        if (!seen.contains(h)) {
            seen.insert(h);
            hosts << h;
        }
    }
    m_selection->pruneHosts(hosts);
    QVariantMap listed;
    if (QMetaObject::invokeMethod(m_devices, "listedCameras", Qt::DirectConnection,
                                  Q_RETURN_ARG(QVariantMap, listed)))
        m_selection->pruneCameras(listed);
    emit pruned();
}

// ---- the mosaic panes ---------------------------------------------------------------------

void SelectionSession::rebuild()
{
    if (!m_waitOver && selectionMissing())
        return;   // at launch the first build waits for the selected cameras
    m_settleDue = 0;
    QVariantList panes;
    for (const QString &k : m_selection->paneOrder()) {
        const int row = rowOf(k);
        if (row < 0)
            continue;   // a selected camera whose recorder has not answered yet gets its pane when it arrives
        const double ratio = computeRatio(k);
        m_ratios.insert(k, ratio);
        panes.append(QVariantMap{{QStringLiteral("row"), row},
                                 {QStringLiteral("large"), m_large.contains(k)},
                                 {QStringLiteral("ratio"), ratio}});
    }
    m_panes = panes;
    emit panesChanged();
    rearm();
}

// ---- shapes -------------------------------------------------------------------------------

double SelectionSession::rememberedFrame(const QString &key) const
{
    QSettings s;
    s.beginGroup(QLatin1String(kShapesGroup));
    const double v = s.value(key, 0).toDouble();
    return v > 0 ? v : 0;
}

double SelectionSession::computeRatio(const QString &key) const
{
    const double frame = rememberedFrame(key);
    const int row = rowOf(key);
    if (row < 0)
        return frameshape::drawnRatio(frame, 0);
    const QModelIndex i = m_devices->index(row, 0);
    const bool fisheye = m_roleFisheye >= 0 && m_devices->data(i, m_roleFisheye).toBool();
    const QString view = m_roleView >= 0 ? m_devices->data(i, m_roleView).toString() : QString();
    if (fisheye && !view.isEmpty())
        return frameshape::viewRatio(view);   // the view's shape
    const int rotation = m_roleRotation >= 0 ? m_devices->data(i, m_roleRotation).toInt() : 0;
    return frameshape::drawnRatio(frame, rotation);
}

double SelectionSession::ratioOf(const QString &key) const
{
    return computeRatio(key);
}

void SelectionSession::noteFrame(const QString &key, double ratio)
{
    if (key.isEmpty())
        return;
    if (frameshape::shapeChanged(rememberedFrame(key), ratio)) {
        QSettings s;
        s.beginGroup(QLatin1String(kShapesGroup));
        s.setValue(key, QString::number(ratio, 'f', 4));
    }
    refreshRatio(key);
}

// A selected camera drawn differently from how it was laid out: re-pack once things settle.
void SelectionSession::refreshRatio(const QString &key)
{
    if (!m_selection->contains(key) || rowOf(key) < 0)
        return;   // laid out in no pane: nothing to move
    const double was = m_ratios.value(key, 0);
    if (was > 0 && !frameshape::shapeChanged(was, computeRatio(key)))
        return;
    scheduleSettle();
}

// ---- budget wording -----------------------------------------------------------------------

QString SelectionSession::hdRefusal(const QString &key) const
{
    if (m_budget->canHd(key))
        return {};
    const QString site = m_sites ? m_sites->siteOfKey(key) : QString();
    return tr("%1 is at %2 / %3 Mbit/s; HD needs %4 more.")
               .arg(site, mbps(m_budget->used(site)), mbps(m_budget->budgetOf(site)),
                    mbps(m_budget->hdExtra(key)))
        + QLatin1Char(' ') + m_advice;
}

} // namespace rl
