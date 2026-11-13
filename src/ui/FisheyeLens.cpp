#include "ui/FisheyeLens.h"

#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace rl {
namespace fisheye {

namespace {
constexpr double kDeg = M_PI / 180;
// How far below the horizon a panorama strip reaches, towards the floor under the camera.
constexpr double kPanoDepth = 70 * kDeg;
constexpr double kPano360 = 3.3;    // panorama strip, width / height
constexpr double kPanoDual = 2.1;   // each dual strip
constexpr double kMinTilt = 8 * kDeg;
constexpr double kMinFov = 30 * kDeg;
constexpr double kMaxFov = 120 * kDeg;

Tile ptzTile(double x, double y, double w, double h)
{
    Tile t;
    t.kind = Tile::Ptz;
    t.x = x; t.y = y; t.w = w; t.h = h;
    t.fov = 90 * kDeg;
    t.tilt = std::min(55 * kDeg, maxTilt(t.fov, h / std::max(1.0, w)));
    return t;
}

Tile panoTile(double x, double y, double w, double h, double phi0, double span)
{
    Tile t;
    t.kind = Tile::Pano;
    t.x = x; t.y = y; t.w = w; t.h = h;
    t.phi0 = phi0;
    t.span = span;
    return t;
}

Tile aimed(Tile t, const std::optional<Aim> &aim)
{
    if (!aim || t.kind == Tile::Raw)
        return t;
    if (t.kind == Tile::Ptz) {
        t.pan = aim->pan;
        t.tilt = aim->tilt;
        t.fov = aim->fov;
    } else {
        t.phi0 = aim->phi0;
    }
    return t;
}

double aspectOf(const Tile &t) { return t.h / std::max(1.0, t.w); }

QString kindName(Tile::Kind k)
{
    switch (k) {
    case Tile::Raw: return QStringLiteral("raw");
    case Tile::Ptz: return QStringLiteral("ptz");
    case Tile::Pano: return QStringLiteral("pano");
    }
    return {};
}
} // namespace

QStringList views()
{
    return {QStringLiteral("fisheye"), QStringLiteral("quad"), QStringLiteral("ptz"),
            QStringLiteral("panorama"), QStringLiteral("dual"), QStringLiteral("fisheye_ptz")};
}

bool movable(const QString &view)
{
    return view != QLatin1String("fisheye") && views().contains(view);
}

QList<Tile> tiles(const QString &view, double W, double H, double g, const Aims &aims)
{
    QList<Tile> out;
    if (!(W > 0) || !(H > 0))
        return out;
    if (view == QLatin1String("fisheye")) {
        Tile t;
        t.w = W; t.h = H;
        out << t;
    } else if (view == QLatin1String("ptz")) {
        out << ptzTile(0, 0, W, H);
    } else if (view == QLatin1String("panorama")) {
        // The strip keeps its shape, centred, in a pane of another shape.
        const double ph = std::min(H, W / kPano360), pw = ph * kPano360;
        out << panoTile((W - pw) / 2, (H - ph) / 2, pw, ph, 0, 2 * M_PI);
    } else if (view == QLatin1String("dual")) {
        const double th = std::min((H - g) / 2, W / kPanoDual), tw = th * kPanoDual;
        const double top = (H - (2 * th + g)) / 2;
        out << panoTile((W - tw) / 2, top, tw, th, 0, M_PI)
            << panoTile((W - tw) / 2, top + th + g, tw, th, M_PI, M_PI);
    } else if (view == QLatin1String("quad")) {
        const double qw = (W - g) / 2, qh = (H - g) / 2;
        for (int k = 0; k < 4; ++k) {
            Tile t;
            t.kind = Tile::Ptz;
            t.x = (k % 2) ? qw + g : 0;
            t.y = (k / 2) ? qh + g : 0;
            t.w = qw; t.h = qh;
            t.pan = k * 90 * kDeg;
            t.fov = kQuadFov;
            t.tilt = std::min(kQuadTilt, maxTilt(t.fov, qh / std::max(1.0, qw)));
            out << t;
        }
    } else if (view == QLatin1String("fisheye_ptz")) {
        // The circle at full height beside a virtual camera; at 7:3 that camera is 4:3.
        const double s = std::min(H, W * 3 / 7);
        Tile raw;
        raw.y = (H - s) / 2;
        raw.w = raw.h = s;
        out << raw << ptzTile(s + g, 0, W - s - g, H);
    }
    for (int i = 0; i < out.size() && i < aims.size(); ++i)
        out[i] = aimed(out[i], aims[i]);
    return out;
}

int at(const QList<Tile> &tiles, double x, double y)
{
    for (int i = 0; i < tiles.size(); ++i) {
        const Tile &t = tiles[i];
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h)
            return i;
    }
    return -1;
}

