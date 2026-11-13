#pragma once

#include <QObject>
#include <QSize>
#include <QSizeF>

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

} // namespace frameshape

// The same for QML (registered as the FrameShape singleton).
class FrameShape : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE QSizeF cardSize(double feedWidth, double ratio) const;
    Q_INVOKABLE QSizeF fitIn(double width, double height, double ratio) const;
};

} // namespace rl
