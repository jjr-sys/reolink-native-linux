#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cmath>
#include <functional>
#include <optional>

namespace rl {

// How a ceiling-mounted fisheye camera (180° equidistant lens, axis straight down) is shown as
// dewarped views, shared by Live, Playback and Events (wayfinder/playback-controls ticket 13).
// The maths is dewarp.frag's; `sample` does the shader's sum so it can be tested. Ported from
// Outpost's tested FisheyeLens.kt.
namespace fisheye {

// The lens circle inside the square frame, as fractions of its side (measured on the real
// Entrance and Kitchen cameras from a 2560x2560 still).
inline constexpr double kLensCx = 0.5;
inline constexpr double kLensCy = 0.5;
inline constexpr double kLensRadius = 0.476;

// Quad: four 16:9 views, 120° wide, tilted 43° from straight down. Together they see from the
// floor under the camera out to the rim, and none shows sky (ticket 13, plan point 1). In a
// pane of another shape the tilt is held under the horizon (maxTilt).
inline constexpr double kQuadFov = 120 * M_PI / 180;
inline constexpr double kQuadTilt = 43 * M_PI / 180;

// One part of a dewarped picture, in pixels of the pane. Raw copies the circle; Ptz is a
// virtual camera looking at azimuth `pan`, `tilt` from straight down, `fov` wide; Pano is a
// strip from azimuth `phi0` across `span`.
struct Tile {
    enum Kind { Raw, Ptz, Pano };
    Kind kind = Raw;
    double x = 0, y = 0, w = 0, h = 0;
    double pan = 0, tilt = 0, fov = 0, phi0 = 0, span = 0;
};

// Where one part has been turned to.
struct Aim {
    double pan = 0, tilt = 0, fov = 0, phi0 = 0;
};
using Aims = QList<std::optional<Aim>>;   // by tile index; empty = where the view starts

// The six views, in menu order ("fisheye" is the camera's own circle).
QStringList views();
// A view whose parts can be aimed (any but the circle).
bool movable(const QString &view);

// The tiles of `view` in a `w` x `h` pane, `gap` px apart, each looking where `aims` says.
QList<Tile> tiles(const QString &view, double w, double h, double gap = 2, const Aims &aims = {});
// Which tile is at x, y (-1 for none).
int at(const QList<Tile> &tiles, double x, double y);
// `tile` after the pointer moved dx, dy over it (the picture follows the pointer). A virtual
// camera tilts no further up than the horizon (or than it started), nor below 8°.
Tile look(const Tile &tile, const Tile &start, double dx, double dy);
// `tile` zoomed by `zoom` (> 1 narrower, < 1 wider), 30–120°, never past the horizon.
Tile widen(const Tile &tile, const Tile &start, double zoom);
// How far a virtual camera may tilt before its top edge passes the horizon (aspect = h / w).
double maxTilt(double fov, double aspect);
// Where in the camera frame (0..1) the point u, v (0..1 across the tile) comes from; none
// above the horizon.
std::optional<QPointF> sample(const Tile &tile, double u, double v);

Aim aimOf(const Tile &tile);
QString encodeAims(const Aims &aims);
Aims decodeAims(const QString &text);

} // namespace fisheye

// The aim of each camera's views ("hostId:channel" x view), remembered across runs and shared
// by every page (QML singleton FisheyeAims). Double-click / View ▸ Reset aim forgets a view's.
// Aims live in memory while the pointer moves; they reach the settings file half a second after
// the last change (or on flush / destruction), not on every mouse move.
class FisheyeAims : public QObject
{
    Q_OBJECT
    // Bumps on every change, so QML bindings that read aims re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    explicit FisheyeAims(QObject *parent = nullptr);
    ~FisheyeAims() override;

    fisheye::Aims aims(const QString &key, const QString &view) const;
    void setAim(const QString &key, const QString &view, int index, const fisheye::Aim &aim);
    int revision() const { return m_revision; }
    // Write pending aims to the settings file now.
    void flush();

    // QML: the tiles for a pane as maps {kind ("raw"/"ptz"/"pano"), x, y, w, h, pan, tilt,
    // fov, phi0, span}.
    Q_INVOKABLE QVariantList tileList(const QString &key, const QString &view, double w, double h) const;
    // QML: a drag / wheel step at x, y in a w x h pane aims the tile under it; false if none.
    Q_INVOKABLE bool drag(const QString &key, const QString &view, double w, double h,
                          double x, double y, double dx, double dy);
    Q_INVOKABLE bool zoom(const QString &key, const QString &view, double w, double h,
                          double x, double y, bool zoomIn);
    Q_INVOKABLE bool aimed(const QString &key, const QString &view) const;
    Q_INVOKABLE void reset(const QString &key, const QString &view);

signals:
    void changed(const QString &key, const QString &view);

private:
    bool aimTileAt(const QString &key, const QString &view, double w, double h, double x, double y,
                   const std::function<fisheye::Tile(const fisheye::Tile &, const fisheye::Tile &)> &move);
    fisheye::Aims &cached(const QString &key, const QString &view) const;
    mutable QHash<QString, fisheye::Aims> m_cache;   // settings key -> aims
    QSet<QString> m_dirty;
    QTimer m_saveTimer;
    int m_revision = 0;
};

} // namespace rl
