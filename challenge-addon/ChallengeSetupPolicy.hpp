#pragma once
#include <array>
#include <cstdint>
#include <string_view>

// Candidate policy only. No DLL initializer, hooks, scans, asset loads or offsets.
// The native adapter must call this solely from verified game-thread callbacks.
namespace f13::challenges {
inline constexpr std::string_view SupportedExeSha256 =
    "941E249E4CAABF93A22FB57A6EB61D894D224C16A7464CD7451B41205436E62F";
enum class Option : std::uint8_t {
    UnlimitedKnives, FasterRecharge, BoostAttributes, StealthPractice,
    UntimedMission, SkipReplayIntro, PreventEscapes, DisableJasonBoundary, Count
};
inline constexpr std::size_t OptionCount = static_cast<std::size_t>(Option::Count);
struct Row { Option option; std::string_view label; };
inline constexpr std::array<Row, OptionCount> SetupRows{{
    {Option::UnlimitedKnives, "UNLIMITED THROWING KNIVES"},
    {Option::FasterRecharge, "FASTER ABILITY RECHARGE"},
    {Option::BoostAttributes, "BOOSTED JASON ATTRIBUTES"},
    {Option::StealthPractice, "STEALTH PRACTICE"},
    {Option::UntimedMission, "UNTIMED MISSION"},
    {Option::SkipReplayIntro, "SKIP INTRO ON REPLAY"},
    {Option::PreventEscapes, "PREVENT COUNSELOR ESCAPES"},
    {Option::DisableJasonBoundary, "DISABLE JASON BOUNDARY"}
}};
struct Settings {
    std::array<bool, OptionCount> enabled{}; // Stock defaults: all off.
    float attributeMultiplier = 2.0f;
    float rechargeDurationMultiplier = 0.5f;
    float perceptionRangeMultiplier = 0.1f;
    float reactionDelayMultiplier = 2.0f;
};
enum class Stage { Dormant, Selecting, Setup, Committed, Mission, Complete };
// Supplied by a native adapter after validating owning world/controller, exact
// SPChallenges class ancestry, local authority and absence of a net session.
struct MissionScope {
    std::uintptr_t world = 0;
    std::uintptr_t controller = 0;
    std::uint64_t attempt = 0;
    std::uint32_t challengeSlot = 0; // Adapter resolves stock ChallengeID to 1..10.
    std::uintptr_t jasonClass = 0;
    bool stockSPChallenges = false;
    bool localAuthority = false;
    bool networkSession = true; // Unknown fails closed.
};
// Each bit requires its own native seam audit and runtime validation. A string
// or property with a plausible name is not enough to mark a capability true.
struct Capabilities { std::array<bool, OptionCount> verified{}; };
enum class EscapePurpose { Unknown, Discretionary, ScriptedObjective };
enum class IntroPurpose { Unknown, PresentationOnly, ScriptedObjective };

class SetupPolicy final {
public:
    using LazyLoader = bool (*)(void*);
    explicit SetupPolicy(std::string_view exeSha256) noexcept;
    bool OnOfflineChallengesClicked(std::uintptr_t frontendWorld,
                                    LazyLoader load, void* context);
    bool OnChallengeAndJasonSelected(std::uintptr_t frontendWorld,
                                    std::uint32_t challenge, std::uintptr_t jasonClass);
    bool SetOption(Option option, bool enabled) noexcept;
    bool StepOption(Option option, int arrowDirection) noexcept;
    bool ConfigureMultipliers(float attributes, float recharge,
                              float perceptionRange, float reactionDelay) noexcept;
    bool Commit(std::uintptr_t frontendWorld) noexcept;
    bool OnMissionStarted(const MissionScope& scope) noexcept;
    bool OnReplayRequested(std::uintptr_t frontendWorld) noexcept;
    bool OnMissionCompleted(const MissionScope& scope) noexcept;
    // Always call on frontend route changes, disconnect, failed travel and
    // module unload. Native adapter separately restores owned actor overrides.
    void Reset() noexcept;
    bool IsEnabled(Option option, const MissionScope& scope,
                   const Capabilities& caps) const noexcept;
    bool ShouldBlockEscape(const MissionScope& scope, const Capabilities& caps,
                           EscapePurpose purpose) const noexcept;
    bool ShouldSkipIntro(const MissionScope& scope, const Capabilities& caps,
                         IntroPurpose purpose) const noexcept;
    Stage CurrentStage() const noexcept { return stage_; }
    const Settings& Draft() const noexcept { return draft_; }
    const Settings& Committed() const noexcept { return committed_; }
    bool IsReplay() const noexcept { return replay_; }
private:
    bool MatchesMission(const MissionScope& scope) const noexcept;
    bool supported_ = false;
    bool loaded_ = false;
    bool replay_ = false;
    Stage stage_ = Stage::Dormant;
    Settings draft_{};
    Settings committed_{};
    std::uintptr_t frontendWorld_ = 0;
    std::uint32_t challenge_ = 0;
    std::uintptr_t jasonClass_ = 0;
    MissionScope mission_{};
};
} // namespace f13::challenges