double maxTilt(double fov, double aspect)
{
    return 88 * kDeg - std::atan(std::tan(fov / 2) * aspect);
}

Tile look(const Tile &tile, const Tile &start, double dx, double dy)
{
    Tile t = tile;
    if (t.kind == Tile::Ptz) {
        const double a = aspectOf(t);
        const double top = std::max(maxTilt(t.fov, a), start.tilt);
        t.pan = t.pan - dx / std::max(1.0, t.w) * t.fov;
        t.tilt = std::clamp(t.tilt + dy / std::max(1.0, t.h) * t.fov * a, kMinTilt, top);
    } else if (t.kind == Tile::Pano) {
        t.phi0 = t.phi0 - dx / std::max(1.0, t.w) * t.span;
    }
    return t;
}

Tile widen(const Tile &tile, const Tile &start, double zoom)
{
    if (tile.kind != Tile::Ptz || !(zoom > 0))
        return tile;
    Tile t = tile;
    t.fov = std::clamp(t.fov / zoom, kMinFov, kMaxFov);
    t.tilt = std::min(t.tilt, std::max(maxTilt(t.fov, aspectOf(t)), start.tilt));
    return t;
}

std::optional<QPointF> sample(const Tile &t, double u, double v)
{
    double ray[3];
    if (t.kind == Tile::Raw) {
        return QPointF(u, v);
    } else if (t.kind == Tile::Ptz) {
        const double f[3] = {std::sin(t.tilt) * std::cos(t.pan), std::sin(t.tilt) * std::sin(t.pan), std::cos(t.tilt)};
        // right = normalize(f x (0, 0, -1)); up = right x f  (as dewarp.frag)
        double r[3] = {-f[1], f[0], 0};
        const double rn = std::hypot(r[0], r[1]);
        r[0] /= rn; r[1] /= rn;
        const double up[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        const double th = std::tan(t.fov / 2), a = aspectOf(t);
        const double x = u * 2 - 1, y = v * 2 - 1;
        for (int i = 0; i < 3; ++i)
            ray[i] = f[i] + r[i] * (x * th) - up[i] * (y * th * a);
    } else {
        const double az = t.phi0 + t.span * u;
        const double e = -kPanoDepth * v;
        ray[0] = std::cos(e) * std::cos(az);
        ray[1] = std::cos(e) * std::sin(az);
        ray[2] = -std::sin(e);
    }
    const double n = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
    const double theta = std::acos(std::clamp(ray[2] / n, -1.0, 1.0));
    if (theta > M_PI / 2 + 1e-3)   // behind the lens; a hair of slack keeps the horizon row
        return std::nullopt;
    const double phi = std::atan2(ray[1], ray[0]);
    const double rr = theta / (M_PI / 2) * kLensRadius;
    return QPointF(kLensCx + rr * std::cos(phi), kLensCy + rr * std::sin(phi));
}

Aim aimOf(const Tile &t)
{
    return Aim{t.pan, t.tilt, t.fov, t.phi0};
}

QString encodeAims(const Aims &aims)
{
    QStringList parts;
    for (const auto &a : aims)
        parts << (a ? QStringLiteral("%1,%2,%3,%4").arg(a->pan, 0, 'g', 10).arg(a->tilt, 0, 'g', 10)
                          .arg(a->fov, 0, 'g', 10).arg(a->phi0, 0, 'g', 10)
                    : QString());
    return parts.join(QLatin1Char(';'));
}

Aims decodeAims(const QString &text)
{
    Aims out;
    if (text.isEmpty())
        return out;
    for (const QString &part : text.split(QLatin1Char(';'))) {
        const QStringList n = part.split(QLatin1Char(','));
        std::optional<Aim> aim;
        if (n.size() == 4) {
            bool ok[4];
            const Aim a{n[0].toDouble(&ok[0]), n[1].toDouble(&ok[1]), n[2].toDouble(&ok[2]), n[3].toDouble(&ok[3])};
            if (ok[0] && ok[1] && ok[2] && ok[3])
                aim = a;
        }
        out << aim;
    }
    return out;
}

} // namespace fisheye

