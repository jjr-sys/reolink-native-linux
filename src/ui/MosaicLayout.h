#pragma once

#include "ui/MosaicPacker.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include <memory>

namespace rl {

// The mosaic packer for QML: give it the panes and the box, read back one rectangle per
// pane (whole pixels, in pane order, relative to the box). Building runs off the UI thread
// (up to ~60 ms for 25 panes with Large); the previous layout stays until it is done, so
// `rects` can briefly still describe the previous panes while `building` is true.
// A resize only re-scores (well under 1 ms) and holds the layout on screen while it is
// still good enough, so dragging the window does not flap between layouts. With Large
// panes the hold can fall short of the fresh rule; the layout is then re-packed fresh once
// the size has been still for `settleMs`, announced by settled() just before `rects`
// changes. New panes, a new gap, reset() or a jump of more than 10% in width or height
// start fresh at once.
class MosaicLayout : public QObject
{
    Q_OBJECT
    // Per pane, in mosaic order: { ratio: width/height of the drawn picture, large: bool }.
    Q_PROPERTY(QVariantList panes READ panes WRITE setPanes NOTIFY panesChanged)
    Q_PROPERTY(qreal width READ width WRITE setWidth NOTIFY widthChanged)
    Q_PROPERTY(qreal height READ height WRITE setHeight NOTIFY heightChanged)
    Q_PROPERTY(qreal gap READ gap WRITE setGap NOTIFY gapChanged)
    // QRectF per pane of the last finished build; empty until the first one.
    Q_PROPERTY(QVariantList rects READ rects NOTIFY rectsChanged)
    // The `panes` entries (with any extra fields the caller put in them) that `rects`
    // describes, one for one. It changes, just before `rects`, when a build lands.
    Q_PROPERTY(QVariantList layoutPanes READ layoutPanes NOTIFY layoutPanesChanged)
    Q_PROPERTY(bool building READ building NOTIFY buildingChanged)
    // Black share of the box in percent, gaps included.
    Q_PROPERTY(qreal black READ black NOTIFY rectsChanged)

public:
    explicit MosaicLayout(QObject *parent = nullptr);

    QVariantList panes() const { return m_panesIn; }
    void setPanes(const QVariantList &panes);
    qreal width() const { return m_w; }
    void setWidth(qreal w);
    qreal height() const { return m_h; }
    void setHeight(qreal h);
    qreal gap() const { return m_gap; }
    void setGap(qreal g);
    QVariantList rects() const { return m_rects; }
    QVariantList layoutPanes() const { return m_layoutPanes; }
    bool building() const { return m_pending > 0; }
    qreal black() const { return m_black; }

    // The mosaic was reopened: forget the held layout.
    Q_INVOKABLE void reset();

    int settleMs() const { return m_settle.interval(); }
    void setSettleMs(int ms) { m_settle.setInterval(ms); }

signals:
    void panesChanged();
    void widthChanged();
    void heightChanged();
    void gapChanged();
    void rectsChanged();
    void layoutPanesChanged();
    void buildingChanged();
    void settled(); // a held layout is about to be replaced by a fresh one

private:
    void startBuild();
    void relayout(bool fresh);

    QVariantList m_panesIn;
    std::vector<mosaic::Pane> m_panes;
    qreal m_w = 0, m_h = 0, m_gap = 4;
    std::shared_ptr<mosaic::Mosaic> m_engine;
    mosaic::Tree m_tree;
    bool m_haveTree = false;
    quint64 m_generation = 0;
    int m_pending = 0;
    QVariantList m_rects;
    QVariantList m_layoutPanes;
    qreal m_black = 0;
    QTimer m_settle;
};

} // namespace rl
