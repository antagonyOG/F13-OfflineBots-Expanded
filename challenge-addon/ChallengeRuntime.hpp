#pragma once
#include "ChallengeSetupPolicy.hpp"
#include <cstddef>
namespace f13::challenges::runtime {
enum class FieldKind { Int, Float };
struct Field {
    void* data = nullptr;
    std::size_t elementSize = 0;
    int arrayDim = 0;
    std::uint64_t ownerIdentity = 0; // GObjects index+serial for nested components.
};
struct Scope {
    std::uintptr_t world = 0;
    std::uintptr_t controller = 0;
    void* primaryJason = nullptr;
    bool stockChallengeMission = false;
    bool localAuthority = false;
    bool networkSession = true;
    bool inProgress = false;
    void* gameState = nullptr;
};
struct Host {
    // Must reject CDO/archetype, foreign world/owner, unreadable/unwritable
    // storage, mismatched reflected property type/size/dimension and EXE.
    Field (*field)(void* object, const char* name, FieldKind kind, int dimension) = nullptr;
    bool (*isCurrentInstance)(void* object, std::uintptr_t world,
                              std::uintptr_t controller) = nullptr;
    std::uint64_t (*objectIdentity)(void* object) = nullptr;
    void (*log)(const char*) = nullptr;
};
bool Configure(const Host&, std::string_view exeHash) noexcept;
Capabilities AuditedCapabilities() noexcept;
void Tick(const Scope&, const Settings& committed, bool committedValid);
void Reset() noexcept;
}
