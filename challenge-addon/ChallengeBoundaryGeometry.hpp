#pragma once
#include <algorithm>
#include <cmath>

namespace f13::challenges::boundary {
struct Point { double x, y, z; };
// Check the swept center, not just the endpoints: a fast scripted car can
// cross a small escape sphere between the existing 250ms samples.
inline bool SegmentTouchesSphere(Point from, Point to, Point center, double radius)
{
    if (!std::isfinite(radius) || radius <= 0 ||
        !std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z) ||
        !std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z) ||
        !std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) return false;
    const Point delta{to.x-from.x, to.y-from.y, to.z-from.z};
    const Point offset{center.x-from.x, center.y-from.y, center.z-from.z};
    const double length2 = delta.x*delta.x + delta.y*delta.y + delta.z*delta.z;
    const double t = length2 > 0 ? std::clamp(
        (offset.x*delta.x + offset.y*delta.y + offset.z*delta.z)/length2, 0.0, 1.0) : 0.0;
    const Point nearest{from.x+t*delta.x-center.x, from.y+t*delta.y-center.y, from.z+t*delta.z-center.z};
    return nearest.x*nearest.x + nearest.y*nearest.y + nearest.z*nearest.z <= radius*radius;
}
}