namespace {
QString aimKey(const QString &key, const QString &view)
{
    return QStringLiteral("fisheyeAim/%1/%2").arg(QString(key).replace(QLatin1Char(':'), QLatin1Char('_')), view);
}
} // namespace

FisheyeAims::FisheyeAims(QObject *parent)
    : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &FisheyeAims::flush);
}

FisheyeAims::~FisheyeAims()
{
    flush();
}

fisheye::Aims &FisheyeAims::cached(const QString &key, const QString &view) const
{
    const QString k = aimKey(key, view);
    auto it = m_cache.find(k);
    if (it == m_cache.end())
        it = m_cache.insert(k, fisheye::decodeAims(QSettings().value(k).toString()));
    return it.value();
}

fisheye::Aims FisheyeAims::aims(const QString &key, const QString &view) const
{
    return cached(key, view);
}

void FisheyeAims::flush()
{
    m_saveTimer.stop();
    if (m_dirty.isEmpty())
        return;
    QSettings s;
    for (const QString &k : std::as_const(m_dirty))
        s.setValue(k, fisheye::encodeAims(m_cache.value(k)));
    m_dirty.clear();
}

void FisheyeAims::setAim(const QString &key, const QString &view, int index, const fisheye::Aim &aim)
{
    if (index < 0 || !fisheye::movable(view))
        return;
    fisheye::Aims &all = cached(key, view);
    while (all.size() <= index)
        all << std::nullopt;
    all[index] = aim;
    m_dirty.insert(aimKey(key, view));
    m_saveTimer.start();
    ++m_revision;
    emit changed(key, view);
}

bool FisheyeAims::aimed(const QString &key, const QString &view) const
{
    const fisheye::Aims &all = cached(key, view);
    return std::any_of(all.cbegin(), all.cend(), [](const auto &a) { return a.has_value(); });
}

void FisheyeAims::reset(const QString &key, const QString &view)
{
    if (!aimed(key, view))
        return;
    const QString k = aimKey(key, view);
    m_cache.insert(k, {});
    m_dirty.remove(k);
    QSettings().remove(k);
    ++m_revision;
    emit changed(key, view);
}

QVariantList FisheyeAims::tileList(const QString &key, const QString &view, double w, double h) const
{
    QVariantList out;
    for (const fisheye::Tile &t : fisheye::tiles(view, w, h, 2, aims(key, view)))
        out << QVariantMap{{QStringLiteral("kind"), fisheye::kindName(t.kind)},
                           {QStringLiteral("x"), t.x}, {QStringLiteral("y"), t.y},
                           {QStringLiteral("w"), t.w}, {QStringLiteral("h"), t.h},
                           {QStringLiteral("pan"), t.pan}, {QStringLiteral("tilt"), t.tilt},
                           {QStringLiteral("fov"), t.fov}, {QStringLiteral("phi0"), t.phi0},
                           {QStringLiteral("span"), t.span}};
    return out;
}

bool FisheyeAims::aimTileAt(const QString &key, const QString &view, double w, double h, double x, double y,
                            const std::function<fisheye::Tile(const fisheye::Tile &, const fisheye::Tile &)> &move)
{
    if (!fisheye::movable(view))
        return false;
    const QList<fisheye::Tile> now = fisheye::tiles(view, w, h, 2, aims(key, view));
    const int i = fisheye::at(now, x, y);
    if (i < 0 || now[i].kind == fisheye::Tile::Raw)
        return false;
    const fisheye::Tile start = fisheye::tiles(view, w, h, 2)[i];
    setAim(key, view, i, fisheye::aimOf(move(now[i], start)));
    return true;
}

bool FisheyeAims::drag(const QString &key, const QString &view, double w, double h,
                       double x, double y, double dx, double dy)
{
    return aimTileAt(key, view, w, h, x, y, [=](const fisheye::Tile &t, const fisheye::Tile &start) {
        return fisheye::look(t, start, dx, dy);
    });
}

bool FisheyeAims::zoom(const QString &key, const QString &view, double w, double h,
                       double x, double y, bool zoomIn)
{
    return aimTileAt(key, view, w, h, x, y, [=](const fisheye::Tile &t, const fisheye::Tile &start) {
        return fisheye::widen(t, start, zoomIn ? 1 / 0.9 : 0.9);
    });
}

} // namespace rl
