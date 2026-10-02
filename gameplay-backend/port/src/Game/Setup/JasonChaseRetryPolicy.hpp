#pragma once
#include <cmath>
#include <cstdint>

// Pure decisions only: no timers, allocations, engine calls, or path queries.
namespace JasonChaseRetryPolicy
{
    inline bool IsSuspiciousAlreadyAtGoal(
        uint8_t result, float distanceSquared, bool destinationMatchesTarget)
    {
        // Three meters excludes the native melee/interaction band. Failed and
        // accepted requests are not evidence of this particular contradiction.
        return result == 1 && destinationMatchesTarget &&
            std::isfinite(distanceSquared) && distanceSquared > 300.0f * 300.0f;
    }

    inline int AdvanceRepeatedResult(int previous, bool sameTarget)
    {
        return sameTarget && previous >= 1 ? 2 : 1;
    }
}
