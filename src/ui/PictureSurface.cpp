#include "ui/PictureSurface.h"

#include "ui/FisheyeLens.h"
#include "ui/FrameShape.h"

#include <algorithm>
#include <cmath>

namespace rl {

namespace {
constexpr double kMinZoom = 1.0;
constexpr double kMaxZoom = 8.0;
constexpr double kWheelIn = 1.15;
constexpr double kWheelOut = 0.87;
// The button's zoom: back to 1 above this (a hair over 1 counts as unzoomed), else this much.
constexpr double kZoomedAbove = 1.01;
constexpr double kButtonZoom = 2.0;
// A drawn picture is a new shape only past this: it is rounded to pixels, and a re-pack
// resizing the pane must not read back as a new shape.
constexpr double kShapeSlack = 0.03;
// A press moves this far (pixels) before it picks the pane up.
constexpr double kDragThreshold = 8.0;
constexpr double kMinDrawn = 2.0;   // a picture drawn smaller than this is not drawn yet
} // namespace

PictureSurface::PictureSurface(QObject *parent) : QObject(parent) {}

// ---- inputs ---------------------------------------------------------------------------------

void PictureSurface::setViewport(const QSizeF &size)
{
    if (size == m_viewport)
        return;
    m_viewport = size;
    emit viewportChanged();
}

void PictureSurface::setContentRect(const QRectF &rect)
{
    if (rect == m_content)
        return;
    m_content = rect;
    emit contentRectChanged();
    if (!m_streaming || m_dewarp || rect.width() < kMinDrawn || rect.height() < kMinDrawn)
        return;   // a dewarp stretches the frame over the pane: that is not the picture's shape
    const bool turned = m_rotation % 180 != 0;
    const double m = turned ? rect.height() / rect.width() : rect.width() / rect.height();
    if (!(m_ratio > 0) || std::abs(m - m_ratio) / m_ratio > kShapeSlack) {
        m_ratio = m;
        emit ratioChanged();
    }
}

void PictureSurface::setPictureRatio(double ratio)
{
    if (!std::isfinite(ratio) || ratio < 0)
        ratio = 0;
    if (ratio == m_pictureRatio)
        return;
    m_pictureRatio = ratio;
    emit pictureRatioChanged();
    recompute();
}

void PictureSurface::setRotation(int degrees)
{
    if (degrees == m_rotation)
        return;
    m_rotation = degrees;
    emit rotationChanged();
}

void PictureSurface::setStreaming(bool on)
{
    if (on == m_streaming)
        return;
    m_streaming = on;
    emit streamingChanged();
}

void PictureSurface::setFisheyeLens(bool on)
{
    if (on == m_lens)
        return;
    m_lens = on;
    emit fisheyeLensChanged();
    recompute();
}

void PictureSurface::setFisheyeMode(const QString &mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    emit fisheyeModeChanged();
    recompute();
}

void PictureSurface::setFlatCopy(bool on)
{
    if (on == m_flat)
        return;
    m_flat = on;
    emit flatCopyChanged();
    recompute();
}

void PictureSurface::setCameraKey(const QString &key)
{
    if (key == m_key)
        return;
    m_key = key;
    emit cameraKeyChanged();
}

void PictureSurface::setAims(FisheyeAims *aims)
{
    if (aims == m_aims)
        return;
    m_aims = aims;
    emit aimsChanged();
}

// ---- the dewarp rule ------------------------------------------------------------------------

bool PictureSurface::circle() const
{
    return !m_flat && frameshape::isCircle(m_pictureRatio, m_lens);
}

double PictureSurface::shownRatio() const
{
    return circle() ? frameshape::viewRatio(m_mode) : frameshape::shownRatio(m_pictureRatio, false, m_mode);
}

void PictureSurface::recompute()
{
    // Before the first picture (ratio 0) the camera's lens and view decide.
    const bool wantsView = m_lens && m_mode != QLatin1String("fisheye");
    const bool dewarp = wantsView && !m_flat && (m_pictureRatio <= 0 || circle());
    // The shape answers (circle, shownRatio) follow the same inputs, so they share the signal.
    const bool changed = dewarp != m_dewarp;
    m_dewarp = dewarp;
    emit dewarpChanged();
    if (changed && m_dewarp)
        resetZoom();   // a dewarped view zooms itself; no digital zoom over it
}

// ---- zoom and pan ---------------------------------------------------------------------------

void PictureSurface::setZoom(double zoom)
{
    zoom = std::isfinite(zoom) ? std::clamp(zoom, kMinZoom, kMaxZoom) : kMinZoom;
    if (zoom == m_zoom)
        return;
    m_zoom = zoom;
    emit zoomChanged();
    if (m_zoom <= kMinZoom && (m_panX != 0 || m_panY != 0)) {
        m_panX = m_panY = 0;
        emit panChanged();
    }
}

void PictureSurface::setPanX(double x)
{
    if (x == m_panX)
        return;
    m_panX = x;
    emit panChanged();
}

void PictureSurface::setPanY(double y)
{
    if (y == m_panY)
        return;
    m_panY = y;
    emit panChanged();
}

void PictureSurface::clampPan()
{
    // Keep the zoomed picture pinned to the viewport edges: panning must not drag it off into
    // empty space, and zooming out reels it back in.
    const double mx = std::max(0.0, (m_content.width() * m_zoom - m_viewport.width()) / 2);
    const double my = std::max(0.0, (m_content.height() * m_zoom - m_viewport.height()) / 2);
    const double x = std::clamp(m_panX, -mx, mx);
    const double y = std::clamp(m_panY, -my, my);
    if (x == m_panX && y == m_panY)
        return;
    m_panX = x;
    m_panY = y;
    emit panChanged();
}

void PictureSurface::zoomStep(bool zoomIn)
{
    setZoom(m_zoom * (zoomIn ? kWheelIn : kWheelOut));
    clampPan();
}

void PictureSurface::panBy(double dx, double dy)
{
    if (dx == 0 && dy == 0)
        return;
    m_panX += dx;
    m_panY += dy;
    emit panChanged();
    clampPan();
}

void PictureSurface::toggleZoom()
{
    if (m_zoom > kZoomedAbove)
        resetZoom();
    else
        setZoom(kButtonZoom);
}

void PictureSurface::resetZoom()
{
    setZoom(kMinZoom);
    setPanX(0);
    setPanY(0);
}

double PictureSurface::dragThreshold() const
{
    return kDragThreshold;
}

bool PictureSurface::pastDragThreshold(double dx, double dy) const
{
    return std::hypot(dx, dy) > kDragThreshold;
}

// ---- aim ------------------------------------------------------------------------------------

bool PictureSurface::aimed() const
{
    return m_dewarp && m_aims && m_aims->aimed(m_key, m_mode);
}

bool PictureSurface::resetAim()
{
    if (!aimed())
        return false;
    m_aims->reset(m_key, m_mode);
    return true;
}

} // namespace rl
