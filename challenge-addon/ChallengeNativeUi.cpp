#include "ChallengeNativeUi.hpp"
#include <memory>

namespace f13::challenges::nativeui {
namespace {
constexpr std::string_view Click =
    "BndEvt__SinglePlayerChallengesButton_K2Node_ComponentBoundEvent_20_OnClicked__DelegateSignature";
constexpr std::array<const wchar_t*, OptionCount> Titles{{
    L"Unlimited Throwing Knives", L"Faster Ability Recharge", L"Faster Jason Movement (2x)",
    L"Stealth Practice (Low Detection)", L"No Match Timeout", L"Skip Intro on Replay", L"Prevent Counselor Escapes",
    L"Disable Jason Boundary"
}};
struct State {
    explicit State(std::uintptr_t world) : frontendWorld(world) {}
    std::uintptr_t frontendWorld;
    void* settings = nullptr;
    std::uint64_t settingsIdentity = 0;
    std::array<void*, OptionCount> rows{};
    Settings committed{};
    bool committedValid = false;
    std::uint64_t submission = 0;
    bool attempted = false;
    Capabilities capabilities{};
};
Host host{};
bool configured = false;
std::uint64_t nextSubmission = 0;
std::unique_ptr<State> state; // Allocated only by verified challenge click.
void Log(const char* message) { if (host.log) host.log(message); }
bool InFrontend(void* object) {
    return state && configured && object &&
           host.currentWorld() == state->frontendWorld && host.isLiveFrontendObject(object);
}
void AppendSetup(void* settings) {
    if (!InFrontend(settings)) return;
    const auto identity = host.objectIdentity(settings);
    if (!identity) return;
    if (state->attempted && state->settings == settings && state->settingsIdentity == identity) return;
    // A rebuilt/replayed menu gets fresh native children. Never read retained
    // combo addresses from a destroyed or recycled widget incarnation.
    state->rows.fill(nullptr);
    state->settings = nullptr;
    state->committedValid = false;
    state->settingsIdentity = identity;
    state->attempted = true;
    auto* donor = host.readWidgetObject(settings, "DifficultyComboBox");
    if (!donor) { Log("CHALLENGE SETUP: stock DifficultyComboBox missing; UI extension unavailable"); return; }
    state->settings = settings;
    state->capabilities = host.capabilities();
    for (std::size_t row = 0; row < OptionCount; ++row) {
        if (!state->capabilities.verified[row]) {
            Log("CHALLENGE SETUP: unsupported native modifier row omitted; no inert toggle exposed");
            continue;
        }
        state->rows[row] = host.appendToggle(settings, donor, Titles[row], row, 0);
        if (!state->rows[row]) {
            for (auto* added : state->rows) if (added) host.removeWidget(added);
            state->rows.fill(nullptr);
            state->settings = nullptr;
            Log("CHALLENGE SETUP: row append failed; partial rows removed");
            return;
        }
    }
    const bool header = host.labelHeader(settings, L"OFFLINE CHALLENGES SETUP");
    Log(header ? "CHALLENGE SETUP: audited native Off/On rows appended; header labeled"
               : "CHALLENGE SETUP: audited native Off/On rows appended; header unavailable");
}
void CommitSetup(void* settings) {
    if (!InFrontend(settings) || state->settings != settings ||
        host.objectIdentity(settings) != state->settingsIdentity) return;
    Settings draft{};
    for (std::size_t row = 0; row < OptionCount; ++row) {
        if (!state->capabilities.verified[row]) continue;
        const int selected = host.readToggle(state->rows[row]);
        if (selected != 0 && selected != 1) {
            state->committedValid = false;
            Log("CHALLENGE SETUP: invalid row selection; modifiers disarmed");
            return;
        }
        draft.enabled[row] = selected == 1;
    }
    state->committed = draft;
    state->committedValid = true;
    state->submission = ++nextSubmission;
    Log("CHALLENGE SETUP: audited selections captured before native challenge Start");
    Log(draft.enabled[static_cast<std::size_t>(Option::PreventEscapes)]
        ? "CHALLENGE ESCAPE: selected On (offline challenges only)"
        : "CHALLENGE ESCAPE: selected Off; native escapes unchanged");
    Log(draft.enabled[static_cast<std::size_t>(Option::DisableJasonBoundary)]
        ? "CHALLENGE JASON BOUNDARY: selected On (offline challenges only)"
        : "CHALLENGE JASON BOUNDARY: selected Off; native boundary unchanged");
}
}
bool Configure(const Host& callbacks, std::string_view hash) noexcept {
    if (state || hash != SupportedExeSha256 || !callbacks.currentWorld ||
        !callbacks.isLiveFrontendObject || !callbacks.objectIdentity || !callbacks.readWidgetObject ||
        !callbacks.appendToggle || !callbacks.readToggle || !callbacks.labelHeader ||
        !callbacks.removeWidget || !callbacks.capabilities) return false;
    host = callbacks;
    configured = true;
    return true;
}
void BeforeEvent(void* object, std::string_view klass, std::string_view event) {
    if (!configured) return;
    if (klass == "OfflinePlayMenuWidget_C" && event == Click &&
        host.isLiveFrontendObject(object)) {
        const auto world = host.currentWorld();
        if (!world) return;
        state = std::make_unique<State>(world);
        Log("CHALLENGE SETUP: lazy route armed by stock Offline Challenges click");
    } else if (klass == "SPChallengesSettingsMenuWidget_C" && event == "OnClicked_Start") {
        CommitSetup(object);
    } else if ((klass == "PickSPChallengesMapMenuWidget_C" ||
                klass == "SPChallengesSettingsMenuWidget_C") && event == "OnClicked_Back") {
        Reset();
    } else if (klass == "OfflinePlayMenuWidget_C" && event.find("OnClicked__DelegateSignature") != std::string_view::npos) {
        Reset();
    }
}
void AfterEvent(void* object, std::string_view klass, std::string_view event) {
    if (!configured || !state) return;
    if (klass == "SPChallengesSettingsMenuWidget_C" && event == "Construct") AppendSetup(object);
    // The runtime adapter must separately observe the challenge world's native
    // mission-start/completion and re-arm replay; no frontend polling is used.
}
void Reset() noexcept { state.reset(); }
bool HasCommittedSettings() noexcept { return state && state->committedValid; }
std::uint64_t CommittedSubmission() noexcept { return HasCommittedSettings() ? state->submission : 0; }
Settings GetCommittedSettings() noexcept {
    return HasCommittedSettings() ? state->committed : Settings{};
}
}
