#include "ChallengeSetupPolicy.hpp"
#include <cmath>

namespace f13::challenges {
namespace {
bool Valid(Option option) noexcept { return static_cast<std::size_t>(option) < OptionCount; }
bool Bounded(float value, float low, float high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}
bool LocalChallenge(const MissionScope& scope) noexcept {
    return scope.world && scope.controller && scope.attempt &&
           scope.stockSPChallenges && scope.localAuthority && !scope.networkSession;
}
}
SetupPolicy::SetupPolicy(std::string_view hash) noexcept : supported_(hash == SupportedExeSha256) {}

bool SetupPolicy::OnOfflineChallengesClicked(std::uintptr_t world, LazyLoader load, void* context) {
    if (!supported_ || !world || stage_ != Stage::Dormant || !load) return false;
    // This is the only loader call in the module, and requires the actual click.
    if (!loaded_ && !load(context)) return false;
    loaded_ = true;
    frontendWorld_ = world;
    stage_ = Stage::Selecting;
    return true;
}
bool SetupPolicy::OnChallengeAndJasonSelected(std::uintptr_t world,
                                             std::uint32_t challenge, std::uintptr_t jason) {
    if (stage_ != Stage::Selecting || !world || world != frontendWorld_ ||
        challenge < 1 || challenge > 10 || !jason) return false;
    challenge_ = challenge;
    jasonClass_ = jason;
    stage_ = Stage::Setup;
    return true;
}
bool SetupPolicy::SetOption(Option option, bool enabled) noexcept {
    if (stage_ != Stage::Setup || !Valid(option)) return false;
    draft_.enabled[static_cast<std::size_t>(option)] = enabled;
    return true;
}
bool SetupPolicy::StepOption(Option option, int direction) noexcept {
    if (direction != -1 && direction != 1) return false;
    if (stage_ != Stage::Setup || !Valid(option)) return false;
    return SetOption(option, !draft_.enabled[static_cast<std::size_t>(option)]);
}
bool SetupPolicy::ConfigureMultipliers(float attributes, float recharge,
                                      float range, float delay) noexcept {
    if (stage_ != Stage::Setup || !Bounded(attributes, 1.0f, 4.0f) ||
        !Bounded(recharge, 0.1f, 1.0f) || !Bounded(range, 0.1f, 1.0f) ||
        !Bounded(delay, 1.0f, 4.0f)) return false;
    draft_.attributeMultiplier = attributes;
    draft_.rechargeDurationMultiplier = recharge;
    draft_.perceptionRangeMultiplier = range;
    draft_.reactionDelayMultiplier = delay;
    return true;
}
bool SetupPolicy::Commit(std::uintptr_t world) noexcept {
    if (stage_ != Stage::Setup || !world || world != frontendWorld_ ||
        !challenge_ || !jasonClass_) return false;
    committed_ = draft_;
    stage_ = Stage::Committed;
    return true;
}
bool SetupPolicy::OnMissionStarted(const MissionScope& scope) noexcept {
    if (stage_ != Stage::Committed || !LocalChallenge(scope) ||
        scope.world == frontendWorld_ || scope.attempt == mission_.attempt ||
        scope.challengeSlot != challenge_ || scope.jasonClass != jasonClass_) return false;
    mission_ = scope;
    stage_ = Stage::Mission;
    return true;
}
bool SetupPolicy::MatchesMission(const MissionScope& scope) const noexcept {
    return stage_ == Stage::Mission && LocalChallenge(scope) &&
           scope.world == mission_.world && scope.controller == mission_.controller &&
           scope.attempt == mission_.attempt && scope.challengeSlot == challenge_ &&
           scope.jasonClass == jasonClass_;
}
bool SetupPolicy::IsEnabled(Option option, const MissionScope& scope,
                            const Capabilities& caps) const noexcept {
    if (!Valid(option) || !MatchesMission(scope)) return false;
    const auto index = static_cast<std::size_t>(option);
    return caps.verified[index] && committed_.enabled[index];
}
bool SetupPolicy::ShouldBlockEscape(const MissionScope& scope, const Capabilities& caps,
                                    EscapePurpose purpose) const noexcept {
    return purpose == EscapePurpose::Discretionary &&
           IsEnabled(Option::PreventEscapes, scope, caps);
}
bool SetupPolicy::ShouldSkipIntro(const MissionScope& scope, const Capabilities& caps,
                                  IntroPurpose purpose) const noexcept {
    return replay_ && purpose == IntroPurpose::PresentationOnly &&
           IsEnabled(Option::SkipReplayIntro, scope, caps);
}
bool SetupPolicy::OnMissionCompleted(const MissionScope& scope) noexcept {
    if (!MatchesMission(scope)) return false;
    stage_ = Stage::Complete; // Never generates skulls, kills, score or completion.
    return true;
}
bool SetupPolicy::OnReplayRequested(std::uintptr_t world) noexcept {
    if ((stage_ != Stage::Complete && stage_ != Stage::Mission) || !world) return false;
    frontendWorld_ = world;
    replay_ = true;
    draft_ = committed_;
    stage_ = Stage::Setup; // Replay requires explicit setup confirmation again.
    return true;
}
void SetupPolicy::Reset() noexcept {
    stage_ = Stage::Dormant;
    draft_ = Settings{};
    committed_ = Settings{};
    frontendWorld_ = 0;
    challenge_ = 0;
    jasonClass_ = 0;
    mission_ = MissionScope{};
    replay_ = false;
    // Retain loaded native adapter resources, but disarm all gameplay policy.
}
} // namespace f13::challenges
