#include "ui/FrameShape.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace rl {
namespace frameshape {

namespace {
constexpr double kWide = 16.0 / 9;

double validRatio(double ratio)
{
    return std::isfinite(ratio) && ratio > 0 ? ratio : kWide;
}
} // namespace

QSize cardSize(double feedWidth, double ratio)
{
    if (!(feedWidth > 0))
        return {0, 0};
    const double r = validRatio(ratio);
    const double area = feedWidth * feedWidth / kWide; // today's 16:9 card at full width
    const double w = std::min(feedWidth, std::sqrt(area * r));
    return {qRound(w), qRound(w / r)};
}

QSize fitIn(double width, double height, double ratio)
{
    if (!(width > 0) || !(height > 0))
        return {0, 0};
    const double r = validRatio(ratio);
    double w = width, h = width / r;
    if (h > height) {
        h = height;
        w = height * r;
    }
    return {qRound(w), qRound(h)};
}

bool shapeChanged(double remembered, double measured)
{
    if (!std::isfinite(measured) || !(measured > 0))
        return false;
    if (!std::isfinite(remembered) || !(remembered > 0))
        return true;
    // "At least 0.5 %", with room for the rounding of a stored "1.0000"-style value.
    return std::abs(measured - remembered) / remembered >= 0.005 - 1e-9;
}

double drawnRatio(double remembered, int rotation)
{
    const double r = validRatio(remembered);
    return rotation % 180 != 0 ? 1 / r : r;
}

} // namespace frameshape

QSizeF FrameShape::cardSize(double feedWidth, double ratio) const
{
    return frameshape::cardSize(feedWidth, ratio);
}

QSizeF FrameShape::fitIn(double width, double height, double ratio) const
{
    return frameshape::fitIn(width, height, ratio);
}

bool FrameShape::shapeChanged(double remembered, double measured) const
{
    return frameshape::shapeChanged(remembered, measured);
}

double FrameShape::drawnRatio(double remembered, int rotation) const
{
    return frameshape::drawnRatio(remembered, rotation);
}

} // namespace rl
