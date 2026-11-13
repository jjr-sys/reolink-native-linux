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

} // namespace frameshape

QSizeF FrameShape::cardSize(double feedWidth, double ratio) const
{
    return frameshape::cardSize(feedWidth, ratio);
}

QSizeF FrameShape::fitIn(double width, double height, double ratio) const
{
    return frameshape::fitIn(width, height, ratio);
}

} // namespace rl
