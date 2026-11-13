#pragma once

#include <QObject>
#include <QSize>
#include <QSizeF>
#include <QString>

namespace rl {

// Frames that take their picture's shape: no black bars, no cropping
// (wayfinder/fisheye-and-framing tickets 03 and 06). Pure maths, tested directly.
namespace frameshape {

// An Events feed card for a picture of `ratio` (width / height): about the area of a 16:9
// card at the feed's full width, never wider than the feed. A missing or invalid ratio
// counts as 16:9.
QSize cardSize(double feedWidth, double ratio);

// The largest box of `ratio` that fits in `width` x `height` (16:9 when the ratio is unknown).
QSize fitIn(double width, double height, double ratio);

// Whether a newly measured picture shape should replace the remembered one: a first
// measurement, or a move of at least 0.5 % (decoder jitter stays put). No measurement never is.
bool shapeChanged(double remembered, double measured);

// The shape a pane is drawn at: the remembered decoded shape (16:9 if never seen), turned by
// a quarter rotation (`rotation` in degrees).
double drawnRatio(double remembered, int rotation);

// The shape a fisheye camera's view is drawn at (wayfinder/fisheye-and-framing ticket 03):
// Quad 16:9 (four 16:9 views), Original 1:1, PTZ 16:9, Panorama 3.3:1, Dual two 2.1:1 strips
// stacked, Fisheye+PTZ 7:3 (the circle beside a 4:3 view). Unknown views count as 16:9.
double viewRatio(const QString &view);

// Whether a picture of `ratio` from a fisheye camera is its lens circle: square within 1 %
// (encoder rounding). Frigate's flattened copy of a fisheye is not.
bool isCircle(double ratio, bool fisheye);
// The shape a picture is drawn at: its view's for a circle, else its own.
double shownRatio(double ratio, bool fisheye, const QString &view);
// Whether it is drawn through the view (a circle not shown as the circle itself).
bool dewarps(double ratio, bool fisheye, const QString &view);

} // namespace frameshape

// The same for QML (registered as the FrameShape singleton).
class FrameShape : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE QSizeF cardSize(double feedWidth, double ratio) const;
    Q_INVOKABLE QSizeF fitIn(double width, double height, double ratio) const;
    Q_INVOKABLE bool shapeChanged(double remembered, double measured) const;
    Q_INVOKABLE double drawnRatio(double remembered, int rotation) const;
    Q_INVOKABLE double viewRatio(const QString &view) const;
    Q_INVOKABLE bool isCircle(double ratio, bool fisheye) const;
    Q_INVOKABLE double shownRatio(double ratio, bool fisheye, const QString &view) const;
    Q_INVOKABLE bool dewarps(double ratio, bool fisheye, const QString &view) const;
};

} // namespace rl
