#pragma once
#include <cmath>

namespace JasonDoorApproachPolicy
{
    // Local geometry only. A selected door must lie between Jason and the
    // chase target, within interaction range and a narrow approach corridor.
    inline bool IsOnApproach(float doorX, float doorY, float doorZ,
        float targetX, float targetY, float targetZ)
    {
        if (!std::isfinite(doorX) || !std::isfinite(doorY) ||
            !std::isfinite(doorZ) || !std::isfinite(targetX) ||
            !std::isfinite(targetY) || !std::isfinite(targetZ)) return false;
        const float targetSquared = targetX * targetX + targetY * targetY;
        const float doorSquared = doorX * doorX + doorY * doorY;
        if (!std::isfinite(targetSquared) || !std::isfinite(doorSquared) ||
            targetSquared < 125.0f * 125.0f || doorSquared > 200.0f * 200.0f ||
            std::fabs(doorZ) > 150.0f) return false;
        const float along = doorX * targetX + doorY * targetY;
        if (!std::isfinite(along) || along <= 0.0f || along > targetSquared) return false;
        const float cross = doorX * targetY - doorY * targetX;
        return std::isfinite(cross) &&
            cross * cross <= 100.0f * 100.0f * targetSquared;
    }
}
