#include "MosaicLayout.h"

#include <QFutureWatcher>
#include <QRectF>
#include <QVariantMap>
#include <QtConcurrent>

namespace rl {

using EnginePtr = std::shared_ptr<mosaic::Mosaic>;

MosaicLayout::MosaicLayout(QObject *parent) : QObject(parent) {}

void MosaicLayout::setPanes(const QVariantList &panes)
{
    std::vector<mosaic::Pane> parsed;
    parsed.reserve(panes.size());
    for (const QVariant &v : panes) {
        const QVariantMap m = v.toMap();
        const double r = m.value(QStringLiteral("ratio")).toDouble();
        parsed.push_back({r > 0 ? r : 16.0 / 9, m.value(QStringLiteral("large")).toBool() ? 2 : 1});
    }
    m_panesIn = panes;
    emit panesChanged();
    const bool same = parsed.size() == m_panes.size() &&
                      std::equal(parsed.begin(), parsed.end(), m_panes.begin(),
                                 [](const mosaic::Pane &a, const mosaic::Pane &b) {
                                     return a.ratio == b.ratio && a.weight == b.weight;
                                 });
    if (same && m_pending)
        return; // the build under way has these shapes; it lands with these entries
    if (same && m_engine) {
        // Same shapes, other entries (cameras swapped places): same geometry.
        if (m_layoutPanes != panes) {
            m_layoutPanes = panes;
            emit layoutPanesChanged();
        }
        return;
    }
    m_panes = std::move(parsed);
    startBuild();
}

void MosaicLayout::setWidth(qreal w)
{
    if (w == m_w)
        return;
    m_w = w;
    emit widthChanged();
    relayout(false);
}

void MosaicLayout::setHeight(qreal h)
{
    if (h == m_h)
        return;
    m_h = h;
    emit heightChanged();
    relayout(false);
}

void MosaicLayout::setGap(qreal g)
{
    if (g == m_gap)
        return;
    m_gap = g;
    emit gapChanged();
    relayout(true);
}

void MosaicLayout::reset()
{
    m_haveTree = false;
    relayout(true);
}

// Builds are pure functions of the panes, so a stale one is simply dropped when it lands.
void MosaicLayout::startBuild()
{
    const quint64 gen = ++m_generation;
    if (m_pending++ == 0)
        emit buildingChanged();
    auto *watcher = new QFutureWatcher<EnginePtr>(this);
    connect(watcher, &QFutureWatcher<EnginePtr>::finished, this, [this, watcher, gen] {
        watcher->deleteLater();
        if (gen == m_generation) {
            m_engine = watcher->result();
            m_layoutPanes = m_panesIn;
            emit layoutPanesChanged();
            relayout(true);
        }
        if (--m_pending == 0)
            emit buildingChanged();
    });
    watcher->setFuture(QtConcurrent::run([panes = m_panes] {
        auto engine = std::make_shared<mosaic::Mosaic>();
        engine->build(panes);
        return engine;
    }));
}

void MosaicLayout::relayout(bool fresh)
{
    if (fresh)
        m_haveTree = false;
    if (!m_engine)
        return;
    QVariantList out;
    double black = 0;
    if (m_w > 0 && m_h > 0 && m_engine->size() > 0) {
        const mosaic::Result res =
            m_engine->layout(m_w, m_h, m_gap, m_haveTree ? &m_tree : nullptr);
        m_tree = res.tree;
        m_haveTree = true;
        black = res.black;
        for (const mosaic::Rect &r : mosaic::snapped(res.rects))
            out.append(QRectF(r.x, r.y, r.w, r.h));
    }
    if (out == m_rects && black == m_black)
        return;
    m_rects = out;
    m_black = black;
    emit rectsChanged();
}

} // namespace rl
