#include "ChallengeRuntime.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace f13::challenges::runtime {
namespace {
Host host{};
bool configured = false;
struct OwnedState {
    std::uintptr_t world = 0;
    std::uintptr_t controller = 0;
    void* pawn = nullptr;
    std::uint64_t identity = 0;
    int knives = 0;
    int appliedKnives = 0;
    bool knivesCaptured = false;
    std::array<float, 4> recharge{};
    std::array<float, 4> appliedRecharge{};
    bool rechargeCaptured = false;
    std::array<float, 4> stealth{};
    std::array<float, 4> appliedStealth{};
    bool stealthCaptured = false;
    std::array<float, 2> movement{};
    std::array<float, 2> appliedMovement{};
    std::uint64_t movementOwnerIdentity = 0;
    bool movementCaptured = false;
    void* timerState = nullptr;
    std::uint64_t timerStateIdentity = 0;
    int timerBaseline = 0;
    bool timerCaptured = false;
    std::array<bool, OptionCount> reported{};
} owned;
bool Checked(const Field& field, std::size_t bytes, int dim) {
    return field.data && field.elementSize == bytes && field.arrayDim == dim;
}
constexpr std::array<const char*, 4> StealthFields{{
    "StealthNoiseScale", "StealthRadiusScale", "AntiStealthNoiseScale", "AntiStealthRadiusScale"
}};
constexpr std::array<const char*, 2> MovementFields{{"MaxRunSpeed", "MaxSprintSpeed"}};
void LogOnce(Option option, const char* message) {
    const auto index = static_cast<std::size_t>(option);
    if (!owned.reported[index]) { owned.reported[index] = true; if (host.log) host.log(message); }
}
bool InScope(const Scope& scope) {
    return configured && scope.world && scope.controller && scope.primaryJason &&
           scope.stockChallengeMission && scope.localAuthority && !scope.networkSession &&
           scope.inProgress && host.isCurrentInstance(scope.primaryJason, scope.world, scope.controller);
}
}
bool Configure(const Host& callbacks, std::string_view hash) noexcept {
    if (owned.pawn || hash != SupportedExeSha256 || !callbacks.field ||
        !callbacks.isCurrentInstance || !callbacks.objectIdentity) return false;
    host = callbacks;
    configured = true;
    return true;
}
Capabilities AuditedCapabilities() noexcept {
    Capabilities result{};
    if (configured) {
        result.verified[static_cast<std::size_t>(Option::UnlimitedKnives)] = true;
        result.verified[static_cast<std::size_t>(Option::FasterRecharge)] = true;
        result.verified[static_cast<std::size_t>(Option::BoostAttributes)] = true;
        result.verified[static_cast<std::size_t>(Option::StealthPractice)] = true;
        result.verified[static_cast<std::size_t>(Option::UntimedMission)] = true;
    }
    return result;
}
void Reset() noexcept {
    if (configured && owned.pawn && owned.identity && host.objectIdentity(owned.pawn) == owned.identity &&
        host.isCurrentInstance(owned.pawn, owned.world, owned.controller)) {
        if (owned.knivesCaptured) {
            const auto field = host.field(owned.pawn, "NumKnives", FieldKind::Int, 1);
            if (Checked(field, sizeof(int), 1)) {
                int current{}; std::memcpy(&current, field.data, sizeof(current));
                // Restore only the value we still own; stock changes win.
                if (current == owned.appliedKnives) std::memcpy(field.data, &owned.knives, sizeof(owned.knives));
            }
        }
        if (owned.rechargeCaptured) {
            const auto field = host.field(owned.pawn, "AbilityRechargeTime", FieldKind::Float, 4);
            if (Checked(field, sizeof(float), 4)) {
                std::array<float, 4> current{}; std::memcpy(current.data(), field.data, sizeof(current));
                if (current == owned.appliedRecharge) std::memcpy(field.data, owned.recharge.data(), sizeof(owned.recharge));
            }
        }
        if (owned.stealthCaptured) {
            for (std::size_t i = 0; i < StealthFields.size(); ++i) {
                const auto field = host.field(owned.pawn, StealthFields[i], FieldKind::Float, 1);
                if (Checked(field, sizeof(float), 1)) {
                    float current{}; std::memcpy(&current, field.data, sizeof(current));
                    if (current == owned.appliedStealth[i]) std::memcpy(field.data, &owned.stealth[i], sizeof(float));
                }
            }
        }
        if (owned.movementCaptured) {
            for (std::size_t i = 0; i < MovementFields.size(); ++i) {
                const auto field = host.field(owned.pawn, MovementFields[i], FieldKind::Float, 1);
                if (Checked(field, sizeof(float), 1) && field.ownerIdentity == owned.movementOwnerIdentity) {
                    float current{}; std::memcpy(&current, field.data, sizeof(current));
                    if (current == owned.appliedMovement[i]) std::memcpy(field.data, &owned.movement[i], sizeof(float));
                }
            }
        }
    }
    // Never restore the match countdown on lifecycle exit. Native challenge
    // HandleMatchHasEnded uses the SAME -1 sentinel for PostMatchWaitTime, so
    // compare-before-restore alone cannot distinguish our InProgress override
    // from the game's legitimate postmatch value. Native end flow owns it.
    owned = OwnedState{};
}
void Tick(const Scope& scope, const Settings& settings, bool committedValid) {
    if (!committedValid || !InScope(scope)) { Reset(); return; }
    const auto identity = host.objectIdentity(scope.primaryJason);
    if (!identity) { Reset(); return; }
    if (owned.pawn != scope.primaryJason || owned.world != scope.world ||
        owned.controller != scope.controller || owned.identity != identity) {
        Reset();
        owned.pawn = scope.primaryJason;
        owned.world = scope.world;
        owned.controller = scope.controller;
        owned.identity = identity;
    }
    if (settings.enabled[static_cast<std::size_t>(Option::UnlimitedKnives)]) {
        const auto field = host.field(owned.pawn, "NumKnives", FieldKind::Int, 1);
        if (!Checked(field, sizeof(int), 1)) {
            LogOnce(Option::UnlimitedKnives, "CHALLENGE KNIVES: reflected NumKnives unavailable; capability inactive");
        } else {
            int current{}; std::memcpy(&current, field.data, sizeof(current));
            if (current >= 0 && current <= 9999) {
                if (!owned.knivesCaptured) { owned.knives = current; owned.knivesCaptured = true; }
                // Stock RefillKnives authority implementation sets this count
                // to 99. Maintain that stock refill ceiling after each throw.
                const int refill = std::max(current, 99);
                std::memcpy(field.data, &refill, sizeof(refill));
                owned.appliedKnives = refill;
                LogOnce(Option::UnlimitedKnives, "CHALLENGE KNIVES: stock NumKnives refill maintained on primary local Jason only");
            }
        }
    }
    if (settings.enabled[static_cast<std::size_t>(Option::FasterRecharge)] && !owned.rechargeCaptured) {
        const auto field = host.field(owned.pawn, "AbilityRechargeTime", FieldKind::Float, 4);
        if (!Checked(field, sizeof(float), 4)) {
            LogOnce(Option::FasterRecharge, "CHALLENGE RECHARGE: reflected float[4] unavailable; capability inactive");
        } else {
            std::array<float, 4> baseline{}; std::memcpy(baseline.data(), field.data, sizeof(baseline));
            const bool valid = std::all_of(baseline.begin(), baseline.end(), [](float value) {
                return std::isfinite(value) && value > 0.0f && value <= 3600.0f;
            });
            if (valid && std::isfinite(settings.rechargeDurationMultiplier) &&
                settings.rechargeDurationMultiplier >= 0.1f && settings.rechargeDurationMultiplier <= 1.0f) {
                owned.recharge = baseline;
                for (std::size_t i = 0; i < baseline.size(); ++i)
                    owned.appliedRecharge[i] = baseline[i] * settings.rechargeDurationMultiplier;
                std::memcpy(field.data, owned.appliedRecharge.data(), sizeof(owned.appliedRecharge));
                owned.rechargeCaptured = true;
                LogOnce(Option::FasterRecharge, "CHALLENGE RECHARGE: primary local Jason native ability thresholds scaled from captured baseline; unlock order preserved");
            } else {
                LogOnce(Option::FasterRecharge, "CHALLENGE RECHARGE: invalid native baseline/multiplier; capability inactive");
            }
        }
    }
    if (settings.enabled[static_cast<std::size_t>(Option::StealthPractice)] && !owned.stealthCaptured) {
        std::array<Field, 4> fields{};
        std::array<float, 4> baseline{};
        bool valid = std::isfinite(settings.perceptionRangeMultiplier) &&
            settings.perceptionRangeMultiplier >= 0.1f && settings.perceptionRangeMultiplier <= 1.0f;
        for (std::size_t i = 0; i < fields.size() && valid; ++i) {
            fields[i] = host.field(owned.pawn, StealthFields[i], FieldKind::Float, 1);
            valid = Checked(fields[i], sizeof(float), 1);
            if (valid) {
                std::memcpy(&baseline[i], fields[i].data, sizeof(float));
                valid = std::isfinite(baseline[i]) && baseline[i] >= 0.0f && baseline[i] <= 100.0f;
            }
        }
        if (valid) {
            owned.stealth = baseline;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                owned.appliedStealth[i] = baseline[i] * settings.perceptionRangeMultiplier;
                std::memcpy(fields[i].data, &owned.appliedStealth[i], sizeof(float));
            }
            owned.stealthCaptured = true;
            LogOnce(Option::StealthPractice, "CHALLENGE STEALTH PRACTICE: native noise/detection-radius endpoints scaled from local Jason baseline (default 10%); scripted reactions and skull/objective callbacks preserved");
        } else {
            LogOnce(Option::StealthPractice, "CHALLENGE STEALTH PRACTICE: native interpolation endpoints unavailable/invalid; capability inactive");
        }
    }
    if (settings.enabled[static_cast<std::size_t>(Option::BoostAttributes)]) {
        std::array<Field, 2> fields{};
        std::array<float, 2> baseline{};
        bool valid = std::isfinite(settings.attributeMultiplier) &&
            settings.attributeMultiplier >= 1.0f && settings.attributeMultiplier <= 4.0f;
        for (std::size_t i = 0; i < fields.size() && valid; ++i) {
            fields[i] = host.field(owned.pawn, MovementFields[i], FieldKind::Float, 1);
            valid = Checked(fields[i], sizeof(float), 1) && fields[i].ownerIdentity;
            if (valid) {
                std::memcpy(&baseline[i], fields[i].data, sizeof(float));
                valid = std::isfinite(baseline[i]) && baseline[i] > 0.0f && baseline[i] <= 5000.0f;
            }
        }
        valid = valid && fields[0].ownerIdentity == fields[1].ownerIdentity;
        if (valid && (!owned.movementCaptured || owned.movementOwnerIdentity != fields[0].ownerIdentity)) {
            owned.movement = baseline;
            owned.movementOwnerIdentity = fields[0].ownerIdentity;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                owned.appliedMovement[i] = baseline[i] * settings.attributeMultiplier;
                std::memcpy(fields[i].data, &owned.appliedMovement[i], sizeof(float));
            }
            owned.movementCaptured = true;
            LogOnce(Option::BoostAttributes, "CHALLENGE JASON MOVEMENT: owned native run/sprint speeds scaled from component baseline (default 2x); damage and scripted animation/special-kill timing unchanged");
        } else if (!valid) {
            LogOnce(Option::BoostAttributes, "CHALLENGE JASON MOVEMENT: native owned movement fields unavailable/invalid; capability inactive");
        }
    }
    const bool noMatchTimeout = settings.enabled[static_cast<std::size_t>(Option::UntimedMission)];
    if (scope.gameState && (noMatchTimeout || owned.timerCaptured)) {
        const auto field = host.field(scope.gameState, "RemainingTime", FieldKind::Int, 1);
        const auto stateIdentity = host.objectIdentity(scope.gameState);
        if (Checked(field, sizeof(int), 1) && stateIdentity) {
            int current{}; std::memcpy(&current, field.data, sizeof(current));
            if (noMatchTimeout && current >= -1 && current <= 86400) {
                if (!owned.timerCaptured || owned.timerState != scope.gameState || owned.timerStateIdentity != stateIdentity) {
                    owned.timerBaseline = current;
                    owned.timerState = scope.gameState;
                    owned.timerStateIdentity = stateIdentity;
                    owned.timerCaptured = true;
                }
                const int infinite = -1;
                std::memcpy(field.data, &infinite, sizeof(infinite));
                LogOnce(Option::UntimedMission, "CHALLENGE NO MATCH TIMEOUT: native InProgress RemainingTime=-1; elapsed scoring time, scripted mission/objective/escape timers and postmatch flow preserved");
            } else if (!noMatchTimeout && owned.timerCaptured && owned.timerState == scope.gameState &&
                       owned.timerStateIdentity == stateIdentity && current == -1) {
                // Explicit toggle-off is allowed only while this same native
                // challenge is still InProgress (already required by InScope).
                std::memcpy(field.data, &owned.timerBaseline, sizeof(owned.timerBaseline));
                owned.timerCaptured = false;
            }
        } else if (noMatchTimeout) {
            LogOnce(Option::UntimedMission, "CHALLENGE NO MATCH TIMEOUT: native GameState RemainingTime unavailable; capability inactive");
        }
    }
}
}
