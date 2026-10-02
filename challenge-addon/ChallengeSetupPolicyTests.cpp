#include "ChallengeSetupPolicy.hpp"
#include "ChallengeBoundaryScriptPolicy.hpp"
#include "../gameplay-backend/port/src/Game/Setup/GamepadReconnectPolicy.hpp"
#include "ChallengeRuntime.hpp"
#include "ChallengeNativeUi.hpp"
#include "ChallengeMissionBinding.hpp"
#include "ChallengeBoundaryGeometry.hpp"
#include <cstdio>
#include <limits>
#include <cstdlib>
using namespace f13::challenges;
namespace {
void Require(bool value, int line) {
    if (!value) { std::printf("FAILED line %d\n", line); std::exit(1); }
}
bool Load(void* context) { ++*static_cast<int*>(context); return true; }
struct PawnFixture {
    int knives = 3;
    std::array<float, 4> recharge{{20, 30, 40, 50}};
    std::array<float, 4> stealth{{0.2f, 0.4f, 1.0f, 1.0f}};
    std::array<float, 2> movement{{250, 400}};
} fixture;
int fixtureRemaining = 600;
std::uint64_t generation = 1;
std::uint64_t movementGeneration = 321;
std::uint64_t ObjectIdentity(void* pawn) {
    return pawn == &fixture ? generation : pawn == &fixtureRemaining ? 999 : 0;
}
bool CurrentInstance(void* pawn, std::uintptr_t world, std::uintptr_t controller) {
    return pawn == &fixture && world == 100 && controller == 200;
}
runtime::Field Field(void* pawn, const char* name, runtime::FieldKind kind, int dim) {
    if (pawn == &fixtureRemaining && std::string_view(name) == "RemainingTime" && kind == runtime::FieldKind::Int && dim == 1)
        return {&fixtureRemaining, sizeof(int), 1, 999};
    if (pawn != &fixture) return {};
    if (std::string_view(name) == "NumKnives" && kind == runtime::FieldKind::Int && dim == 1)
        return {&fixture.knives, sizeof(int), 1};
    if (std::string_view(name) == "AbilityRechargeTime" && kind == runtime::FieldKind::Float && dim == 4)
        return {fixture.recharge.data(), sizeof(float), 4};
    constexpr std::array<const char*, 4> names{{"StealthNoiseScale", "StealthRadiusScale", "AntiStealthNoiseScale", "AntiStealthRadiusScale"}};
    for (std::size_t i = 0; i < names.size(); ++i)
        if (std::string_view(name) == names[i] && kind == runtime::FieldKind::Float && dim == 1)
            return {&fixture.stealth[i], sizeof(float), 1};
    if (std::string_view(name) == "MaxRunSpeed" && kind == runtime::FieldKind::Float && dim == 1)
        return {&fixture.movement[0], sizeof(float), 1, movementGeneration};
    if (std::string_view(name) == "MaxSprintSpeed" && kind == runtime::FieldKind::Float && dim == 1)
        return {&fixture.movement[1], sizeof(float), 1, movementGeneration};
    return {};
}
int uiCalls = 0, uiRows = 0;
std::array<int, OptionCount> toggleValues{};
std::uint64_t uiGeneration = 1;
std::uintptr_t UiWorld() { ++uiCalls; return 1; }
bool UiFrontend(void*) { ++uiCalls; return true; }
std::uint64_t UiIdentity(void*) { ++uiCalls; return uiGeneration; }
void* UiWidget(void*, const char*) { ++uiCalls; return &toggleValues; }
void* UiAppend(void*, void*, const wchar_t*, std::size_t row, int initial) {
    ++uiCalls; ++uiRows; toggleValues[row] = initial; return &toggleValues[row];
}
int UiRead(void* value) { ++uiCalls; return *static_cast<int*>(value); }
bool UiHeader(void*, const wchar_t*) { ++uiCalls; return true; }
void UiRemove(void*) { ++uiCalls; }
Capabilities UiAllCapabilities() { Capabilities caps{}; caps.verified.fill(true); return caps; }
}
#define CHECK(expression) Require((expression), __LINE__)
int main() {
    f13::input::ReconnectGate reconnect;
    CHECK(reconnect.Due(0));
    reconnect.Missing(100);
    CHECK(!reconnect.Due(100) && !reconnect.Due(1099));
    CHECK(reconnect.Due(1100));
    reconnect.Missing(1100);
    CHECK(!reconnect.Due(2099) && reconnect.Due(2100));
    reconnect.Found();
    CHECK(reconnect.Due(2101) && reconnect.Due(2117)); // Connected responsiveness unchanged.
    using boundaryscript::Kind;
    using boundaryscript::MatchesStock;
    std::array<std::uint8_t, 922> outsideScript{};
    outsideScript[0]=0x4C; outsideScript[1]=0x97; outsideScript[2]=0x03;
    outsideScript[919]=0x04; outsideScript[920]=0x0B; outsideScript[921]=0x53;
    CHECK(MatchesStock(outsideScript.data(), 922, 922, Kind::Outside));
    CHECK(!MatchesStock(outsideScript.data(), 922, 921, Kind::Outside));
    CHECK(!MatchesStock(outsideScript.data(), 921, 922, Kind::Outside));
    CHECK(!MatchesStock(outsideScript.data(), 922, 922, Kind::Restart));
    CHECK(!MatchesStock(nullptr, 922, 922, Kind::Outside));
    const std::array<std::uint8_t, 2> original{{outsideScript[0], outsideScript[1]}};
    std::memcpy(outsideScript.data(), boundaryscript::VoidReturn, 2);
    CHECK(!MatchesStock(outsideScript.data(), 922, 922, Kind::Outside)); // No stacking over owned/foreign edit.
    std::memcpy(outsideScript.data(), original.data(), 2);
    CHECK(MatchesStock(outsideScript.data(), 922, 922, Kind::Outside)); // Off restores stock.
    outsideScript[2]=0x02;
    CHECK(!MatchesStock(outsideScript.data(), 922, 922, Kind::Outside)); // Wrong jump signature.
    std::array<std::uint8_t, 161> restartScript{};
    restartScript[0]=0x5F;
    restartScript[158]=0x04; restartScript[159]=0x0B; restartScript[160]=0x53;
    CHECK(MatchesStock(restartScript.data(), 161, 161, Kind::Restart));
    restartScript[160]=0;
    CHECK(!MatchesStock(restartScript.data(), 161, 161, Kind::Restart));
    using boundary::Point;
    using boundary::SegmentTouchesSphere;
    Require(SegmentTouchesSphere(Point{-1000,0,0}, Point{1000,0,0}, Point{0,0,0}, 341), __LINE__);
    Require(!SegmentTouchesSphere(Point{-1000,342,0}, Point{1000,342,0}, Point{0,0,0}, 341), __LINE__);
    Require(SegmentTouchesSphere(Point{341,0,0}, Point{341,0,0}, Point{0,0,0}, 341), __LINE__);
    Require(!SegmentTouchesSphere(Point{342,0,0}, Point{500,0,0}, Point{0,0,0}, 341), __LINE__);
    Require(!SegmentTouchesSphere(Point{0,0,0}, Point{1,0,0}, Point{0,0,0}, 0), __LINE__);
    Require(!SegmentTouchesSphere(Point{0,0,0}, Point{1,0,0}, Point{0,0,0}, std::numeric_limits<double>::quiet_NaN()), __LINE__);
    Require(!SegmentTouchesSphere(Point{0,0,0}, Point{std::numeric_limits<double>::infinity(),0,0}, Point{0,0,0}, 341), __LINE__);
    constexpr std::array<const char*, 10> stockChallenges{{
        "ChallengeC01_BrokenDown", "ChallengeC02_PackanackParty", "ChallengeC03_Stargazing",
        "ChallengeC04_PowerStruggle", "ChallengeC05_StripPoker", "ChallengeC06_PowerOut",
        "ChallengeC07_VacationParty", "ChallengeC08_Escaping", "ChallengeC09_JasonIsHere",
        "ChallengeC10_SnuggleByTheFire"
    }};
    MissionSettingsBinding binding;
    for (std::size_t i = 0; i < stockChallenges.size(); ++i) {
        const std::string name = stockChallenges[i];
        const std::string key = "/Game/Blueprints/MapRegistry/Challenges/" + name + "." + name + "_C";
        CHECK(binding.Accept(i + 1, "/Game/Maps/Single_Player/TestMission", key));
        CHECK(binding.Accept(i + 1, "/Game/Maps/Single_Player/TestMission", key)); // Same mission replay.
        CHECK(!binding.Accept(i + 1, "/Game/Maps/Single_Player/DifferentMap", key));
        CHECK(!binding.Accept(i + 1, "/Game/Maps/OfflineBots/Test", key));
        CHECK(!binding.Accept(0, "/Game/Maps/Single_Player/TestMission", key));
        const std::string next = stockChallenges[(i + 1) % stockChallenges.size()];
        const std::string nextKey = "/Game/Blueprints/MapRegistry/Challenges/" + next + "." + next + "_C";
        CHECK(!binding.Accept(i + 1, "/Game/Maps/Single_Player/TestMission", nextKey));
    }
    CHECK(!binding.Accept(99, "/Game/Maps/Single_Player/TestMission", "/Game/UnknownChallenge"));
    int loads = 0;
    SetupPolicy unsupported("unknown build");
    CHECK(!unsupported.OnOfflineChallengesClicked(1, Load, &loads));
    CHECK(loads == 0);
    SetupPolicy policy(SupportedExeSha256);
    CHECK(policy.CurrentStage() == Stage::Dormant && loads == 0);
    CHECK(!policy.SetOption(Option::PreventEscapes, true));
    CHECK(!policy.Commit(1));
    CHECK(policy.OnOfflineChallengesClicked(1, Load, &loads) && loads == 1);
    runtime::Host runtimeHost{Field, CurrentInstance, ObjectIdentity, nullptr};
    CHECK(runtime::Configure(runtimeHost, SupportedExeSha256));
    auto abilities = runtime::AuditedCapabilities();
    CHECK(abilities.verified[0] && abilities.verified[1] && abilities.verified[2] && abilities.verified[3] && abilities.verified[4]);
    CHECK(!abilities.verified[5] && !abilities.verified[6] && !abilities.verified[7]);
    Settings modifiers{};
    for (std::size_t i = 0; i < 5; ++i) modifiers.enabled[i] = true;
    modifiers.perceptionRangeMultiplier = 0.5f;
    runtime::Scope local{100, 200, &fixture, true, true, false, true};
    local.gameState = &fixtureRemaining;
    auto networked = local; networked.networkSession = true;
    runtime::Tick(networked, modifiers, true);
    CHECK(fixture.knives == 3 && fixture.recharge[0] == 20);
    auto wrongMode = local; wrongMode.stockChallengeMission = false;
    runtime::Tick(wrongMode, modifiers, true);
    CHECK(fixture.knives == 3 && fixture.recharge[0] == 20);
    runtime::Tick(local, modifiers, false);
    CHECK(fixture.knives == 3 && fixture.recharge[0] == 20);
    runtime::Tick(local, modifiers, true);
    CHECK(fixture.knives == 99 && fixture.recharge[0] == 10 && fixture.recharge[3] == 25);
    CHECK(fixture.stealth[0] == 0.1f && fixture.stealth[1] == 0.2f && fixture.stealth[2] == 0.5f);
    CHECK(fixture.movement[0] == 500 && fixture.movement[1] == 800 && fixtureRemaining == -1);
    fixture.knives = 98; // One real native throw; refill without compounding recharge.
    runtime::Tick(local, modifiers, true);
    CHECK(fixture.knives == 99 && fixture.recharge[0] == 10);
    runtime::Reset();
    CHECK(fixture.knives == 3 && fixture.recharge[0] == 20);
    CHECK(fixture.stealth[0] == 0.2f && fixture.stealth[2] == 1.0f);
    CHECK(fixture.movement[0] == 250 && fixture.movement[1] == 400 && fixtureRemaining == -1);
    runtime::Tick(local, modifiers, true);
    fixture.recharge[0] = 17; // Foreign native/script mutation must win on reset.
    runtime::Reset();
    CHECK(fixture.recharge[0] == 17);
    fixture.recharge = {{20, 30, 40, 50}};
    runtime::Tick(local, modifiers, true);
    ++generation; // New UObject reused exact old address/world/controller.
    fixture.knives = 8;
    fixture.recharge = {{80, 90, 100, 110}};
    runtime::Reset();
    CHECK(fixture.knives == 8 && fixture.recharge[0] == 80);
    fixtureRemaining = 500;
    runtime::Tick(local, modifiers, true);
    CHECK(fixtureRemaining == -1);
    modifiers.enabled[4] = false;
    runtime::Tick(local, modifiers, true);
    CHECK(fixtureRemaining == 500); // Same InProgress explicit toggle-off only.
    modifiers.enabled[4] = true;
    runtime::Tick(local, modifiers, true);
    auto complete = local; complete.inProgress = false;
    runtime::Tick(complete, modifiers, true);
    CHECK(fixtureRemaining == -1); // Native postmatch -1 never replaced by old positive count.
    runtime::Tick(local, modifiers, true);
    ++movementGeneration; // Pawn unchanged; only its native movement component was replaced.
    fixture.movement = {{300, 600}};
    runtime::Reset();
    CHECK(fixture.movement[0] == 300 && fixture.movement[1] == 600);
    runtime::Tick(local, modifiers, true);
    CHECK(fixture.movement[0] == 600 && fixture.movement[1] == 1200);
    runtime::Tick(local, modifiers, true);
    CHECK(fixture.movement[0] == 600 && fixture.movement[1] == 1200);
    runtime::Reset();
    CHECK(fixture.movement[0] == 300 && fixture.movement[1] == 600);
    nativeui::Host uiHost{};
    uiHost.currentWorld = UiWorld; uiHost.isLiveFrontendObject = UiFrontend;
    uiHost.objectIdentity = UiIdentity; uiHost.readWidgetObject = UiWidget;
    uiHost.appendToggle = UiAppend; uiHost.readToggle = UiRead; uiHost.labelHeader = UiHeader;
    uiHost.removeWidget = UiRemove; uiHost.capabilities = runtime::AuditedCapabilities;
    CHECK(nativeui::Configure(uiHost, SupportedExeSha256));
    CHECK(uiCalls == 0); // Configuration alone performs zero engine work.
    nativeui::AfterEvent(&fixture, "SPChallengesSettingsMenuWidget_C", "Construct");
    CHECK(uiCalls == 0 && uiRows == 0); // No startup/other-route initialization.
    nativeui::BeforeEvent(&fixture, "OfflinePlayMenuWidget_C",
        "BndEvt__SinglePlayerChallengesButton_K2Node_ComponentBoundEvent_20_OnClicked__DelegateSignature");
    nativeui::AfterEvent(&fixture, "SPChallengesSettingsMenuWidget_C", "Construct");
    CHECK(uiRows == 5); // Three unsupported adapter rows omitted, never inert toggles.
    nativeui::AfterEvent(&fixture, "SPChallengesSettingsMenuWidget_C", "Construct");
    CHECK(uiRows == 5); // No duplicate append from both PE and native thunk.
    toggleValues[0] = 1;
    nativeui::BeforeEvent(&fixture, "SPChallengesSettingsMenuWidget_C", "OnClicked_Start");
    CHECK(nativeui::HasCommittedSettings() && nativeui::GetCommittedSettings().enabled[0]);
    const auto firstSubmission = nativeui::CommittedSubmission();
    CHECK(firstSubmission != 0);
    CHECK(!nativeui::GetCommittedSettings().enabled[5] && !nativeui::GetCommittedSettings().enabled[6]);
    ++uiGeneration;
    nativeui::AfterEvent(&fixture, "SPChallengesSettingsMenuWidget_C", "Construct");
    CHECK(uiRows == 10 && !nativeui::HasCommittedSettings()); // Recycled widget incarnation rebuilt.
    nativeui::BeforeEvent(&fixture, "OfflinePlayMenuWidget_C",
        "BndEvt__OfflineBotsButton_K2Node_ComponentBoundEvent_49_OnClicked__DelegateSignature");
    CHECK(!nativeui::HasCommittedSettings());
    CHECK(nativeui::CommittedSubmission() == 0);
    uiHost.capabilities = UiAllCapabilities;
    CHECK(nativeui::Configure(uiHost, SupportedExeSha256));
    const int previousRows = uiRows;
    nativeui::BeforeEvent(&fixture, "OfflinePlayMenuWidget_C",
        "BndEvt__SinglePlayerChallengesButton_K2Node_ComponentBoundEvent_20_OnClicked__DelegateSignature");
    nativeui::AfterEvent(&fixture, "SPChallengesSettingsMenuWidget_C", "Construct");
    CHECK(uiRows == previousRows + static_cast<int>(OptionCount));
    const auto boundaryOption = static_cast<std::size_t>(Option::DisableJasonBoundary);
    CHECK(toggleValues[boundaryOption] == 0); // New row retains stock Off default.
    toggleValues[boundaryOption] = 1;
    toggleValues[6] = 1;
    nativeui::BeforeEvent(&fixture, "SPChallengesSettingsMenuWidget_C", "OnClicked_Start");
    CHECK(nativeui::HasCommittedSettings() && nativeui::GetCommittedSettings().enabled[6]);
    CHECK(nativeui::GetCommittedSettings().enabled[boundaryOption]);
    CHECK(nativeui::CommittedSubmission() > firstSubmission);
    toggleValues[6] = 0;
    toggleValues[boundaryOption] = 0;
    nativeui::BeforeEvent(&fixture, "SPChallengesSettingsMenuWidget_C", "OnClicked_Start");
    CHECK(!nativeui::GetCommittedSettings().enabled[6]);
    CHECK(!nativeui::GetCommittedSettings().enabled[boundaryOption]);
    nativeui::BeforeEvent(&fixture, "OfflinePlayMenuWidget_C",
        "BndEvt__OfflineBotsButton_K2Node_ComponentBoundEvent_49_OnClicked__DelegateSignature");
    CHECK(!nativeui::HasCommittedSettings());
    CHECK(!policy.OnChallengeAndJasonSelected(2, 1, 3));
    CHECK(policy.OnChallengeAndJasonSelected(1, 1, 3));
    for (const auto& row : SetupRows) CHECK(!policy.Draft().enabled[static_cast<std::size_t>(row.option)]);
    CHECK(!policy.StepOption(Option::Count, 1));
    CHECK(!policy.StepOption(Option::PreventEscapes, 0));
    for (const auto& row : SetupRows) CHECK(policy.StepOption(row.option, 1));
    CHECK(!policy.ConfigureMultipliers(2, 0.5f, 0.5f, 0.5f)); // No instant-react hardmode.
    CHECK(!policy.ConfigureMultipliers(std::numeric_limits<float>::infinity(), 0.5f, 0.5f, 2));
    CHECK(policy.ConfigureMultipliers(2, 0.5f, 0.5f, 2));
    CHECK(policy.Commit(1));
    CHECK(!policy.SetOption(Option::PreventEscapes, false));
    MissionScope mission{10, 20, 1, 1, 3, true, true, false};
    auto online = mission; online.networkSession = true;
    CHECK(!policy.OnMissionStarted(online));
    auto bots = mission; bots.stockSPChallenges = false;
    CHECK(!policy.OnMissionStarted(bots));
    auto wrongChallenge = mission; wrongChallenge.challengeSlot = 2;
    CHECK(!policy.OnMissionStarted(wrongChallenge));
    CHECK(policy.OnMissionStarted(mission));
    Capabilities caps{};
    CHECK(!policy.IsEnabled(Option::UnlimitedKnives, mission, caps));
    caps.verified.fill(true);
    CHECK(policy.IsEnabled(Option::UnlimitedKnives, mission, caps));
    CHECK(policy.IsEnabled(Option::DisableJasonBoundary, mission, caps));
    CHECK(!policy.IsEnabled(Option::DisableJasonBoundary, online, caps));
    CHECK(!policy.IsEnabled(Option::DisableJasonBoundary, bots, caps));
    CHECK(!policy.IsEnabled(Option::UnlimitedKnives, online, caps));
    CHECK(!policy.IsEnabled(Option::UnlimitedKnives, bots, caps));
    auto stale = mission; stale.attempt = 2;
    CHECK(!policy.IsEnabled(Option::UnlimitedKnives, stale, caps));
    CHECK(!policy.ShouldBlockEscape(mission, caps, EscapePurpose::Unknown));
    CHECK(!policy.ShouldBlockEscape(mission, caps, EscapePurpose::ScriptedObjective));
    CHECK(policy.ShouldBlockEscape(mission, caps, EscapePurpose::Discretionary));
    CHECK(!policy.ShouldSkipIntro(mission, caps, IntroPurpose::PresentationOnly));
    CHECK(policy.OnMissionCompleted(mission));
    CHECK(!policy.IsEnabled(Option::UntimedMission, mission, caps));
    CHECK(policy.OnReplayRequested(1));
    CHECK(policy.Commit(1));
    CHECK(!policy.OnMissionStarted(mission)); // Old attempt cannot re-arm.
    auto replay = mission; replay.world = 11; replay.attempt = 2;
    CHECK(policy.OnMissionStarted(replay));
    CHECK(!policy.ShouldSkipIntro(replay, caps, IntroPurpose::ScriptedObjective));
    CHECK(policy.ShouldSkipIntro(replay, caps, IntroPurpose::PresentationOnly));
    CHECK(!policy.IsEnabled(Option::UntimedMission, mission, caps));
    policy.Reset();
    CHECK(!policy.IsEnabled(Option::UnlimitedKnives, replay, caps));
    CHECK(policy.CurrentStage() == Stage::Dormant);
    CHECK(policy.OnOfflineChallengesClicked(1, Load, &loads) && loads == 1);
    std::puts("Challenge setup policy: all route/lifecycle/objective isolation checks passed.");
}
