#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace f13::challenges::boundaryscript {
enum class Kind { Outside, Restart };
inline constexpr std::uint8_t VoidReturn[]{0x04, 0x0B};
// Stock dispatcher runtime-expanded scripts (original cooked asset audit).
inline bool MatchesStock(const std::uint8_t* data, std::int32_t count,
                         std::int32_t capacity, Kind kind) noexcept {
    constexpr std::uint8_t outside[]{0x4C, 0x97, 0x03, 0x00, 0x00};
    constexpr std::uint8_t restart[]{0x5F, 0x00};
    const auto expected = kind == Kind::Outside ? 922 : 161;
    const auto* prefix = kind == Kind::Outside ? outside : restart;
    const auto bytes = kind == Kind::Outside ? sizeof(outside) : sizeof(restart);
    return data && count == expected && capacity >= count &&
        std::memcmp(data, prefix, bytes) == 0 && data[count-3] == VoidReturn[0] &&
        data[count-2] == VoidReturn[1] && data[count-1] == 0x53;
}
} // namespace f13::challenges::boundaryscript
