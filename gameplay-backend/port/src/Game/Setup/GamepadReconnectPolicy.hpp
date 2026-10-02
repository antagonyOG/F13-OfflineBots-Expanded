#pragma once
#include <cstdint>

namespace f13::input {
// Normal connected polling remains unchanged. Only disconnected-slot searches
// back off: XInput drivers may take appreciable time for absent devices.
struct ReconnectGate {
    bool connected = true;
    std::uint64_t nextDiscovery = 0;
    bool Due(std::uint64_t now) const noexcept { return connected || now >= nextDiscovery; }
    void Found() noexcept { connected = true; nextDiscovery = 0; }
    void Missing(std::uint64_t now) noexcept { connected = false; nextDiscovery = now + 1000; }
};
} // namespace f13::input
