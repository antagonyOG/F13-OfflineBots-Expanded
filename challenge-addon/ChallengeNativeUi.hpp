#pragma once
#include "ChallengeSetupPolicy.hpp"
#include <cstddef>

namespace f13::challenges::nativeui {
// Bridge to OfflineSetup's already-validated reflection/UMG helpers. Configure
// stores pointers only; it must never resolve/load challenge assets at startup.
struct Host {
    std::uintptr_t (*currentWorld)() = nullptr;
    bool (*isLiveFrontendObject)(void*) = nullptr;
    std::uint64_t (*objectIdentity)(void*) = nullptr;
    void* (*readWidgetObject)(void*, const char*) = nullptr;
    void* (*appendToggle)(void* settings, void* donorCombo,
                          const wchar_t* title, std::size_t row, int initialIndex) = nullptr;
    int (*readToggle)(void*) = nullptr;
    bool (*labelHeader)(void* settings, const wchar_t*) = nullptr;
    Capabilities (*capabilities)() = nullptr;
    void (*removeWidget)(void*) = nullptr;
    void (*log)(const char*) = nullptr;
};
bool Configure(const Host& host, std::string_view supportedExeHash) noexcept;
// Root's existing ProcessEvent hook calls these with its checked SafeName
// strings before/after original dispatch. They never suppress original events.
void BeforeEvent(void* object, std::string_view className, std::string_view eventName);
void AfterEvent(void* object, std::string_view className, std::string_view eventName);
// Root calls on leaving the challenge route, failed travel, disconnect/unload.
void Reset() noexcept;
bool HasCommittedSettings() noexcept;
std::uint64_t CommittedSubmission() noexcept;
Settings GetCommittedSettings() noexcept;
// Native adapters for each option still require separately audited capability
// bits and world/controller/attempt scope via SetupPolicy. This UI bridge does
// not mutate pawn stats, timers, AI, cinematics, escape or backend functions.
}
