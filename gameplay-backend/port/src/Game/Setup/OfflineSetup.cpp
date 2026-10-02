#include "OfflineSetup.hpp"
#include "FrozenJasonBridge.hpp"
#include "../../../../../challenge-addon/ChallengeNativeUi.hpp"
#include "../../../../../challenge-addon/ChallengeRuntime.hpp"
#include "../../../../../challenge-addon/ChallengeMissionBinding.hpp"
#include "../../../../../challenge-addon/ChallengeBoundaryGeometry.hpp"
#include "../../../../../challenge-addon/ChallengeBoundaryScriptPolicy.hpp"
#include "../StockPortMap.hpp"
#include "../Engine/Engine.hpp"
#include "../../Utils/Memory.hpp"
#include "../../Utils/Logger/Logger.hpp"
#include <Windows.h>
#include <Xinput.h>
#include <atomic>
#include <array>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cwchar>
#include <cmath>
#include "../../../vendor/minhook/include/MinHook.h"

extern "C" __declspec(noinline) bool SafeProcessEventCall(
    uintptr_t vptr,
    void* obj,
    void* func,
    void* params
);

namespace
{
    enum class SetupStage : int
    {
        Idle = 0,
        NeedPreload,
        WaitingForAssets,
        ReadyToApply,
        Sticky,
        Done,
        Failed
    };

    enum class CounselorSelectStage : int
    {
        Idle = 0,
        NeedRequest,
        Monitoring,
        Done,
        Failed
    };

    enum class SandboxCounselorSyncStage : int
    {
        Idle = 0,
        NeedRequest,
        Waiting,
        Done,
        Failed
    };

    struct UPropertyLite : UField
    {
        int32_t ArrayDim;
        int32_t ElementSize;
        uint64_t PropertyFlags;
        uint16_t RepIndex;
        uint8_t BlueprintReplicationCondition;
        uint8_t Pad_43;
        int32_t Offset_Internal;
    };

    struct RawArray
    {
        uint8_t* Data;
        int32_t Count;
        int32_t Max;
    };

    struct RawFString
    {
        wchar_t* Data;
        int32_t Count;
        int32_t Max;
    };

    // Verified in this Shipping EXE at RVA 0xBD860 and 0x2040FE:
    // GObjects indexes (index * 3) * 8, not an array of UObject pointers.
    // Treating its count as a pointer count silently omitted the final 2/3
    // of objects, including widgets loaded after the frontend had started.
    struct NativeObjectItem
    {
        UObject* Object;
        int32_t Flags;
        int32_t ClusterRootIndex;
        int32_t SerialNumber;
        int32_t Padding;
    };
    static_assert(sizeof(NativeObjectItem) == 0x18,
        "Resurrected GObjects records must have their native 24-byte stride");

    std::atomic<int> g_Stage{ (int)SetupStage::Idle };
    std::atomic<bool> g_BackgroundScanStarted{ false };
    std::atomic<uintptr_t> g_SelectionSaveObject{ 0 };
    std::atomic<uintptr_t> g_SelectionSaveClass{ 0 };
    std::atomic<uintptr_t> g_TargetJasonClass{ 0 };
#if defined(F13_BASE_GAME_PORT)
    std::atomic<bool> g_StockKillerRequestCaptured{ false };
    std::atomic<bool> g_StockCounselorPickerCaptured{ false };
    UFunction::FNativeFuncPtr g_OriginalStockJasonAcceptExec = nullptr;
    UFunction::FNativeFuncPtr g_OriginalStockJasonOfflineAcceptExec = nullptr;
    std::atomic<uintptr_t> g_StockJasonPickerClass{ 0 };
    std::atomic<int32_t> g_StockJasonPickerLoggedIndex{ -1 };
#endif
    std::atomic<int32_t> g_KillerPickOffset{ -1 };
    std::atomic<int32_t> g_CounselorPickOffset{ -1 };
    std::atomic<ULONGLONG> g_PreloadAt{ 0 };
    std::atomic<ULONGLONG> g_LastTargetScanAt{ 0 };
    std::atomic<bool> g_StockMainMenuVisible{ false };
#if defined(F13_BASE_GAME_PORT)
    std::atomic<bool> g_StockOfflinePlayVisible{ false };
    std::atomic<bool> g_StockOfflinePlayProbePending{ false };
    std::atomic<ULONGLONG> g_StockOfflinePlayProbeNextAt{ 0 };
    std::atomic<ULONGLONG> g_StockOfflinePlayFullScanAt{ 0 };
    std::atomic<uintptr_t> g_StockOfflinePlayWidget{ 0 };
    // Observe the normal OnlineFix/game login path, but never synthesize its
    // success event. The mod may remain loaded on the sign-in screen.
    std::atomic<ULONGLONG> g_StockLoginStartedAt{ 0 };
    std::atomic<ULONGLONG> g_StockLoginDiscoveryAt{ 0 };
    std::atomic<bool> g_StockBackendLoginSucceeded{ false };
    std::atomic<uintptr_t> g_StockLoginWidget{ 0 };
    std::atomic<bool> g_StockLoginFailureObserved{ false };
    std::atomic<bool> g_StockLoginTimeoutPending{ false };
    std::atomic<bool> g_StockLoginTimeoutDispatched{ false };
#endif
    std::atomic<uintptr_t> g_SelectionWorld{ 0 };
    std::atomic<uint32_t> g_StickyRewriteCount{ 0 };

    // 18K player-counselor selection remains deliberately separate from the
    // proven Jason setup state machine so both selections can coexist.
    // 18K keeps the proven stock SCPlayerState::RequestCounselorClass path with
    // the exact native 40-byte TSoftClassPtr.  No manual LoadAssetClass.
    std::atomic<int> g_CounselorStage{ (int)CounselorSelectStage::Idle };
    // 18K: once Sandbox has spawned its default local counselor (normally
    // Vanessa/Athlete), use Sandbox's own SERVER_RequestNextCharacter API
    // until SCPlayerState::SpawnedCharacterClass matches the launcher choice.
    std::atomic<int> g_SandboxCounselorSyncStage{ (int)SandboxCounselorSyncStage::Idle };
    std::atomic<uintptr_t> g_SandboxSyncWorld{ 0 };
    std::atomic<uintptr_t> g_SandboxSyncPlayerState{ 0 };
    std::atomic<int32_t> g_SandboxSpawnedClassOffset{ -1 };
    std::atomic<uint32_t> g_SandboxNextCharacterAttempts{ 0 };
    std::atomic<ULONGLONG> g_SandboxNextCharacterAt{ 0 };
    // Separate Offline Bots - Counselor architecture coordinator. 18L-AB no longer
    // uses Sandbox as the gameplay foundation; the Sandbox menu row is only the
    // temporary frontend click source and is synchronously redirected into
    // stock OfflineBots before Blueprint execution resumes.
    std::atomic<bool> g_CounselorModeAutoStartArmed{ false };
    std::atomic<bool> g_CounselorBirthComplete{ false };
    std::atomic<bool> g_CounselorMatchInProgress{ false };
    std::atomic<bool> g_CounselorJasonActive{ false };
    std::atomic<int32_t> g_CounselorBotsCreated{ 0 };
    std::atomic<uintptr_t> g_CounselorBirthPawn{ 0 };
    // 18L-AB: stock Sandbox menu row is repurposed as the counselor entry.
    std::atomic<bool> g_CounselorMenuRouteEnabled{ false };
    std::atomic<bool> g_CounselorMenuRouteLatched{ false };
    std::atomic<uintptr_t> g_CounselorMenuWorld{ 0 };
    std::atomic<uintptr_t> g_CounselorMenuObject{ 0 };
    std::atomic<uintptr_t> g_CounselorOfflineBotsModeClass{ 0 };
    std::atomic<uintptr_t> g_CounselorSandboxModeClass{ 0 };
    // The SteamRIP reference menu has a dedicated counselor row whose picker
    // sets ModeDef_OfflineBotsC_C. Keep Sandbox as a compatibility fallback
    // for the earlier two-row patched menu, but prefer the explicit mode.
    std::atomic<uintptr_t> g_CounselorReferenceModeClass{ 0 };
    std::atomic<uint32_t> g_CounselorMenuRewriteCount{ 0 };

    // 18L-AC: stop racing PendingOfflineMode from a worker thread.  Instead
    // detour the native exec wrapper for
    // ILLBackendBlueprintLibrary::RequestOfflineMode and
    // rewrite Sandbox -> OfflineBots synchronously before Blueprint execution
    // resumes.  This is route-only infrastructure; it does not alter gameplay
    // lifecycle or frozen AI.
    UFunction::FNativeFuncPtr g_OriginalRequestOfflineModeExec = nullptr;
    std::atomic<bool> g_RequestOfflineModeHookInstalled{ false };
    std::atomic<uintptr_t> g_RequestOfflineModeHookTarget{ 0 };
    std::atomic<uint32_t> g_RequestOfflineModeHookHits{ 0 };

    // The native five-entry asset reuses Resurrected's dormant VC3 row and
    // gives it the stock Offline Bots click bytecode. Mark that one event here
    // so the shared Offline Bots picker can still distinguish Counselor from
    // Jason, while the real Sandbox row remains completely stock.
    UFunction::FNativeFuncPtr g_OriginalCounselorEntryExec = nullptr;
    std::atomic<bool> g_CounselorEntryHookInstalled{ false };
    std::atomic<uintptr_t> g_CounselorEntryHookTarget{ 0 };
    std::atomic<bool> g_CounselorEntryHookPending{ false };
    std::atomic<ULONGLONG> g_CounselorEntryHookLastAttempt{ 0 };
    std::atomic<bool> g_CounselorEntryClickPending{ false };
    std::atomic<ULONGLONG> g_CounselorEntryClickedAt{ 0 };

    // Packed-install menu presentation.  The stock Resurrected asset already
    // owns a fifth VC3 button, its slot, focus wiring, visibility binding and
    // click event.  Keep the signed stock PAK untouched and expose only that
    // dormant row at runtime.
    UFunction::FNativeFuncPtr g_OriginalVC3VisibilityExec = nullptr;
    std::atomic<bool> g_VC3VisibilityHookInstalled{ false };
    std::atomic<uintptr_t> g_VC3VisibilityHookTarget{ 0 };
    // Donor loose assets contain OfflineBotsButtonC and Resurrected contains a
    // dormant VC3Button.  The untouched packed base game contains neither, so
    // its four-row menu reuses SandboxButton as the counselor surface and
    // forwards that click into the native Offline Bots flow.
    std::atomic<bool> g_UsingStockBaseCounselorRow{ false };
    std::atomic<bool> g_UsingStockBaseSandboxRow{ false };
    std::atomic<uintptr_t> g_RuntimeMenuLabeledWidget{ 0 };
    std::atomic<bool> g_FrontendReturnEventPending{ false };
    std::atomic<ULONGLONG> g_RuntimeMenuLabelLastAttempt{ 0 };
    std::atomic<bool> g_RuntimeMenuLabelFailureLogged{ false };
    std::atomic<int32_t> g_RuntimeMenuLabelFailureCode{ 0 };
    std::atomic<uintptr_t> g_RuntimeMenuOrderedWidget{ 0 };
    std::atomic<bool> g_RuntimeMenuOrderFailureLogged{ false };
    std::atomic<bool> g_RuntimeMenuOrderRequested{ false };
    std::atomic<uintptr_t> g_RuntimeMenuOrderWidget{ 0 };
    std::atomic<int32_t> g_RuntimeMenuOrderFailureCode{ 0 };

    // Integrated counselor picker.  Reuse Resurrected's native Customize
    // counselor widget so its portraits, roster navigation, profile save and
    // SCPlayerState::RequestCounselorClass behavior stay authoritative.  Only
    // the dedicated Offline Bots - Counselor row activates this continuation;
    // normal Customize usage is untouched.
    UFunction::FNativeFuncPtr g_OriginalCounselorPickerAcceptExec = nullptr;
    UFunction::FNativeFuncPtr g_OriginalCounselorPickerConstructExec = nullptr;
    std::atomic<bool> g_CounselorPickerAcceptHookInstalled{ false };
    std::atomic<bool> g_CounselorPickerConstructHookInstalled{ false };
    std::atomic<bool> g_CounselorPickerTransitionHookInstalled{ false };
    std::atomic<bool> g_CounselorPickerActive{ false };
    std::atomic<bool> g_CounselorPickerAccepted{ false };
    std::atomic<uintptr_t> g_CounselorPickerWidget{ 0 };
    std::atomic<uintptr_t> g_CounselorPickerSourceMenu{ 0 };
    std::atomic<bool> g_CounselorPickerOfflineCastPatched{ false };
    std::atomic<uintptr_t> g_CounselorPickerCastOperand{ 0 };
    std::atomic<uintptr_t> g_CounselorPickerCastOriginal{ 0 };
    // The proven loose build widens the SCGameState_Hunt import in eight
    // objective widgets.  Mirror those same import substitutions in the
    // runtime-expanded Ubergraphs so the stock signed PAK remains untouched.
    struct ObjectiveWidgetPatchSpec
    {
        const char* ClassName;
        const char* UbergraphName;
    };
    static constexpr ObjectiveWidgetPatchSpec g_ObjectiveWidgetPatchSpecs[] =
    {
        { "Objective_Boat_Widget_C", "ExecuteUbergraph_Objective_Boat_Widget" },
        { "Objective_Car_Widget_C", "ExecuteUbergraph_Objective_Car_Widget" },
        { "Objective_GrendelBridgeRepairWidget_C", "ExecuteUbergraph_Objective_GrendelBridgeRepairWidget" },
        { "Objective_GrendelPolice_Widget_C", "ExecuteUbergraph_Objective_GrendelPolice_Widget" },
        { "Objective_GrendelRadio_Widget_C", "ExecuteUbergraph_Objective_GrendelRadio_Widget" },
        { "Objective_Police_Widget_C", "ExecuteUbergraph_Objective_Police_Widget" },
        { "Objective_Radio_Widget_C", "ExecuteUbergraph_Objective_Radio_Widget" },
        { "ObjectivesWidget_C", "ExecuteUbergraph_ObjectivesWidget" }
    };
    static constexpr uint32_t g_AllObjectiveWidgetPatchBits =
        (1u << 8) - 1u;
    std::atomic<bool> g_ObjectivesGameStateCastPatched{ false };
    std::atomic<bool> g_ObjectivesGameStateCastPatchPending{ false };
    std::atomic<ULONGLONG> g_ObjectivesGameStateCastNextAttempt{ 0 };
    std::atomic<uint32_t> g_ObjectivesWidgetPatchMask{ 0 };
    std::atomic<uint32_t> g_ObjectivesWidgetPatchFailureLoggedMask{ 0 };
    // The stock setup widget labels this value "Counselors" (1-7). In
    // counselor mode it is the total counselor population, including the
    // local human, so the native selection maps to zero-to-six AI bots.
    UFunction::FNativeFuncPtr g_OriginalGameSetupStartExec = nullptr;
    std::atomic<bool> g_GameSetupStartHookInstalled{ false };
    UFunction::FNativeFuncPtr g_OriginalGameSetupConstructExec = nullptr;
    std::atomic<bool> g_GameSetupConstructHookInstalled{ false };
    UFunction::FNativeFuncPtr g_OriginalSkillComboUbergraphExec = nullptr;
    std::atomic<bool> g_SkillComboUbergraphHookInstalled{ false };
    std::atomic<uintptr_t> g_ActiveSkillCombo{ 0 };
    std::atomic<uintptr_t> g_ActiveSkillValueText{ 0 };
    std::atomic<int32_t> g_LastSkillDisplayedIndex{ -1 };
    std::atomic<ULONGLONG> g_NextSkillDisplayPumpAt{ 0 };
    std::atomic<bool> g_SkillDisplayRefreshInProgress{ false };
    std::atomic<uintptr_t> g_WeatherSettingsWidget{ 0 };
    std::atomic<uintptr_t> g_ActiveWeatherCombo{ 0 };
    std::atomic<uintptr_t> g_ActiveJasonDifficultyCombo{ 0 };
    std::atomic<uintptr_t> g_ActiveMatchLengthCombo{ 0 };
    std::atomic<uintptr_t> g_ActiveJasonUnlimitedKnivesCombo{ 0 };
    std::atomic<uintptr_t> g_ActiveJasonFastRechargeCombo{ 0 };
    std::atomic<uintptr_t> g_ActiveJasonFastMovementCombo{ 0 };
    std::atomic<bool> g_CommittedJasonUnlimitedKnives{ false };
    std::atomic<bool> g_CommittedJasonFastRecharge{ false };
    std::atomic<bool> g_CommittedJasonFastMovement{ false };
    std::atomic<uintptr_t> g_ActiveStartingWeaponCombo{ 0 };
    std::atomic<uintptr_t> g_ActiveStartingInventoryCombo{ 0 };
    std::atomic<int32_t> g_CommittedStartingWeapon{ 0 };
    std::atomic<int32_t> g_CommittedStartingInventory{ 0 };
    // Two native grants, serialized so the second never overwrites PickingItem.
    std::atomic<int32_t> g_StartingLoadoutStage{ 0 };
    std::atomic<bool> g_PrivateLobbyAuditPending{ false };
    std::atomic<bool> g_HostedArmPending{ false };
    std::atomic<bool> g_HostedJasonIntent{ false };
    std::atomic<bool> g_HostedF1Pending{ false };
    std::atomic<bool> g_HostedF3Armed{ false };
    std::atomic<bool> g_HostedF3Pending{ false };
    std::atomic<bool> g_HostedF3Busy{ false };
    std::atomic<ULONGLONG> g_HostedNextPump{ 0 };
    std::atomic<bool> g_StartingItemPending{ false };
    std::atomic<ULONGLONG> g_NextStartingItemAt{ 0 };
    std::atomic<int32_t> g_StartingItemAttempts{ 0 };
    std::atomic<uintptr_t> g_StartingItemWorld{ 0 };
    std::atomic<int32_t> g_CommittedJasonDifficulty{ -1 };
    std::atomic<int32_t> g_CommittedMatchSeconds{ -1 };
    std::atomic<uintptr_t> g_CommittedSettingsWorld{ 0 };
    std::atomic<uintptr_t> g_WeatherTravelSourceWorld{ 0 };
    std::atomic<int32_t> g_CommittedOfflineRain{ -1 };
    std::atomic<bool> g_OfflineWeatherApplyPending{ false };
    std::atomic<ULONGLONG> g_NextWeatherApplyAt{ 0 };
    std::atomic<ULONGLONG> g_WeatherApplyStartedAt{ 0 };
    std::atomic<bool> g_OfflineModifiersApplied{ false };
    // Game-thread only. Each pawn receives the threshold scale once, never
    // multiplicatively on ticks; reset when the committed world changes.
    uintptr_t g_DifficultyPawnWorld = 0;
    std::vector<uintptr_t> g_DifficultyPawns;
    std::atomic<uintptr_t> g_PrimaryKillerPlayerState{ 0 };
    std::atomic<int32_t> g_NativeSelectedCounselorTotal{ 0 };
    std::atomic<int32_t> g_NativeSelectedCounselorSkill{ -1 };
    std::atomic<int32_t> g_NativeSelectedWeather{ -1 };
    // Once the user presses Start, the picker/profile values are final.  The
    // selection save can briefly fall back to its default J5 entry while the
    // frontend tears down; do not let that stale value overwrite the choice
    // that was visible on the committed Game Setup screen.
    std::atomic<bool> g_GameSetupSelectionLocked{ false };
    using PlayTransitionOutFn = void(__fastcall*)(UObject*, UClass*);
    PlayTransitionOutFn g_OriginalPlayTransitionOut = nullptr;
    static constexpr uintptr_t RVA_ILLUserWidgetPlayTransitionOut =
#if defined(F13_BASE_GAME_PORT)
        0x2A30B0;
#else
        0x2AC730;
#endif

    static void TickNativeProfileSelection();
    static bool ClassDerivesFrom(UClass* cls, const char* baseName);
    static std::string ReadFStringAscii(UObject* object, uintptr_t offset);
#if defined(F13_BASE_GAME_PORT)
    static void InstallStockJasonPickerAcceptHooks();
    static void CaptureStockJasonPickerChoice(
        void* context,
        const char* eventName);
#endif
    static bool PatchCounselorPickerConstructPlayerStateCast(
        UClass* counselorMenuClass,
        bool applyPatch);
    static void RestoreCounselorPickerPlayerStateCast();
    static bool PatchObjectivesWidgetGameStateCast(
        const char* requestedClassName = nullptr);
    static bool InstallGameSetupStartHook(UClass* settingsClass);
    static bool InstallGameSetupConstructHook(UClass* settingsClass);
    static void CommitOfflineMatchSettings(UObject* settings);

    static void ArmObjectivesWidgetGameStatePatch()
    {
        // ObjectivesWidget is constructed before HandleMatchHasStarted on
        // the packed Counselor route. Arm the one-shot widening when the row
        // is committed so its first Construct sees SCGameState_OfflineBots.
        g_ObjectivesGameStateCastNextAttempt.store(0);
        g_ObjectivesWidgetPatchFailureLoggedMask.store(0);
        g_ObjectivesGameStateCastPatchPending.store(
            !g_ObjectivesGameStateCastPatched.load());

        // The route commit itself is a game-thread ProcessEvent.  Patch any
        // objective classes that are already resident now; classes streamed
        // later are still handled by their first exact widget event.
        if (g_ObjectivesGameStateCastPatchPending.load() &&
            PatchObjectivesWidgetGameStateCast() &&
            g_ObjectivesGameStateCastPatched.load())
        {
            g_ObjectivesGameStateCastPatchPending.store(false);
        }
    }

    // Resurrected does not register the donor's native OfflineBotsC class or
    // its OFLBC GameMode alias in a stock packed installation. Counselor mode
    // therefore travels through the stock OFLB host and is distinguished by
    // the dedicated row latch; the hooked OfflineBots lifecycle supplies the
    // donor-style counselor birth before the intro. The legacy alias state is
    // retained only for rollback compatibility and is not armed by this path.
    std::atomic<bool> g_CounselorAliasArmed{ false };
    std::atomic<bool> g_CounselorAliasRestorePending{ false };
    std::atomic<uintptr_t> g_CounselorAliasOwner{ 0 };
    std::atomic<int32_t> g_CounselorAliasOffset{ -1 };
    RawFString g_CounselorAliasOriginal{};
    wchar_t* g_CounselorAliasAllocation = nullptr;

    // Compatibility shim for the frozen AI's initial Sandbox-only cache lookup.
    std::atomic<uintptr_t> g_AISpoofObject{ 0 };
    std::atomic<uintptr_t> g_AISpoofOriginalClass{ 0 };
    std::atomic<uintptr_t> g_AISpoofController{ 0 };
    std::atomic<uintptr_t> g_AISpoofControllerOriginalClass{ 0 };
    std::atomic<int32_t> g_SelectedPlayerCounselorIndex{ 0 };
    std::atomic<uintptr_t> g_TargetPlayerCounselorClass{ 0 };
    std::atomic<uintptr_t> g_CounselorSelectionWorld{ 0 };
    std::atomic<uintptr_t> g_CounselorPlayerState{ 0 };
    std::atomic<int32_t> g_PickedCounselorClassOffset{ -1 };
    std::atomic<ULONGLONG> g_CounselorRequestSubmittedAt{ 0 };
    std::atomic<bool> g_CounselorPickedConfirmed{ false };
    std::atomic<bool> g_CounselorInitialMismatchLogged{ false };
    std::atomic<bool> g_CounselorNaturalClassScanDone{ false };
    std::atomic<bool> g_CounselorSaveScanAttempted{ false };
    std::atomic<bool> g_CounselorFallbackApplied{ false };
    std::atomic<bool> g_CounselorStockPickLogged{ false };
    std::atomic<uint32_t> g_CounselorSoftStickyRewriteCount{ 0 };
    std::atomic<uint32_t> g_CounselorClassStickyRewriteCount{ 0 };
    std::atomic<ULONGLONG> g_NextProfileSelectionSyncAt{ 0 };
    std::atomic<ULONGLONG> g_NextAutomaticRouteArmAt{ 0 };
    // Stock/base-game frontend guard.  The corrected GUObjectArray makes a
    // full exact-class lookup real work; doing it from the 50 ms worker loop
    // can starve OnlineFix while it is signing in.  Cache the two frontend
    // classes and only look for a live Offline Play widget at a bounded rate.
    std::atomic<uintptr_t> g_SCGameMenuClass{ 0 };
    std::atomic<uintptr_t> g_OfflinePlayMenuClass{ 0 };
#if defined(F13_BASE_GAME_PORT)
    std::atomic<uintptr_t> g_LoginMenuWidgetClass{ 0 };
#endif
    std::atomic<uintptr_t> g_LastProfileCounselorClass{ 0 };
    std::atomic<uintptr_t> g_LastProfileKillerClass{ 0 };
    // The Resurrected beta notice must stay alive long enough for its native
    // StartWidget to finish Construct, but it should not require a player
    // click.  The worker only discovers the live widget; its native Accept
    // event is invoked later by the existing ProcessEvent game-thread bridge.
    std::atomic<bool> g_StartSplashAcceptRequested{ false };
    std::atomic<bool> g_StartSplashFinished{ false };
    std::atomic<uintptr_t> g_StartSplashWidget{ 0 };
    std::atomic<ULONGLONG> g_NextStartSplashScanAt{ 0 };
    std::atomic<uint32_t> g_StartSplashInactiveChecks{ 0 };
    std::array<uint8_t, 40> g_SelectedPlayerCounselorSoftClass{};
    std::string g_SelectedPlayerCounselorPath;

    static const char* kPlayerCounselorClassNames[] =
    {
        "Athlete_Counselor_C",
        "Prep_Counselor_C",
        "Bookworm_Counselor_C",
        "Flirt_Counselor_C",
        "Nerd_Counselor_C",
        "Tough_Counselor_C",
        "Hero_Counselor_C",
        "Rocker_Counselor_C",
        "Jock_Counselor_C",
        "Head_Counselor_C",
        "Stoner_Counselor_C",
        "Biker_Counselor_C",
        "Shelly_Counselor_C",
        "Catty_Counselor_C",
        "Rob_Counselor_C",
        "Tina_Counselor_C",
        "Hunter_Counselor_C"
    };

    // Unified in-process setup preset (18A):
    // Packanack Small / Hard / 3 counselors / Rain.
    //
    // This deliberately starts with the exact combination already proven by
    // the external Prototype 11 helper.  The goal of 18A is to prove those
    // same writes now work entirely inside ResurrectedOfflineBots.dll.
    std::atomic<bool> g_ArmPresetRequested{ false };
    std::atomic<bool> g_ApplyGameSetupRequested{ false };
    std::atomic<bool> g_DumpCounselorRosterRequested{ false };
    std::atomic<bool> g_MapStickyActive{ false };
    std::atomic<uintptr_t> g_MapStickyWorld{ 0 };
    std::atomic<uintptr_t> g_MapStickyMenu{ 0 };
    std::atomic<uintptr_t> g_MapStickyMapClass{ 0 };
    std::atomic<uintptr_t> g_MapStickyModeClass{ 0 };
    std::atomic<uint32_t> g_MapStickyRewriteCount{ 0 };

    // Selectable setup state. Defaults match the already-proven 18A preset.
    std::atomic<int32_t> g_SelectedMapIndex{ 5 };       // Packanack Small
    std::atomic<int32_t> g_SelectedJasonIndex{ 4 };     // Jason 5
    std::atomic<int32_t> g_SelectedDifficulty{ 2 };     // Hard
    std::atomic<int32_t> g_SelectedCounselorCount{ 3 }; // 3
    std::atomic<int32_t> g_SelectedWeather{ 1 };        // Rain

    struct MapChoice
    {
        const char* Display;
        const char* ClassName;
    };

    // Start with the six map-definition class names already proven by the
    // external setup prototypes. Jarvis/Pinehurst can be added after this
    // selectable path is verified.
    static const MapChoice kMapChoices[] =
    {
        { "Crystal Lake",        "MapDef_CrystalLake_C" },
        { "Higgins Haven",       "MapDef_HigginsHaven_C" },
        { "Packanack",           "MapDef_Packanack_C" },
        { "Crystal Lake Small",  "MapDef_CrystalLake_Small_C" },
        { "Higgins Haven Small", "MapDef_HigginsHaven_Small_C" },
        { "Packanack Small",     "MapDef_Packanack_Small_C" }
    };

    static const char* kDifficultyNames[] =
    {
        "Easy", "Normal", "Hard"
    };

    static const char* kWeatherNames[] =
    {
        "Off", "Rain", "Fog"
    };

    static const wchar_t* kWeatherStrings[] =
    {
        L"off", L"rain", L"fog"
    };

    constexpr uintptr_t GOBJECTS_OFFSET = 0x030D9870;
    constexpr int32_t SoftClassSize = 40;

    static bool SafeTouchObject(UObject* obj)
    {
        if (!obj)
            return false;

        __try
        {
            volatile uintptr_t vtable = *(uintptr_t*)obj;
            return vtable != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static std::string SafeName(UObject* obj)
    {
        if (!obj || !Memory::IsReadable(obj, sizeof(UObject)) || !SafeTouchObject(obj))
            return {};

        // Do not call UObject::GetName on arbitrary scan candidates.  Some
        // non-UObject-looking memory can pass the coarse pointer checks and
        // produce noisy "Bad NameIndex" diagnostics.  Validate through the
        // known Resurrected GNames table first.
        if (!GNames || !GNames->IsValidIndex(obj->NameIndex))
            return {};

        const FNameEntry* entry = GNames->GetById(obj->NameIndex);
        if (!entry || !Memory::IsReadable(entry, 0x20))
            return {};

        return std::string(entry->AnsiName);
    }

    static std::string ReadSoftClassAssetPath(const uint8_t* softClass)
    {
        if (!softClass || !Memory::IsReadable((void*)softClass, SoftClassSize))
            return {};

        // UE4 TSoftClassPtr on this build is 40 bytes.  The persistent object
        // pointer header occupies 0x10 bytes and FSoftObjectPath::AssetPathName
        // begins at +0x10 as an FName.
        const FName* assetPathName = (const FName*)(softClass + 0x10);
        if (!GNames || !GNames->IsValidIndex(assetPathName->ComparisonIndex))
            return {};

        const FNameEntry* entry = GNames->GetById(assetPathName->ComparisonIndex);
        if (!entry || !Memory::IsReadable(entry, 0x20))
            return {};

        return std::string(entry->AnsiName);
    }

    static bool SafeNameEquals(UObject* obj, const char* wanted)
    {
        // Exact-name scans need neither allocations nor several VirtualQuery
        // calls per object. Keep the whole speculative dereference guarded;
        // loaded objects can still disappear during a worker-thread scan.
        if (!obj || !wanted || !GNames)
            return false;
        __try
        {
            const FNameEntry* entry = GNames->GetById(obj->NameIndex);
            return entry && strcmp(entry->AnsiName, wanted) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool IsStockFrontendMenuEventObject(UObject* object)
    {
        // ProcessEvent runs for every actor/widget. Reject unrelated events
        // without allocating names or querying page mappings on each call.
        __try
        {
            return object && object->Class &&
                (SafeNameEquals(reinterpret_cast<UObject*>(object->Class), "OfflinePlayMenuWidget_C") ||
                 SafeNameEquals(reinterpret_cast<UObject*>(object->Class), "MainMenuWidget_C") ||
                 SafeNameEquals(reinterpret_cast<UObject*>(object->Class), "LoginMenuWidget_C"));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool GetGObjects(NativeObjectItem** objectsOut, int32_t* countOut)
    {
        if (!objectsOut || !countOut)
            return false;

        uintptr_t moduleBase = (uintptr_t)GetModuleHandle(nullptr);
        if (!moduleBase)
            return false;

        uintptr_t base = moduleBase + GOBJECTS_OFFSET;
        if (!Memory::IsReadable((void*)base, 0x10))
            return false;

        NativeObjectItem* objects = *(NativeObjectItem**)(base + 0x00);
        int32_t count = *(int32_t*)(base + 0x0C);

        if (!objects || count <= 0 || count > 500000)
            return false;

        if (!Memory::IsReadable(objects, sizeof(NativeObjectItem) * (size_t)count))
            return false;

        *objectsOut = objects;
        *countOut = count;
        return true;
    }

    static UObject* FindObjectExact(const char* wanted)
    {
        if (!wanted)
            return nullptr;

        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* obj = objects[i].Object;
            if (SafeNameEquals(obj, wanted) &&
                Memory::IsReadable(obj, sizeof(UObject)))
                return obj;
        }

        return nullptr;
    }

    static UClass* FindClassExact(const char* wanted)
    {
        if (!wanted)
            return nullptr;

        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* obj = objects[i].Object;
            if (!SafeNameEquals(obj, wanted) ||
                !Memory::IsReadable(obj, sizeof(UObject)) || !obj->Class)
            {
                continue;
            }

            std::string meta = SafeName((UObject*)obj->Class);
            if (meta == "Class" ||
                meta.find("GeneratedClass") != std::string::npos)
            {
                return (UClass*)obj;
            }
        }

        return nullptr;
    }

    static UClass* GetCachedClass(
        std::atomic<uintptr_t>& cache,
        const char* exactName)
    {
        UClass* cached = reinterpret_cast<UClass*>(cache.load());
        if (cached && Memory::IsReadable(cached, sizeof(UClass)))
            return cached;

        UClass* resolved = FindClassExact(exactName);
        if (resolved)
            cache.store(reinterpret_cast<uintptr_t>(resolved));
        return resolved;
    }

    static UObject* FindFirstObjectAnyName(
        const char* a,
        const char* b,
        const char* c)
    {
        if (a)
        {
            if (auto* obj = FindObjectExact(a))
                return obj;
        }
        if (b)
        {
            if (auto* obj = FindObjectExact(b))
                return obj;
        }
        if (c)
        {
            if (auto* obj = FindObjectExact(c))
                return obj;
        }
        return nullptr;
    }

    struct SetupMetadataIdentity
    {
        UObject* Object = nullptr;
        int32_t Index = -1;
        int32_t Serial = 0;
        int32_t Name = -1;
        int32_t Number = 0;
        UClass* Class = nullptr;
        UObject* Outer = nullptr;
    };

    static bool ReadSetupMetadataIdentity(
        UObject* object, SetupMetadataIdentity& identity)
    {
        // GObjects membership/serial distinguishes a recycled address from
        // the previous UObject. Guard the complete speculative read because
        // the menu worker can overlap world teardown. No engine calls here.
        static const uintptr_t module =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!object || !module)
            return false;
        __try
        {
            const uintptr_t registry = module + GOBJECTS_OFFSET;
            const auto* items = *reinterpret_cast<NativeObjectItem**>(registry);
            const int32_t count = *reinterpret_cast<int32_t*>(registry + 0x0C);
            const int32_t index = object->InternalIndex;
            if (!items || count <= 0 || count > 500000 ||
                index < 0 || index >= count || items[index].Object != object)
                return false;
            identity.Object = object;
            identity.Index = index;
            identity.Serial = items[index].SerialNumber;
            identity.Name = object->NameIndex;
            identity.Number = object->NameNumber;
            identity.Class = object->Class;
            identity.Outer = object->OuterPrivate;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool SameSetupMetadataIdentity(
        const SetupMetadataIdentity& a, const SetupMetadataIdentity& b)
    {
        return a.Object == b.Object && a.Index == b.Index &&
            a.Serial == b.Serial && a.Name == b.Name &&
            a.Number == b.Number && a.Class == b.Class && a.Outer == b.Outer;
    }

    struct SetupMetadataCacheEntry
    {
        SetupMetadataIdentity World;
        SetupMetadataIdentity Class;
        SetupMetadataIdentity Field;
        UField* Children = nullptr;
        UStruct* Super = nullptr;
        std::string Name;
        bool Property = false;
    };

    struct SetupMetadataCache
    {
        std::array<SetupMetadataCacheEntry, 64> Entries{};
        size_t Next = 0;
    };

    static SetupMetadataCache& GetSetupMetadataCache()
    {
        // Both the setup worker and game-thread callbacks use these helpers.
        // Independent bounded caches avoid a lock on the event/tick path.
        static thread_local SetupMetadataCache cache;
        return cache;
    }

    static bool ReadSetupClassLinks(UClass* cls, UField*& children, UStruct*& super)
    {
        __try
        {
            children = cls->Children;
            super = cls->Super;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static UField* FindCachedSetupMetadata(
        UClass* cls, const char* name, bool property)
    {
        SetupMetadataIdentity worldIdentity{}, classIdentity{};
        UField* children = nullptr;
        UStruct* super = nullptr;
        if (!ReadSetupMetadataIdentity(
                reinterpret_cast<UObject*>(Engine::GetWorld()), worldIdentity) ||
            !ReadSetupMetadataIdentity(reinterpret_cast<UObject*>(cls), classIdentity) ||
            !ReadSetupClassLinks(cls, children, super))
            return nullptr;

        for (SetupMetadataCacheEntry& entry : GetSetupMetadataCache().Entries)
        {
            if (entry.Class.Object != reinterpret_cast<UObject*>(cls) ||
                entry.Property != property || entry.Name != name)
                continue;
            SetupMetadataIdentity fieldIdentity{};
            if (SameSetupMetadataIdentity(entry.World, worldIdentity) &&
                SameSetupMetadataIdentity(entry.Class, classIdentity) &&
                entry.Children == children && entry.Super == super &&
                ReadSetupMetadataIdentity(entry.Field.Object, fieldIdentity) &&
                SameSetupMetadataIdentity(entry.Field, fieldIdentity) &&
                Memory::IsReadable(entry.Field.Object,
                    property ? sizeof(UPropertyLite) : sizeof(UFunction)))
                return reinterpret_cast<UField*>(entry.Field.Object);
            entry.Class.Object = nullptr;
        }
        return nullptr;
    }

    static void CacheSetupMetadata(
        UClass* cls, const char* name, bool property, UField* field)
    {
        SetupMetadataCacheEntry entry;
        if (!field || !ReadSetupMetadataIdentity(
                reinterpret_cast<UObject*>(Engine::GetWorld()), entry.World) ||
            !ReadSetupMetadataIdentity(reinterpret_cast<UObject*>(cls), entry.Class) ||
            !ReadSetupMetadataIdentity(reinterpret_cast<UObject*>(field), entry.Field) ||
            !ReadSetupClassLinks(cls, entry.Children, entry.Super))
            return;
        entry.Name = name;
        entry.Property = property;
        SetupMetadataCache& cache = GetSetupMetadataCache();
        cache.Entries[cache.Next] = entry;
        cache.Next = (cache.Next + 1) % cache.Entries.size();
        // Cache successes only: widgets/classes can finish loading later in
        // the same world, so a missing field must remain retryable.
    }

    static UFunction* FindFunctionInHierarchyByName(
        UClass* cls,
        const char* targetName)
    {
        if (!cls || !targetName)
            return nullptr;

        if (UField* cached = FindCachedSetupMetadata(cls, targetName, false))
            return reinterpret_cast<UFunction*>(cached);

        for (UStruct* current = (UStruct*)cls;
            current;
            current = current->Super)
        {
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;

            UField* field = current->Children;
            int guard = 0;

            while (field && guard++ < 2048)
            {
                if (!Memory::IsReadable(field, sizeof(UField)))
                    break;

                if (field->GetName() == targetName)
                {
                    CacheSetupMetadata(cls, targetName, false, field);
                    return (UFunction*)field;
                }

                field = field->Next;
            }
        }

        return nullptr;
    }

    static UFunction* FindFunctionObjectExact(
        UClass* owner,
        const char* targetName)
    {
        if (!targetName)
            return nullptr;

        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        UFunction* nameMatch = nullptr;
        for (int32_t i = 0; i < count; ++i)
        {
            UObject* obj = objects[i].Object;
            // SafeNameEquals protects the speculative UObject dereference with
            // SEH.  Do not VirtualQuery every one of the 140,000 fixed-array
            // slots: that made the two menu lookups block OnlineFix sign-in
            // for almost 28 seconds.  Validate the full UFunction only after
            // its inexpensive exact FName match succeeds.
            if (!obj || !SafeNameEquals(obj, targetName))
            {
                continue;
            }

            if (!Memory::IsReadable(obj, sizeof(UFunction)))
                continue;

            const std::string className = SafeName((UObject*)obj->Class);
            if (className != "Function")
                continue;

            auto* function = (UFunction*)obj;
            if (owner && obj->OuterPrivate == (UObject*)owner)
                return function;

            if (!nameMatch)
                nameMatch = function;
        }

        return nameMatch;
    }

    static UPropertyLite* FindPropertyInHierarchyByName(
        UClass* cls,
        const char* targetName)
    {
        if (!cls || !targetName)
            return nullptr;

        if (UField* cached = FindCachedSetupMetadata(cls, targetName, true))
            return reinterpret_cast<UPropertyLite*>(cached);

        for (UStruct* current = (UStruct*)cls;
            current;
            current = current->Super)
        {
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;

            UField* field = current->Children;
            int guard = 0;

            while (field && guard++ < 2048)
            {
                if (!Memory::IsReadable(field, sizeof(UField)))
                    break;

                UObject* fieldClass = (UObject*)field->ClassPrivate;
                if (fieldClass && Memory::IsReadable(fieldClass, sizeof(UObject)))
                {
                    std::string typeName = SafeName(fieldClass);
                    if (typeName.find("Property") != std::string::npos &&
                        field->GetName() == targetName)
                    {
                        auto* prop = (UPropertyLite*)field;
                        if (Memory::IsReadable(prop, sizeof(UPropertyLite)))
                        {
                            CacheSetupMetadata(cls, targetName, true, field);
                            return prop;
                        }
                    }
                }

                field = field->Next;
            }
        }

        return nullptr;
    }

    static UPropertyLite* FindPropertyInStructByName(
        UStruct* owner,
        const char* targetName)
    {
        if (!owner || !targetName ||
            !Memory::IsReadable(owner, sizeof(UStruct)))
        {
            return nullptr;
        }

        UField* field = owner->Children;
        int guard = 0;
        while (field && guard++ < 256)
        {
            if (!Memory::IsReadable(field, sizeof(UField)))
                break;

            UObject* fieldClass =
                reinterpret_cast<UObject*>(field->ClassPrivate);
            if (fieldClass &&
                Memory::IsReadable(fieldClass, sizeof(UObject)) &&
                SafeName(fieldClass).find("Property") != std::string::npos &&
                field->GetName() == targetName)
            {
                auto* property = reinterpret_cast<UPropertyLite*>(field);
                return Memory::IsReadable(
                    property,
                    sizeof(UPropertyLite))
                    ? property
                    : nullptr;
            }

            field = field->Next;
        }

        return nullptr;
    }

    static UPropertyLite* FindFunctionPropertyByType(
        UFunction* function,
        const char* typeName,
        bool requireReturnProperty)
    {
        if (!function || !typeName ||
            !Memory::IsReadable(function, sizeof(UFunction)))
        {
            return nullptr;
        }

        constexpr uint64_t CPF_Parm = 0x0000000000000080ull;
        constexpr uint64_t CPF_ReturnParm = 0x0000000000000400ull;
        UField* field = function->Children;
        int guard = 0;
        while (field && guard++ < 64)
        {
            if (!Memory::IsReadable(field, sizeof(UField)))
                break;

            UObject* fieldClass =
                reinterpret_cast<UObject*>(field->ClassPrivate);
            if (fieldClass &&
                Memory::IsReadable(fieldClass, sizeof(UObject)) &&
                SafeName(fieldClass) == typeName)
            {
                auto* property = reinterpret_cast<UPropertyLite*>(field);
                if (Memory::IsReadable(property, sizeof(UPropertyLite)))
                {
                    const bool isParameter =
                        (property->PropertyFlags & CPF_Parm) != 0;
                    const bool isReturn =
                        (property->PropertyFlags & CPF_ReturnParm) != 0;
                    if (isParameter &&
                        isReturn == requireReturnProperty)
                    {
                        return property;
                    }
                }
            }

            field = field->Next;
        }

        return nullptr;
    }

    static bool InitializeCounselorActiveCamera(AActor* counselor)
    {
        if (!counselor || !counselor->Class ||
            !Memory::IsReadable(counselor, sizeof(UObject)))
        {
            return false;
        }

        // Counselor mode creates the human through RestartPlayer before the
        // OfflineBots intro. On this route Hunter_Counselor can reach its
        // first grab/pocket-knife blend without SetActiveCamera ever having
        // established a source camera; UE then chooses GrabCamera as the
        // "first" camera and leaves the player viewing an off-map transform.
        // Seed the same ordinary outdoor camera that normal counselor birth
        // uses. This changes only the character's internal active-camera
        // bookkeeping; the intro remains free to own the controller view.
        UObject* camera = nullptr;
        const char* cameraPropertyNames[] =
        {
            "OutdoorCamera",
            "OutdoorCamera_GEN_VARIABLE",
            "ChaseCamera",
            "ChaseCamera_GEN_VARIABLE"
        };
        for (const char* propertyName : cameraPropertyNames)
        {
            UPropertyLite* property = FindPropertyInHierarchyByName(
                counselor->Class,
                propertyName);
            if (!property || property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000 ||
                property->ElementSize < static_cast<int32_t>(sizeof(UObject*)))
            {
                continue;
            }

            UObject** cameraSlot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(counselor) +
                property->Offset_Internal);
            if (!Memory::IsReadable(cameraSlot, sizeof(UObject*)) ||
                !*cameraSlot ||
                !Memory::IsReadable(*cameraSlot, sizeof(UObject)))
            {
                continue;
            }

            UObject* cameraClass = reinterpret_cast<UObject*>(
                (*cameraSlot)->Class);
            if (!cameraClass ||
                !Memory::IsReadable(cameraClass, sizeof(UObject)) ||
                SafeName(cameraClass).find("Camera") == std::string::npos)
            {
                continue;
            }
            camera = *cameraSlot;
            break;
        }

        UFunction* setActiveCamera = FindFunctionInHierarchyByName(
            counselor->Class,
            "SetActiveCamera");
        if (!camera || !setActiveCamera)
        {
            Logger::Error(
                "18L-BL counselor camera seed unavailable; native camera lifecycle retained");
            return false;
        }

        constexpr uint64_t CPF_Parm = 0x0000000000000080ull;
        constexpr uint64_t CPF_ReturnParm = 0x0000000000000400ull;
        alignas(16) uint8_t params[0x100]{};
        bool parameterWritten = false;
        for (UField* field = setActiveCamera->Children;
            field;
            field = field->Next)
        {
            if (!Memory::IsReadable(field, sizeof(UField)))
                break;
            UObject* fieldClass = reinterpret_cast<UObject*>(
                field->ClassPrivate);
            if (!fieldClass ||
                !Memory::IsReadable(fieldClass, sizeof(UObject)) ||
                SafeName(fieldClass).find("ObjectProperty") ==
                    std::string::npos)
            {
                continue;
            }

            UPropertyLite* property = reinterpret_cast<UPropertyLite*>(field);
            if (!Memory::IsReadable(property, sizeof(UPropertyLite)) ||
                (property->PropertyFlags & CPF_Parm) == 0 ||
                (property->PropertyFlags & CPF_ReturnParm) != 0 ||
                property->Offset_Internal < 0 ||
                property->Offset_Internal >
                    static_cast<int32_t>(sizeof(params) - sizeof(UObject*)))
            {
                continue;
            }

            memcpy(
                params + property->Offset_Internal,
                &camera,
                sizeof(camera));
            parameterWritten = true;
            break;
        }

        if (!parameterWritten)
        {
            Logger::Error(
                "18L-BL counselor SetActiveCamera object parameter unavailable");
            return false;
        }

        const bool initialized = SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(counselor),
            counselor,
            setActiveCamera,
            params);
        Logger::Success(
            std::string("18L-BL counselor active camera seeded before intro | call=") +
            (initialized ? "true" : "false") +
            " | camera=" + SafeName(camera));
        return initialized;
    }

    static bool ResolveSelectionSaveMetadata()
    {
        if (g_SelectionSaveClass.load() &&
            g_KillerPickOffset.load() >= 0 &&
            g_CounselorPickOffset.load() >= 0)
        {
            return true;
        }

        UClass* cls = FindClassExact("SCCharacterSelectionsSaveGame");
        if (!cls || !Memory::IsReadable(cls, sizeof(UClass)))
            return false;

        auto* killerPick =
            FindPropertyInHierarchyByName(cls, "KillerPick");

        auto* counselorPick =
            FindPropertyInHierarchyByName(cls, "CounselorPick");

        if (!killerPick ||
            !counselorPick ||
            killerPick->Offset_Internal <= 0 ||
            killerPick->Offset_Internal >= 0x10000 ||
            counselorPick->Offset_Internal <= 0 ||
            counselorPick->Offset_Internal >= 0x10000)
        {
            return false;
        }

        g_SelectionSaveClass.store((uintptr_t)cls);
        g_KillerPickOffset.store(killerPick->Offset_Internal);
        g_CounselorPickOffset.store(counselorPick->Offset_Internal);

        Logger::Debug(
            "Counselor selection 18K: CounselorPick offset=0x" +
            std::to_string((uint32_t)counselorPick->Offset_Internal));

        return true;
    }

    static UObject* RawFindSelectionSaveObject(UClass* targetClass)
    {
        if (!targetClass)
            return nullptr;

        // Every live UObject is already present in GUObjectArray.  The old
        // implementation swept the process address space looking for an
        // embedded class pointer; that caused long frontend pauses and could
        // mistake arbitrary memory for a save object.  Use the verified
        // 24-byte object records and accept only an exact live class match.
        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* candidate = objects[i].Object;
            if (!candidate ||
                !Memory::IsReadable(candidate, sizeof(UObject)) ||
                candidate->Class != targetClass)
            {
                continue;
            }

            const std::string name = SafeName(candidate);
            if (!name.empty() && name.rfind("Default__", 0) != 0)
                return candidate;
        }

        return nullptr;
    }

    static bool ClassIsOrDerivesFrom(UClass* cls, UClass* target)
    {
        if (!cls || !target)
            return false;

        for (UStruct* current = (UStruct*)cls;
            current;
            current = current->Super)
        {
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;

            if ((UClass*)current == target)
                return true;
        }

        return false;
    }

    static UObject* FindFirstLiveObjectByClass(UClass* targetClass)
    {
        if (!targetClass)
            return nullptr;

        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* candidate = objects[i].Object;
            if (!candidate ||
                !Memory::IsReadable(candidate, sizeof(UObject)) ||
                !candidate->Class ||
                !ClassIsOrDerivesFrom(candidate->Class, targetClass))
            {
                continue;
            }

            const std::string name = SafeName(candidate);
            if (!name.empty() && name.rfind("Default__", 0) != 0)
                return candidate;
        }

        return nullptr;
    }

    static UObject* FindNewestLiveObjectByClass(UClass* targetClass)
    {
        if (!targetClass)
            return nullptr;

        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = count - 1; i >= 0; --i)
        {
            UObject* candidate = objects[i].Object;
            if (!candidate ||
                !Memory::IsReadable(candidate, sizeof(UObject)) ||
                !candidate->Class ||
                !ClassIsOrDerivesFrom(candidate->Class, targetClass))
            {
                continue;
            }

            const std::string name = SafeName(candidate);
            if (!name.empty() && name.rfind("Default__", 0) != 0)
                return candidate;
        }

        return nullptr;
    }

    static void TickStartSplashAutoAccept()
    {
        if (g_StartSplashFinished.load() ||
            g_StartSplashAcceptRequested.load())
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG next = g_NextStartSplashScanAt.load();
        if (next && now < next)
            return;

        g_NextStartSplashScanAt.store(now + 250);

        UClass* startWidgetClass = FindClassExact("StartWidget_C");
        if (!startWidgetClass)
            return;

        UObject* widget = FindFirstLiveObjectByClass(startWidgetClass);
        if (!widget)
            return;

        g_StartSplashWidget.store((uintptr_t)widget);
        g_StartSplashAcceptRequested.store(true);
    }

    static bool AcceptStartSplashOnGameThread()
    {
        struct IsInViewportParams
        {
            uint8_t ReturnValue;
        };

        UClass* startWidgetClass = FindClassExact("StartWidget_C");
        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!startWidgetClass || !GetGObjects(&objects, &count))
            return false;

        bool sawLiveWidget = false;
        // Frontend transitions leave older hidden StartWidget instances in
        // GUObjectArray. Search newest-to-oldest and accept the one that is
        // actually visible instead of repeatedly selecting the first stale
        // instance and giving up after twelve polls.
        for (int32_t i = count - 1; i >= 0; --i)
        {
            UObject* widget = objects[i].Object;
            if (!widget ||
                !Memory::IsReadable(widget, sizeof(UObject)) ||
                !widget->Class ||
                !ClassIsOrDerivesFrom(widget->Class, startWidgetClass))
            {
                continue;
            }

            const std::string name = SafeName(widget);
            if (name.empty() || name.rfind("Default__", 0) == 0)
                continue;

            sawLiveWidget = true;
            UFunction* isInViewport =
                FindFunctionInHierarchyByName(widget->Class, "IsInViewport");
            UFunction* accept =
                FindFunctionInHierarchyByName(widget->Class, "OnAcceptClick");
            if (!isInViewport || !accept)
                continue;

            IsInViewportParams params{};
            if (!SafeProcessEventCall(
                    (uintptr_t)widget,
                    widget,
                    isInViewport,
                    &params) ||
                !params.ReturnValue)
            {
                continue;
            }

            if (!SafeProcessEventCall(
                    (uintptr_t)widget,
                    widget,
                    accept,
                    nullptr))
            {
                return false;
            }

            g_StartSplashWidget.store((uintptr_t)widget);
            g_StartSplashFinished.store(true);
            Logger::Success(
                "STARTUP SPLASH BYPASSED: visible StartWidget accepted on the game thread");
            return true;
        }

        if (sawLiveWidget)
        {
            const uint32_t checks =
                g_StartSplashInactiveChecks.fetch_add(1) + 1;
            if (checks >= 12)
            {
                g_StartSplashFinished.store(true);
                Logger::Debug(
                    "Startup splash bypass: no visible StartWidget remains");
            }
            return true;
        }
        return false;
    }

    static UObject* GetFrontendMenuForWorld(UWorld* world)
    {
        if (!world)
            return nullptr;

        uintptr_t authorityGameModeAddress =
            (uintptr_t)world + 0xF0;

        if (!Memory::IsReadable(
            (void*)authorityGameModeAddress,
            sizeof(UObject*)))
        {
            return nullptr;
        }

        UObject* menu =
            *(UObject**)authorityGameModeAddress;

        if (!menu ||
            !Memory::IsReadable(menu, sizeof(UObject)) ||
            !menu->Class)
        {
            return nullptr;
        }

        UClass* scGameMenu = GetCachedClass(
            g_SCGameMenuClass,
            "SCGame_Menu");

        if (scGameMenu &&
            ClassIsOrDerivesFrom(menu->Class, scGameMenu))
        {
            return menu;
        }

        // Empirically the frontend authority GameMode is EntryGame_C.
        // Keep this fallback narrow rather than accepting arbitrary objects.
        std::string objectName = SafeName(menu);
        std::string className = SafeName((UObject*)menu->Class);

        if (objectName.find("EntryGame") != std::string::npos ||
            className.find("EntryGame") != std::string::npos)
        {
            return menu;
        }

        return nullptr;
    }

    static UObject* GetActiveFrontendMenu()
    {
        return GetFrontendMenuForWorld(Engine::GetWorld());
    }

    static UObject* GetActiveOfflinePlayWidget()
    {
        UClass* offlinePlayMenuClass = GetCachedClass(
            g_OfflinePlayMenuClass,
            "OfflinePlayMenuWidget_C");
        if (!offlinePlayMenuClass)
            return nullptr;

        return FindFirstLiveObjectByClass(offlinePlayMenuClass);
    }

#if defined(F13_BASE_GAME_PORT)
    static bool ConfirmVisibleStockOfflinePlayOnGameThread()
    {
        if (g_StockOfflinePlayVisible.load())
            return true;

        // A loaded/preconstructed widget is not evidence that the player has
        // left the login screen. Ask the stock menu stack which widget is
        // current on the confirmed game thread before arming or loading Hook.
        if (!GetActiveFrontendMenu())
            return false;

        UClass* targetClass = GetCachedClass(
            g_OfflinePlayMenuClass,
            "OfflinePlayMenuWidget_C");
        if (!targetClass)
            return false;

        auto isVisible = [&](UObject* widget) -> bool
        {
            if (!widget ||
                !Memory::IsReadable(widget, sizeof(UObject)) ||
                !widget->Class ||
                !ClassIsOrDerivesFrom(widget->Class, targetClass))
            {
                return false;
            }

            UFunction* getCurrentMenu = FindFunctionInHierarchyByName(
                widget->Class, "GetCurrentMenu");
            if (getCurrentMenu)
            {
                UPropertyLite* returnedMenu = FindFunctionPropertyByType(
                    getCurrentMenu, "ObjectProperty", true);
                if (returnedMenu &&
                    returnedMenu->Offset_Internal >= 0 &&
                    returnedMenu->Offset_Internal <= 56 &&
                    returnedMenu->ElementSize >=
                        static_cast<int32_t>(sizeof(UObject*)))
                {
                    uint8_t params[64]{};
                    if (SafeProcessEventCall(
                            reinterpret_cast<uintptr_t>(widget),
                            widget,
                            getCurrentMenu,
                            params))
                    {
                        UObject* current = nullptr;
                        memcpy(&current,
                            params + returnedMenu->Offset_Internal,
                            sizeof(current));
                        return current == widget;
                    }
                }

                return false;
            }

            // Some stock variants do not expose GetCurrentMenu on this
            // subclass. Their directly attached menu still needs both
            // viewport membership and non-hidden Slate visibility.
            UFunction* inViewport = FindFunctionInHierarchyByName(
                widget->Class, "IsInViewport");
            UFunction* isVisibleFunction = FindFunctionInHierarchyByName(
                widget->Class, "IsVisible");
            if (!inViewport || !isVisibleFunction)
                return false;

            struct { uint8_t ReturnValue; } viewport{};
            struct { uint8_t ReturnValue; } visible{};
            return SafeProcessEventCall(
                       reinterpret_cast<uintptr_t>(widget),
                       widget,
                       inViewport,
                       &viewport) &&
                viewport.ReturnValue != 0 &&
                SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(widget),
                    widget,
                    isVisibleFunction,
                    &visible) &&
                visible.ReturnValue != 0;
        };

        UObject* cached = reinterpret_cast<UObject*>(
            g_StockOfflinePlayWidget.load());
        if (isVisible(cached))
        {
            g_StockOfflinePlayVisible.store(true);
            Logger::Success(
                "Stock Offline Play confirmed as current menu; Counselor menu and optional Hook may activate");
            return true;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastScan = g_StockOfflinePlayFullScanAt.load();
        if (lastScan && now - lastScan < 10000)
            return false;

        g_StockOfflinePlayFullScanAt.store(now);
        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return false;

        for (int32_t i = count - 1; i >= 0; --i)
        {
            UObject* widget = objects[i].Object;
            if (!widget ||
                !Memory::IsReadable(widget, sizeof(UObject)) ||
                !widget->Class ||
                !ClassIsOrDerivesFrom(widget->Class, targetClass))
            {
                continue;
            }

            const std::string name = SafeName(widget);
            if (name.empty() || name.rfind("Default__", 0) == 0)
                continue;

            g_StockOfflinePlayWidget.store(
                reinterpret_cast<uintptr_t>(widget));
            if (isVisible(widget))
            {
                g_StockOfflinePlayVisible.store(true);
                Logger::Success(
                    "Stock Offline Play confirmed as current menu; Counselor menu and optional Hook may activate");
                return true;
            }
        }

        return false;
    }
#endif

    static int32_t ClampMapIndex(int32_t value)
    {
        constexpr int32_t count =
            (int32_t)(sizeof(kMapChoices) / sizeof(kMapChoices[0]));

        if (value < 0) return 0;
        if (value >= count) return count - 1;
        return value;
    }

    static int32_t ClampJasonIndex(int32_t value)
    {
        if (value < 0) return 0;
        if (value > 14) return 14;
        return value;
    }

    static int32_t ClampDifficulty(int32_t value)
    {
        if (value < 0) return 0;
        if (value > 2) return 2;
        return value;
    }

    static int32_t ClampCounselorCount(int32_t value)
    {
        if (value < 1) return 1;
        if (value > 7) return 7;
        return value;
    }

    static int32_t GetRequestedCounselorBotCount()
    {
        const int32_t nativeTotal =
            g_NativeSelectedCounselorTotal.load();
        if (nativeTotal >= 1 && nativeTotal <= 10)
            return (nativeTotal > 7 ? 7 : nativeTotal) - 1;

        // Compatibility fallback for the retired controller/export path,
        // whose selector was explicitly labelled "Counselor Bots".
        int32_t fallback = ClampCounselorCount(
            g_SelectedCounselorCount.load());
        return fallback > 6 ? 6 : fallback;
    }

    static int32_t ClampWeather(int32_t value)
    {
        if (value < 0) return 0;
        if (value > 2) return 2;
        return value;
    }

    static int32_t ClampPlayerCounselorIndex(int32_t value);

    static bool ArmDedicatedCounselorAlias(UClass* modeClass)
    {
        if (g_CounselorAliasArmed.load())
            return true;

        if (!modeClass ||
            !Memory::IsReadable(modeClass, sizeof(UClass)) ||
            !modeClass->DefaultObject ||
            !Memory::IsReadable(modeClass->DefaultObject, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD OFLBC route: OfflineBots mode CDO is unavailable");
            return false;
        }

        UPropertyLite* aliasProperty =
            FindPropertyInHierarchyByName(modeClass, "Alias");

        if (!aliasProperty ||
            aliasProperty->Offset_Internal <= 0 ||
            aliasProperty->Offset_Internal >= 0x10000 ||
            aliasProperty->ElementSize < (int32_t)sizeof(RawFString))
        {
            Logger::Error(
                "18L-AD OFLBC route: SCModeDefinition::Alias property is unavailable");
            return false;
        }

        UObject* modeCDO = modeClass->DefaultObject;
        RawFString* alias =
            reinterpret_cast<RawFString*>(
                reinterpret_cast<uintptr_t>(modeCDO) +
                aliasProperty->Offset_Internal);

        if (!Memory::IsReadable(alias, sizeof(RawFString)) ||
            !alias->Data ||
            alias->Count <= 0 ||
            alias->Count > 64 ||
            alias->Max < alias->Count ||
            !Memory::IsReadable(
                alias->Data,
                sizeof(wchar_t) * (size_t)alias->Count))
        {
            Logger::Error(
                "18L-AD OFLBC route: stock OfflineBots Alias FString is invalid");
            return false;
        }

        constexpr wchar_t DedicatedAlias[] = L"oflbc";
        constexpr size_t DedicatedAliasChars =
            sizeof(DedicatedAlias) / sizeof(DedicatedAlias[0]);

        wchar_t* replacement =
            reinterpret_cast<wchar_t*>(
                HeapAlloc(
                    GetProcessHeap(),
                    HEAP_ZERO_MEMORY,
                    sizeof(DedicatedAlias)));

        if (!replacement)
        {
            Logger::Error(
                "18L-AD OFLBC route: could not allocate dedicated alias storage");
            return false;
        }

        std::wmemcpy(
            replacement,
            DedicatedAlias,
            DedicatedAliasChars);

        g_CounselorAliasOriginal = *alias;
        g_CounselorAliasAllocation = replacement;
        g_CounselorAliasOwner.store((uintptr_t)modeCDO);
        g_CounselorAliasOffset.store(aliasProperty->Offset_Internal);

        alias->Data = replacement;
        alias->Count = (int32_t)DedicatedAliasChars;
        alias->Max = (int32_t)DedicatedAliasChars;

        g_CounselorAliasArmed.store(true);
        g_CounselorAliasRestorePending.store(false);

        Logger::Success(
            "18L-AD DEDICATED ROUTE ARMED: ModeDef_OfflineBots alias is temporarily OFLBC for the Counselor row only");
        return true;
    }

    static void RestoreDedicatedCounselorAlias()
    {
        if (!g_CounselorAliasArmed.load())
            return;

        UObject* owner = reinterpret_cast<UObject*>(
            g_CounselorAliasOwner.load());
        UClass* modeClass = reinterpret_cast<UClass*>(
            g_CounselorOfflineBotsModeClass.load());
        int32_t offset = g_CounselorAliasOffset.load();
        wchar_t* allocation = g_CounselorAliasAllocation;

        if (!owner ||
            !modeClass ||
            offset <= 0 ||
            !allocation ||
            !Memory::IsReadable(owner, sizeof(UObject)) ||
            !Memory::IsReadable(modeClass, sizeof(UClass)) ||
            modeClass->DefaultObject != owner)
        {
            Logger::Error(
                "18L-AO OFLBC alias restore deferred: original mode CDO identity is no longer exact");
            return;
        }

        RawFString* alias =
            reinterpret_cast<RawFString*>(
                reinterpret_cast<uintptr_t>(owner) + offset);
        if (!Memory::IsReadable(alias, sizeof(RawFString)) ||
            alias->Data != allocation ||
            alias->Count != 6 ||
            alias->Max != 6)
        {
            Logger::Error(
                "18L-AO OFLBC alias restore deferred: live Alias no longer owns our exact replacement");
            return;
        }

        *alias = g_CounselorAliasOriginal;

        // Native travel may retain a raw pointer to the alias after the mode
        // CDO is restored. Keep this 12-byte buffer valid for process lifetime
        // rather than guessing when every asynchronous reader has finished.
        g_CounselorAliasAllocation = nullptr;
        g_CounselorAliasOwner.store(0);
        g_CounselorAliasOffset.store(-1);
        g_CounselorAliasArmed.store(false);
        g_CounselorAliasRestorePending.store(false);

        g_CounselorAliasOriginal = {};
        Logger::Success(
            "18L-AO OFLBC route: restored stock OfflineBots alias on game thread after counselor birth; retired replacement retained for process lifetime");
    }

    static bool CommitCounselorPickerRoute()
    {
        UObject* menu = reinterpret_cast<UObject*>(
            g_CounselorMenuObject.load());
        UClass* offlineBotsMode = reinterpret_cast<UClass*>(
            g_CounselorOfflineBotsModeClass.load());
        constexpr uintptr_t PendingOfflineModeOffset = 0x4D0;

        if (!menu ||
            !Memory::IsReadable(menu, sizeof(UObject)) ||
            !offlineBotsMode ||
            !Memory::IsReadable(offlineBotsMode, sizeof(UClass)))
        {
            return false;
        }

        UClass** pendingMode = reinterpret_cast<UClass**>(
            reinterpret_cast<uintptr_t>(menu) +
            PendingOfflineModeOffset);
        if (!Memory::IsReadable(pendingMode, sizeof(UClass*)))
            return false;

        *pendingMode = offlineBotsMode;
        g_CounselorEntryClickPending.store(false);
        g_CounselorMenuRouteLatched.store(true);
        ArmObjectivesWidgetGameStatePatch();
        Logger::Success(
            "PACKED COUNSELOR ROUTE COMMITTED: stock OFLB host selected; dedicated in-process lifecycle latch retained for counselor birth");
        return true;
    }

    static bool CaptureNativeGameSetupCounselorCount(UObject* settings)
    {
        if (!settings ||
            !Memory::IsReadable(settings, sizeof(UObject)) ||
            !settings->Class ||
            SafeName(reinterpret_cast<UObject*>(settings->Class)) !=
                "OfflineBotsSettingsMenuWidget_C")
        {
            Logger::Error("Native Game Setup capture: Start context is not the settings widget");
            return false;
        }

        auto readComboIndex = [&](const char* propertyName,
                                  int32_t maxIndex) -> int32_t
        {
            auto* comboProperty = FindPropertyInHierarchyByName(
                settings->Class, propertyName);
            if (!comboProperty || comboProperty->Offset_Internal <= 0 ||
                comboProperty->Offset_Internal >= 0x10000)
            {
                Logger::Error(std::string(
                    "Native Game Setup capture: missing combo property ") +
                    propertyName);
                return -1;
            }
            UObject** comboAddress = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(settings) +
                comboProperty->Offset_Internal);
            if (!Memory::IsReadable(comboAddress, sizeof(UObject*)) ||
                !*comboAddress ||
                !Memory::IsReadable(*comboAddress, sizeof(UObject)) ||
                !(*comboAddress)->Class)
            {
                Logger::Error(std::string(
                    "Native Game Setup capture: combo unavailable ") +
                    propertyName);
                return -1;
            }
            UObject* combo = *comboAddress;
            auto* indexProperty = FindPropertyInHierarchyByName(
                combo->Class, "CurrentOptionIndex");
            if (!indexProperty || indexProperty->Offset_Internal <= 0 ||
                indexProperty->Offset_Internal >= 0x10000)
            {
                Logger::Error(std::string(
                    "Native Game Setup capture: CurrentOptionIndex unavailable on ") +
                    propertyName);
                return -1;
            }
            const int32_t* index = reinterpret_cast<const int32_t*>(
                reinterpret_cast<uintptr_t>(combo) +
                indexProperty->Offset_Internal);
            if (!Memory::IsReadable(index, sizeof(int32_t)) ||
                *index < 0 || *index > maxIndex)
            {
                Logger::Error(std::string(
                    "Native Game Setup capture: invalid option on ") +
                    propertyName);
                return -1;
            }
            return *index;
        };

        const int32_t counselorIndex = readComboIndex(
            "CounselorCountComboBox", g_CounselorMenuRouteLatched.load() ? 9 : 6);
        if (counselorIndex < 0)
            return false;
        const int32_t totalCounselors = counselorIndex + 1;
        g_NativeSelectedCounselorTotal.store(totalCounselors);
        if (totalCounselors > 7)
        {
            // Keep stock birth arrays within their native seven-person limit.
            // The existing bounded F3 path supplies the additional roster.
            auto* comboProperty = FindPropertyInHierarchyByName(settings->Class, "CounselorCountComboBox");
            UObject* combo = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(settings) + comboProperty->Offset_Internal);
            auto* indexProperty = FindPropertyInHierarchyByName(combo->Class, "CurrentOptionIndex");
            *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(combo) + indexProperty->Offset_Internal) = 6;
        }
        const int32_t difficulty = readComboIndex(
            "DifficultyComboBox", 2);
        // The packed base asset has no WeatherComboBox. Do not read the
        // Resurrected-only offset as if it were a live control.
        const int32_t weather = g_NativeSelectedWeather.load();
        g_NativeSelectedCounselorSkill.store(difficulty);
        g_NativeSelectedWeather.store(weather);
        Logger::Success(
            "NATIVE GAME SETUP COMMITTED: Counselors=" +
            std::to_string(totalCounselors) +
            " | human=1 | AI counselor bots=" +
            std::to_string(totalCounselors - 1) +
            " | stock difficulty index=" +
            std::to_string(difficulty) +
            " | stock weather index=" + std::to_string(weather));
        return true;
    }

    static void GameSetupStartExecHook(
        void* context,
        void* stack,
        void* result)
    {
        // Both native Offline Bots routes use this settings widget. Weather
        // is committed once before travel; counselor birth remains scoped.
        CommitOfflineMatchSettings(reinterpret_cast<UObject*>(context));
        if (g_CounselorMenuRouteLatched.load())
        {
            // Force one last synchronous read before native Start begins world
            // travel, then freeze both picker selections for this match.
            g_NextProfileSelectionSyncAt.store(0);
            TickNativeProfileSelection();
            g_GameSetupSelectionLocked.store(true);

            UClass* committedJason = reinterpret_cast<UClass*>(
                g_TargetJasonClass.load());
            Logger::Success(
                "NATIVE GAME SETUP SELECTION LOCKED: Jason=" +
                (committedJason
                    ? SafeName(reinterpret_cast<UObject*>(committedJason))
                    : std::string("NULL")));

            if (!CaptureNativeGameSetupCounselorCount(
                    reinterpret_cast<UObject*>(context)))
            {
                Logger::Error(
                    "Native Game Setup Start: selected counselor count could not be captured");
            }
        }

        g_GameSetupSelectionLocked.store(true);
        if (g_OriginalGameSetupStartExec)
            g_OriginalGameSetupStartExec(context, stack, result);
    }

    static bool InstallGameSetupStartHook(UClass* settingsClass)
    {
        if (!settingsClass ||
            !Memory::IsReadable(settingsClass, sizeof(UClass)))
        {
            return false;
        }

        UFunction* start = FindFunctionInHierarchyByName(
            settingsClass,
            "OnClicked_Start");
        if (!start ||
            !Memory::IsReadable(start, sizeof(UFunction)) ||
            !start->ExecFunction)
        {
            return false;
        }

        // Frontend travel can reconstruct/relink this Blueprint UFunction.
        // The process-wide flag is therefore not sufficient proof that the
        // currently loaded Game Setup screen still points at our wrapper.
        if (start->ExecFunction == &GameSetupStartExecHook)
        {
            g_GameSetupStartHookInstalled.store(true);
            return true;
        }

        const bool repairingRelinkedEvent =
            g_GameSetupStartHookInstalled.exchange(false);
        g_OriginalGameSetupStartExec = start->ExecFunction;
        DWORD oldProtect = 0;
        void* targetSlot = &start->ExecFunction;
        if (!VirtualProtect(
                targetSlot,
                sizeof(start->ExecFunction),
                PAGE_READWRITE,
                &oldProtect))
        {
            g_OriginalGameSetupStartExec = nullptr;
            return false;
        }

        start->ExecFunction = &GameSetupStartExecHook;
        DWORD ignoredProtect = 0;
        VirtualProtect(
            targetSlot,
            sizeof(start->ExecFunction),
            oldProtect,
            &ignoredProtect);
        g_GameSetupStartHookInstalled.store(true);
        Logger::Success(
            repairingRelinkedEvent
                ? "Native Game Setup Start hook relinked for replay: the newly selected Jason and Counselor count are authoritative"
                : "Native Game Setup Start hook installed: the in-game Counselors selection is authoritative");
        return true;
    }

#if defined(F13_BASE_GAME_PORT)
    static void CaptureStockCounselorPickerChoice(void* context)
    {
        UObject* widget = reinterpret_cast<UObject*>(context);
        if (!widget || !Memory::IsReadable(widget, sizeof(UObject)) ||
            !widget->Class ||
            SafeName(reinterpret_cast<UObject*>(widget->Class)) !=
                "Counselor_Menu_C")
            return;

        UPropertyLite* indexProperty = FindPropertyInHierarchyByName(
            widget->Class, "CurrentIndex");
        if (!indexProperty || indexProperty->Offset_Internal <= 0 ||
            indexProperty->Offset_Internal >= 0x10000 ||
            indexProperty->ElementSize != sizeof(int32_t))
            return;

        int32_t index = -1;
        void* address = reinterpret_cast<uint8_t*>(widget) +
            indexProperty->Offset_Internal;
        if (!Memory::IsReadable(address, sizeof(index)))
            return;
        std::memcpy(&index, address, sizeof(index));

        // Exact order from the stock Counselor_Menu default asset. The
        // Customize profile is not committed by OnlineFix on this route.
        static constexpr const char* stockCounselors[] = {
            "Athlete_Counselor_C", "Bookworm_Counselor_C",
            "Flirt_Counselor_C", "Head_Counselor_C",
            "Hero_Counselor_C", "Prep_Counselor_C",
            "Rocker_Counselor_C", "Jock_Counselor_C",
            "Nerd_Counselor_C", "Tough_Counselor_C",
            "Stoner_Counselor_C", "Biker_Counselor_C",
            "Shelly_Counselor_C", "Catty_Counselor_C",
            "Hunter_Counselor_C", "KM_Counselor_C",
            "Rob_Counselor_C", "Chuck_Counselor_C",
            "Jock_Wheelchair_Counselor_C"
        };
        constexpr int32_t count = static_cast<int32_t>(
            sizeof(stockCounselors) / sizeof(stockCounselors[0]));
        if (index < 0 || index >= count)
        {
            Logger::Error(
                "STOCK COUNSELOR PICKER ACCEPT: invalid CurrentIndex=" +
                std::to_string(index));
            return;
        }

        UClass* selectedClass = FindClassExact(stockCounselors[index]);
        if (!selectedClass ||
            !ClassDerivesFrom(selectedClass, "SCCounselorCharacter"))
        {
            Logger::Error(
                std::string("STOCK COUNSELOR PICKER ACCEPT: class unavailable ") +
                stockCounselors[index]);
            return;
        }

        g_TargetPlayerCounselorClass.store(
            reinterpret_cast<uintptr_t>(selectedClass));
        for (int32_t i = 0; i < static_cast<int32_t>(
                sizeof(kPlayerCounselorClassNames) /
                sizeof(kPlayerCounselorClassNames[0])); ++i)
        {
            if (std::strcmp(kPlayerCounselorClassNames[i],
                    stockCounselors[index]) == 0)
            {
                g_SelectedPlayerCounselorIndex.store(i);
                break;
            }
        }
        g_StockCounselorPickerCaptured.store(true);
        Logger::Success(std::string("MenuSelectedCounselor=") + stockCounselors[index]);
        Logger::Success(
            "STOCK COUNSELOR PICKER ACCEPT: CurrentIndex=" +
            std::to_string(index) + " selected=" +
            stockCounselors[index]);
    }
#endif

    static void CounselorPickerAcceptExecHook(
        void* context,
        void* stack,
        void* result)
    {
        const uintptr_t trackedPicker = g_CounselorPickerWidget.load();
        const bool routePickerAccept =
            g_CounselorPickerActive.load() &&
            context &&
            (!trackedPicker ||
                trackedPicker == reinterpret_cast<uintptr_t>(context));
        if (routePickerAccept)
        {
#if defined(F13_BASE_GAME_PORT)
            CaptureStockCounselorPickerChoice(context);
#endif
            // Set before entering the original Blueprint. Its synchronous
            // branch calls RequestCounselorClass/profile save and then
            // PlayTransitionOut before this wrapper returns.
            g_CounselorPickerAccepted.store(true);
            Logger::Success(
                "18L-AK COUNSELOR PICKER ACCEPT: native selection/profile path is continuing into Offline Bots setup");
        }

        if (g_OriginalCounselorPickerAcceptExec)
        {
            g_OriginalCounselorPickerAcceptExec(
                context,
                stack,
                result);
        }

        // OnClick_Accept synchronously invokes RequestCounselorClass and the
        // profile-save path before it returns. Capture that native result now,
        // rather than waiting for the 500 ms frontend poll (the route has
        // already latched by then). This makes the character the player just
        // accepted authoritative for counselor birth.
        if (routePickerAccept &&
            g_CounselorMenuRouteLatched.load())
        {
            g_NextProfileSelectionSyncAt.store(0);
            g_CounselorMenuRouteLatched.store(false);
            TickNativeProfileSelection();
            g_CounselorMenuRouteLatched.store(true);
            ArmObjectivesWidgetGameStatePatch();
            Logger::Success(
                "18L-AK COUNSELOR PICKER CAPTURED: accepted native profile selection is locked for this match");
        }
    }

    static void CounselorPickerConstructExecHook(
        void* context,
        void* stack,
        void* result)
    {
        UObject* widget = reinterpret_cast<UObject*>(context);
        const bool routePickerConstruct =
            g_CounselorPickerActive.load() &&
            widget &&
            Memory::IsReadable(widget, sizeof(UObject)) &&
            widget->Class &&
            SafeName(reinterpret_cast<UObject*>(widget->Class)) ==
                "Counselor_Menu_C";

        bool castPatched = false;
        if (routePickerConstruct)
        {
            g_CounselorPickerWidget.store(
                reinterpret_cast<uintptr_t>(widget));
            castPatched =
                PatchCounselorPickerConstructPlayerStateCast(
                    widget->Class,
                    true);
            if (!castPatched)
            {
                Logger::Error(
                    "18L-AN counselor picker: route-scoped Construct cast patch failed");
            }
        }

        if (g_OriginalCounselorPickerConstructExec)
        {
            g_OriginalCounselorPickerConstructExec(
                context,
                stack,
                result);
        }

        if (castPatched)
            RestoreCounselorPickerPlayerStateCast();

        if (routePickerConstruct &&
            g_CounselorPickerActive.load())
        {
            Logger::Success(
                "18L-AN COUNSELOR PICKER CONSTRUCTED: menu-stack-owned native selector retained with offline player-state support");
        }
    }

    static void __fastcall CounselorPickerPlayTransitionOutHook(
        UObject* widget,
        UClass* destination)
    {
#if defined(F13_BASE_GAME_PORT)
        if (g_CounselorMenuRouteLatched.load() && widget &&
            Memory::IsReadable(widget, sizeof(UObject)) &&
            widget->Class &&
            SafeName(reinterpret_cast<UObject*>(widget->Class)) ==
                "Jason_Select_Widget_C")
        {
            // The stock picker transitions synchronously before the polling
            // hook is installed. Its CurrentIndex is still live here.
            CaptureStockJasonPickerChoice(widget, "PlayTransitionOut");
        }
#endif
        if (destination &&
            Memory::IsReadable(destination, sizeof(UClass)) &&
            SafeName(reinterpret_cast<UObject*>(destination)) ==
                "OfflineBotsSettingsMenuWidget_C")
        {
            if (!InstallGameSetupConstructHook(destination))
            {
                Logger::Error(
                    "Native Game Setup Construct hook was unavailable during settings transition");
            }
            if (!InstallGameSetupStartHook(destination))
            {
                Logger::Error(
                    "Native Game Setup Start hook was unavailable during settings transition");
            }
        }

        const bool active = g_CounselorPickerActive.load();
        const uintptr_t trackedPicker = g_CounselorPickerWidget.load();
        const bool isCounselorPicker =
            active &&
            widget &&
            Memory::IsReadable(widget, sizeof(UObject)) &&
            widget->Class &&
            ((!trackedPicker &&
                SafeName(reinterpret_cast<UObject*>(widget->Class)) ==
                    "Counselor_Menu_C") ||
                trackedPicker == reinterpret_cast<uintptr_t>(widget));

        if (isCounselorPicker)
        {
            // Customizing Wave is a detour, not cancelling the offline route.
            // Keep the native picker transaction through its stock emote menu;
            // Construct will bind the returning Counselor_Menu instance.
            if (!g_CounselorPickerAccepted.load() && destination &&
                Memory::IsReadable(destination, sizeof(UClass)) &&
                SafeName(reinterpret_cast<UObject*>(destination)) == "Counselor_Emote_Menu_C")
            {
                g_CounselorPickerWidget.store(0);
                Logger::Debug("COUNSELOR PICKER: emote customization detour retained for native return");
                if (g_OriginalPlayTransitionOut)
                    g_OriginalPlayTransitionOut(widget, destination);
                return;
            }
            Logger::Debug(
                "18L-AM counselor picker transition observed | accepted=" +
                std::string(g_CounselorPickerAccepted.load() ? "true" : "false") +
                " | destination=" +
                (destination
                    ? SafeName(reinterpret_cast<UObject*>(destination))
                    : std::string("<null>")));

            if (g_CounselorPickerAccepted.load())
            {
                UClass* mapPicker =
                    FindClassExact("PickOfflineBotsMapMenuWidget_C");
                if (mapPicker && CommitCounselorPickerRoute())
                {
                    destination = mapPicker;
                    Logger::Success(
                        "18L-AK COUNSELOR PICKER COMPLETE: native Counselor_Menu -> map -> Jason -> game setup flow armed");
                }
                else
                {
                    Logger::Error(
                        "18L-AK counselor picker could not resolve the stock Offline Bots map picker/route state; preserving the native transition");
                }
            }
            else
            {
                // Back/cancel uses another event and therefore never sets the
                // accepted flag. Return through the ordinary menu stack.
                g_CounselorEntryClickPending.store(false);
                Logger::Debug(
                    "18L-AK counselor picker cancelled; returning without arming counselor gameplay");
            }

            g_CounselorPickerActive.store(false);
            g_CounselorPickerAccepted.store(false);
            g_CounselorPickerWidget.store(0);
            g_CounselorPickerSourceMenu.store(0);
        }

        if (g_OriginalPlayTransitionOut)
            g_OriginalPlayTransitionOut(widget, destination);
    }

    static bool InstallCounselorPickerHooks(UClass* counselorMenuClass)
    {
        if (!counselorMenuClass ||
            !Memory::IsReadable(counselorMenuClass, sizeof(UClass)))
        {
            return false;
        }

        if (!g_CounselorPickerConstructHookInstalled.load())
        {
            UFunction* construct = FindFunctionInHierarchyByName(
                counselorMenuClass,
                "Construct");
            if (!construct ||
                !Memory::IsReadable(construct, sizeof(UFunction)) ||
                !construct->ExecFunction)
            {
                Logger::Error(
                    "18L-AN counselor picker: Counselor_Menu.Construct is unavailable");
                return false;
            }

            g_OriginalCounselorPickerConstructExec =
                construct->ExecFunction;
            DWORD oldProtect = 0;
            void* targetSlot = &construct->ExecFunction;
            if (!VirtualProtect(
                    targetSlot,
                    sizeof(construct->ExecFunction),
                    PAGE_READWRITE,
                    &oldProtect))
            {
                g_OriginalCounselorPickerConstructExec = nullptr;
                return false;
            }
            construct->ExecFunction =
                &CounselorPickerConstructExecHook;
            DWORD ignoredProtect = 0;
            VirtualProtect(
                targetSlot,
                sizeof(construct->ExecFunction),
                oldProtect,
                &ignoredProtect);
            g_CounselorPickerConstructHookInstalled.store(true);
        }

        if (!g_CounselorPickerAcceptHookInstalled.load())
        {
            UFunction* accept = FindFunctionInHierarchyByName(
                counselorMenuClass,
                "OnClick_Accept");
            if (!accept ||
                !Memory::IsReadable(accept, sizeof(UFunction)) ||
                !accept->ExecFunction)
            {
                Logger::Error(
                    "18L-AK counselor picker: Counselor_Menu.OnClick_Accept is unavailable");
                return false;
            }

            g_OriginalCounselorPickerAcceptExec = accept->ExecFunction;
            DWORD oldProtect = 0;
            void* targetSlot = &accept->ExecFunction;
            if (!VirtualProtect(
                    targetSlot,
                    sizeof(accept->ExecFunction),
                    PAGE_READWRITE,
                    &oldProtect))
            {
                g_OriginalCounselorPickerAcceptExec = nullptr;
                return false;
            }
            accept->ExecFunction = &CounselorPickerAcceptExecHook;
            DWORD ignoredProtect = 0;
            VirtualProtect(
                targetSlot,
                sizeof(accept->ExecFunction),
                oldProtect,
                &ignoredProtect);
            g_CounselorPickerAcceptHookInstalled.store(true);
        }

        if (!g_CounselorPickerTransitionHookInstalled.load())
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(
                GetModuleHandleW(nullptr));
            LPVOID target = reinterpret_cast<LPVOID>(
                base + RVA_ILLUserWidgetPlayTransitionOut);
            if (!base || !Memory::IsReadable(target, 16))
                return false;
#if defined(F13_BASE_GAME_PORT)
            // Verified against this stock SummerCamp.exe: the same native
            // transition routine as Resurrected, relocated to RVA 0x2A30B0.
            constexpr uint8_t stockPrologue[] = {
                0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56,
                0x57, 0x41, 0x56, 0x48, 0x8B, 0xEC, 0x48, 0x83,
                0xEC, 0x60
            };
            if (!Memory::IsReadable(target, sizeof(stockPrologue)) ||
                std::memcmp(target, stockPrologue,
                    sizeof(stockPrologue)) != 0)
            {
                Logger::Error(
                    "STOCK COUNSELOR PICKER: native transition signature mismatch");
                return false;
            }
#endif

            const MH_STATUS initStatus = MH_Initialize();
            if (initStatus != MH_OK &&
                initStatus != MH_ERROR_ALREADY_INITIALIZED)
            {
                return false;
            }

            const MH_STATUS createStatus = MH_CreateHook(
                target,
                reinterpret_cast<LPVOID>(
                    &CounselorPickerPlayTransitionOutHook),
                reinterpret_cast<LPVOID*>(
                    &g_OriginalPlayTransitionOut));
            if (createStatus != MH_OK)
                return false;
            if (MH_EnableHook(target) != MH_OK)
                return false;

            g_CounselorPickerTransitionHookInstalled.store(true);
        }

        return true;
    }

    struct NativeScriptArray
    {
        uint8_t* Data;
        int32_t Count;
        int32_t Max;
    };

    static bool PatchCounselorPickerConstructPlayerStateCast(
        UClass* counselorMenuClass,
        bool applyPatch)
    {
        if (applyPatch &&
            g_CounselorPickerOfflineCastPatched.load())
            return true;

        UClass* lobbyPlayerState = FindClassExact("SCPlayerState_Lobby");
        UClass* basePlayerState = FindClassExact("SCPlayerState");
        UFunction* ubergraph = FindFunctionInHierarchyByName(
            counselorMenuClass,
            "ExecuteUbergraph_Counselor_Menu");

        if (!lobbyPlayerState ||
            !basePlayerState ||
            !ubergraph ||
            !Memory::IsReadable(ubergraph, sizeof(UFunction)))
        {
            Logger::Error(
                "18L-AM counselor picker: Ubergraph/player-state classes are unavailable");
            return false;
        }

        // UE4 Shipping UStruct stores its runtime-expanded Script TArray at
        // +0x48 in this 0x30-byte UField layout. Object operands in that
        // buffer are native UObject pointers.
        // Widen only Counselor_Menu's one DynamicCast operand from
        // SCPlayerState_Lobby to its SCPlayerState base inside the class
        // Ubergraph (Construct itself is only a small entry-point wrapper).
        // This is the in-memory equivalent of the proven one-import asset
        // change, without reserializing the fragile cooked widget package.
        constexpr uintptr_t ScriptArrayOffset = 0x48;
        auto* script = reinterpret_cast<NativeScriptArray*>(
            reinterpret_cast<uintptr_t>(ubergraph) + ScriptArrayOffset);
        if (!Memory::IsReadable(script, sizeof(NativeScriptArray)) ||
            !script->Data ||
            script->Count <= 0 ||
            script->Count > 0x10000 ||
            script->Max < script->Count ||
            !Memory::IsReadable(script->Data, script->Count))
        {
            Logger::Error(
                "18L-AM counselor picker: Ubergraph runtime Script buffer is invalid");
            return false;
        }

        uint8_t* operand = nullptr;
        uint32_t matches = 0;
        const uintptr_t lobbyPointer =
            reinterpret_cast<uintptr_t>(lobbyPlayerState);
        constexpr uint8_t ExDynamicCast = 0x2E;
        for (int32_t i = 0;
             i <= script->Count -
                1 - static_cast<int32_t>(sizeof(uintptr_t));
             ++i)
        {
            if (script->Data[i] != ExDynamicCast)
                continue;

            uintptr_t candidate = 0;
            std::memcpy(
                &candidate,
                script->Data + i + 1,
                sizeof(candidate));
            if (candidate == lobbyPointer)
            {
                operand = script->Data + i + 1;
                ++matches;
            }
        }

        if (matches != 1 || !operand)
        {
            Logger::Error(
                "18L-AM counselor picker: expected one SCPlayerState_Lobby operand in Counselor_Menu Ubergraph; found " +
                std::to_string(matches) +
                " | ScriptBytes=" + std::to_string(script->Count));
            return false;
        }

        if (!applyPatch)
        {
            Logger::Success(
                "18L-AM COUNSELOR PICKER BYTECODE VERIFIED: one 0x2E SCPlayerState_Lobby DynamicCast is ready for route-scoped widening");
            return true;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(
                operand,
                sizeof(uintptr_t),
                PAGE_READWRITE,
                &oldProtect))
        {
            Logger::Error(
                "18L-AM counselor picker: could not make the Ubergraph cast operand writable");
            return false;
        }

        const uintptr_t basePointer =
            reinterpret_cast<uintptr_t>(basePlayerState);
        g_CounselorPickerCastOperand.store(
            reinterpret_cast<uintptr_t>(operand));
        g_CounselorPickerCastOriginal.store(lobbyPointer);
        std::memcpy(operand, &basePointer, sizeof(basePointer));
        FlushInstructionCache(
            GetCurrentProcess(),
            operand,
            sizeof(basePointer));
        DWORD ignoredProtect = 0;
        VirtualProtect(
            operand,
            sizeof(uintptr_t),
            oldProtect,
            &ignoredProtect);

        g_CounselorPickerOfflineCastPatched.store(true);
        Logger::Success(
            "18L-AM COUNSELOR PICKER OFFLINE CAST READY: Counselor_Menu Ubergraph now accepts the frontend SCPlayerState at runtime");
        return true;
    }

    static void RestoreCounselorPickerPlayerStateCast()
    {
        if (!g_CounselorPickerOfflineCastPatched.exchange(false))
            return;

        uint8_t* operand = reinterpret_cast<uint8_t*>(
            g_CounselorPickerCastOperand.exchange(0));
        const uintptr_t original =
            g_CounselorPickerCastOriginal.exchange(0);
        if (!operand ||
            !original ||
            !Memory::IsReadable(operand, sizeof(original)))
        {
            Logger::Error(
                "18L-AM counselor picker: could not restore the route-scoped cast operand");
            return;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(
                operand,
                sizeof(original),
                PAGE_READWRITE,
                &oldProtect))
        {
            Logger::Error(
                "18L-AM counselor picker: cast-operand restore protection failed");
            return;
        }

        std::memcpy(operand, &original, sizeof(original));
        FlushInstructionCache(
            GetCurrentProcess(),
            operand,
            sizeof(original));
        DWORD ignoredProtect = 0;
        VirtualProtect(
            operand,
            sizeof(original),
            oldProtect,
            &ignoredProtect);
        Logger::Debug(
            "18L-AM counselor picker: stock SCPlayerState_Lobby cast restored after offline widget construction");
    }

    static bool PatchObjectivesWidgetGameStateCast(
        const char* requestedClassName)
    {
        if (g_ObjectivesGameStateCastPatched.load())
            return true;

        UClass* huntGameState = FindClassExact("SCGameState_Hunt");
        UClass* baseGameState = FindClassExact("SCGameState");
        if (!huntGameState || !baseGameState)
            return false;

        const uintptr_t huntPointer =
            reinterpret_cast<uintptr_t>(huntGameState);
        const uintptr_t basePointer =
            reinterpret_cast<uintptr_t>(baseGameState);
        constexpr uintptr_t ScriptArrayOffset = 0x48;
        bool requestedClassMatched = requestedClassName == nullptr;
        bool patchedAny = false;

        for (uint32_t specIndex = 0; specIndex < 8; ++specIndex)
        {
            const ObjectiveWidgetPatchSpec& spec =
                g_ObjectiveWidgetPatchSpecs[specIndex];
            if (requestedClassName &&
                std::strcmp(requestedClassName, spec.ClassName) != 0)
            {
                continue;
            }
            requestedClassMatched = true;

            const uint32_t bit = 1u << specIndex;
            if ((g_ObjectivesWidgetPatchMask.load() & bit) != 0)
            {
                patchedAny = true;
                continue;
            }

            UClass* widgetClass = FindClassExact(spec.ClassName);
            if (!widgetClass ||
                !Memory::IsReadable(widgetClass, sizeof(UClass)))
            {
                continue;
            }

            struct ObjectiveClassOperand
            {
                uint8_t* Address = nullptr;
                uintptr_t Original = 0;
            };

            std::vector<ObjectiveClassOperand> huntOperands;
            uint32_t baseMatches = 0;
            uint32_t functionsScanned = 0;
            std::vector<std::string> referenceFunctions;
            // UE4.18 EX_DynamicCast is 0x2E. 0x16 is
            // EX_EndFunctionParms, so the old scan could never find the
            // widgets' SCGameState_Hunt casts and left objectives blank.
            constexpr uint8_t ExDynamicCast = 0x2E;
            for (UField* field = widgetClass->Children;
                 field && Memory::IsReadable(field, sizeof(UField));
                 field = field->Next)
            {
                // The cast sites used by the pause objectives are in helper
                // UFunctions directly owned by each generated widget class
                // (FindPhone, FindCBRadio, IsStarted, and similar), not only
                // in ExecuteUbergraph.  Restrict enumeration to exact,
                // directly-owned UFunctions so no unrelated scripts change.
                if (field->OuterPrivate !=
                        reinterpret_cast<UField*>(widgetClass) ||
                    !field->ClassPrivate ||
                    !Memory::IsReadable(field->ClassPrivate, sizeof(UObject)) ||
                    SafeName(reinterpret_cast<UObject*>(field->ClassPrivate)) !=
                        "Function")
                {
                    continue;
                }

                UFunction* function = reinterpret_cast<UFunction*>(field);
                auto* script = reinterpret_cast<NativeScriptArray*>(
                    reinterpret_cast<uintptr_t>(function) + ScriptArrayOffset);
                if (!Memory::IsReadable(script, sizeof(NativeScriptArray)) ||
                    !script->Data || script->Count <= 0 ||
                    script->Count > 0x40000 || script->Max < script->Count ||
                    !Memory::IsReadable(script->Data, script->Count))
                {
                    continue;
                }

                ++functionsScanned;
                bool functionHasReference = false;
                for (int32_t i = 0;
                     i <= script->Count - 1 -
                        static_cast<int32_t>(sizeof(uintptr_t));
                     ++i)
                {
                    if (script->Data[i] != ExDynamicCast)
                        continue;

                    uintptr_t candidate = 0;
                    std::memcpy(
                        &candidate,
                        script->Data + i + 1,
                        sizeof(candidate));
                    if (candidate == huntPointer)
                    {
                        huntOperands.push_back(
                            { script->Data + i + 1, candidate });
                        functionHasReference = true;
                    }
                    else if (candidate == basePointer)
                    {
                        ++baseMatches;
                        functionHasReference = true;
                    }
                }

                if (functionHasReference)
                    referenceFunctions.push_back(SafeName(
                        reinterpret_cast<UObject*>(function)));
            }

            // The loose development build already contains the same import
            // widening. Count that widget as complete without writing it.
            if (huntOperands.empty() && baseMatches > 0)
            {
                g_ObjectivesWidgetPatchMask.fetch_or(bit);
                patchedAny = true;
                Logger::Success(
                    std::string("PACKED OBJECTIVES READY: ") +
                    spec.ClassName +
                    " helper functions already accept SCGameState modes | functions=" +
                    std::to_string(functionsScanned) +
                    " | references=" + std::to_string(baseMatches));
                continue;
            }

            if (huntOperands.empty())
            {
                if ((g_ObjectivesWidgetPatchFailureLoggedMask.fetch_or(bit) & bit) == 0)
                {
                    Logger::Error(
                        std::string("PACKED OBJECTIVES: SCGameState_Hunt runtime reference not found | class=") +
                        spec.ClassName +
                        " | helperFunctionsScanned=" +
                        std::to_string(functionsScanned));
                }
                continue;
            }

            std::vector<ObjectiveClassOperand> patchedOperands;
            bool writeFailed = false;
            for (const ObjectiveClassOperand& operand : huntOperands)
            {
                DWORD oldProtect = 0;
                if (!VirtualProtect(
                        operand.Address,
                        sizeof(basePointer),
                        PAGE_READWRITE,
                        &oldProtect))
                {
                    writeFailed = true;
                    break;
                }

                std::memcpy(
                    operand.Address,
                    &basePointer,
                    sizeof(basePointer));
                FlushInstructionCache(
                    GetCurrentProcess(),
                    operand.Address,
                    sizeof(basePointer));
                DWORD ignoredProtect = 0;
                VirtualProtect(
                    operand.Address,
                    sizeof(basePointer),
                    oldProtect,
                    &ignoredProtect);
                patchedOperands.push_back(operand);
            }

            if (writeFailed)
            {
                for (const ObjectiveClassOperand& patched : patchedOperands)
                {
                    DWORD rollbackProtect = 0;
                    if (VirtualProtect(
                            patched.Address,
                            sizeof(patched.Original),
                            PAGE_READWRITE,
                            &rollbackProtect))
                    {
                        std::memcpy(
                            patched.Address,
                            &patched.Original,
                            sizeof(patched.Original));
                        DWORD ignoredProtect = 0;
                        VirtualProtect(
                            patched.Address,
                            sizeof(patched.Original),
                            rollbackProtect,
                            &ignoredProtect);
                    }
                }
                continue;
            }

            g_ObjectivesWidgetPatchMask.fetch_or(bit);
            patchedAny = true;
            Logger::Success(
                std::string("PACKED OBJECTIVES READY: ") +
                spec.ClassName +
                " helper SCGameState_Hunt references widened to SCGameState | functions=" +
                std::to_string(functionsScanned) +
                " | referenceFunctions=" +
                std::to_string(referenceFunctions.size()) +
                " | references=" + std::to_string(patchedOperands.size()));
        }

        if (!requestedClassMatched)
            return false;

        if ((g_ObjectivesWidgetPatchMask.load() &
             g_AllObjectiveWidgetPatchBits) ==
            g_AllObjectiveWidgetPatchBits)
        {
            g_ObjectivesGameStateCastPatched.store(true);
            Logger::Success(
                "PACKED OBJECTIVES COMPLETE: all eight proven loose-build widget imports are active in memory");
        }
        return patchedAny;
    }

    static void RestoreCounselorEntryFocus(UObject* sourceMenu)
    {
        if (!sourceMenu ||
            !Memory::IsReadable(sourceMenu, sizeof(UObject)) ||
            !sourceMenu->Class)
        {
            return;
        }

        UFunction* setKeyboardFocus = FindFunctionInHierarchyByName(
            sourceMenu->Class,
            "SetKeyboardFocus");
        if (setKeyboardFocus &&
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(sourceMenu),
                sourceMenu,
                setKeyboardFocus,
                nullptr))
        {
            Logger::Debug(
                "18L-AM counselor picker: restored keyboard/controller focus to Offline Play");
        }
    }

    static bool OpenIntegratedCounselorPicker(UObject* sourceMenu)
    {
        if (!sourceMenu ||
            !Memory::IsReadable(sourceMenu, sizeof(UObject)) ||
            !sourceMenu->Class)
        {
            return false;
        }

        UClass* counselorMenuClass = FindClassExact("Counselor_Menu_C");
        if (!counselorMenuClass ||
            !InstallCounselorPickerHooks(counselorMenuClass))
        {
            Logger::Error(
                "18L-AK counselor picker: native Counselor_Menu_C is not loaded/usable");
            return false;
        }

        if (!g_OriginalPlayTransitionOut ||
            !PatchCounselorPickerConstructPlayerStateCast(
                counselorMenuClass,
                false))
        {
            Logger::Error(
                "18L-AN counselor picker: transition/cast surface is unavailable");
            return false;
        }

        g_CounselorPickerAccepted.store(false);
        g_CounselorPickerWidget.store(0);
        g_CounselorPickerSourceMenu.store(
            reinterpret_cast<uintptr_t>(sourceMenu));
        g_CounselorPickerActive.store(true);
        g_OriginalPlayTransitionOut(
            sourceMenu,
            counselorMenuClass);

        Logger::Success(
            "18L-AN COUNSELOR PICKER TRANSITION REQUESTED: current Offline Play menu is transitioning to the native selector through the authoritative menu stack");
        return true;
    }

    struct PersistentRuntimeText
    {
        uint8_t* Value = nullptr;
        int32_t Size = 0;
    };

    static bool CreatePersistentRuntimeText(
        const wchar_t* text,
        PersistentRuntimeText* output)
    {
        if (!text || !output)
        {
            g_RuntimeMenuLabelFailureCode.store(101);
            return false;
        }

        UObject* textLibrary =
            FindObjectExact("Default__KismetTextLibrary");
        if (!textLibrary || !textLibrary->Class)
        {
            g_RuntimeMenuLabelFailureCode.store(102);
            return false;
        }

        UFunction* convert = FindFunctionInHierarchyByName(
            textLibrary->Class,
            "Conv_StringToText");
        auto* inputProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(convert),
            "InString");
        if (!inputProperty)
        {
            inputProperty = FindFunctionPropertyByType(
                convert,
                "StrProperty",
                false);
        }
        auto* returnProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(convert),
            "ReturnValue");
        if (!returnProperty)
        {
            returnProperty = FindFunctionPropertyByType(
                convert,
                "TextProperty",
                true);
        }

        if (!convert || !inputProperty || !returnProperty ||
            inputProperty->Offset_Internal < 0 ||
            returnProperty->Offset_Internal < 0 ||
            inputProperty->ElementSize < (int32_t)sizeof(RawFString) ||
            returnProperty->ElementSize < 16 ||
            returnProperty->ElementSize > 64)
        {
            g_RuntimeMenuLabelFailureCode.store(
                !convert ? 103 :
                (!inputProperty ? 104 :
                (!returnProperty ? 105 : 106)));
            return false;
        }

        size_t parameterBytes = static_cast<size_t>(convert->Size);
        const size_t requiredBytes = std::max(
            static_cast<size_t>(inputProperty->Offset_Internal) +
                sizeof(RawFString),
            static_cast<size_t>(returnProperty->Offset_Internal) +
                static_cast<size_t>(returnProperty->ElementSize));
        parameterBytes = std::max(parameterBytes, requiredBytes);
        if (parameterBytes == 0 || parameterBytes > 0x400)
        {
            g_RuntimeMenuLabelFailureCode.store(107);
            return false;
        }

        // FText contains shared state. Keep this tiny one-time parameter block
        // alive for the process lifetime so the copied menu label can never
        // reference a temporary conversion result.
        auto* params = reinterpret_cast<uint8_t*>(HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            parameterBytes));
        if (!params)
        {
            g_RuntimeMenuLabelFailureCode.store(108);
            return false;
        }

        RawFString input{};
        input.Data = const_cast<wchar_t*>(text);
        input.Count = static_cast<int32_t>(wcslen(text)) + 1;
        input.Max = input.Count;
        memcpy(
            params + inputProperty->Offset_Internal,
            &input,
            sizeof(input));

        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(textLibrary),
                textLibrary,
                convert,
                params))
        {
            HeapFree(GetProcessHeap(), 0, params);
            g_RuntimeMenuLabelFailureCode.store(109);
            return false;
        }

        output->Value = params + returnProperty->Offset_Internal;
        output->Size = returnProperty->ElementSize;
        g_RuntimeMenuLabelFailureCode.store(0);
        return true;
    }

    static bool SetWideMenuButtonText(
        UObject* button,
        const wchar_t* text,
        PersistentRuntimeText* cachedText,
        const char* textWidgetProperty = "TitleText")
    {
        if (!button || !button->Class || !text || !cachedText)
        {
            g_RuntimeMenuLabelFailureCode.store(201);
            return false;
        }

        if (!cachedText->Value &&
            !CreatePersistentRuntimeText(text, cachedText))
        {
            if (!g_RuntimeMenuLabelFailureCode.load())
                g_RuntimeMenuLabelFailureCode.store(203);
            return false;
        }

        if (!cachedText->Value || cachedText->Size <= 0)
        {
            g_RuntimeMenuLabelFailureCode.store(204);
            return false;
        }

        auto* titleTextProperty = FindPropertyInHierarchyByName(
            button->Class,
            textWidgetProperty);
        if (!titleTextProperty ||
            titleTextProperty->Offset_Internal <= 0 ||
            titleTextProperty->Offset_Internal >= 0x10000)
        {
            g_RuntimeMenuLabelFailureCode.store(206);
            return false;
        }

        UObject** titleTextSlot = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(button) +
            titleTextProperty->Offset_Internal);
        if (!Memory::IsReadable(titleTextSlot, sizeof(UObject*)) ||
            !*titleTextSlot || !(*titleTextSlot)->Class)
        {
            g_RuntimeMenuLabelFailureCode.store(207);
            return false;
        }

        UObject* titleText = *titleTextSlot;
        UFunction* setText = FindFunctionInHierarchyByName(
            titleText->Class,
            "SetText");
        auto* inputTextProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(setText),
            "InText");
        if (!inputTextProperty)
        {
            inputTextProperty = FindFunctionPropertyByType(
                setText,
                "TextProperty",
                false);
        }
        if (!setText || !inputTextProperty ||
            inputTextProperty->Offset_Internal < 0 ||
            inputTextProperty->ElementSize != cachedText->Size)
        {
            g_RuntimeMenuLabelFailureCode.store(
                !setText ? 208 :
                (!inputTextProperty ? 209 : 210));
            return false;
        }

        size_t parameterBytes = std::max(
            static_cast<size_t>(setText->Size),
            static_cast<size_t>(inputTextProperty->Offset_Internal) +
                static_cast<size_t>(inputTextProperty->ElementSize));
        if (parameterBytes == 0 || parameterBytes > 0x200)
        {
            g_RuntimeMenuLabelFailureCode.store(211);
            return false;
        }

        std::vector<uint8_t> params(parameterBytes, 0);
        memcpy(
            params.data() + inputTextProperty->Offset_Internal,
            cachedText->Value,
            cachedText->Size);
        const bool set = SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(titleText),
            titleText,
            setText,
            params.data());
        g_RuntimeMenuLabelFailureCode.store(set ? 0 : 212);
        return set;
    }

    static void RefreshCounselorSkillValue(UObject* combo)
    {
        if (!combo || !combo->Class ||
            !Memory::IsReadable(combo->Class, sizeof(UClass)) ||
            SafeName(reinterpret_cast<UObject*>(combo->Class)) !=
                "SettingsOptionComboBoxWidget_C" ||
            reinterpret_cast<uintptr_t>(combo) !=
                g_ActiveSkillCombo.load() ||
            g_GameSetupSelectionLocked.load())
            return;

        auto* indexProperty = FindPropertyInHierarchyByName(
            combo->Class, "CurrentOptionIndex");
        if (!indexProperty || indexProperty->Offset_Internal <= 0 ||
            indexProperty->Offset_Internal >= 0x10000)
            return;
        const int32_t* index = reinterpret_cast<const int32_t*>(
            reinterpret_cast<uintptr_t>(combo) +
            indexProperty->Offset_Internal);
        if (!Memory::IsReadable(index, sizeof(int32_t)) ||
            *index < 0 || *index > 2)
            return;
        if (g_LastSkillDisplayedIndex.load() == *index)
            return;

        bool expected = false;
        if (!g_SkillDisplayRefreshInProgress.compare_exchange_strong(
                expected, true))
            return;

        // Only the visible text changes. The stock Easy/Normal/Hard option
        // array and the selected index still drive native bot difficulty.
        static PersistentRuntimeText values[3]{};
        static constexpr const wchar_t* labels[3] =
            { L"Low", L"Medium", L"High" };
        const int32_t selected = *index;
        const bool updated = SetWideMenuButtonText(
            combo, labels[selected], &values[selected], "ValueText");
        if (updated)
        {
            g_LastSkillDisplayedIndex.store(selected);
            Logger::Success(
                std::string("Game Setup Counselor Bot Skill displayed=") +
                (selected == 0 ? "Low" :
                    selected == 1 ? "Medium" : "High") +
                " | nativeIndex=" + std::to_string(selected));
        }
        g_SkillDisplayRefreshInProgress.store(false);
    }

    static void SkillComboUbergraphExecHook(
        void* context, void* stack, void* result)
    {
        if (g_OriginalSkillComboUbergraphExec)
            g_OriginalSkillComboUbergraphExec(context, stack, result);
        RefreshCounselorSkillValue(reinterpret_cast<UObject*>(context));
    }

    static bool InstallSkillComboUbergraphHook(UObject* combo)
    {
        if (!combo || !combo->Class)
            return false;
        UFunction* graph = FindFunctionInHierarchyByName(
            combo->Class,
            "ExecuteUbergraph_SettingsOptionComboBoxWidget");
        if (!graph || !graph->ExecFunction)
            return false;
        if (graph->ExecFunction == &SkillComboUbergraphExecHook)
            return true;

        DWORD oldProtect = 0;
        void* slot = &graph->ExecFunction;
        if (!VirtualProtect(slot, sizeof(graph->ExecFunction),
                PAGE_READWRITE, &oldProtect))
            return false;
        g_OriginalSkillComboUbergraphExec = graph->ExecFunction;
        graph->ExecFunction = &SkillComboUbergraphExecHook;
        DWORD ignoredProtect = 0;
        VirtualProtect(slot, sizeof(graph->ExecFunction),
            oldProtect, &ignoredProtect);
        g_SkillComboUbergraphHookInstalled.store(true);
        return true;
    }

    static UObject* ReadRuntimeWidgetObject(UObject* object, const char* name)
    {
        if (!object || !Memory::IsReadable(object, sizeof(UObject)) || !object->Class)
            return nullptr;
        UPropertyLite* property = FindPropertyInHierarchyByName(object->Class, name);
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 || property->ElementSize != sizeof(UObject*))
            return nullptr;
        UObject** field = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(object) + property->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(UObject*)) || !*field ||
            !Memory::IsReadable(*field, sizeof(UObject)))
            return nullptr;
        return *field;
    }

    static bool WriteRuntimeCallArgument(UFunction* function, uint8_t* params,
        const char* name, const void* value, size_t bytes)
    {
        if (!function || function->Size <= 0 || function->Size > 0x200)
            return false;
        UPropertyLite* property = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(function), name);
        if (!property || property->Offset_Internal < 0 ||
            property->ElementSize != bytes ||
            static_cast<size_t>(property->Offset_Internal) + bytes > function->Size)
            return false;
        memcpy(params + property->Offset_Internal, value, bytes);
        return true;
    }

    static UObject* ReadRuntimeCallObjectResult(UFunction* function, uint8_t* params)
    {
        UPropertyLite* property = FindFunctionPropertyByType(function, "ObjectProperty", true);
        if (!property || property->Offset_Internal < 0 ||
            property->ElementSize != sizeof(UObject*) ||
            property->Offset_Internal + sizeof(UObject*) > function->Size ||
            function->Size > 0x200)
            return nullptr;
        UObject* result = nullptr;
        memcpy(&result, params + property->Offset_Internal, sizeof(result));
        return result && Memory::IsReadable(result, sizeof(UObject)) ? result : nullptr;
    }

    static int32_t ReadRuntimeComboIndex(UObject* combo, int32_t maxIndex = 2)
    {
        if (!combo || !Memory::IsReadable(combo, sizeof(UObject)) || !combo->Class)
            return -1;
        UPropertyLite* property = FindPropertyInHierarchyByName(combo->Class, "CurrentOptionIndex");
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 || property->ElementSize != sizeof(int32_t))
            return -1;
        const int32_t* index = reinterpret_cast<const int32_t*>(
            reinterpret_cast<uintptr_t>(combo) + property->Offset_Internal);
        return Memory::IsReadable(index, sizeof(*index)) && *index >= 0 && *index <= maxIndex
            ? *index : -1;
    }

    static UObject* CreateOfflineSettingsCombo(UObject* settings, UObject* skillCombo,
        const wchar_t* titleText, const wchar_t* const names[3], int32_t initialIndex,
        PersistentRuntimeText* titleStorage, int32_t optionCount = 3)
    {
#if defined(F13_BASE_GAME_PORT)
        // Append one native combo. Never clear/rebuild the panel or transplant
        // a cooked Resurrected widget with incompatible property offsets.
        UObject* panel = ReadRuntimeWidgetObject(
            ReadRuntimeWidgetObject(skillCombo, "Slot"), "Parent");
        UObject* library = FindObjectExact("Default__WidgetBlueprintLibrary");
        if (!panel || !panel->Class || !ClassDerivesFrom(panel->Class, "VerticalBox") ||
            !library || !library->Class || !skillCombo || !skillCombo->Class)
        {
            Logger::Error("Offline weather row: native panel/Create library unavailable");
            return nullptr;
        }
        UFunction* create = FindFunctionInHierarchyByName(library->Class, "Create");
        UFunction* setOptions = FindFunctionInHierarchyByName(skillCombo->Class, "SetOptions");
        UFunction* add = FindFunctionInHierarchyByName(panel->Class, "AddChildToVerticalBox");
        if (!create || !setOptions || !add)
        {
            Logger::Error("Offline weather row: required native widget methods missing");
            return nullptr;
        }
        alignas(16) uint8_t createParams[0x200]{};
        UObject* player = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
        UClass* widgetClass = skillCombo->Class;
        if (!WriteRuntimeCallArgument(create, createParams, "WorldContextObject", &settings, sizeof(settings)) ||
            !WriteRuntimeCallArgument(create, createParams, "WidgetType", &widgetClass, sizeof(widgetClass)) ||
            !WriteRuntimeCallArgument(create, createParams, "OwningPlayer", &player, sizeof(player)) ||
            !SafeProcessEventCall(reinterpret_cast<uintptr_t>(library), library, create, createParams))
        {
            Logger::Error("Offline weather row: reflected Create arguments/call rejected");
            return nullptr;
        }
        UObject* weatherCombo = ReadRuntimeCallObjectResult(create, createParams);
        if (!weatherCombo)
            return nullptr;
        // Attaching can construct/rebuild the Slate widget. Initialize its
        // independent option state only AFTER that lifecycle has completed.
        alignas(16) uint8_t addParams[0x200]{};
        if (!WriteRuntimeCallArgument(add, addParams, "Content", &weatherCombo, sizeof(weatherCombo)) ||
            !SafeProcessEventCall(reinterpret_cast<uintptr_t>(panel), panel, add, addParams) ||
            !ReadRuntimeCallObjectResult(add, addParams))
        {
            Logger::Error("Offline settings row: append failed; existing controls preserved");
            return nullptr;
        }

        // SetOptions bytecode copies Items into its own FString array. These
        // stack descriptors are input only, never installed as owned storage.
        if (optionCount < 1 || optionCount > 64) return nullptr;
        RawFString items[64]{};
        for (int i = 0; i < optionCount; ++i)
        {
            items[i].Data = const_cast<wchar_t*>(names[i]);
            items[i].Count = items[i].Max = static_cast<int32_t>(wcslen(names[i])) + 1;
        }
        RawArray options{ reinterpret_cast<uint8_t*>(items), optionCount, optionCount };
        alignas(16) uint8_t optionParams[0x200]{};
        if (!WriteRuntimeCallArgument(setOptions, optionParams, "Items", &options, sizeof(options)) ||
            !WriteRuntimeCallArgument(setOptions, optionParams, "SelectedIndex", &initialIndex, sizeof(initialIndex)) ||
            !SafeProcessEventCall(reinterpret_cast<uintptr_t>(weatherCombo), weatherCombo, setOptions, optionParams))
        {
            Logger::Error("Offline weather row: native SetOptions rejected");
            return nullptr;
        }
        const bool labeled = SetWideMenuButtonText(weatherCombo, titleText, titleStorage);
        Logger::Success(std::string("Offline settings native combo appended | title=") +
            (labeled ? "ready" : "missing") + " | options=" + std::to_string(optionCount) + " | input retained");
        return weatherCombo;
#else
        return nullptr;
#endif
    }


    // Stock Challenge addon host. No challenge object lookup, native hook,
    // row creation or runtime capability initialization occurs before its click.
    static uintptr_t ShippingAddress(uintptr_t rva);
    static bool MatchesBytes(uintptr_t address, const uint8_t* expected, size_t size);
    std::atomic<bool> g_ChallengeRouteActive{ false };
    bool g_ChallengeHostConfigured = false;
    UFunction::FNativeFuncPtr g_ChallengeConstructOriginal = nullptr;
    UFunction::FNativeFuncPtr g_ChallengeStartOriginal = nullptr;
    UFunction::FNativeFuncPtr g_ChallengeBackOriginal = nullptr;
    UFunction::FNativeFuncPtr g_ChallengeEntryOriginal = nullptr;
    UFunction::FNativeFuncPtr g_ChallengePickerConstructOriginal = nullptr;
    UFunction::FNativeFuncPtr g_ChallengePickerBackOriginal = nullptr;
    uintptr_t g_ChallengeFrontendWorld = 0;
    UFunction::FNativeFuncPtr g_ChallengeLobbyConstructOriginal = nullptr;
    UFunction::FNativeFuncPtr g_ChallengeLobbyStartOriginal = nullptr;
    UFunction::FNativeFuncPtr g_ChallengeLobbyBackOriginal = nullptr;
    UObject* g_ChallengeStartSource = nullptr;
    uint64_t g_ChallengeStartSourceIdentity = 0;
    UObject* g_ChallengeSetupPage = nullptr;
    uint64_t g_ChallengeSetupPageIdentity = 0;
    bool g_ChallengeContinuingNativeStart = false;
    bool g_ChallengeSetupPushDispatched = false;
    ULONGLONG g_ChallengeNextPumpAt = 0;
    std::atomic<uintptr_t> g_ChallengeCombatPawn{0};
    uint64_t g_ChallengeCombatIdentity = 0;
    uintptr_t g_ChallengeCombatWorld = 0;
    unsigned g_ChallengeCombatReports = 0;
    unsigned g_ChallengeAttackInputs = 0;
    int g_ChallengeLastCanAttack = -1;
    float g_ChallengeSlashCooldownBefore = 0;
    ULONGLONG g_ChallengeCombatNextReport = 0;
    using ChallengeCanAttackFn = bool(__fastcall*)(UObject*);
    using ChallengeAttackFn = void(__fastcall*)(UObject*);
    ChallengeCanAttackFn g_ChallengeCanAttackOriginal = nullptr;
    ChallengeAttackFn g_ChallengeAttackOriginal = nullptr;
    static void InstallChallengeCombatDiagnostics();
    using ChallengeIntroFn = void(__fastcall*)(UObject*);
    ChallengeIntroFn g_ChallengeIntroOriginal = nullptr;
    bool g_ChallengeSkipIntroVerified = false;
    struct ChallengeReplayRecord { std::string Map; std::string Challenge; uint64_t Mode = 0; };
    std::array<ChallengeReplayRecord, 10> g_ChallengeReplayHistory{};
    static void InstallChallengeReplayIntro();

    static uintptr_t ChallengeCurrentWorld()
    {
        return reinterpret_cast<uintptr_t>(Engine::GetWorld());
    }

    static uint64_t ChallengeObjectIdentity(void* value)
    {
        SetupMetadataIdentity identity{};
        if (!ReadSetupMetadataIdentity(reinterpret_cast<UObject*>(value), identity) || identity.Index < 0) return 0;
        return (static_cast<uint64_t>(static_cast<uint32_t>(identity.Serial)) << 32) |
            static_cast<uint32_t>(identity.Index + 1);
    }

    static UObject* ChallengeCurrentGameMode()
    {
        UWorld* world = Engine::GetWorld();
        if (!world || !Memory::IsReadable(world, 0x100)) return nullptr;
        UObject* mode = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF0);
        return mode && Memory::IsReadable(mode, sizeof(UObject)) && mode->Class &&
            Memory::IsReadable(mode->Class, sizeof(UClass)) ? mode : nullptr;
    }

    static bool ChallengeIsLiveFrontend(void* value)
    {
        UObject* object = reinterpret_cast<UObject*>(value);
        UObject* mode = ChallengeCurrentGameMode();
        return object && Memory::IsReadable(object, sizeof(UObject)) && object->Class &&
            !(object->ObjectFlags & 0x30) && mode && GetActiveFrontendMenu() == mode;
    }

    static void* ChallengeReadWidget(void* object, const char* name)
    {
        return ReadRuntimeWidgetObject(reinterpret_cast<UObject*>(object), name);
    }

    static void* ChallengeAppendToggle(void* settings, void* donor,
        const wchar_t* title, size_t row, int initial)
    {
        static PersistentRuntimeText titles[f13::challenges::OptionCount]{};
        static const wchar_t* names[]{ L"Off", L"On" };
        if (row >= f13::challenges::OptionCount) return nullptr;
        return CreateOfflineSettingsCombo(reinterpret_cast<UObject*>(settings),
            reinterpret_cast<UObject*>(donor), title, names, initial, &titles[row], 2);
    }

    static int ChallengeReadToggle(void* combo)
    {
        return ReadRuntimeComboIndex(reinterpret_cast<UObject*>(combo), 1);
    }

    static bool ChallengeLabelHeader(void* value, const wchar_t* title)
    {
        static PersistentRuntimeText headerText{};
        UObject* header = ReadRuntimeWidgetObject(reinterpret_cast<UObject*>(value), "MenuHeaderWidget");
        return header && SetWideMenuButtonText(header, title, &headerText);
    }

    static void ChallengeRemoveWidget(void* value)
    {
        UObject* widget = reinterpret_cast<UObject*>(value);
        if (!widget || !Memory::IsReadable(widget, sizeof(UObject)) || !widget->Class) return;
        UFunction* remove = FindFunctionInHierarchyByName(widget->Class, "RemoveFromParent");
        if (!remove || remove->Size < 0 || remove->Size > 0x200) return;
        alignas(16) uint8_t params[0x200]{};
        SafeProcessEventCall(reinterpret_cast<uintptr_t>(widget), widget, remove, params);
    }

    static void ChallengeLog(const char* message) { Logger::Success(message); }

    static bool ChallengeReadObjectField(UObject* object, const char* name, UObject** output)
    {
        if (!output || !object || !Memory::IsReadable(object, sizeof(UObject)) || !object->Class) return false;
        UPropertyLite* property = FindPropertyInHierarchyByName(object->Class, name);
        if (!property || property->Offset_Internal <= 0 || property->Offset_Internal >= 0x10000 ||
            property->ElementSize != sizeof(UObject*) || property->ArrayDim != 1 ||
            SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) != "ObjectProperty") return false;
        auto* address = reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(object) + property->Offset_Internal);
        if (!Memory::IsReadable(address, sizeof(UObject*))) return false;
        *output = *address;
        return !*output || Memory::IsReadable(*output, sizeof(UObject));
    }

    static bool ChallengeIsCurrentJason(void* value, uintptr_t world, uintptr_t controller)
    {
        UObject* pawn = reinterpret_cast<UObject*>(value);
        if (!pawn || world != ChallengeCurrentWorld() ||
            controller != reinterpret_cast<uintptr_t>(Engine::GetLocalPlayerController()) ||
            !Memory::IsReadable(pawn, 0x1740) || !pawn->Class || (pawn->ObjectFlags & 0x30) ||
            !ClassDerivesFrom(pawn->Class, "SCKillerCharacter")) return false;
        UObject* nativePawn = nullptr;
        UObject* nativeController = nullptr;
        if (!ChallengeReadObjectField(reinterpret_cast<UObject*>(controller), "Pawn", &nativePawn) ||
            !ChallengeReadObjectField(pawn, "Controller", &nativeController)) return false;
        return nativePawn == pawn && nativeController == reinterpret_cast<UObject*>(controller);
    }

    static bool ChallengeCombatDiagnosticScope(UObject* pawn)
    {
        return g_ChallengeRouteActive.load() && reinterpret_cast<uintptr_t>(pawn) == g_ChallengeCombatPawn.load() &&
            ChallengeObjectIdentity(pawn) == g_ChallengeCombatIdentity &&
            ChallengeIsCurrentJason(pawn, g_ChallengeCombatWorld,
                reinterpret_cast<uintptr_t>(Engine::GetLocalPlayerController()));
    }

    static void LogChallengeCombatGates(UObject* pawn, const char* event, int allowed)
    {
        const ULONGLONG now = GetTickCount64();
        if (!ChallengeCombatDiagnosticScope(pawn) || g_ChallengeCombatReports >= 12 ||
            now < g_ChallengeCombatNextReport || !Memory::IsReadable(pawn, 0x1798)) return;
        g_ChallengeCombatNextReport = now + 250;
        ++g_ChallengeCombatReports;
        const auto* bytes = reinterpret_cast<const uint8_t*>(pawn);
        auto byte = [&](size_t at) { return std::to_string(bytes[at]); };
        auto pointer = [&](size_t at) {
            uintptr_t value = 0; std::memcpy(&value, bytes + at, sizeof(value));
            return std::to_string(value != 0);
        };
        auto number = [&](size_t at) {
            float value = 0; std::memcpy(&value, bytes + at, sizeof(value));
            return std::to_string(value);
        };
        std::string message = std::string("CHALLENGE COMBAT: ") + event +
            " | nativeCanAttack=" + std::to_string(allowed) + " | inputs=" + std::to_string(g_ChallengeAttackInputs) +
            " | health=" + number(0xE48) + " | requiredPositive1424=" + number(0x1424) +
            " | slashCooldown=" + number(0x12D0) + " | attackLatch=" + byte(0x1008) + "/" + byte(0x1009) +
            " | held=" + pointer(0x14D0) + " | knife=" + pointer(0x10F0) + " | door=" + pointer(0x1650) +
            " | pending=" + pointer(0xF68) + " | weapon=" + pointer(0xF78) + " | interaction=" + pointer(0xD38) +
            " | flags1758/13E8/E98/EE8/F81=" + byte(0x1758) + "/" + byte(0x13E8) + "/" +
                byte(0xE98) + "/" + byte(0xEE8) + "/" + byte(0xF81);
        message += " | cooldownBeforeInput=" + std::to_string(g_ChallengeSlashCooldownBefore);
        UObject* controller = *reinterpret_cast<UObject* const*>(bytes + 0x3A0);
        if (controller && Memory::IsReadable(controller, 0x7B2))
        {
            const auto* pc = reinterpret_cast<const uint8_t*>(controller);
            message += " | controller530/768/7B0/7B1=" + std::to_string(pc[0x530]) + "/" +
                std::to_string(pc[0x768]) + "/" + std::to_string(pc[0x7B0]) + "/" + std::to_string(pc[0x7B1]);
        }
        UObject* mesh = *reinterpret_cast<UObject* const*>(bytes + 0x3C8);
        UObject* anim = mesh && Memory::IsReadable(mesh, 0x8A8)
            ? *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(mesh) + 0x8A0) : nullptr;
        if (anim && Memory::IsReadable(anim, 0x649))
        {
            const auto* ab = reinterpret_cast<const uint8_t*>(anim);
            message += " | animClass=" + SafeName(reinterpret_cast<UObject*>(anim->Class)) +
                " | anim57C/648=" + std::to_string(ab[0x57C]) + "/" + std::to_string(ab[0x648]) +
                " | anim570=" + std::to_string(*reinterpret_cast<uintptr_t const*>(ab + 0x570) != 0);
        }
        Logger::Success(message);
    }

    static bool __fastcall ChallengeCanAttackObserver(UObject* pawn)
    {
        const bool result = g_ChallengeCanAttackOriginal ? g_ChallengeCanAttackOriginal(pawn) : false;
        // Hot predicate: pointer comparison only. Full identity/ownership
        // validation happens on actual input and the bounded snapshot, never
        // on every animation/HUD CanAttack query.
        if (g_ChallengeCombatReports < 12 && g_ChallengeRouteActive.load() &&
            reinterpret_cast<uintptr_t>(pawn) == g_ChallengeCombatPawn.load())
            g_ChallengeLastCanAttack = result ? 1 : 0;
        return result; // Observation only: preserve every native rejection.
    }

    static void __fastcall ChallengeAttackObserver(UObject* pawn)
    {
        const bool scoped = g_ChallengeCombatReports < 12 &&
            reinterpret_cast<uintptr_t>(pawn) == g_ChallengeCombatPawn.load() && ChallengeCombatDiagnosticScope(pawn);
        if (scoped)
        {
            ++g_ChallengeAttackInputs;
            g_ChallengeLastCanAttack = -1;
            if (Memory::IsReadable(pawn, 0x12D4))
                std::memcpy(&g_ChallengeSlashCooldownBefore,
                    reinterpret_cast<const uint8_t*>(pawn) + 0x12D0, sizeof(float));
        }
        if (g_ChallengeAttackOriginal) g_ChallengeAttackOriginal(pawn);
        if (scoped) LogChallengeCombatGates(pawn, "native attack input processed", g_ChallengeLastCanAttack);
    }

    static void InstallChallengeCombatDiagnostics()
    {
#if defined(F13_BASE_GAME_PORT)
        // Lazy, passive probes only. No input synthesis, attack-state writes,
        // permission bypass or always-on actor scan is introduced.
        struct Probe { uintptr_t rva; const uint8_t* signature; size_t bytes; void* replacement; void** original; };
        static const uint8_t can[]{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9};
        static const uint8_t attack[]{0x48,0x89,0x5C,0x24,0x20,0x55,0x48,0x8B,0xEC,0x48,0x81,0xEC,0x80,0,0,0};
        const Probe probes[]{
            {0x3E6D70, can, sizeof(can), reinterpret_cast<void*>(&ChallengeCanAttackObserver), reinterpret_cast<void**>(&g_ChallengeCanAttackOriginal)},
            {0x3F89A0, attack, sizeof(attack), reinterpret_cast<void*>(&ChallengeAttackObserver), reinterpret_cast<void**>(&g_ChallengeAttackOriginal)}};
        for (const auto& probe : probes)
        {
            if (*probe.original) continue;
            void* address = reinterpret_cast<void*>(ShippingAddress(probe.rva));
            if (!MatchesBytes(reinterpret_cast<uintptr_t>(address), probe.signature, probe.bytes) ||
                MH_CreateHook(address, probe.replacement, probe.original) != MH_OK || MH_EnableHook(address) != MH_OK)
                Logger::Error("CHALLENGE COMBAT: passive probe unavailable | rva=" + std::to_string(probe.rva));
        }
        Logger::Success("CHALLENGE COMBAT: passive input/CanAttack probes ready=" +
            std::to_string(g_ChallengeCanAttackOriginal && g_ChallengeAttackOriginal));
#endif
    }

    static bool ChallengeReplayKey(UObject* mode, std::string& map, std::string& challenge)
    {
        UWorld* world = Engine::GetWorld();
        UObject* controller = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
        if (!g_ChallengeRouteActive.load() || !world || !mode || mode != ChallengeCurrentGameMode() ||
            !Memory::IsReadable(world, 0x100) || !Memory::IsReadable(mode, 0x4F1) || !mode->Class ||
            SafeName(reinterpret_cast<UObject*>(mode->Class)) != "SCGameMode_SPChallenges" ||
            *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(mode) + 0x110) < 3) return false;
        UObject* netDriver = nullptr;
        UObject* connection = nullptr;
        if (!ChallengeReadObjectField(reinterpret_cast<UObject*>(world), "NetDriver", &netDriver) || netDriver ||
            !ChallengeReadObjectField(controller, "NetConnection", &connection) || connection) return false;
        UObject* package = reinterpret_cast<UObject*>(world->OuterPrivate);
        if (!package || !Memory::IsReadable(package, sizeof(UObject)) || !package->Class ||
            SafeName(reinterpret_cast<UObject*>(package->Class)) != "Package") return false;
        map = SafeName(package);
        if (map.find("/Game/Maps/Single_Player/") != 0) return false;
        UObject* state = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF8);
        if (!state || !Memory::IsReadable(state, sizeof(UObject)) || !state->Class ||
            !ClassDerivesFrom(state->Class, "SCGameState_SPChallenges")) return false;
        UObject* dossier = nullptr;
        if (!ChallengeReadObjectField(state, "Dossier", &dossier) || !dossier || !dossier->Class ||
            !ChallengeObjectIdentity(dossier)) return false;
        UPropertyLite* active = FindPropertyInHierarchyByName(dossier->Class, "ActiveChallengeClass");
        if (!active || active->Offset_Internal != 0x58 || active->ElementSize != sizeof(UClass*) ||
            active->ArrayDim != 1 || SafeName(reinterpret_cast<UObject*>(active->ClassPrivate)) != "ClassProperty" ||
            !Memory::IsReadable(reinterpret_cast<uint8_t*>(dossier) + 0x58, sizeof(UClass*))) return false;
        UObject* klass = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(dossier) + 0x58);
        if (!ChallengeObjectIdentity(klass)) return false;
        challenge.clear();
        unsigned depth = 0;
        for (UObject* part = klass; part; part = reinterpret_cast<UObject*>(part->OuterPrivate))
        {
            if (++depth > 12 || !Memory::IsReadable(part, sizeof(UObject))) return false;
            const std::string name = SafeName(part);
            if (name.empty() || name == "None") return false;
            challenge = challenge.empty() ? name : name + "." + challenge;
        }
        return challenge.find("/Game/") == 0 && ChallengeCurrentGameMode() == mode;
    }

    static f13::challenges::MissionSettingsBinding g_ChallengeMissionBinding;
    static uint64_t g_ChallengeBoundWorldIdentity = 0, g_ChallengeBoundModeIdentity = 0;
    static uint64_t g_ChallengeBoundSubmission = 0;
    static bool g_ChallengeMissionAuthorized = false;
    static bool g_ChallengeMissionKeyResolved = false;
    static ULONGLONG g_ChallengeNextMissionBindAt = 0;
    static unsigned g_ChallengeMissionBindReports = 0;

    static bool AuthorizeChallengeMissionSettings(UObject* mode)
    {
        const uint64_t submission = f13::challenges::nativeui::CommittedSubmission();
        const uint64_t worldIdentity = ChallengeObjectIdentity(Engine::GetWorld());
        const uint64_t modeIdentity = ChallengeObjectIdentity(mode);
        if (!submission || !worldIdentity || !modeIdentity) return false;
        if (submission != g_ChallengeBoundSubmission || worldIdentity != g_ChallengeBoundWorldIdentity ||
            modeIdentity != g_ChallengeBoundModeIdentity)
        {
            g_ChallengeBoundSubmission = submission;
            g_ChallengeBoundWorldIdentity = worldIdentity;
            g_ChallengeBoundModeIdentity = modeIdentity;
            g_ChallengeMissionAuthorized = false;
            g_ChallengeMissionKeyResolved = false;
            g_ChallengeNextMissionBindAt = 0;
            g_ChallengeMissionBindReports = 0;
        }
        if (g_ChallengeMissionKeyResolved) return g_ChallengeMissionAuthorized;
        const ULONGLONG now = GetTickCount64();
        if (now < g_ChallengeNextMissionBindAt) return false;
        g_ChallengeNextMissionBindAt = now + 1000;
        std::string map, challenge;
        if (!ChallengeReplayKey(mode, map, challenge))
        {
            // Travel can expose InProgress before the dossier is readable.
            // Stay inactive and retry at most once per second, not every tick.
            if (g_ChallengeMissionBindReports++ < 3)
                Logger::Debug("CHALLENGE SCOPE: awaiting exact mission dossier; modifiers inactive");
            return false;
        }
        g_ChallengeMissionKeyResolved = true;
        g_ChallengeMissionAuthorized = g_ChallengeMissionBinding.Accept(submission, map, challenge);
        if (g_ChallengeMissionAuthorized)
            Logger::Success("CHALLENGE SCOPE: settings bound to selected stock mission | map=" + map + " | challenge=" + challenge);
        else
            Logger::Error("CHALLENGE SCOPE: mission/settings mismatch; modifiers inactive until a fresh setup submission");
        return g_ChallengeMissionAuthorized;
    }

    static void RecordChallengeReplayReady(UObject* mode)
    {
        std::string map, challenge;
        const uint64_t identity = ChallengeObjectIdentity(mode);
        if (!identity || !ChallengeReplayKey(mode, map, challenge))
        {
            Logger::Error("CHALLENGE REPLAY: exact map/dossier identity unavailable; intro skipping remains inactive");
            return;
        }
        for (auto& record : g_ChallengeReplayHistory)
        {
            if (record.Challenge.empty() || (record.Map == map && record.Challenge == challenge))
            {
                record = {map, challenge, identity};
                Logger::Success("CHALLENGE REPLAY: native InProgress attempt recorded | map=" + map + " | challenge=" + challenge);
                return;
            }
        }
    }

    static void __fastcall ChallengeReplayIntroObserver(UObject* mode)
    {
        const uint64_t identity = ChallengeObjectIdentity(mode);
        std::string map, challenge;
        bool replay = false;
        if (g_ChallengeSkipIntroVerified && f13::challenges::nativeui::HasCommittedSettings() &&
            f13::challenges::nativeui::GetCommittedSettings().enabled[
                static_cast<size_t>(f13::challenges::Option::SkipReplayIntro)] && identity &&
            ChallengeReplayKey(mode, map, challenge))
        {
            for (const auto& record : g_ChallengeReplayHistory)
                if (record.Mode && record.Mode != identity && record.Map == map && record.Challenge == challenge)
                    replay = true;
        }
        uint8_t* skip = replay ? reinterpret_cast<uint8_t*>(mode) + 0x4F0 : nullptr;
        const uint8_t before = skip ? *skip : 0;
        if (skip && before == 0)
        {
            *skip = 1; // Stock handler forwards this to its own intro-skip path.
            Logger::Success("CHALLENGE REPLAY: skipping repeated opening intro via native handler | map=" + map);
        }
        if (g_ChallengeIntroOriginal) g_ChallengeIntroOriginal(mode);
        // Never override a preexisting skip setting or restore into a recycled
        // mode, and never call the generic intro/outro skipping function.
        if (skip && before == 0 && ChallengeObjectIdentity(mode) == identity &&
            Memory::IsReadable(skip, 1) && *skip == 1) *skip = before;
    }

    static void InstallChallengeReplayIntro()
    {
#if defined(F13_BASE_GAME_PORT)
        if (g_ChallengeIntroOriginal) return;
        static const uint8_t entry[]{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9};
        static const uint8_t challengeCall[]{0x49,0x8B,0xCE,0xE8,0x69,0x64,0xFC,0xFF};
        static const uint8_t skipRead[]{0x0F,0xB6,0x97,0xF0,0x04,0x00,0x00,0x48,0x8B,0xCB,0xE8,0x4D,0x36,0x0C,0x00};
        void* address = reinterpret_cast<void*>(ShippingAddress(0x382090));
        g_ChallengeSkipIntroVerified = MatchesBytes(reinterpret_cast<uintptr_t>(address), entry, sizeof(entry)) &&
            MatchesBytes(ShippingAddress(0x3BBC1F), challengeCall, sizeof(challengeCall)) &&
            MatchesBytes(ShippingAddress(0x3820E4), skipRead, sizeof(skipRead)) &&
            MH_CreateHook(address, reinterpret_cast<void*>(&ChallengeReplayIntroObserver),
                reinterpret_cast<void**>(&g_ChallengeIntroOriginal)) == MH_OK && MH_EnableHook(address) == MH_OK;
        Logger::Success("CHALLENGE REPLAY: lazy native intro seam ready=" + std::to_string(g_ChallengeSkipIntroVerified));
#endif
    }

    #include "ChallengeEscapeBoundary.inl"

    static f13::challenges::Capabilities ChallengeAuditedCapabilities()
    {
        auto capabilities = f13::challenges::runtime::AuditedCapabilities();
        capabilities.verified[static_cast<size_t>(f13::challenges::Option::SkipReplayIntro)] = g_ChallengeSkipIntroVerified;
        capabilities.verified[static_cast<size_t>(f13::challenges::Option::PreventEscapes)] = g_ChallengeEscapeVerified;
        // Stock shared dispatcher bytecode audited for this supported build.
        // Exact class/function/parameter validation is repeated lazily in the
        // selected mission before any boundary event override becomes active.
        capabilities.verified[static_cast<size_t>(f13::challenges::Option::DisableJasonBoundary)] = true;
        return capabilities;
    }

    static bool ChallengeWritable(const void* address, size_t bytes)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & PAGE_GUARD)) return false;
        const DWORD protection = info.Protect & 0xFF;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t at = reinterpret_cast<uintptr_t>(address);
        const uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
        return at >= region && bytes <= info.RegionSize && at - region <= info.RegionSize - bytes;
    }

    static f13::challenges::runtime::Field ChallengeRuntimeField(
        void* value, const char* name, f13::challenges::runtime::FieldKind kind, int dimension)
    {
        using namespace f13::challenges::runtime;
        UObject* pawn = reinterpret_cast<UObject*>(value);
        if (strcmp(name, "RemainingTime") == 0 && kind == FieldKind::Int && dimension == 1)
        {
            UWorld* world = Engine::GetWorld();
            UObject* mode = ChallengeCurrentGameMode();
            UObject* state = world && Memory::IsReadable(world, 0x100)
                ? *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF8) : nullptr;
            if (!state || pawn != state || !mode || !ClassDerivesFrom(mode->Class, "SCGameMode_SPChallenges") ||
                !Memory::IsReadable(state, sizeof(UObject)) || !state->Class || (state->ObjectFlags & 0x30) ||
                !ClassDerivesFrom(state->Class, "SCGameState_SPChallenges")) return {};
            UPropertyLite* property = FindPropertyInHierarchyByName(state->Class, name);
            if (property && (property->Offset_Internal != 0x3FC || property->ElementSize != 4 || property->ArrayDim != 1 ||
                SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) != "IntProperty")) return {};
            const auto identity = ChallengeObjectIdentity(state);
            void* address = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(state) + 0x3FC);
            return identity && ChallengeWritable(address, 4) ? Field{ address, 4, 1, identity } : Field{};
        }
        if (!ChallengeIsCurrentJason(pawn, ChallengeCurrentWorld(),
                reinterpret_cast<uintptr_t>(Engine::GetLocalPlayerController()))) return {};
        const bool knives = strcmp(name, "NumKnives") == 0 && kind == FieldKind::Int && dimension == 1;
        const bool recharge = strcmp(name, "AbilityRechargeTime") == 0 && kind == FieldKind::Float && dimension == 4;
        int movementOffset = 0;
        if (kind == FieldKind::Float && dimension == 1)
        {
            if (strcmp(name, "MaxRunSpeed") == 0) movementOffset = 0x724;
            else if (strcmp(name, "MaxSprintSpeed") == 0) movementOffset = 0x720;
        }
        int stealthOffset = 0;
        if (kind == FieldKind::Float && dimension == 1)
        {
            if (strcmp(name, "StealthNoiseScale") == 0) stealthOffset = 0x1784;
            else if (strcmp(name, "StealthRadiusScale") == 0) stealthOffset = 0x1788;
            else if (strcmp(name, "AntiStealthNoiseScale") == 0) stealthOffset = 0x178C;
            else if (strcmp(name, "AntiStealthRadiusScale") == 0) stealthOffset = 0x1790;
        }
        if (!knives && !recharge && !stealthOffset && !movementOffset) return {};
        UObject* owner = pawn;
        if (movementOffset)
        {
            owner = ReadRuntimeWidgetObject(pawn, "CharacterMovement");
            if (!owner || (owner->ObjectFlags & 0x30) || owner->OuterPrivate != pawn ||
                !ClassDerivesFrom(owner->Class, "SCCharacterMovement")) return {};
        }
        const int expectedOffset = knives ? 0x15BC : recharge ? 0x1218 : movementOffset ? movementOffset : stealthOffset;
        UPropertyLite* property = FindPropertyInHierarchyByName(owner->Class, name);
        if (property && (property->Offset_Internal != expectedOffset || property->ElementSize != 4 ||
            property->ArrayDim != dimension || SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) !=
                (knives ? "IntProperty" : "FloatProperty"))) return {};
        // Supported native signatures are validated on the challenge click.
        // Native offsets are independently proven in property registration and
        // authoritative refill/CanUseAbility/Tick; no speculative layout fallback.
        const auto identity = ChallengeObjectIdentity(owner);
        void* address = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(owner) + expectedOffset);
        return ChallengeWritable(address, 4 * static_cast<size_t>(dimension))
            && identity ? Field{ address, 4, dimension, identity } : Field{};
    }

    static bool ConfigureChallengeHostOnClick()
    {
        if (g_ChallengeHostConfigured) return true;
#if !defined(F13_BASE_GAME_PORT)
        return false;
#else
        InstallChallengeCombatDiagnostics();
        InstallChallengeReplayIntro();
        InstallChallengeEscapeBoundary();
        static const uint8_t refill[]{ 0x80,0xB9,0x10,0x01,0x00,0x00,0x03,0x0F,0x82,0xB3,0x41,0x0A,0x00,
            0xC7,0x81,0xBC,0x15,0x00,0x00,0x63,0x00,0x00,0x00,0xC3 };
        static const uint8_t threshold[]{ 0xF3,0x0F,0x10,0x84,0x8B,0x18,0x12,0x00,0x00 };
        static const uint8_t noiseEndpoints[]{ 0xF3,0x0F,0x10,0x93,0x8C,0x17,0x00,0x00,
            0xF3,0x0F,0x10,0x8B,0x84,0x17,0x00,0x00 };
        static const uint8_t antiRadius[]{ 0xF3,0x0F,0x10,0x8B,0x90,0x17,0x00,0x00 };
        static const uint8_t stealthRadius[]{ 0xF3,0x0F,0x10,0xBB,0x88,0x17,0x00,0x00 };
        static const uint8_t nativeSprint[]{ 0xF3,0x0F,0x59,0xB7,0x20,0x07,0x00,0x00 };
        static const uint8_t nativeRun[]{ 0xF3,0x0F,0x59,0xB7,0x24,0x07,0x00,0x00 };
        static const uint8_t countdown[]{ 0x8B,0x86,0xFC,0x03,0x00,0x00,0x85,0xC0,0x7E,0x08,
            0xFF,0xC8,0x89,0x86,0xFC,0x03,0x00,0x00 };
        static const uint8_t timeout[]{ 0x83,0xBE,0xFC,0x03,0x00,0x00,0x00 };
        if (!MatchesBytes(ShippingAddress(0x404DE0), refill, sizeof(refill)) ||
            !MatchesBytes(ShippingAddress(0x3F4E5C), threshold, sizeof(threshold)) ||
            !MatchesBytes(ShippingAddress(0x3E6D2A), noiseEndpoints, sizeof(noiseEndpoints)) ||
            !MatchesBytes(ShippingAddress(0x3F5888), antiRadius, sizeof(antiRadius)) ||
            !MatchesBytes(ShippingAddress(0x3F5893), stealthRadius, sizeof(stealthRadius)) ||
            !MatchesBytes(ShippingAddress(0x3B7BC2), nativeSprint, sizeof(nativeSprint)) ||
            !MatchesBytes(ShippingAddress(0x3B7C45), nativeRun, sizeof(nativeRun)) ||
            !MatchesBytes(ShippingAddress(0x37C4E4), countdown, sizeof(countdown)) ||
            !MatchesBytes(ShippingAddress(0x37C567), timeout, sizeof(timeout)))
        {
            Logger::Error("CHALLENGE ADDON: supported native modifier signature mismatch; lazy setup refused");
            return false;
        }
        f13::challenges::runtime::Host runtimeHost{ ChallengeRuntimeField, ChallengeIsCurrentJason, ChallengeObjectIdentity, ChallengeLog };
        if (!f13::challenges::runtime::Configure(runtimeHost, f13::challenges::SupportedExeSha256)) return false;
        f13::challenges::nativeui::Host uiHost{};
        uiHost.currentWorld = ChallengeCurrentWorld;
        uiHost.isLiveFrontendObject = ChallengeIsLiveFrontend;
        uiHost.objectIdentity = ChallengeObjectIdentity;
        uiHost.readWidgetObject = ChallengeReadWidget;
        uiHost.appendToggle = ChallengeAppendToggle;
        uiHost.readToggle = ChallengeReadToggle;
        uiHost.labelHeader = ChallengeLabelHeader;
        uiHost.capabilities = ChallengeAuditedCapabilities;
        uiHost.removeWidget = ChallengeRemoveWidget;
        uiHost.log = ChallengeLog;
        g_ChallengeHostConfigured = f13::challenges::nativeui::Configure(uiHost, f13::challenges::SupportedExeSha256);
        return g_ChallengeHostConfigured;
#endif
    }

    static void InstallChallengeSettingsHooksAfterClick();
    static void InstallChallengePickerHooksAfterClick();
    static bool OpenChallengeSetupPage(UObject* source);
    static bool ResumeChallengeNativeStart(UObject* page);

    static bool ActivateChallengeRoute(void* context)
    {
        if (!ChallengeIsLiveFrontend(context)) return false;
        const uintptr_t world = ChallengeCurrentWorld();
        // ProcessEvent and the class-local Blueprint thunk can both see one
        // click. Do not reset committed/widget state twice for that dispatch.
        if (g_ChallengeRouteActive.load() && g_ChallengeFrontendWorld == world)
            return true;
        if (!ConfigureChallengeHostOnClick()) return false;
        f13::challenges::runtime::Reset();
        f13::challenges::nativeui::Reset();
        // A Challenges click is not the counselor lifecycle, even on replay.
        g_CounselorMenuRouteLatched.store(false);
        g_CounselorEntryClickPending.store(false);
        f13::challenges::nativeui::BeforeEvent(context, "OfflinePlayMenuWidget_C",
            "BndEvt__SinglePlayerChallengesButton_K2Node_ComponentBoundEvent_20_OnClicked__DelegateSignature");
        g_ChallengeFrontendWorld = world;
        g_ChallengeStartSource = nullptr;
        g_ChallengeSetupPage = nullptr;
        g_ChallengeCombatPawn.store(0);
        g_ChallengeReplayHistory = {};
        g_ChallengeRouteActive.store(true);
        InstallChallengeSettingsHooksAfterClick();
        InstallChallengePickerHooksAfterClick();
        return true;
    }

    static void ChallengeEntryExec(void* context, void* stack, void* result)
    {
        ActivateChallengeRoute(context);
        if (g_ChallengeEntryOriginal) g_ChallengeEntryOriginal(context, stack, result);
    }

    static void ChallengeConstructExec(void* context, void* stack, void* result)
    {
        if (g_ChallengeConstructOriginal) g_ChallengeConstructOriginal(context, stack, result);
        f13::challenges::nativeui::AfterEvent(context, "SPChallengesSettingsMenuWidget_C", "Construct");
    }
    static void ChallengeStartExec(void* context, void* stack, void* result)
    {
        f13::challenges::nativeui::BeforeEvent(context, "SPChallengesSettingsMenuWidget_C", "OnClicked_Start");
        if (context == g_ChallengeSetupPage &&
            ChallengeObjectIdentity(context) == g_ChallengeSetupPageIdentity)
        {
            if (!ResumeChallengeNativeStart(reinterpret_cast<UObject*>(context)))
                Logger::Error("CHALLENGE SETUP FAILED: retained native Start handoff; Back remains available");
            return; // Never execute the obsolete settings widget's travel URL.
        }
        if (g_ChallengeStartOriginal) g_ChallengeStartOriginal(context, stack, result);
    }
    static void ChallengeBackExec(void* context, void* stack, void* result)
    {
        if (context == g_ChallengeSetupPage &&
            ChallengeObjectIdentity(context) == g_ChallengeSetupPageIdentity)
        {
            // Back returns to the actual challenge lobby. Keep this route
            // active so Start can construct a fresh, independent setup page.
            g_ChallengeSetupPage = nullptr;
            g_ChallengeStartSource = nullptr;
            if (g_ChallengeBackOriginal) g_ChallengeBackOriginal(context, stack, result);
            return;
        }
        f13::challenges::runtime::Reset();
        f13::challenges::nativeui::Reset();
        g_ChallengeRouteActive.store(false);
        if (g_ChallengeBackOriginal) g_ChallengeBackOriginal(context, stack, result);
    }

    static void ChallengePickerConstructExec(void* context, void* stack, void* result)
    {
        // Picker dependencies can become available after the entry click.
        // Install before its original Construct can create the settings page.
        if (g_ChallengeRouteActive.load()) InstallChallengeSettingsHooksAfterClick();
        if (g_ChallengePickerConstructOriginal) g_ChallengePickerConstructOriginal(context, stack, result);
        if (g_ChallengeRouteActive.load()) InstallChallengeSettingsHooksAfterClick();
    }

    static void ChallengePickerBackExec(void* context, void* stack, void* result)
    {
        f13::challenges::runtime::Reset();
        f13::challenges::nativeui::Reset();
        g_ChallengeRouteActive.store(false);
        if (g_ChallengePickerBackOriginal) g_ChallengePickerBackOriginal(context, stack, result);
    }

    static void ChallengeLobbyConstructExec(void* context, void* stack, void* result)
    {
        if (g_ChallengeLobbyConstructOriginal) g_ChallengeLobbyConstructOriginal(context, stack, result);
        if (g_ChallengeRouteActive.load() && ChallengeIsLiveFrontend(context))
        {
            // The stock postmatch flow can rebuild its SP lobby in a new
            // frontend world without clicking Offline Challenges again.
            if (ChallengeCurrentWorld() != g_ChallengeFrontendWorld)
            {
                f13::challenges::runtime::Reset();
                f13::challenges::nativeui::Reset();
                f13::challenges::nativeui::BeforeEvent(context, "OfflinePlayMenuWidget_C",
                    "BndEvt__SinglePlayerChallengesButton_K2Node_ComponentBoundEvent_20_OnClicked__DelegateSignature");
                g_ChallengeFrontendWorld = ChallengeCurrentWorld();
                g_ChallengeSetupPage = nullptr;
                g_ChallengeStartSource = nullptr;
            }
            InstallChallengeSettingsHooksAfterClick();
        }
    }

    static void ChallengeLobbyStartExec(void* context, void* stack, void* result)
    {
        if (g_ChallengeRouteActive.load() && !g_ChallengeContinuingNativeStart &&
            ChallengeIsLiveFrontend(context))
        {
            if (!OpenChallengeSetupPage(reinterpret_cast<UObject*>(context)))
            {
                Logger::Error("CHALLENGE SETUP: modifiers unavailable; see preceding failure stage");
                if (!g_ChallengeSetupPushDispatched)
                {
                    // Never trap the user in the lobby when an optional page
                    // cannot load. No menu push has been attempted, so the
                    // original native challenge selection/stack is intact.
                    f13::challenges::runtime::Reset();
                    f13::challenges::nativeui::Reset();
                    Logger::Error("CHALLENGE SETUP: native mission Start fallback; modifiers NOT applied");
                    if (g_ChallengeLobbyStartOriginal) g_ChallengeLobbyStartOriginal(context, stack, result);
                }
            }
            return;
        }
        if (g_ChallengeLobbyStartOriginal) g_ChallengeLobbyStartOriginal(context, stack, result);
    }

    static void ChallengeLobbyBackExec(void* context, void* stack, void* result)
    {
        f13::challenges::runtime::Reset();
        f13::challenges::nativeui::Reset();
        g_ChallengeRouteActive.store(false);
        g_ChallengeSetupPage = nullptr;
        g_ChallengeStartSource = nullptr;
        if (g_ChallengeLobbyBackOriginal) g_ChallengeLobbyBackOriginal(context, stack, result);
    }

    static bool HookChallengeExec(UClass* klass, const char* name,
        UFunction::FNativeFuncPtr replacement, UFunction::FNativeFuncPtr* original)
    {
        UFunction* function = FindFunctionInHierarchyByName(klass, name);
        if (!function || !Memory::IsReadable(function, sizeof(UFunction)) || !function->ExecFunction) return false;
        if (function->ExecFunction == replacement) return true;
        // Hook only the class's own cooked Blueprint event, never an inherited
        // UserWidget Construct shared by unrelated routes.
        if (reinterpret_cast<void*>(function->OuterPrivate) != reinterpret_cast<void*>(klass)) return false;
        DWORD protection = 0;
        if (!VirtualProtect(&function->ExecFunction, sizeof(function->ExecFunction), PAGE_READWRITE, &protection)) return false;
        *original = function->ExecFunction;
        function->ExecFunction = replacement;
        DWORD unused = 0;
        VirtualProtect(&function->ExecFunction, sizeof(function->ExecFunction), protection, &unused);
        return true;
    }

    #include "ChallengeJasonBoundaryScript.inl"

    static void InstallChallengeSettingsHooksAfterClick()
    {
        UObject* object = FindObjectExact("SPChallengesSettingsMenuWidget_C");
        if (!object || !Memory::IsReadable(object, sizeof(UClass))) return;
        auto* klass = reinterpret_cast<UClass*>(object);
        const bool construct = HookChallengeExec(klass, "Construct", ChallengeConstructExec, &g_ChallengeConstructOriginal);
        const bool start = HookChallengeExec(klass, "OnClicked_Start", ChallengeStartExec, &g_ChallengeStartOriginal);
        const bool back = HookChallengeExec(klass, "OnClicked_Back", ChallengeBackExec, &g_ChallengeBackOriginal);
        Logger::Success("CHALLENGE SETUP: lazy class-local event hooks | Construct=" + std::to_string(construct) +
            " | Start=" + std::to_string(start) + " | Back=" + std::to_string(back));
    }

    static void InstallChallengePickerHooksAfterClick()
    {
        UObject* lobby = FindObjectExact("SP_LobbyWidget_C");
        if (lobby && Memory::IsReadable(lobby, sizeof(UClass)))
        {
            auto* klass = reinterpret_cast<UClass*>(lobby);
            const bool start = HookChallengeExec(klass, "OnStartPressed", ChallengeLobbyStartExec, &g_ChallengeLobbyStartOriginal);
            HookChallengeExec(klass, "Construct", ChallengeLobbyConstructExec, &g_ChallengeLobbyConstructOriginal);
            HookChallengeExec(klass, "OnClicked_Back", ChallengeLobbyBackExec, &g_ChallengeLobbyBackOriginal);
            Logger::Success("CHALLENGE SETUP: actual SP lobby native start hook=" + std::to_string(start));
        }
        UObject* object = FindObjectExact("PickSPChallengesMapMenuWidget_C");
        if (!object || !Memory::IsReadable(object, sizeof(UClass))) return;
        auto* klass = reinterpret_cast<UClass*>(object);
        HookChallengeExec(klass, "Construct", ChallengePickerConstructExec, &g_ChallengePickerConstructOriginal);
        HookChallengeExec(klass, "OnClicked_Back", ChallengePickerBackExec, &g_ChallengePickerBackOriginal);
    }

    static void InstallChallengeEntryHook(UClass* klass)
    {
#if defined(F13_BASE_GAME_PORT)
        // Only observe the already-loaded Offline Play click. The addon,
        // challenge-class discovery and modifiers stay dormant until clicked.
        HookChallengeExec(klass,
            "BndEvt__SinglePlayerChallengesButton_K2Node_ComponentBoundEvent_20_OnClicked__DelegateSignature",
            ChallengeEntryExec, &g_ChallengeEntryOriginal);
#endif
    }

    static UClass* LoadChallengeSetupClass()
    {
        if (UClass* loaded = FindClassExact("SPChallengesSettingsMenuWidget_C")) return loaded;
        // Resolve precisely one stock generated class, only after user Start.
        // A native FName plus empty subpath needs no owned FString allocation.
        static const wchar_t path[] = L"/Game/UI/Menu/FrontEnd/OfflinePlay/SPChallengesSettingsMenuWidget.SPChallengesSettingsMenuWidget_C";
        RawFString text{const_cast<wchar_t*>(path), static_cast<int32_t>(std::size(path)), static_cast<int32_t>(std::size(path))};
        // Stock Conv_StringToName implementation: output FName*, borrowed
        // FString*. Unlike the reflection path this needs no library CDO or
        // parameter-name discovery; it neither retains nor frees the buffer.
        static const uint8_t nameSig[]{0x40,0x53,0x48,0x83,0xEC,0x20,0x83,0x7A,0x08,0x00,0x48,0x8B,0xD9,0x74,0x05,0x48};
        const uintptr_t nameAddress = ShippingAddress(0x14BC0C0);
        if (!MatchesBytes(nameAddress, nameSig, sizeof(nameSig)))
        {
            Logger::Error("CHALLENGE SETUP FAILED: native name converter signature");
            return nullptr;
        }
        FName assetName{};
        reinterpret_cast<FName*(__fastcall*)(FName*, const RawFString*)>(nameAddress)(&assetName, &text);
        if (!assetName.ComparisonIndex)
        {
            Logger::Error("CHALLENGE SETUP FAILED: empty class path name");
            return nullptr;
        }
        alignas(16) uint8_t softClass[40]{};
        const int32_t invalidWeakIndex = -1;
        std::memcpy(softClass, &invalidWeakIndex, sizeof(invalidWeakIndex));
        std::memcpy(softClass + 0x10, &assetName, sizeof(FName));
        static const uint8_t loadSig[]{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20};
        const uintptr_t address = ShippingAddress(0x29D880);
        if (!MatchesBytes(address, loadSig, sizeof(loadSig)))
        {
            Logger::Error("CHALLENGE SETUP FAILED: native class loader signature");
            return nullptr;
        }
        UClass* loaded = reinterpret_cast<UClass*(__fastcall*)(const void*)>(address)(softClass);
        if (!loaded || !Memory::IsReadable(loaded, sizeof(UClass)) ||
            SafeName(reinterpret_cast<UObject*>(loaded)) != "SPChallengesSettingsMenuWidget_C")
        {
            Logger::Error("CHALLENGE SETUP FAILED: stock settings class load | result=" + SafeName(reinterpret_cast<UObject*>(loaded)));
            return nullptr;
        }
        return loaded;
    }

    static bool OpenChallengeSetupPage(UObject* source)
    {
        g_ChallengeSetupPushDispatched = false;
        if (!g_ChallengeRouteActive.load() || !ChallengeIsLiveFrontend(source) ||
            !SafeNameEquals(reinterpret_cast<UObject*>(source->Class), "SP_LobbyWidget_C"))
        {
            Logger::Error("CHALLENGE SETUP FAILED: source/route/frontend guard");
            return false;
        }
        const uint64_t identity = ChallengeObjectIdentity(source);
        UClass* pageClass = LoadChallengeSetupClass();
        UFunction* push = FindFunctionInHierarchyByName(source->Class, "PushMenu");
        if (!identity || !pageClass || !push)
        {
            Logger::Error("CHALLENGE SETUP FAILED: prerequisites | identity=" + std::to_string(identity != 0) +
                " | class=" + std::to_string(pageClass != nullptr) + " | PushMenu=" + std::to_string(push != nullptr));
            return false;
        }
        InstallChallengeSettingsHooksAfterClick();
        UFunction* ownedStart = FindFunctionInHierarchyByName(pageClass, "OnClicked_Start");
        if (!ownedStart || ownedStart->ExecFunction != ChallengeStartExec)
        {
            Logger::Error("CHALLENGE SETUP FAILED: settings Start interception");
            return false;
        }
        g_ChallengeStartSource = source;
        g_ChallengeStartSourceIdentity = identity;
        alignas(16) uint8_t params[0x200]{};
        // The stock PushMenu bytecode passes MenuClass then true. Resolve
        // the boolean descriptor rather than inventing its argument name.
        UPropertyLite* pushFlag = FindFunctionPropertyByType(push, "BoolProperty", false);
        if (!pushFlag || pushFlag->ElementSize != 1 || pushFlag->Offset_Internal < 0 ||
            pushFlag->Offset_Internal >= sizeof(params))
        {
            Logger::Error("CHALLENGE SETUP FAILED: PushMenu boolean descriptor");
            return false;
        }
        params[pushFlag->Offset_Internal] = 1;
        if (!WriteRuntimeCallArgument(push, params, "MenuClass", &pageClass, sizeof(pageClass)))
        {
            Logger::Error("CHALLENGE SETUP FAILED: PushMenu class argument");
            return false;
        }
        g_ChallengeSetupPushDispatched = true;
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(source), source, push, params))
        {
            Logger::Error("CHALLENGE SETUP FAILED: PushMenu dispatch");
            return false;
        }
        UObject* page = ReadRuntimeCallObjectResult(push, params);
        if (!page || !ChallengeIsLiveFrontend(page) || page->Class != pageClass)
        {
            Logger::Error("CHALLENGE SETUP FAILED: PushMenu result | class=" +
                (page && page->Class ? SafeName(reinterpret_cast<UObject*>(page->Class)) : "null"));
            return false;
        }
        g_ChallengeSetupPage = page;
        g_ChallengeSetupPageIdentity = ChallengeObjectIdentity(page);
        f13::challenges::nativeui::AfterEvent(page, "SPChallengesSettingsMenuWidget_C", "Construct");
        UObject* donor = ReadRuntimeWidgetObject(page, "DifficultyComboBox");
        UFunction* visibility = donor && donor->Class ? FindFunctionInHierarchyByName(donor->Class, "SetVisibility") : nullptr;
        if (visibility)
        {
            alignas(16) uint8_t visibilityParams[0x200]{};
            const uint8_t collapsed = 1;
            if (WriteRuntimeCallArgument(visibility, visibilityParams, "InVisibility", &collapsed, sizeof(collapsed)))
                SafeProcessEventCall(reinterpret_cast<uintptr_t>(donor), donor, visibility, visibilityParams);
        }
        Logger::Success("CHALLENGE SETUP: modifiers page pushed from actual challenge Start; stock challenge identity retained");
        return g_ChallengeSetupPageIdentity != 0;
    }

    static bool ResumeChallengeNativeStart(UObject* page)
    {
        UObject* source = g_ChallengeStartSource;
        if (!ChallengeIsLiveFrontend(page) || !ChallengeIsLiveFrontend(source) ||
            ChallengeObjectIdentity(source) != g_ChallengeStartSourceIdentity ||
            ChallengeCurrentWorld() != g_ChallengeFrontendWorld) return false;
        UFunction* pop = FindFunctionInHierarchyByName(page->Class, "PopMenu");
        UFunction* start = FindFunctionInHierarchyByName(source->Class, "OnStartPressed");
        if (!pop || !start || start->ExecFunction != ChallengeLobbyStartExec) return false;
        const bool committed = f13::challenges::nativeui::HasCommittedSettings();
        if (!committed)
        {
            f13::challenges::runtime::Reset();
            f13::challenges::nativeui::Reset();
            Logger::Error("CHALLENGE SETUP: modifier rows did not commit; native mission Start fallback, modifiers NOT applied");
        }
        alignas(16) uint8_t params[0x200]{};
        UPropertyLite* force = FindFunctionPropertyByType(pop, "BoolProperty", false);
        if (force && force->ElementSize == 1 && force->Offset_Internal >= 0 && force->Offset_Internal < sizeof(params))
            params[force->Offset_Internal] = 1;
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(page), page, pop, params)) return false;
        g_ChallengeSetupPage = nullptr;
        g_ChallengeContinuingNativeStart = true;
        const bool started = SafeProcessEventCall(reinterpret_cast<uintptr_t>(source), source, start, nullptr);
        g_ChallengeContinuingNativeStart = false;
        if (started)
            Logger::Success(committed ? "CHALLENGE SETUP: committed modifiers; forwarded actual native BeginChallenge"
                                      : "CHALLENGE SETUP: forwarded native BeginChallenge without modifiers");
        else Logger::Error("CHALLENGE SETUP: native challenge start failed");
        return started;
    }

    static void AddOfflineSettingsRows(UObject* settings, UObject* skillCombo)
    {
        if (g_WeatherSettingsWidget.load() == reinterpret_cast<uintptr_t>(settings))
            return;
        g_WeatherSettingsWidget.store(reinterpret_cast<uintptr_t>(settings));
        // Do not run whole-registry discovery from a menu construction event.
        // The 2026-09-26 trace blocked this transition for twelve seconds.
        g_ActiveWeatherCombo.store(0);
        g_ActiveJasonDifficultyCombo.store(0);
        g_ActiveMatchLengthCombo.store(0);
        g_ActiveJasonUnlimitedKnivesCombo.store(0);
        g_ActiveJasonFastRechargeCombo.store(0);
        g_ActiveJasonFastMovementCombo.store(0);
        g_ActiveStartingWeaponCombo.store(0);
        g_ActiveStartingInventoryCombo.store(0);
        static PersistentRuntimeText titles[8]{};
        static const wchar_t* weather[3] = { L"Off", L"Rain", L"Random" };
        static const wchar_t* difficulty[3] = { L"Easy", L"Normal", L"Hard" };
        static const wchar_t* duration[4] = { L"30 minutes", L"60 minutes", L"90 minutes", L"Unlimited" };
        UObject* weatherCombo = CreateOfflineSettingsCombo(
            settings, skillCombo, L"Weather", weather, 2, &titles[0]);
        UObject* difficultyCombo = CreateOfflineSettingsCombo(
            settings, skillCombo, L"Jason Difficulty", difficulty, 1, &titles[1]);
        UObject* durationCombo = CreateOfflineSettingsCombo(
            settings, skillCombo, L"Match Length", duration, 1, &titles[2],
            g_CounselorMenuRouteLatched.load() ? 3 : 4);
        g_ActiveWeatherCombo.store(reinterpret_cast<uintptr_t>(weatherCombo));
        g_ActiveJasonDifficultyCombo.store(reinterpret_cast<uintptr_t>(difficultyCombo));
        g_ActiveMatchLengthCombo.store(reinterpret_cast<uintptr_t>(durationCombo));
        if (!g_CounselorMenuRouteLatched.load())
        {
            static const wchar_t* toggle[2] = { L"Off", L"On" };
            g_ActiveJasonUnlimitedKnivesCombo.store(reinterpret_cast<uintptr_t>(
                CreateOfflineSettingsCombo(settings, skillCombo,
                    L"Unlimited Throwing Knives", toggle, 0, &titles[5], 2)));
            g_ActiveJasonFastRechargeCombo.store(reinterpret_cast<uintptr_t>(
                CreateOfflineSettingsCombo(settings, skillCombo,
                    L"Faster Ability Recharge", toggle, 0, &titles[6], 2)));
            g_ActiveJasonFastMovementCombo.store(reinterpret_cast<uintptr_t>(
                CreateOfflineSettingsCombo(settings, skillCombo,
                    L"Faster Jason Movement (2x)", toggle, 0, &titles[7], 2)));
        }
        if (g_CounselorMenuRouteLatched.load())
        {
            static const wchar_t* weapons[5] = { L"None", L"Random", L"Machete", L"Axe", L"Shotgun" };
            static const wchar_t* inventory[6] = { L"None", L"Random", L"Pocket Knife",
                L"First Aid Spray", L"Firecrackers", L"Map" };
            g_ActiveStartingWeaponCombo.store(reinterpret_cast<uintptr_t>(CreateOfflineSettingsCombo(
                settings, skillCombo, L"Starting Weapon", weapons, 0, &titles[3], 5)));
            g_ActiveStartingInventoryCombo.store(reinterpret_cast<uintptr_t>(CreateOfflineSettingsCombo(
                settings, skillCombo, L"Starting Inventory", inventory, 0, &titles[4], 6)));
            UObject* countCombo = ReadRuntimeWidgetObject(settings, "CounselorCountComboBox");
            UFunction* setOptions = countCombo && countCombo->Class
                ? FindFunctionInHierarchyByName(countCombo->Class, "SetOptions") : nullptr;
            if (setOptions)
            {
                static const wchar_t* counts[10] = { L"1",L"2",L"3",L"4",L"5",L"6",L"7",L"8",L"9",L"10" };
                RawFString entries[10]{};
                for (int i = 0; i < 10; ++i)
                {
                    entries[i].Data = const_cast<wchar_t*>(counts[i]);
                    entries[i].Count = entries[i].Max = static_cast<int32_t>(wcslen(counts[i])) + 1;
                }
                RawArray options{ reinterpret_cast<uint8_t*>(entries),10,10 };
                int32_t selected = ReadRuntimeComboIndex(countCombo, 9);
                if (selected < 0) selected = 6;
                alignas(16) uint8_t params[0x200]{};
                if (WriteRuntimeCallArgument(setOptions, params, "Items", &options, sizeof(options)) &&
                    WriteRuntimeCallArgument(setOptions, params, "SelectedIndex", &selected, sizeof(selected)) &&
                    SafeProcessEventCall(reinterpret_cast<uintptr_t>(countCombo), countCombo, setOptions, params))
                    Logger::Success("COUNSELOR COUNT: options 1-10 | total includes human | additional slots share F3 cap");
            }
        }
        Logger::Success(std::string("OFFLINE SETTINGS ROWS: route=") +
            (g_CounselorMenuRouteLatched.load() ? "Counselor" : "Jason") +
            " | weather=" + (weatherCombo ? "ready" : "missing") +
            " | JasonDifficulty=" + (difficultyCombo ? "ready" : "missing") +
            " | MatchLength=" + (durationCombo ? "ready" : "missing"));
    }

    static void CommitOfflineMatchSettings(UObject* settings)
    {
#if defined(F13_BASE_GAME_PORT)
        g_OfflineWeatherApplyPending.store(false);
        g_CommittedOfflineRain.store(-1);
        g_CommittedMatchSeconds.store(-1);
        g_CommittedJasonDifficulty.store(-1);
        g_CommittedJasonUnlimitedKnives.store(false);
        g_CommittedJasonFastRecharge.store(false);
        g_CommittedJasonFastMovement.store(false);
        g_CommittedStartingWeapon.store(0);
        g_CommittedStartingInventory.store(0);
        g_StartingLoadoutStage.store(0);
        g_StartingItemPending.store(false);
        g_CommittedSettingsWorld.store(0);
        g_OfflineModifiersApplied.store(false);
        if (!settings || reinterpret_cast<uintptr_t>(settings) != g_WeatherSettingsWidget.load())
            return;
        if (g_CounselorMenuRouteLatched.load())
        {
            int32_t weapon = ReadRuntimeComboIndex(reinterpret_cast<UObject*>(g_ActiveStartingWeaponCombo.load()), 4);
            int32_t inventory = ReadRuntimeComboIndex(reinterpret_cast<UObject*>(g_ActiveStartingInventoryCombo.load()), 5);
            const ULONGLONG randomSeed = GetTickCount64();
            if (weapon == 1) weapon = 2 + static_cast<int32_t>(randomSeed % 3);
            if (inventory == 1) inventory = 2 + static_cast<int32_t>((randomSeed / 3) % 4);
            g_CommittedStartingWeapon.store(weapon > 0 ? weapon : 0);
            g_CommittedStartingInventory.store(inventory > 0 ? inventory : 0);
            Logger::Success("STARTING LOADOUT COMMITTED: weapon=" + std::to_string(g_CommittedStartingWeapon.load()) +
                " | inventory=" + std::to_string(g_CommittedStartingInventory.load()) + " | human counselor only");
        }
        else
        {
            const int knives = ReadRuntimeComboIndex(reinterpret_cast<UObject*>(
                g_ActiveJasonUnlimitedKnivesCombo.load()), 1);
            const int recharge = ReadRuntimeComboIndex(reinterpret_cast<UObject*>(
                g_ActiveJasonFastRechargeCombo.load()), 1);
            const int movement = ReadRuntimeComboIndex(reinterpret_cast<UObject*>(
                g_ActiveJasonFastMovementCombo.load()), 1);
            g_CommittedJasonUnlimitedKnives.store(knives == 1);
            g_CommittedJasonFastRecharge.store(recharge == 1);
            g_CommittedJasonFastMovement.store(movement == 1);
            Logger::Success("OFFLINE JASON CHALLENGE-STYLE OPTIONS COMMITTED: knives=" +
                std::to_string(knives == 1) + " | recharge=" +
                std::to_string(recharge == 1) + " | movement=" +
                std::to_string(movement == 1));
        }
        const int32_t duration = ReadRuntimeComboIndex(
            reinterpret_cast<UObject*>(g_ActiveMatchLengthCombo.load()),
            g_CounselorMenuRouteLatched.load() ? 2 : 3);
        if (duration >= 0)
        {
            const int32_t seconds = duration == 3 ? -2 : (duration + 1) * 1800;
            g_CommittedMatchSeconds.store(seconds);
            Logger::Success("OFFLINE MATCH LENGTH COMMITTED: " +
                (seconds == -2 ? std::string("Unlimited") :
                    std::to_string(seconds) + " seconds") + " | route=" +
                (g_CounselorMenuRouteLatched.load() ? "Counselor" : "Jason"));
        }
        const int32_t difficulty = ReadRuntimeComboIndex(
            reinterpret_cast<UObject*>(g_ActiveJasonDifficultyCombo.load()));
        g_CommittedJasonDifficulty.store(difficulty);
        if (difficulty >= 0)
            Logger::Success(std::string("OFFLINE JASON DIFFICULTY COMMITTED: ") +
                (difficulty == 0 ? "Easy" : difficulty == 1 ? "Normal" : "Hard") +
                " | normal starting state | Rage threshold scale=" +
                (difficulty == 0 ? "2" : difficulty == 1 ? "1" : "0.5"));
        g_WeatherTravelSourceWorld.store(reinterpret_cast<uintptr_t>(Engine::GetWorld()));
        g_WeatherApplyStartedAt.store(0);
        g_NextWeatherApplyAt.store(0);
        UObject* combo = reinterpret_cast<UObject*>(g_ActiveWeatherCombo.load());
        const int32_t selected = ReadRuntimeComboIndex(combo);
        if (selected < 0)
        {
            Logger::Error("Offline weather commit: combo index invalid; stock rain preserved");
            return;
        }
        const int32_t rain = selected == 2 ? static_cast<int32_t>(GetTickCount64() & 1) : selected;
        g_NativeSelectedWeather.store(selected);
        g_CommittedOfflineRain.store(rain);
        // Arm application only in the verified OfflineBots match-start hook,
        // not during travel or authentication/frontend actor ticks.
        Logger::Success(std::string("OFFLINE WEATHER COMMITTED: selection=") +
            (selected == 0 ? "Off" : selected == 1 ? "Rain" : "Random") +
            " | resolved=" + (rain ? "Rain" : "Off") +
            " | route=" + (g_CounselorMenuRouteLatched.load() ? "Counselor" : "Jason"));
#endif
    }

    static void GameSetupConstructExecHook(
        void* context,
        void* stack,
        void* result)
    {
        if (g_OriginalGameSetupConstructExec)
            g_OriginalGameSetupConstructExec(context, stack, result);

        UObject* settings = reinterpret_cast<UObject*>(context);
        if (!settings || !Memory::IsReadable(settings, sizeof(UObject)) ||
            !settings->Class ||
            SafeName(reinterpret_cast<UObject*>(settings->Class)) !=
                "OfflineBotsSettingsMenuWidget_C")
        {
            return;
        }

        g_GameSetupSelectionLocked.store(false);

        auto* comboProperty = FindPropertyInHierarchyByName(
            settings->Class, "DifficultyComboBox");
        if (!comboProperty || comboProperty->Offset_Internal <= 0 ||
            comboProperty->Offset_Internal >= 0x10000)
        {
            Logger::Error("Game Setup label: DifficultyComboBox property missing");
            return;
        }
        UObject** comboSlot = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(settings) +
            comboProperty->Offset_Internal);
        if (!Memory::IsReadable(comboSlot, sizeof(UObject*)) ||
            !*comboSlot || !Memory::IsReadable(*comboSlot, sizeof(UObject)))
        {
            Logger::Error("Game Setup label: DifficultyComboBox unavailable");
            return;
        }

        AddOfflineSettingsRows(settings, *comboSlot);

        static PersistentRuntimeText label{};
        g_RuntimeMenuLabelFailureCode.store(0);
        const bool titleRelabeled = SetWideMenuButtonText(
            *comboSlot, L"Counselor Bot Skill", &label);
        g_ActiveSkillCombo.store(
            reinterpret_cast<uintptr_t>(*comboSlot));
        g_ActiveSkillValueText.store(0);
        g_LastSkillDisplayedIndex.store(-1);
        g_NextSkillDisplayPumpAt.store(0);
        UPropertyLite* valueTextProperty = FindPropertyInHierarchyByName(
            (*comboSlot)->Class, "ValueText");
        if (valueTextProperty &&
            valueTextProperty->Offset_Internal > 0 &&
            valueTextProperty->Offset_Internal < 0x10000 &&
            valueTextProperty->ElementSize == sizeof(UObject*))
        {
            UObject** valueTextSlot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(*comboSlot) +
                valueTextProperty->Offset_Internal);
            if (Memory::IsReadable(valueTextSlot, sizeof(UObject*)) &&
                *valueTextSlot &&
                Memory::IsReadable(*valueTextSlot, sizeof(UObject)))
            {
                g_ActiveSkillValueText.store(
                    reinterpret_cast<uintptr_t>(*valueTextSlot));
            }
        }
        const bool valueHooked = InstallSkillComboUbergraphHook(*comboSlot);
        RefreshCounselorSkillValue(*comboSlot);
        if (titleRelabeled)
        {
            Logger::Success(
                std::string("Game Setup: Counselor Bot Skill shows Low/Medium/High over the unchanged native difficulty indices | valueHook=") +
                (valueHooked ? "true" : "false") +
                " | valueText=" +
                (g_ActiveSkillValueText.load() ? "ready" : "missing"));
        }
        else
        {
            Logger::Error(
                "Game Setup Counselor Bot Skill title unavailable" +
                std::string(" | title=") +
                std::to_string(titleRelabeled) +
                " | code=" +
                std::to_string(g_RuntimeMenuLabelFailureCode.load()));
        }
    }

    static bool InstallGameSetupConstructHook(UClass* settingsClass)
    {
        if (!settingsClass ||
            !Memory::IsReadable(settingsClass, sizeof(UClass)))
        {
            return false;
        }
        UFunction* construct = FindFunctionInHierarchyByName(
            settingsClass, "Construct");
        if (!construct ||
            !Memory::IsReadable(construct, sizeof(UFunction)) ||
            !construct->ExecFunction)
        {
            return false;
        }
        if (construct->ExecFunction == &GameSetupConstructExecHook)
        {
            g_GameSetupConstructHookInstalled.store(true);
            return true;
        }

        g_GameSetupConstructHookInstalled.store(false);
        auto original = construct->ExecFunction;
        DWORD oldProtect = 0;
        void* targetSlot = &construct->ExecFunction;
        if (!VirtualProtect(
                targetSlot, sizeof(construct->ExecFunction),
                PAGE_READWRITE, &oldProtect))
        {
            return false;
        }
        g_OriginalGameSetupConstructExec = original;
        construct->ExecFunction = &GameSetupConstructExecHook;
        DWORD ignoredProtect = 0;
        VirtualProtect(
            targetSlot, sizeof(construct->ExecFunction),
            oldProtect, &ignoredProtect);
        g_GameSetupConstructHookInstalled.store(true);
        return true;
    }

    static bool ReorderRuntimeOfflinePlayMenu(UObject* menuWidget)
    {
        if (!menuWidget || !menuWidget->Class)
        {
            g_RuntimeMenuOrderFailureCode.store(301);
            return false;
        }

        if (g_RuntimeMenuOrderedWidget.load() ==
            reinterpret_cast<uintptr_t>(menuWidget))
        {
            return true;
        }

        const auto readObjectProperty = [menuWidget](
            const char* propertyName) -> UObject*
        {
            auto* property = FindPropertyInHierarchyByName(
                menuWidget->Class,
                propertyName);
            if (!property || property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000)
            {
                return nullptr;
            }

            UObject** slot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(menuWidget) +
                property->Offset_Internal);
            return Memory::IsReadable(slot, sizeof(UObject*))
                ? *slot
                : nullptr;
        };

        UObject* verticalBox = readObjectProperty("VerticalBox_95");
        UObject* jason = readObjectProperty("OfflineBotsButton");
        UObject* counselor = readObjectProperty(
            g_UsingStockBaseCounselorRow.load()
                ? "OfflineBotsButtonC"
                : (g_UsingStockBaseSandboxRow.load()
                    ? "SandboxButton"
                    : "VC3Button"));

        // VerticalBox_95 is not marked IsVariable in the stock generated
        // class, so it has no direct UObject property on the menu instance.
        // Resolve the same live panel through the counselor widget's native
        // UWidget::Slot -> UPanelSlot::Parent relationship instead.
        if (!verticalBox && counselor && counselor->Class)
        {
            auto* slotProperty = FindPropertyInHierarchyByName(
                counselor->Class,
                "Slot");
            UObject* panelSlot = nullptr;
            if (slotProperty && slotProperty->Offset_Internal > 0 &&
                slotProperty->Offset_Internal < 0x10000)
            {
                UObject** slotAddress = reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(counselor) +
                    slotProperty->Offset_Internal);
                if (Memory::IsReadable(slotAddress, sizeof(UObject*)))
                    panelSlot = *slotAddress;
            }

            if (panelSlot && panelSlot->Class)
            {
                auto* parentProperty = FindPropertyInHierarchyByName(
                    panelSlot->Class,
                    "Parent");
                if (parentProperty && parentProperty->Offset_Internal > 0 &&
                    parentProperty->Offset_Internal < 0x10000)
                {
                    UObject** parentAddress = reinterpret_cast<UObject**>(
                        reinterpret_cast<uintptr_t>(panelSlot) +
                        parentProperty->Offset_Internal);
                    if (Memory::IsReadable(parentAddress, sizeof(UObject*)))
                        verticalBox = *parentAddress;
                }

                if (!verticalBox &&
                    Memory::IsReadable(panelSlot->OuterPrivate, sizeof(UObject)))
                {
                    verticalBox = panelSlot->OuterPrivate;
                }
            }
        }

        if (!verticalBox || !verticalBox->Class || !jason || !counselor ||
            jason == counselor)
        {
            g_RuntimeMenuOrderFailureCode.store(302);
            return false;
        }

        UFunction* clearChildren = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "ClearChildren");
        UFunction* addChild = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "AddChildToVerticalBox");
        UFunction* getChildAt = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "GetChildAt");
        UFunction* getChildrenCount = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "GetChildrenCount");
        auto* contentProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(addChild),
            "Content");
        if (!contentProperty)
        {
            contentProperty = FindFunctionPropertyByType(
                addChild,
                "ObjectProperty",
                false);
        }

        auto* addReturnProperty = FindFunctionPropertyByType(
            addChild,
            "ObjectProperty",
            true);
        auto* childIndexProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(getChildAt),
            "Index");
        if (!childIndexProperty)
        {
            childIndexProperty = FindFunctionPropertyByType(
                getChildAt,
                "IntProperty",
                false);
        }
        auto* childReturnProperty = FindFunctionPropertyByType(
            getChildAt,
            "ObjectProperty",
            true);
        auto* countReturnProperty = FindFunctionPropertyByType(
            getChildrenCount,
            "IntProperty",
            true);

        if (!clearChildren || !addChild || !getChildAt || !getChildrenCount ||
            !contentProperty || !addReturnProperty ||
            !childIndexProperty || !childReturnProperty ||
            !countReturnProperty ||
            contentProperty->Offset_Internal < 0 ||
            contentProperty->ElementSize != sizeof(UObject*) ||
            addReturnProperty->Offset_Internal < 0 ||
            addReturnProperty->ElementSize != sizeof(UObject*) ||
            childIndexProperty->Offset_Internal < 0 ||
            childIndexProperty->ElementSize != sizeof(int32_t) ||
            childReturnProperty->Offset_Internal < 0 ||
            childReturnProperty->ElementSize != sizeof(UObject*) ||
            countReturnProperty->Offset_Internal < 0 ||
            countReturnProperty->ElementSize != sizeof(int32_t))
        {
            g_RuntimeMenuOrderFailureCode.store(303);
            return false;
        }

        size_t addParameterBytes = std::max(
            static_cast<size_t>(addChild->Size),
            static_cast<size_t>(contentProperty->Offset_Internal) +
                sizeof(UObject*));
        addParameterBytes = std::max(
            addParameterBytes,
            static_cast<size_t>(addReturnProperty->Offset_Internal) +
                sizeof(UObject*));
        size_t getParameterBytes = std::max(
            static_cast<size_t>(getChildAt->Size),
            static_cast<size_t>(childIndexProperty->Offset_Internal) +
                sizeof(int32_t));
        getParameterBytes = std::max(
            getParameterBytes,
            static_cast<size_t>(childReturnProperty->Offset_Internal) +
                sizeof(UObject*));
        const size_t countParameterBytes = std::max(
            static_cast<size_t>(getChildrenCount->Size),
            static_cast<size_t>(countReturnProperty->Offset_Internal) +
                sizeof(int32_t));
        if (addParameterBytes == 0 || addParameterBytes > 0x200 ||
            getParameterBytes == 0 || getParameterBytes > 0x200 ||
            countParameterBytes == 0 || countParameterBytes > 0x200)
        {
            g_RuntimeMenuOrderFailureCode.store(304);
            return false;
        }

        const auto addOne = [verticalBox, addChild, contentProperty,
            addReturnProperty, addParameterBytes](UObject* child) -> bool
        {
            std::vector<uint8_t> params(addParameterBytes, 0);
            memcpy(
                params.data() + contentProperty->Offset_Internal,
                &child,
                sizeof(child));
            if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                addChild,
                params.data()))
            {
                return false;
            }

            UObject* returnedSlot = nullptr;
            memcpy(
                &returnedSlot,
                params.data() + addReturnProperty->Offset_Internal,
                sizeof(returnedSlot));
            return returnedSlot &&
                Memory::IsReadable(returnedSlot, sizeof(UObject));
        };

        const auto getChild = [verticalBox, getChildAt,
            childIndexProperty, childReturnProperty,
            getParameterBytes](int32_t index) -> UObject*
        {
            std::vector<uint8_t> params(getParameterBytes, 0);
            memcpy(
                params.data() + childIndexProperty->Offset_Internal,
                &index,
                sizeof(index));
            if (!SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(verticalBox),
                    verticalBox,
                    getChildAt,
                    params.data()))
            {
                return nullptr;
            }

            UObject* child = nullptr;
            memcpy(
                &child,
                params.data() + childReturnProperty->Offset_Internal,
                sizeof(child));
            return child;
        };

        // Snapshot the actual stock panel before changing it. Stock builds
        // differ from Resurrected: some have a dedicated counselor row,
        // others repurpose Sandbox. Move only that row, preserving every
        // other native item and its relative position.
        std::vector<uint8_t> countParams(countParameterBytes, 0);
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                getChildrenCount,
                countParams.data()))
        {
            g_RuntimeMenuOrderFailureCode.store(308);
            return false;
        }
        int32_t childCount = 0;
        memcpy(&childCount,
            countParams.data() + countReturnProperty->Offset_Internal,
            sizeof(childCount));
        if (childCount < 2 || childCount > 16)
        {
            g_RuntimeMenuOrderFailureCode.store(309);
            return false;
        }
        std::vector<UObject*> original;
        original.reserve(static_cast<size_t>(childCount));
        for (int32_t index = 0; index < childCount; ++index)
        {
            UObject* child = getChild(index);
            if (!child ||
                std::find(original.begin(), original.end(), child) !=
                    original.end())
            {
                g_RuntimeMenuOrderFailureCode.store(310);
                return false;
            }
            original.push_back(child);
        }
        if (std::count(original.begin(), original.end(), jason) != 1 ||
            std::count(original.begin(), original.end(), counselor) != 1)
        {
            g_RuntimeMenuOrderFailureCode.store(311);
            return false;
        }
        std::vector<UObject*> desired = original;
        desired.erase(std::find(desired.begin(), desired.end(), counselor));
        desired.insert(std::find(desired.begin(), desired.end(), jason) + 1,
            counselor);
        if (desired == original)
        {
            g_RuntimeMenuOrderedWidget.store(
                reinterpret_cast<uintptr_t>(menuWidget));
            return true;
        }

        // Native clear/re-add changes only the live Slate order. No raw
        // Slate pointers or cooked asset bytes change.
        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                clearChildren,
                nullptr))
        {
            g_RuntimeMenuOrderFailureCode.store(305);
            return false;
        }

        bool complete = true;
        for (UObject* child : desired)
        {
            if (!addOne(child))
            {
                complete = false;
                break;
            }
        }

        if (!complete)
        {
            // Restore the original stock order if an unexpected native add
            // fails, rather than leaving a partially populated menu.
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                clearChildren,
                nullptr);
            for (UObject* child : original)
                addOne(child);
            g_RuntimeMenuOrderFailureCode.store(306);
            return false;
        }

        bool verified = true;
        for (int32_t index = 0;
            index < static_cast<int32_t>(desired.size());
            ++index)
        {
            if (getChild(index) != desired[static_cast<size_t>(index)])
            {
                verified = false;
                break;
            }
        }
        if (!verified)
        {
            g_RuntimeMenuOrderFailureCode.store(307);
            return false;
        }

        // UMG can retain cached Slate geometry after children are rebuilt at
        // runtime. Invalidate both the panel and owning menu, then force a
        // prepass so the verified logical order is also the rendered order.
        UFunction* invalidatePanel = FindFunctionInHierarchyByName(
            verticalBox->Class,
            "InvalidateLayoutAndVolatility");
        UFunction* invalidateMenu = FindFunctionInHierarchyByName(
            menuWidget->Class,
            "InvalidateLayoutAndVolatility");
        UFunction* forcePrepass = FindFunctionInHierarchyByName(
            menuWidget->Class,
            "ForceLayoutPrepass");
        if (invalidatePanel)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(verticalBox),
                verticalBox,
                invalidatePanel,
                nullptr);
        }
        if (invalidateMenu)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(menuWidget),
                menuWidget,
                invalidateMenu,
                nullptr);
        }
        if (forcePrepass)
        {
            SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(menuWidget),
                menuWidget,
                forcePrepass,
                nullptr);
        }

        g_RuntimeMenuOrderedWidget.store(
            reinterpret_cast<uintptr_t>(menuWidget));
        g_RuntimeMenuOrderFailureLogged.store(false);
        g_RuntimeMenuOrderFailureCode.store(0);
        Logger::Success(
            "OFFLINE MENU ORDER VERIFIED: Counselor immediately follows Jason; other rows preserved");
        return true;
    }

    static bool LabelRuntimeOfflinePlayMenu(UObject* menuWidget)
    {
        if (!menuWidget || !menuWidget->Class ||
            !Memory::IsReadable(menuWidget, sizeof(UObject)))
        {
            return false;
        }

        if (g_RuntimeMenuLabeledWidget.load() ==
            reinterpret_cast<uintptr_t>(menuWidget))
        {
            if (g_RuntimeMenuOrderedWidget.load() !=
                reinterpret_cast<uintptr_t>(menuWidget))
            {
                const ULONGLONG now = GetTickCount64();
                const ULONGLONG lastAttempt =
                    g_RuntimeMenuLabelLastAttempt.load();
                if ((!lastAttempt || now - lastAttempt >= 500) &&
                    !g_RuntimeMenuOrderRequested.exchange(true))
                {
                    g_RuntimeMenuLabelLastAttempt.store(now);
                    g_RuntimeMenuOrderWidget.store(
                        reinterpret_cast<uintptr_t>(menuWidget));
                }
            }
            return true;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastAttempt =
            g_RuntimeMenuLabelLastAttempt.load();
        if (lastAttempt && now - lastAttempt < 500)
            return false;
        g_RuntimeMenuLabelLastAttempt.store(now);

        const bool stockBaseRow =
            g_UsingStockBaseCounselorRow.load();
        const bool stockBaseSandboxRow =
            g_UsingStockBaseSandboxRow.load();
        auto* counselorButtonProperty = FindPropertyInHierarchyByName(
            menuWidget->Class,
            stockBaseRow
                ? "OfflineBotsButtonC"
                : (stockBaseSandboxRow
                    ? "SandboxButton"
                    : "VC3Button"));
        auto* jasonButtonProperty = FindPropertyInHierarchyByName(
            menuWidget->Class,
            "OfflineBotsButton");
        if (!counselorButtonProperty || !jasonButtonProperty)
            return false;

        const auto readButton = [menuWidget](
            UPropertyLite* property) -> UObject*
        {
            if (property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000)
            {
                return nullptr;
            }

            UObject** slot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(menuWidget) +
                property->Offset_Internal);
            return Memory::IsReadable(slot, sizeof(UObject*))
                ? *slot
                : nullptr;
        };

        UObject* counselorButton = readButton(counselorButtonProperty);
        UObject* jasonButton = readButton(jasonButtonProperty);
        if (!counselorButton || !jasonButton)
            return false;

        // FText owns reference-counted history. A prior menu instance releases
        // its text when the frontend world is replaced, so never reuse that
        // instance's raw FText bytes for a replay menu. Each successful label
        // gets its own intentionally process-lifetime conversion buffer, and
        // the native TextBlock::SetText call performs the owned copy.
        PersistentRuntimeText counselorText{};
        PersistentRuntimeText jasonText{};
        g_RuntimeMenuLabelFailureCode.store(0);
        const bool counselorLabeled = SetWideMenuButtonText(
            counselorButton,
            L"Offline Bots - Play as a Counselor",
            &counselorText);
        const int32_t counselorFailure =
            g_RuntimeMenuLabelFailureCode.load();
        g_RuntimeMenuLabelFailureCode.store(0);
        const bool jasonLabeled = SetWideMenuButtonText(
            jasonButton,
            L"Offline Bots - Play as Jason",
            &jasonText);
        const int32_t jasonFailure =
            g_RuntimeMenuLabelFailureCode.load();

        if (!counselorLabeled || !jasonLabeled)
        {
            if (!g_RuntimeMenuLabelFailureLogged.exchange(true))
            {
                Logger::Error(
                    "Packed menu label diagnostic | CounselorCode=" +
                    std::to_string(counselorFailure) +
                    " | JasonCode=" +
                    std::to_string(jasonFailure));
            }
            return false;
        }

        g_RuntimeMenuLabeledWidget.store(
            reinterpret_cast<uintptr_t>(menuWidget));
        g_RuntimeMenuLabelFailureLogged.store(false);
        Logger::Success(
            stockBaseSandboxRow
                ? "STOCK PACKED MENU READY: Sandbox row is now Offline Bots - Play as a Counselor and forwards into the native Offline Bots flow"
                : stockBaseRow
                ? "STOCK MENU READY: native OfflineBotsButtonC row exposed as Offline Bots - Play as a Counselor without a replacement PAK"
                : "PACKED MENU READY: stock VC3 row labeled Offline Bots - Play as a Counselor and stock Jason row labeled without a replacement PAK");
        const uintptr_t menuAddress =
            reinterpret_cast<uintptr_t>(menuWidget);
        if (g_RuntimeMenuOrderedWidget.load() != menuAddress)
        {
            g_RuntimeMenuOrderWidget.store(menuAddress);
            g_RuntimeMenuOrderRequested.store(true);
        }
        return true;
    }

    static bool InstallCounselorEntryClickHook(UObject* menu, bool logUnavailable);

    static void VC3VisibilityExecHook(
        void* context,
        void* stack,
        void* result)
    {
        if (g_OriginalVC3VisibilityExec)
            g_OriginalVC3VisibilityExec(context, stack, result);

        if (!g_CounselorMenuRouteEnabled.load())
            return;

        UObject* menuWidget = reinterpret_cast<UObject*>(context);
        if (result && Memory::IsReadable(result, sizeof(uint8_t)))
        {
            // UE4 ESlateVisibility::Visible is the zero-valued enum entry.
            *reinterpret_cast<uint8_t*>(result) = 0;
        }

        // Repair once per visible widget. Missing VC3/donor functions make
        // the fallback search walk GObjects; doing that on every Slate
        // visibility evaluation stalls the menu even without gameplay AI.
        static UObject* repairedWidget = nullptr;
        static ULONGLONG nextRepairAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (repairedWidget != menuWidget && now >= nextRepairAt)
        {
            nextRepairAt = now + 1000;
            const ULONGLONG startedAt = GetTickCount64();
            if (InstallCounselorEntryClickHook(menuWidget, false))
                repairedWidget = menuWidget;
            Logger::Debug("18L-BV visible menu click repair | ms=" +
                std::to_string(GetTickCount64() - startedAt));
        }
        LabelRuntimeOfflinePlayMenu(menuWidget);
    }

    static bool InstallVC3VisibilityHook(bool logUnavailable = true)
    {
        UClass* offlinePlayMenuClass = GetCachedClass(
            g_OfflinePlayMenuClass,
            "OfflinePlayMenuWidget_C");
        if (!offlinePlayMenuClass)
            return false;

        // The visibility event is the lifecycle trigger; the worker only
        // verifies its known slot as a relink safety net. Do not search the
        // entire object table again while the same class/slot is installed.
        auto* installedVisibility = reinterpret_cast<UFunction*>(
            g_VC3VisibilityHookTarget.load());
        if (installedVisibility &&
            Memory::IsReadable(installedVisibility, sizeof(UFunction)) &&
            reinterpret_cast<UObject*>(installedVisibility)->OuterPrivate == reinterpret_cast<UObject*>(offlinePlayMenuClass) &&
            installedVisibility->ExecFunction == &VC3VisibilityExecHook)
        {
            // ArmSelectedPreset resets per-route layout flags. The UFunction
            // hook itself survives a Challenges trip, so its fast path must
            // restore the layout instead of silently selecting absent VC3 rows.
            // Read only this already-validated function; no GObjects search.
            const bool sandbox = SafeNameEquals(reinterpret_cast<UObject*>(installedVisibility), "Get_SandboxButton_Visibility");
            const bool counselor = SafeNameEquals(reinterpret_cast<UObject*>(installedVisibility), "Get_OfflineBotsCButton_Visibility");
            const bool vc3 = SafeNameEquals(reinterpret_cast<UObject*>(installedVisibility), "Get_VC3Button_Visibility");
            if (sandbox || counselor || vc3)
            {
                g_UsingStockBaseSandboxRow.store(sandbox);
                g_UsingStockBaseCounselorRow.store(counselor);
                return true;
            }
        }

        const bool packedSandboxLayout = g_UsingStockBaseSandboxRow.load();
        UFunction* visibility = packedSandboxLayout ? nullptr :
            FindFunctionInHierarchyByName(offlinePlayMenuClass,
                "Get_VC3Button_Visibility");
        if (!visibility && !packedSandboxLayout)
        {
            visibility = FindFunctionObjectExact(
                offlinePlayMenuClass,
                "Get_VC3Button_Visibility");
        }
        bool stockBaseRow = false;
        bool stockBaseSandboxRow = false;
        if (!visibility && !packedSandboxLayout)
        {
            visibility = FindFunctionInHierarchyByName(
                offlinePlayMenuClass,
                "Get_OfflineBotsCButton_Visibility");
            if (!visibility)
            {
                visibility = FindFunctionObjectExact(
                    offlinePlayMenuClass,
                    "Get_OfflineBotsCButton_Visibility");
            }
            stockBaseRow = visibility != nullptr;
        }
        if (!visibility)
        {
            // The untouched packed base-game asset has no VC3 or dedicated
            // counselor row.  Its stable four-row surface is SandboxButton.
            visibility = FindFunctionInHierarchyByName(
                offlinePlayMenuClass,
                "Get_SandboxButton_Visibility");
            if (!visibility)
            {
                visibility = FindFunctionObjectExact(
                    offlinePlayMenuClass,
                    "Get_SandboxButton_Visibility");
            }
            stockBaseSandboxRow = visibility != nullptr;
        }
        if (!visibility ||
            !Memory::IsReadable(visibility, sizeof(UFunction)) ||
            !visibility->ExecFunction)
        {
            if (logUnavailable)
            {
                Logger::Error(
                    "Offline Play menu: VC3, donor counselor, and packed Sandbox visibility bindings were unavailable");
            }
            return false;
        }

        InstallChallengeEntryHook(offlinePlayMenuClass);

        g_UsingStockBaseCounselorRow.store(stockBaseRow);
        g_UsingStockBaseSandboxRow.store(stockBaseSandboxRow);
        g_VC3VisibilityHookTarget.store(reinterpret_cast<uintptr_t>(visibility));
        if (visibility->ExecFunction == &VC3VisibilityExecHook)
        {
            g_VC3VisibilityHookInstalled.store(true);
            return true;
        }

        g_OriginalVC3VisibilityExec = visibility->ExecFunction;
        g_UsingStockBaseCounselorRow.store(stockBaseRow);
        g_UsingStockBaseSandboxRow.store(stockBaseSandboxRow);
        DWORD oldProtect = 0;
        void* targetSlot = &visibility->ExecFunction;
        if (!VirtualProtect(
                targetSlot,
                sizeof(visibility->ExecFunction),
                PAGE_READWRITE,
                &oldProtect))
        {
            g_OriginalVC3VisibilityExec = nullptr;
            return false;
        }

        visibility->ExecFunction = &VC3VisibilityExecHook;
        DWORD ignoredProtect = 0;
        VirtualProtect(
            targetSlot,
            sizeof(visibility->ExecFunction),
            oldProtect,
            &ignoredProtect);

        g_VC3VisibilityHookInstalled.store(true);
        Logger::Success(
            stockBaseSandboxRow
                ? "Stock packed menu: SandboxButton visibility hook installed for the counselor surface"
                : stockBaseRow
                ? "Stock menu: dormant OfflineBotsButtonC visibility hook installed"
                : "Packed menu: dormant stock VC3 row visibility hook installed");
        return true;
    }

    static void CounselorEntryExecHook(
        void* context,
        void* stack,
        void* result)
    {
        if (g_CounselorMenuRouteEnabled.load() &&
            g_UsingStockBaseSandboxRow.load())
        {
            // Use the stock game's own Counselor_Menu before the verified
            // map -> Jason -> setup sequence. Preserve the last working
            // direct route if the selector cannot be opened on this build.
            auto* menuWidget = reinterpret_cast<UObject*>(context);
            g_CounselorEntryClickPending.store(true);
            g_CounselorEntryClickedAt.store(GetTickCount64());
            if (OpenIntegratedCounselorPicker(menuWidget))
            {
                Logger::Success(
                    "STOCK COUNSELOR PICKER OPEN: choose the human counselor before map and Jason");
                return;
            }
            g_CounselorEntryClickPending.store(false);
            Logger::Error(
                "STOCK COUNSELOR PICKER: native selector unavailable; retaining the verified direct Offline Bots route");

            // The untouched packed base game has no counselor/VC3 row. Do not
            // execute Sandbox bytecode; forward to its real Offline Bots event.
            UFunction* offlineBotsClick = menuWidget && menuWidget->Class
                ? FindFunctionInHierarchyByName(
                    menuWidget->Class,
                    "BndEvt__OfflineBotsButton_K2Node_ComponentBoundEvent_49_OnClicked__DelegateSignature")
                : nullptr;
            if (!offlineBotsClick && menuWidget && menuWidget->Class)
            {
                offlineBotsClick = FindFunctionObjectExact(
                    menuWidget->Class,
                    "BndEvt__OfflineBotsButton_K2Node_ComponentBoundEvent_49_OnClicked__DelegateSignature");
            }

            if (!offlineBotsClick)
            {
                g_CounselorEntryClickPending.store(false);
                Logger::Error(
                    "Stock packed counselor route: native Offline Bots click event was unavailable");
                return;
            }

            g_CounselorEntryClickPending.store(true);
            g_CounselorEntryClickedAt.store(GetTickCount64());
            if (!SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(menuWidget),
                    menuWidget,
                    offlineBotsClick,
                    nullptr))
            {
                g_CounselorEntryClickPending.store(false);
                Logger::Error(
                    "Stock packed counselor route: forwarding Sandbox into Offline Bots failed");
                return;
            }

            // The packed stock executable does not submit RequestOfflineMode
            // again after this forwarded click.  Waiting for that callback
            // leaves the route unlatched and lets the stock human-Jason
            // PreMatchIntro run with an empty killer profile.  Commit the
            // counselor lifecycle now: the native Offline Bots picker still
            // owns map/Jason/setup selection, while this one row remains the
            // sole distinction between the Jason and Counselor routes.
            g_CounselorEntryClickPending.store(false);
            g_CounselorMenuRouteLatched.store(true);
            ArmObjectivesWidgetGameStatePatch();
            Logger::Success(
                "STOCK PACKED COUNSELOR CLICK: Sandbox surface forwarded into the native Offline Bots picker; counselor lifecycle latched before setup travel");
            return;
        }

        if (g_CounselorMenuRouteEnabled.load() &&
            g_UsingStockBaseCounselorRow.load())
        {
            // The base game's dormant OfflineBotsButtonC event already opens
            // its native counselor route. Preserve that bytecode and merely
            // latch our enhanced lifecycle before travel. No Resurrected
            // counselor-picker cast patch is needed on this profile.
            g_CounselorEntryClickPending.store(true);
            g_CounselorEntryClickedAt.store(GetTickCount64());
            g_CounselorMenuRouteLatched.store(true);
            ArmObjectivesWidgetGameStatePatch();

            Logger::Success(
                "STOCK COUNSELOR ROUTE LATCHED: native OfflineBotsButtonC click retained");
            if (g_OriginalCounselorEntryExec)
                g_OriginalCounselorEntryExec(context, stack, result);
            g_CounselorEntryClickPending.store(false);
            return;
        }

        if (g_CounselorMenuRouteEnabled.load())
        {
            g_CounselorEntryClickPending.store(true);
            g_CounselorEntryClickedAt.store(GetTickCount64());

            Logger::Success(
                "18L-AK dedicated Offline Bots - Counselor row clicked; opening Resurrected's native counselor picker");

            if (OpenIntegratedCounselorPicker(
                    reinterpret_cast<UObject*>(context)))
            {
                return;
            }

            Logger::Error(
                "18L-AL counselor picker unavailable; blocking the dedicated Counselor row instead of silently starting the stock Jason route");
            g_CounselorEntryClickPending.store(false);
            return;
        }

        if (g_OriginalCounselorEntryExec)
            g_OriginalCounselorEntryExec(context, stack, result);

        if (!g_CounselorMenuRouteEnabled.load())
            return;

        UObject* menu = reinterpret_cast<UObject*>(
            g_CounselorMenuObject.load());
        UClass* offlineBotsMode = reinterpret_cast<UClass*>(
            g_CounselorOfflineBotsModeClass.load());

        constexpr uintptr_t PendingOfflineModeOffset = 0x4D0;
        UClass** pendingMode = menu
            ? reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(menu) +
                PendingOfflineModeOffset)
            : nullptr;

        if (!menu ||
            !Memory::IsReadable(menu, sizeof(UObject)) ||
            !offlineBotsMode ||
            !Memory::IsReadable(offlineBotsMode, sizeof(UClass)) ||
            !pendingMode ||
            !Memory::IsReadable(pendingMode, sizeof(UClass*)))
        {
            Logger::Error(
                "18L-AD dedicated Counselor click: live menu/mode state became unavailable after the native row event");
            return;
        }

        // The native row bytecode has already opened Resurrected's compatible
        // OfflineBots picker. Commit the distinct route before any subsequent
        // picker/settings Blueprint can build the travel URL.
        *pendingMode = offlineBotsMode;

        g_CounselorEntryClickPending.store(false);
        g_CounselorMenuRouteLatched.store(true);
        ArmObjectivesWidgetGameStatePatch();

        Logger::Success(
            "18L-AD SYNCHRONOUS COUNSEL ROUTE LATCH: dedicated row retained stock OFLB travel and armed the counselor lifecycle override");
    }

    static bool InstallCounselorEntryClickHook(
        UObject* menu,
        bool logUnavailable = true)
    {
        if (!menu || !menu->Class)
            return false;

        UClass* offlinePlayMenuClass =
            SafeName(reinterpret_cast<UObject*>(menu->Class)) == "OfflinePlayMenuWidget_C"
                ? menu->Class
                : GetCachedClass(g_OfflinePlayMenuClass, "OfflinePlayMenuWidget_C");

        if (!offlinePlayMenuClass)
        {
            if (logUnavailable)
            {
                Logger::Debug(
                    "18L-AC: OfflinePlayMenuWidget_C is not loaded yet; Counselor row hook remains pending");
            }
            return false;
        }

        InstallChallengeEntryHook(offlinePlayMenuClass);

        auto* installedEntry = reinterpret_cast<UFunction*>(
            g_CounselorEntryHookTarget.load());
        if (installedEntry &&
            Memory::IsReadable(installedEntry, sizeof(UFunction)) &&
            reinterpret_cast<UObject*>(installedEntry)->OuterPrivate == reinterpret_cast<UObject*>(offlinePlayMenuClass) &&
            installedEntry->ExecFunction == &CounselorEntryExecHook)
        {
            g_CounselorEntryHookPending.store(false);
            return true;
        }

        const bool packedSandboxLayout = g_UsingStockBaseSandboxRow.load();
        UFunction* entryFunction = packedSandboxLayout ? nullptr :
            FindFunctionInHierarchyByName(offlinePlayMenuClass,
                "BndEvt__VC3Button_K2Node_ComponentBoundEvent_86_OnClicked__DelegateSignature");
        if (!entryFunction && !packedSandboxLayout)
        {
            entryFunction = FindFunctionObjectExact(
                offlinePlayMenuClass,
                "BndEvt__VC3Button_K2Node_ComponentBoundEvent_86_OnClicked__DelegateSignature");
        }
        bool stockBaseRow = false;
        bool stockBaseSandboxRow = false;
        if (!entryFunction && !packedSandboxLayout)
        {
            entryFunction = FindFunctionInHierarchyByName(
                offlinePlayMenuClass,
                "BndEvt__OfflineBotsButtonC_K2Node_ComponentBoundEvent_148_OnClicked__DelegateSignature");
            if (!entryFunction)
            {
                entryFunction = FindFunctionObjectExact(
                    offlinePlayMenuClass,
                    "BndEvt__OfflineBotsButtonC_K2Node_ComponentBoundEvent_148_OnClicked__DelegateSignature");
            }
            stockBaseRow = entryFunction != nullptr;
        }
        if (!entryFunction)
        {
            entryFunction = FindFunctionInHierarchyByName(
                offlinePlayMenuClass,
                "BndEvt__HostSandboxMatchButton_K2Node_ComponentBoundEvent_34_OnClicked__DelegateSignature");
            if (!entryFunction)
            {
                entryFunction = FindFunctionObjectExact(
                    offlinePlayMenuClass,
                    "BndEvt__HostSandboxMatchButton_K2Node_ComponentBoundEvent_34_OnClicked__DelegateSignature");
            }
            stockBaseSandboxRow = entryFunction != nullptr;
        }

        if (!entryFunction ||
            !Memory::IsReadable(entryFunction, sizeof(UFunction)) ||
            !entryFunction->ExecFunction)
        {
            if (logUnavailable)
            {
                Logger::Error(
                    "18L-AC: VC3, donor counselor, and packed Sandbox row events were unavailable; route hook NOT installed");
            }
            return false;
        }

        g_UsingStockBaseCounselorRow.store(stockBaseRow);
        g_UsingStockBaseSandboxRow.store(stockBaseSandboxRow);
        g_CounselorEntryHookTarget.store(reinterpret_cast<uintptr_t>(entryFunction));

        // Blueprint functions can be relinked while the frontend completes
        // its async load.  The installed flag alone is not proof that the
        // currently active event still points at this hook.
        if (entryFunction->ExecFunction == &CounselorEntryExecHook)
        {
            g_CounselorEntryHookInstalled.store(true);
            g_CounselorEntryHookPending.store(false);
            return true;
        }

        const bool repairingRelinkedEvent =
            g_CounselorEntryHookInstalled.exchange(false);

        g_OriginalCounselorEntryExec = entryFunction->ExecFunction;

        DWORD oldProtect = 0;
        void* targetSlot = &entryFunction->ExecFunction;
        if (!VirtualProtect(
                targetSlot,
                sizeof(entryFunction->ExecFunction),
                PAGE_READWRITE,
                &oldProtect))
        {
            Logger::Error(
                "18L-AC: dedicated Counselor row ExecFunction slot could not be made writable");
            g_OriginalCounselorEntryExec = nullptr;
            return false;
        }

        entryFunction->ExecFunction = &CounselorEntryExecHook;

        DWORD ignoredProtect = 0;
        VirtualProtect(
            targetSlot,
            sizeof(entryFunction->ExecFunction),
            oldProtect,
            &ignoredProtect);

        g_CounselorEntryHookInstalled.store(true);
        g_CounselorEntryHookPending.store(false);

        Logger::Success(
            stockBaseSandboxRow
                ? (repairingRelinkedEvent
                    ? "Stock packed Sandbox counselor hook repaired before input"
                    : "Stock packed Sandbox row hooked as Offline Bots - Counselor")
                : stockBaseRow
                ? (repairingRelinkedEvent
                    ? "Stock base counselor row hook repaired before input"
                    : "Stock base OfflineBotsButtonC click hook installed")
                : (repairingRelinkedEvent
                    ? "18L-AX: relinked Offline Bots - Counselor row hook repaired before input"
                    : "18L-AC: dedicated Offline Bots - Counselor row hook installed; Jason and Sandbox remain distinct"));
        return true;
    }

    static void CounselRequestOfflineModeExecHook(
        void* context,
        void* stack,
        void* result)
    {
        // Let the game's real RequestOfflineMode execute first.  The old
        // O/P/R builds proved that the stock COUNSEL/Sandbox click overwrites
        // PendingOfflineMode inside its own synchronous path.  We therefore
        // inspect the value immediately on RETURN from the native exec wrapper,
        // while the same Blueprint call is still on the stack.
        if (g_OriginalRequestOfflineModeExec)
            g_OriginalRequestOfflineModeExec(context, stack, result);

        if (!g_CounselorMenuRouteEnabled.load())
            return;

        UObject* menu =
            (UObject*)g_CounselorMenuObject.load();

        // RequestOfflineMode is a static ILLBackendBlueprintLibrary call, so
        // Context is the library's default object rather than SCGame_Menu.
        // The frontend menu captured while arming remains the authoritative
        // owner of PendingOfflineMode.
        if (!menu ||
            !Memory::IsReadable(menu, sizeof(UObject)))
        {
            return;
        }

        UClass* offlineBotsMode =
            (UClass*)g_CounselorOfflineBotsModeClass.load();

        UClass* sandboxMode =
            (UClass*)g_CounselorSandboxModeClass.load();

        UClass* counselorReferenceMode =
            (UClass*)g_CounselorReferenceModeClass.load();

        if (!offlineBotsMode ||
            (!counselorReferenceMode && !sandboxMode))
            return;

        constexpr uintptr_t PendingOfflineModeOffset = 0x4D0;

        UClass** pendingMode =
            (UClass**)((uintptr_t)menu +
                PendingOfflineModeOffset);

        if (!Memory::IsReadable(pendingMode, sizeof(UClass*)))
        {
            Logger::Error(
                "18L-AC synchronous route: PendingOfflineMode unreadable on RequestOfflineMode return");
            return;
        }

        UClass* currentMode = *pendingMode;
        std::string currentName =
            currentMode
            ? SafeName((UObject*)currentMode)
            : std::string("<null>");

        uint32_t hit =
            g_RequestOfflineModeHookHits.fetch_add(1) + 1;

        if (hit <= 8)
        {
            Logger::Debug(
                "18L-AC RequestOfflineMode RETURN: PendingOfflineMode=" +
                currentName);
        }

        const bool dedicatedCounselorClick =
            g_CounselorEntryClickPending.load();

        // The native Counselor row intentionally opens the stock Offline Bots
        // picker so map, Jason and game-setup selection all remain available.
        // Its row hook is the only distinction from the Jason entry.
        if ((currentMode == offlineBotsMode ||
                currentName == "ModeDef_OfflineBots_C") &&
            dedicatedCounselorClick)
        {
            g_CounselorEntryClickPending.store(false);
            g_CounselorMenuRouteLatched.store(true);
            ArmObjectivesWidgetGameStatePatch();

            Logger::Success(
                "18L-AC SYNCHRONOUS COUNSEL ROUTE LATCH: dedicated Counselor row submitted ModeDef_OfflineBots_C through the native picker; Jason row remains stock");
        }
        // Keep compatibility with the reference package's explicit counselor
        // mode. Never treat Sandbox as Counselor in the final five-entry menu.
        else if ((counselorReferenceMode && currentMode == counselorReferenceMode) ||
            currentName == "ModeDef_OfflineBotsC_C")
        {
            *pendingMode = offlineBotsMode;
            g_CounselorEntryClickPending.store(false);
            g_CounselorMenuRouteLatched.store(true);
            ArmObjectivesWidgetGameStatePatch();

            Logger::Success(
                "18L-AC SYNCHRONOUS COUNSEL ROUTE LATCH: RequestOfflineMode selected " +
                currentName +
                " -> replaced with ModeDef_OfflineBots_C before Blueprint resumed");
        }
        else if (currentMode == offlineBotsMode ||
            currentName == "ModeDef_OfflineBots_C")
        {
            g_CounselorEntryClickPending.store(false);
            Logger::Debug(
                "18L-AC RequestOfflineMode: direct OfflineBots request observed; leaving PLAY JASON untouched");
        }
        else if (currentMode == sandboxMode ||
            currentName == "ModeDef_Sandbox_C")
        {
            g_CounselorEntryClickPending.store(false);
            Logger::Debug(
                "18L-AC RequestOfflineMode: real Sandbox request observed; leaving Sandbox untouched");
        }
    }

    static bool InstallSynchronousCounselRouteHook(
        UObject* menu)
    {
        if (g_RequestOfflineModeHookInstalled.load())
            return true;

        if (!menu || !menu->Class)
            return false;

        UClass* backendLibrary =
            FindClassExact("ILLBackendBlueprintLibrary");

        if (!backendLibrary)
        {
            Logger::Error(
                "18L-AC: ILLBackendBlueprintLibrary class was not loaded; synchronous route hook NOT installed");
            return false;
        }

        UFunction* requestOfflineMode =
            FindFunctionInHierarchyByName(
                backendLibrary,
                "RequestOfflineMode");

        if (!requestOfflineMode ||
            !Memory::IsReadable(requestOfflineMode, sizeof(UFunction)) ||
            !requestOfflineMode->ExecFunction)
        {
            Logger::Error(
                "18L-AC: ILLBackendBlueprintLibrary::RequestOfflineMode native exec wrapper was not available; synchronous route hook NOT installed");
            return false;
        }

        LPVOID target =
            reinterpret_cast<LPVOID>(
                requestOfflineMode->ExecFunction);

        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(target, &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD))
        {
            Logger::Error(
                "18L-AC: RequestOfflineMode ExecFunction address is not executable/readable");
            return false;
        }

        MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            Logger::Error(
                "18L-AC: MH_Initialize failed while installing synchronous COUNSEL route hook");
            return false;
        }

        MH_STATUS createStatus =
            MH_CreateHook(
                target,
                reinterpret_cast<LPVOID>(
                    &CounselRequestOfflineModeExecHook),
                reinterpret_cast<LPVOID*>(
                    &g_OriginalRequestOfflineModeExec));

        if (createStatus != MH_OK)
        {
            Logger::Error(
                "18L-AC: MH_CreateHook failed for RequestOfflineMode ExecFunction");
            return false;
        }

        if (MH_EnableHook(target) != MH_OK)
        {
            Logger::Error(
                "18L-AC: MH_EnableHook failed for RequestOfflineMode ExecFunction");
            return false;
        }

        g_RequestOfflineModeHookTarget.store(
            (uintptr_t)target);
        g_RequestOfflineModeHookInstalled.store(true);

        Logger::Success(
            "18L-AC: synchronous ILLBackendBlueprintLibrary::RequestOfflineMode hook installed; no worker-thread PendingOfflineMode rewrite is used");

        return true;
    }

    static bool ArmSelectedPreset()
    {
        const ULONGLONG armStartedAt = GetTickCount64();
        if (g_CounselorAliasArmed.load())
        {
            Logger::Debug(
                "18L-AO counselor route arm deferred: previous OFLBC alias still awaits game-thread restoration");
            return false;
        }
        UWorld* world = Engine::GetWorld();
        UObject* menu = GetFrontendMenuForWorld(world);

        if (!menu || !world)
        {
            Logger::Error(
                "Counselor menu 18L-AC: frontend SCGame_Menu/EntryGame_C not ready. Return to Offline Play and try ARM again.");
            return false;
        }

        UClass* offlineBotsMode =
            FindClassExact("ModeDef_OfflineBots_C");

        UClass* sandboxMode =
            FindClassExact("ModeDef_Sandbox_C");

        UClass* counselorReferenceMode =
            FindClassExact("ModeDef_OfflineBotsC_C");

        if (!offlineBotsMode ||
            (!counselorReferenceMode && !sandboxMode))
        {
            Logger::Error(
                "Counselor menu 18L-AC: ModeDef_OfflineBots_C is missing, or neither ModeDef_OfflineBotsC_C nor ModeDef_Sandbox_C is loaded.");
            return false;
        }

        g_CounselorMenuWorld.store((uintptr_t)world);
        g_CounselorMenuObject.store((uintptr_t)menu);
        g_CounselorOfflineBotsModeClass.store(
            (uintptr_t)offlineBotsMode);
        g_CounselorSandboxModeClass.store(
            (uintptr_t)sandboxMode);
        g_CounselorReferenceModeClass.store(
            (uintptr_t)counselorReferenceMode);
        g_CounselorMenuRewriteCount.store(0);
        g_RequestOfflineModeHookHits.store(0);
        g_CounselorMenuRouteLatched.store(false);
        g_CounselorBirthComplete.store(false);
        g_CounselorMatchInProgress.store(false);
        g_CounselorJasonActive.store(false);
        g_CounselorBotsCreated.store(0);
        g_CounselorBirthPawn.store(0);
        g_CounselorEntryClickPending.store(false);
        g_CounselorEntryClickedAt.store(0);
        g_CounselorEntryHookPending.store(false);
        g_CounselorEntryHookLastAttempt.store(0);
        g_CounselorPickerActive.store(false);
        g_CounselorPickerAccepted.store(false);
        g_CounselorPickerWidget.store(0);
        g_CounselorPickerSourceMenu.store(0);
        g_NativeSelectedCounselorTotal.store(0);
        g_NativeSelectedCounselorSkill.store(-1);
        g_NativeSelectedWeather.store(-1);
        g_GameSetupSelectionLocked.store(false);
        g_WeatherSettingsWidget.store(0);
        g_ActiveWeatherCombo.store(0);
        g_ActiveJasonDifficultyCombo.store(0);
        g_ActiveMatchLengthCombo.store(0);
        g_ActiveJasonUnlimitedKnivesCombo.store(0);
        g_ActiveJasonFastRechargeCombo.store(0);
        g_ActiveJasonFastMovementCombo.store(0);
        g_CommittedMatchSeconds.store(-1);
        g_CommittedJasonUnlimitedKnives.store(false);
        g_CommittedJasonFastRecharge.store(false);
        g_CommittedJasonFastMovement.store(false);
        g_ActiveStartingWeaponCombo.store(0);
        g_ActiveStartingInventoryCombo.store(0);
        g_CommittedStartingWeapon.store(0);
        g_CommittedStartingInventory.store(0);
        g_StartingLoadoutStage.store(0);
        g_StartingItemPending.store(false);
        g_CommittedJasonDifficulty.store(-1);
        g_CommittedOfflineRain.store(-1);
        g_OfflineWeatherApplyPending.store(false);
        g_CommittedSettingsWorld.store(0);
        g_ActiveSkillCombo.store(0);
        g_ActiveSkillValueText.store(0);
        g_LastSkillDisplayedIndex.store(-1);
        g_NextSkillDisplayPumpAt.store(0);
        g_PrimaryKillerPlayerState.store(0);
        g_RuntimeMenuLabeledWidget.store(0);
        g_RuntimeMenuLabelLastAttempt.store(0);
        g_RuntimeMenuLabelFailureLogged.store(false);
        g_RuntimeMenuOrderedWidget.store(0);
        g_RuntimeMenuOrderFailureLogged.store(false);
        g_RuntimeMenuOrderRequested.store(false);
        g_RuntimeMenuOrderWidget.store(0);
        g_RuntimeMenuOrderFailureCode.store(0);
        g_UsingStockBaseCounselorRow.store(false);
        g_UsingStockBaseSandboxRow.store(false);

        if (!InstallSynchronousCounselRouteHook(menu))
        {
            g_CounselorMenuRouteEnabled.store(false);
            g_CounselorModeAutoStartArmed.store(false);
            return false;
        }

        const bool runtimeMenuVisibilityReady =
            InstallVC3VisibilityHook();
        const bool counselorEntryHookReady =
            InstallCounselorEntryClickHook(menu);
        g_CounselorEntryHookPending.store(!counselorEntryHookReady);

        UClass* counselorMenuClass =
            FindClassExact("Counselor_Menu_C");
        const bool integratedPickerReady =
            counselorMenuClass &&
            InstallCounselorPickerHooks(counselorMenuClass) &&
            PatchCounselorPickerConstructPlayerStateCast(
                counselorMenuClass,
                false);
        if (integratedPickerReady)
        {
            Logger::Success(
                "18L-AM counselor picker runtime route ready: offline Construct cast + native Accept continuation are installed");
        }
        else
        {
            Logger::Error(
                "18L-AK counselor picker hooks pending: Counselor_Menu_C is not loaded yet; the row will retry when clicked");
        }

        constexpr uintptr_t PendingOfflineModeOffset = 0x4D0;
        UClass** pendingMode =
            (UClass**)((uintptr_t)menu +
                PendingOfflineModeOffset);

        std::string initialMode = "<unreadable>";
        if (Memory::IsReadable(pendingMode, sizeof(UClass*)))
        {
            initialMode =
                *pendingMode
                ? SafeName((UObject*)*pendingMode)
                : std::string("<null>");
        }

        // IMPORTANT: unlike O/P/R/Q, AB does NOT pre-write PendingOfflineMode.
        // It waits for the actual COUNSEL/Sandbox RequestOfflineMode call and
        // changes the result synchronously on that same call stack.
        g_CounselorMenuRouteEnabled.store(true);
        g_CounselorModeAutoStartArmed.store(true);

        int32_t counselors =
            ClampCounselorCount(
                g_SelectedCounselorCount.load());

        int32_t counselorIndex =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        Logger::Success(
            std::string("Counselor menu 18L-AC ARMED: dedicated Counselor row is waiting to submit ModeDef_OfflineBots_C through the native map/settings picker") +
            (runtimeMenuVisibilityReady
                ? " | PackedMenu=visible"
                : " | PackedMenu=pending") +
            (counselorEntryHookReady
                ? " | RowHook=ready"
                : " | RowHook=pending until Offline Play opens") +
            (integratedPickerReady
                ? " | CounselorPicker=ready"
                : " | CounselorPicker=pending") +
            " | InitialPendingMode=" + initialMode +
            " | Player=" +
            kPlayerCounselorClassNames[counselorIndex] +
            " | CounselorBots=" +
            std::to_string(counselors) +
            " | ObjectStride=24 | ArmMs=" +
            std::to_string(GetTickCount64() - armStartedAt));

        Logger::Debug(
            "Counselor menu 18L-AC: now click Offline Bots - Counselor. Offline Bots - Jason and Sandbox are not redirected.");

        return true;
    }

    static void TickMapSticky()
    {
        if (!g_CounselorMenuRouteEnabled.load())
            return;

        UWorld* world = Engine::GetWorld();
        uintptr_t originalWorld =
            g_CounselorMenuWorld.load();
        static uintptr_t transientRouteWorld = 0;
        static ULONGLONG transientWorldSince = 0;
        if (transientRouteWorld != originalWorld)
        {
            transientRouteWorld = originalWorld;
            transientWorldSince = 0;
        }
        // A newly constructed frontend/preview world is not gameplay travel.
        // Rebind only while native authority is positively a frontend menu;
        // never relax the post-click gameplay/world teardown guard.
        UObject* refreshedFrontend = world && originalWorld &&
            reinterpret_cast<uintptr_t>(world) != originalWorld &&
            !g_CounselorMenuRouteLatched.load()
                ? GetFrontendMenuForWorld(world) : nullptr;
        if (refreshedFrontend)
        {
            g_CounselorMenuWorld.store(reinterpret_cast<uintptr_t>(world));
            g_CounselorMenuObject.store(reinterpret_cast<uintptr_t>(refreshedFrontend));
            originalWorld = reinterpret_cast<uintptr_t>(world);
            Logger::Debug("Counselor route: retained picker across confirmed frontend world refresh");
        }

        if (!world ||
            !originalWorld ||
            (uintptr_t)world != originalWorld)
        {
            // GWorld briefly visits empty character-preview/travel worlds.
            // Do not erase the picker transaction and re-arm (which clears its
            // selection/submenus) on one missing authority snapshot. Preserve
            // only an uncommitted, still-live original frontend, for <=3 sec.
            // A positively identified gameplay authority or committed travel
            // continues through the original teardown immediately.
            UObject* currentAuthority = world && Memory::IsReadable(world, 0xF8)
                ? *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF0)
                : nullptr;
            if (!g_CounselorMenuRouteLatched.load() && originalWorld &&
                !currentAuthority &&
                GetFrontendMenuForWorld(reinterpret_cast<UWorld*>(originalWorld)) ==
                    reinterpret_cast<UObject*>(g_CounselorMenuObject.load()))
            {
                const ULONGLONG now = GetTickCount64();
                if (!transientWorldSince) transientWorldSince = now;
                if (now - transientWorldSince < 3000) return;
            }
            transientWorldSince = 0;
            g_CounselorMenuRouteEnabled.store(false);
            g_CounselorEntryClickPending.store(false);
            if (g_CounselorAliasArmed.load())
                g_CounselorAliasRestorePending.store(true);

            if (g_CounselorMenuRouteLatched.load())
            {
                Logger::Success(
                    "Counselor menu 18L-AO: frontend world changed AFTER synchronous latch; stock OFLB host will enter the counselor lifecycle hooks");
            }
            else
            {
                Logger::Error(
                    "Counselor menu 18L-AC: frontend world changed before synchronous RequestOfflineMode latch fired");
            }

            return;
        }
        transientWorldSince = 0;

        // UE can relink this Blueprint event after the initial frontend arm.
        // Re-resolve and validate the exact slot at a bounded menu-only
        // cadence.  This work ends as soon as the frontend world changes.
        const ULONGLONG hookNow = GetTickCount64();
        if (hookNow - g_CounselorEntryHookLastAttempt.load() >= 1000)
        {
            g_CounselorEntryHookLastAttempt.store(hookNow);
            InstallVC3VisibilityHook(false);
            UObject* menu = reinterpret_cast<UObject*>(
                g_CounselorMenuObject.load());
            if (!InstallCounselorEntryClickHook(menu, false))
            {
                g_CounselorEntryHookPending.store(true);
            }
        }

        // Route-only monitor.  Deliberately no asynchronous PendingOfflineMode
        // writes here.  O/P/R already proved that worker-thread sticky writes
        // lose to the stock click path.
    }

    static UObject* RawFindExactClassObject(
        UClass* targetClass,
        const char* exactObjectName)
    {
        if (!targetClass || !exactObjectName)
            return nullptr;

        // Live UObject identity belongs to GUObjectArray. Avoid copying and
        // byte-scanning every committed memory region to locate one widget.
        NativeObjectItem* objects = nullptr;
        int32_t count = 0;
        if (!GetGObjects(&objects, &count))
            return nullptr;

        for (int32_t i = 0; i < count; ++i)
        {
            UObject* candidate = objects[i].Object;
            if (!candidate || !SafeNameEquals(candidate, exactObjectName))
                continue;
            if (Memory::IsReadable(candidate, sizeof(UObject)) &&
                candidate->Class == targetClass)
                return candidate;
        }

        return nullptr;
    }

    static std::string ReadFStringAscii(
        UObject* object,
        uintptr_t offset)
    {
        if (!object)
            return {};

        RawFString* value =
            (RawFString*)((uintptr_t)object + offset);

        if (!Memory::IsReadable(value, sizeof(RawFString)) ||
            !value->Data ||
            value->Count <= 0 ||
            value->Count > 256 ||
            value->Max < value->Count ||
            !Memory::IsReadable(
                value->Data,
                sizeof(wchar_t) * (size_t)value->Count))
        {
            return {};
        }

        std::string out;

        for (int32_t i = 0;
            i < value->Count;
            ++i)
        {
            wchar_t ch = value->Data[i];

            if (!ch)
                break;

            if (ch >= 0 &&
                ch <= 0x7F)
            {
                out.push_back((char)ch);
            }
            else
            {
                out.push_back('?');
            }
        }

        return out;
    }

    static bool WriteFStringAsciiInPlace(
        UObject* object,
        uintptr_t offset,
        const wchar_t* text)
    {
        if (!object || !text)
            return false;

        RawFString* value =
            (RawFString*)((uintptr_t)object + offset);

        if (!Memory::IsReadable(value, sizeof(RawFString)) ||
            !value->Data ||
            value->Max <= 0 ||
            value->Max > 4096)
        {
            return false;
        }

        size_t chars =
            std::wcslen(text) + 1;

        if (chars > (size_t)value->Max ||
            !Memory::IsReadable(
                value->Data,
                sizeof(wchar_t) * chars))
        {
            return false;
        }

        std::wmemcpy(
            value->Data,
            text,
            chars);

        value->Count =
            (int32_t)chars;

        return true;
    }

    static bool ValidateSettingsCombo(
        UObject* object,
        const char* exactName)
    {
        if (!object ||
            !Memory::IsReadable(object, sizeof(UObject)))
        {
            return false;
        }

        return SafeName(object) == exactName;
    }

    static bool ApplyGameSetupPreset()
    {
        UObject* menu = GetActiveFrontendMenu();

        if (!menu)
        {
            Logger::Error(
                "Unified setup 18B: frontend menu not ready. Use F5 while the stock Game Setup screen is visible.");
            return false;
        }

        constexpr uintptr_t PendingOfflineSettingsWidgetOffset =
            0x4D8;

        UClass** settingsClassAddress =
            (UClass**)((uintptr_t)menu +
                PendingOfflineSettingsWidgetOffset);

        if (!Memory::IsReadable(
            settingsClassAddress,
            sizeof(UClass*)) ||
            !*settingsClassAddress)
        {
            Logger::Error(
                "Unified setup 18B: PendingOfflineSettingsWidget class is null/unreadable");
            return false;
        }

        UClass* settingsClass =
            *settingsClassAddress;

        Logger::Debug(
            "Unified setup 18B: searching for live OfflineBotsSettingsMenuWidget_C");

        UObject* settings =
            RawFindExactClassObject(
                settingsClass,
                "OfflineBotsSettingsMenuWidget_C");

        if (!settings)
        {
            Logger::Error(
                "Unified setup 18B: live OfflineBotsSettingsMenuWidget_C not found. Leave Game Setup visible and press F5 again.");
            return false;
        }

        constexpr uintptr_t CounselorComboOffset = 0x400;
        constexpr uintptr_t DifficultyComboOffset = 0x408;
        constexpr uintptr_t WeatherComboOffset = 0x440;
        constexpr uintptr_t WeatherStringOffset = 0x448;
        constexpr uintptr_t TravelPathOffset = 0x458;
        constexpr uintptr_t CurrentOptionIndexOffset = 0x458;

        UObject** counselorComboAddress =
            (UObject**)((uintptr_t)settings +
                CounselorComboOffset);

        UObject** difficultyComboAddress =
            (UObject**)((uintptr_t)settings +
                DifficultyComboOffset);

        UObject** weatherComboAddress =
            (UObject**)((uintptr_t)settings +
                WeatherComboOffset);

        if (!Memory::IsReadable(
                counselorComboAddress,
                sizeof(UObject*)) ||
            !Memory::IsReadable(
                difficultyComboAddress,
                sizeof(UObject*)) ||
            !Memory::IsReadable(
                weatherComboAddress,
                sizeof(UObject*)))
        {
            Logger::Error(
                "Unified setup 18B: Game Setup combo pointers unreadable");
            return false;
        }

        UObject* counselorCombo =
            *counselorComboAddress;

        UObject* difficultyCombo =
            *difficultyComboAddress;

        UObject* weatherCombo =
            *weatherComboAddress;

        if (!ValidateSettingsCombo(
                counselorCombo,
                "CounselorCountComboBox") ||
            !ValidateSettingsCombo(
                difficultyCombo,
                "DifficultyComboBox") ||
            !ValidateSettingsCombo(
                weatherCombo,
                "WeatherComboBox"))
        {
            Logger::Error(
                "Unified setup 18B: live Game Setup combo validation failed");
            return false;
        }

        int32_t* counselorIndex =
            (int32_t*)((uintptr_t)counselorCombo +
                CurrentOptionIndexOffset);

        int32_t* difficultyIndex =
            (int32_t*)((uintptr_t)difficultyCombo +
                CurrentOptionIndexOffset);

        int32_t* weatherIndex =
            (int32_t*)((uintptr_t)weatherCombo +
                CurrentOptionIndexOffset);

        if (!Memory::IsReadable(
                counselorIndex,
                sizeof(int32_t)) ||
            !Memory::IsReadable(
                difficultyIndex,
                sizeof(int32_t)) ||
            !Memory::IsReadable(
                weatherIndex,
                sizeof(int32_t)))
        {
            Logger::Error(
                "Unified setup 18B: Game Setup CurrentOptionIndex fields unreadable");
            return false;
        }

        int32_t beforeCounselors =
            *counselorIndex;

        int32_t beforeDifficulty =
            *difficultyIndex;

        int32_t beforeWeather =
            *weatherIndex;

        std::string beforeWeatherString =
            ReadFStringAscii(
                settings,
                WeatherStringOffset);

        std::string travelPath =
            ReadFStringAscii(
                settings,
                TravelPathOffset);

        int32_t selectedCounselors =
            ClampCounselorCount(g_SelectedCounselorCount.load());

        int32_t selectedDifficulty =
            ClampDifficulty(g_SelectedDifficulty.load());

        int32_t selectedWeather =
            ClampWeather(g_SelectedWeather.load());

        *counselorIndex =
            selectedCounselors - 1;

        *difficultyIndex =
            selectedDifficulty;

        bool weatherWrite =
            WriteFStringAsciiInPlace(
                settings,
                WeatherStringOffset,
                kWeatherStrings[selectedWeather]);

        Logger::Debug(
            "Unified setup 18B BEFORE: counselorIndex=" +
            std::to_string(beforeCounselors) +
            " difficultyIndex=" +
            std::to_string(beforeDifficulty) +
            " weatherIndex=" +
            std::to_string(beforeWeather) +
            " weather=\"" +
            beforeWeatherString +
            "\" travelPath=\"" +
            travelPath +
            "\"");

        std::string expectedWeather;
        const wchar_t* weatherText =
            kWeatherStrings[selectedWeather];

        for (size_t i = 0; weatherText[i]; ++i)
            expectedWeather.push_back((char)weatherText[i]);

        Logger::Success(
            "Unified setup 18B APPLY: Counselors=" +
            std::to_string(selectedCounselors) +
            " index=" +
            std::to_string(*counselorIndex) +
            " | Difficulty=" +
            kDifficultyNames[selectedDifficulty] +
            " index=" +
            std::to_string(*difficultyIndex) +
            " | Weather=" +
            kWeatherNames[selectedWeather] +
            " string=\"" +
            ReadFStringAscii(settings, WeatherStringOffset) +
            "\" write=" +
            (weatherWrite ? std::string("true") :
                std::string("false")));

        return
            *counselorIndex == selectedCounselors - 1 &&
            *difficultyIndex == selectedDifficulty &&
            weatherWrite &&
            ReadFStringAscii(
                settings,
                WeatherStringOffset) == expectedWeather;
    }

    static int32_t ClampPlayerCounselorIndex(int32_t value)
    {
        constexpr int32_t count =
            (int32_t)(sizeof(kPlayerCounselorClassNames) /
                sizeof(kPlayerCounselorClassNames[0]));

        if (value < 0)
            return 0;

        if (value >= count)
            return count - 1;

        return value;
    }

#if defined(F13_BASE_GAME_PORT)
    // The packed stock Jason picker owns this list. Its offline Accept event
    // does not write SCCharacterSelectionsSaveGame.KillerPick on this route.
    // Keep the order in the unmodified Jason_Select_Widget default asset.
    static constexpr const char* kStockPickerJasonClasses[] = {
        "Jason_SackHead_C", "Jason_BigNeck_C", "Jason_J4_C",
        "Jason_J5_C", "Jason_Utility_C", "Jason_Zombie_C",
        "Jason_Manhattan_C", "Jason_SwollenHead_C", "Jason_Uber_C",
        "Jason_TS1_C", "Jason_AJ_C"
    };

    static UClass* LoadLifecycleSoftClass(const uint8_t* softClass);

    // Accept must use the live picker array: stock can remove unavailable
    // rows, so its CurrentIndex is not an index into our default-asset table.
    // Read only validated reflected metadata; no hard-coded struct stride or
    // per-frame class loading. This runs on the native menu Accept thread.
    static UClass* ResolveLiveStockPickerClass(UObject* widget, int32_t index)
    {
        UPropertyLite* arrayProperty = FindPropertyInHierarchyByName(
            widget->Class, "JasonClassList");
        if (!arrayProperty || !Memory::IsReadable(arrayProperty, sizeof(UPropertyLite)) ||
            !arrayProperty->ClassPrivate || SafeName(reinterpret_cast<UObject*>(arrayProperty->ClassPrivate)) != "ArrayProperty" ||
            arrayProperty->Offset_Internal <= 0 || arrayProperty->Offset_Internal >= 0x10000 ||
            arrayProperty->ElementSize != sizeof(RawArray))
            return nullptr;

        // The property extension owns Inner/Struct references. Discover only
        // an exact named/type-checked reference in this small metadata tail,
        // rather than assuming a UProperty extension offset from another SDK.
        const auto findReference = [](UPropertyLite* property,
                                      const char* objectName,
                                      const char* typeName) -> UObject*
        {
            for (size_t offset = sizeof(UPropertyLite); offset < 0xA0;
                 offset += sizeof(uintptr_t))
            {
                auto* address = reinterpret_cast<uint8_t*>(property) + offset;
                if (!Memory::IsReadable(address, sizeof(UObject*))) break;
                UObject* candidate = nullptr;
                std::memcpy(&candidate, address, sizeof(candidate));
                if (!candidate || !Memory::IsReadable(candidate, sizeof(UObject)) ||
                    !candidate->Class || !Memory::IsReadable(candidate->Class, sizeof(UObject)))
                    continue;
                if (SafeName(candidate) == objectName &&
                    ClassDerivesFrom(candidate->Class, typeName))
                    return candidate;
            }
            return nullptr;
        };
        auto* inner = reinterpret_cast<UPropertyLite*>(findReference(
            arrayProperty, "JasonClassList", "StructProperty"));
        if (!inner || !Memory::IsReadable(inner, sizeof(UPropertyLite)) ||
            inner->ElementSize < 40 || inner->ElementSize > 512)
            return nullptr;
        auto* rowStruct = reinterpret_cast<UStruct*>(findReference(
            inner, "FSCCharacterSelectData", "ScriptStruct"));
        if (!rowStruct || !Memory::IsReadable(rowStruct, sizeof(UStruct)) ||
            rowStruct->Size != inner->ElementSize)
            return nullptr;

        RawArray rows{};
        const auto* arrayAddress = reinterpret_cast<uint8_t*>(widget) +
            arrayProperty->Offset_Internal;
        if (!Memory::IsReadable(arrayAddress, sizeof(rows))) return nullptr;
        std::memcpy(&rows, arrayAddress, sizeof(rows));
        if (!rows.Data || rows.Count <= 0 || rows.Count > 64 ||
            rows.Max < rows.Count || rows.Max > 128 || index < 0 || index >= rows.Count)
            return nullptr;
        const uint8_t* row = rows.Data + static_cast<size_t>(index) * inner->ElementSize;
        if (!Memory::IsReadable(row, inner->ElementSize)) return nullptr;
        int32_t remainingFields = 64;
        for (UField* field = rowStruct->Children;
             field && remainingFields-- > 0 && Memory::IsReadable(field, sizeof(UField)); field = field->Next)
        {
            auto* property = reinterpret_cast<UPropertyLite*>(field);
            if (!Memory::IsReadable(property, sizeof(UPropertyLite)) || !property->ClassPrivate ||
                SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) != "SoftClassProperty" ||
                SafeName(reinterpret_cast<UObject*>(property)).find("CharacterClass_") != 0 ||
                property->ElementSize != 40 || property->Offset_Internal < 0 ||
                property->Offset_Internal > inner->ElementSize - 40)
                continue;
            UClass* chosen = LoadLifecycleSoftClass(row + property->Offset_Internal);
            if (chosen && Memory::IsReadable(chosen, sizeof(UClass)) &&
                ClassDerivesFrom(chosen, "SCKillerCharacter"))
                return chosen;
        }
        return nullptr;
    }

    static void CaptureStockJasonPickerChoice(
        void* context,
        const char* eventName)
    {
        if (!g_CounselorMenuRouteLatched.load() ||
            g_CounselorBirthComplete.load())
            return;

        UObject* widget = reinterpret_cast<UObject*>(context);
        if (!widget || !Memory::IsReadable(widget, sizeof(UObject)) ||
            !widget->Class ||
            SafeName(reinterpret_cast<UObject*>(widget->Class)) !=
                "Jason_Select_Widget_C")
            return;

        UPropertyLite* indexProperty = FindPropertyInHierarchyByName(
            widget->Class, "CurrentIndex");
        if (!indexProperty || indexProperty->Offset_Internal <= 0 ||
            indexProperty->Offset_Internal >= 0x10000 ||
            indexProperty->ElementSize != sizeof(int32_t))
        {
            Logger::Error("STOCK JASON PICKER ACCEPT: CurrentIndex is unavailable");
            return;
        }

        int32_t index = -1;
        void* indexAddress = reinterpret_cast<uint8_t*>(widget) +
            indexProperty->Offset_Internal;
        if (!Memory::IsReadable(indexAddress, sizeof(index)))
            return;
        std::memcpy(&index, indexAddress, sizeof(index));
        constexpr int32_t count = static_cast<int32_t>(
            sizeof(kStockPickerJasonClasses) /
            sizeof(kStockPickerJasonClasses[0]));
        if (index < 0 || index >= count)
        {
            Logger::Error(
                std::string("STOCK JASON PICKER ACCEPT: invalid CurrentIndex=") +
                std::to_string(index));
            return;
        }

        g_SelectedJasonIndex.store(index);
        g_StockKillerRequestCaptured.store(true);
        UClass* selectedClass = ResolveLiveStockPickerClass(widget, index);
        const bool liveSelection = selectedClass != nullptr;
        if (!selectedClass)
        {
            UClass* profile = reinterpret_cast<UClass*>(g_TargetJasonClass.load());
            if (profile && Memory::IsReadable(profile, sizeof(UClass)) &&
                ClassDerivesFrom(profile, "SCKillerCharacter"))
                selectedClass = profile;
            else
                selectedClass = FindClassExact(kStockPickerJasonClasses[index]);
        }
        if (selectedClass &&
            ClassDerivesFrom(selectedClass, "SCKillerCharacter"))
            g_TargetJasonClass.store(
                reinterpret_cast<uintptr_t>(selectedClass));

        if (g_StockJasonPickerLoggedIndex.exchange(index) != index)
        {
            Logger::Success(
                std::string("STOCK JASON PICKER ACCEPT: ") + eventName +
                " CurrentIndex=" + std::to_string(index) +
                " selected=" + (selectedClass ? SafeName(reinterpret_cast<UObject*>(selectedClass))
                    : std::string(kStockPickerJasonClasses[index])) +
                " source=" + (liveSelection ? "live-picker" : "validated-fallback") +
                " loaded=" + (selectedClass ? "true" : "false"));
        }
    }

    static void StockJasonAcceptExecHook(
        void* context,
        void* stack,
        void* result)
    {
        CaptureStockJasonPickerChoice(context, "OnClick_Accept");
        if (g_OriginalStockJasonAcceptExec)
            g_OriginalStockJasonAcceptExec(context, stack, result);
    }

    static void StockJasonOfflineAcceptExecHook(
        void* context,
        void* stack,
        void* result)
    {
        CaptureStockJasonPickerChoice(context, "OfflineBots_AcceptJason");
        if (g_OriginalStockJasonOfflineAcceptExec)
            g_OriginalStockJasonOfflineAcceptExec(context, stack, result);
    }

    static void InstallStockJasonPickerAcceptHooks()
    {
        UClass* pickerClass = GetCachedClass(
            g_StockJasonPickerClass, "Jason_Select_Widget_C");
        if (!pickerClass)
            return;

        struct AcceptHook {
            const char* name;
            UFunction::FNativeFuncPtr replacement;
            UFunction::FNativeFuncPtr* original;
        };
        const AcceptHook hooks[] = {
            { "OnClick_Accept", &StockJasonAcceptExecHook,
                &g_OriginalStockJasonAcceptExec },
            { "OfflineBots_AcceptJason", &StockJasonOfflineAcceptExecHook,
                &g_OriginalStockJasonOfflineAcceptExec }
        };
        for (const auto& hook : hooks)
        {
            UFunction* function = FindFunctionInHierarchyByName(
                pickerClass, hook.name);
            if (!function ||
                !Memory::IsReadable(function, sizeof(UFunction)) ||
                !function->ExecFunction ||
                function->ExecFunction == hook.replacement)
                continue;

            DWORD oldProtection = 0;
            if (!VirtualProtect(&function->ExecFunction,
                    sizeof(function->ExecFunction), PAGE_READWRITE,
                    &oldProtection))
                continue;
            *hook.original = function->ExecFunction;
            function->ExecFunction = hook.replacement;
            DWORD ignored = 0;
            VirtualProtect(&function->ExecFunction,
                sizeof(function->ExecFunction), oldProtection,
                &ignored);
            Logger::Success(
                std::string("STOCK JASON PICKER ACCEPT HOOK: ") + hook.name);
        }
    }
#endif

    static void TickNativeProfileSelection()
    {
        if (g_GameSetupSelectionLocked.load())
            return;

        // Until the dedicated Counselor row is committed, the game's own
        // The native profile remains authoritative throughout the frontend
        // setup flow. In particular, Jason_Select_Widget writes KillerPick
        // after the counselor route has already latched. Stopping here merely
        // because the route was latched left the earlier Jason (normally J5)
        // cached for both the intro and the independent AI spawn. Keep this
        // bounded read-only sync alive while EntryGame/SCGame_Menu is still
        // authoritative, then stop before gameplay world travel.
        if (g_CounselorMenuRouteLatched.load() &&
            g_CounselorBirthComplete.load())
            return;

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG next = g_NextProfileSelectionSyncAt.load();
        if (next && now < next)
            return;
        g_NextProfileSelectionSyncAt.store(now + 500);

#if defined(F13_BASE_GAME_PORT)
        if (g_CounselorMenuRouteLatched.load())
            InstallStockJasonPickerAcceptHooks();
#endif

        if (!ResolveSelectionSaveMetadata())
            return;

        UObject* saveObject = reinterpret_cast<UObject*>(
            g_SelectionSaveObject.load());
        if (!saveObject ||
            !Memory::IsReadable(saveObject, sizeof(UObject)))
        {
            saveObject = RawFindSelectionSaveObject(
                reinterpret_cast<UClass*>(g_SelectionSaveClass.load()));
            if (!saveObject)
                return;
            g_SelectionSaveObject.store(reinterpret_cast<uintptr_t>(saveObject));
        }

        const int32_t counselorOffset = g_CounselorPickOffset.load();
        const int32_t killerOffset = g_KillerPickOffset.load();
        if (counselorOffset > 0
#if defined(F13_BASE_GAME_PORT)
            && !g_StockCounselorPickerCaptured.load()
#endif
            )
        {
            UClass** counselorPick = reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(saveObject) + counselorOffset);
            if (Memory::IsReadable(counselorPick, sizeof(UClass*)) &&
                *counselorPick &&
                Memory::IsReadable(*counselorPick, sizeof(UClass)))
            {
                UClass* selectedClass = *counselorPick;
                const std::string selectedName = SafeName(
                    reinterpret_cast<UObject*>(selectedClass));

                constexpr int32_t counselorCount =
                    static_cast<int32_t>(sizeof(kPlayerCounselorClassNames) /
                        sizeof(kPlayerCounselorClassNames[0]));
                for (int32_t i = 0; i < counselorCount; ++i)
                {
                    if (selectedName != kPlayerCounselorClassNames[i])
                        continue;

                    g_SelectedPlayerCounselorIndex.store(i);
                    g_TargetPlayerCounselorClass.store(
                        reinterpret_cast<uintptr_t>(selectedClass));

                    const uintptr_t previous =
                        g_LastProfileCounselorClass.exchange(
                            reinterpret_cast<uintptr_t>(selectedClass));
                    if (previous != reinterpret_cast<uintptr_t>(selectedClass))
                    {
                        Logger::Success(
                            "IN-GAME COUNSELOR SELECTION: Customize profile -> " +
                            selectedName);
                        Logger::Success("SavedCounselor=" + selectedName);
                    }
                    break;
                }
            }
        }

        if (killerOffset > 0
#if defined(F13_BASE_GAME_PORT)
            && !g_StockKillerRequestCaptured.load()
#endif
            )
        {
            UClass** killerPick = reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(saveObject) + killerOffset);
            if (Memory::IsReadable(killerPick, sizeof(UClass*)) &&
                *killerPick &&
                Memory::IsReadable(*killerPick, sizeof(UClass)))
            {
                UClass* selectedClass = *killerPick;
                g_TargetJasonClass.store(
                    reinterpret_cast<uintptr_t>(selectedClass));

                const uintptr_t previous =
                    g_LastProfileKillerClass.exchange(
                        reinterpret_cast<uintptr_t>(selectedClass));
                if (previous != reinterpret_cast<uintptr_t>(selectedClass))
                {
                    Logger::Success(
                        "IN-GAME JASON SELECTION: native picker/profile -> " +
                        SafeName(reinterpret_cast<UObject*>(selectedClass)));
                }
            }
        }
    }

    static UObject* FindDefaultGameModeForCounselorArray(
        NativeObjectItem* objects,
        int32_t objectCount)
    {
        UObject* offlineBots = nullptr;
        UObject* sandbox = nullptr;
        UObject* base = nullptr;

        for (int32_t i = 0; i < objectCount; ++i)
        {
            UObject* obj = objects[i].Object;

            if (!obj ||
                !Memory::IsReadable(obj, sizeof(UObject)))
            {
                continue;
            }

            std::string name = SafeName(obj);

            if (!offlineBots &&
                name == "Default__SCGameMode_OfflineBots")
            {
                offlineBots = obj;
            }
            else if (!sandbox &&
                name == "Default__SCGameMode_Sandbox")
            {
                sandbox = obj;
            }
            else if (!base &&
                name == "Default__SCGameMode")
            {
                base = obj;
            }

            if (offlineBots)
                break;
        }

        return offlineBots ? offlineBots :
            (sandbox ? sandbox : base);
    }

    static bool RequestSelectedCounselorSoftClassOnGameThread()
    {
        NativeObjectItem* objects = nullptr;
        int32_t objectCount = 0;

        if (!GetGObjects(&objects, &objectCount))
        {
            Logger::Error(
                "Counselor selection 18K: GObjects unavailable");
            return false;
        }

        UObject* gameModeDefault =
            FindDefaultGameModeForCounselorArray(
                objects,
                objectCount);

        if (!gameModeDefault ||
            !gameModeDefault->Class)
        {
            Logger::Error(
                "Counselor selection 18K: Offline Bots GameMode CDO not ready");
            return false;
        }

        int32_t index =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        uint8_t* selectedSoftClass = nullptr;

        if (std::string(kPlayerCounselorClassNames[index]) ==
            "Hunter_Counselor_C")
        {
            // Tommy is deliberately not an entry in the normal counselor
            // roster. Use the GameMode's real HunterCharacterClass soft class
            // so the special hero can be selected without inventing an index.
            UPropertyLite* hunterProperty =
                FindPropertyInHierarchyByName(
                    gameModeDefault->Class,
                    "HunterCharacterClass");

            if (!hunterProperty ||
                hunterProperty->Offset_Internal <= 0 ||
                hunterProperty->Offset_Internal >= 0x10000 ||
                hunterProperty->ElementSize < SoftClassSize)
            {
                Logger::Error(
                    "Counselor selection 18K: HunterCharacterClass property unavailable for Tommy");
                return false;
            }

            selectedSoftClass =
                (uint8_t*)((uintptr_t)gameModeDefault +
                    hunterProperty->Offset_Internal);
        }
        else
        {
            UPropertyLite* counselorClassesProperty =
                FindPropertyInHierarchyByName(
                    gameModeDefault->Class,
                    "CounselorCharacterClasses");

            if (!counselorClassesProperty ||
                counselorClassesProperty->Offset_Internal <= 0 ||
                counselorClassesProperty->Offset_Internal >= 0x10000)
            {
                Logger::Error(
                    "Counselor selection 18K: CounselorCharacterClasses property unavailable");
                return false;
            }

            RawArray* counselorClasses =
                (RawArray*)((uintptr_t)gameModeDefault +
                    counselorClassesProperty->Offset_Internal);

            if (!Memory::IsReadable(
                    counselorClasses,
                    sizeof(RawArray)) ||
                !counselorClasses->Data ||
                counselorClasses->Count <= 0 ||
                counselorClasses->Count > 64 ||
                counselorClasses->Max < counselorClasses->Count ||
                counselorClasses->Max > 128 ||
                !Memory::IsReadable(
                    counselorClasses->Data,
                    (size_t)counselorClasses->Count * SoftClassSize))
            {
                Logger::Error(
                    "Counselor selection 18K: counselor soft-class array unavailable");
                return false;
            }

            if (index >= counselorClasses->Count)
            {
                Logger::Error(
                    "Counselor selection 18K: selected index outside native roster");
                return false;
            }

            selectedSoftClass =
                counselorClasses->Data +
                ((uintptr_t)index * SoftClassSize);
        }

        if (!Memory::IsReadable(
                selectedSoftClass,
                SoftClassSize))
        {
            Logger::Error(
                "Counselor selection 18K: selected native soft class unreadable");
            return false;
        }

        g_SelectedPlayerCounselorPath =
            ReadSoftClassAssetPath(selectedSoftClass);

        memcpy(
            g_SelectedPlayerCounselorSoftClass.data(),
            selectedSoftClass,
            SoftClassSize);

        if (g_SelectedPlayerCounselorPath.empty())
        {
            Logger::Error(
                "Counselor selection 18K: selected native soft-class path is empty");
            return false;
        }

        auto localController =
            Engine::GetLocalPlayerController();

        if (!localController ||
            !Memory::IsReadable(
                localController,
                sizeof(UObject)) ||
            !localController->Class)
        {
            Logger::Error(
                "Counselor selection 18K: local PlayerController unavailable");
            return false;
        }

        // Do not guess a PlayerState offset.  Resolve the inherited Controller
        // property from reflection, exactly as the 18G->18H handoff requires.
        UPropertyLite* controllerPlayerStateProperty =
            FindPropertyInHierarchyByName(
                localController->Class,
                "PlayerState");

        if (!controllerPlayerStateProperty ||
            controllerPlayerStateProperty->Offset_Internal <= 0 ||
            controllerPlayerStateProperty->Offset_Internal >= 0x10000 ||
            controllerPlayerStateProperty->ElementSize < (int32_t)sizeof(UObject*))
        {
            Logger::Error(
                "Counselor selection 18K: reflected Controller::PlayerState property unavailable");
            return false;
        }

        UObject** playerStateSlot =
            (UObject**)((uintptr_t)localController +
                controllerPlayerStateProperty->Offset_Internal);

        if (!Memory::IsReadable(
                playerStateSlot,
                sizeof(UObject*)))
        {
            Logger::Error(
                "Counselor selection 18K: reflected PlayerState slot unreadable");
            return false;
        }

        UObject* playerState =
            *playerStateSlot;

        if (!playerState ||
            !Memory::IsReadable(
                playerState,
                sizeof(UObject)) ||
            !playerState->Class)
        {
            Logger::Error(
                "Counselor selection 18K: local PlayerState unavailable");
            return false;
        }

        UClass* scPlayerStateClass =
            FindClassExact("SCPlayerState");

        if (!scPlayerStateClass ||
            !ClassIsOrDerivesFrom(
                playerState->Class,
                scPlayerStateClass))
        {
            Logger::Error(
                "Counselor selection 18K: local PlayerState is not SCPlayerState-derived | class=" +
                SafeName((UObject*)playerState->Class));
            return false;
        }

        UFunction* requestFunction =
            FindFunctionInHierarchyByName(
                playerState->Class,
                "RequestCounselorClass");

        UPropertyLite* pickedProperty =
            FindPropertyInHierarchyByName(
                playerState->Class,
                "PickedCounselorClass");

        if (!requestFunction ||
            !pickedProperty ||
            pickedProperty->Offset_Internal <= 0 ||
            pickedProperty->Offset_Internal >= 0x10000 ||
            pickedProperty->ElementSize != SoftClassSize)
        {
            Logger::Error(
                "Counselor selection 18K: RequestCounselorClass/PickedCounselorClass unavailable or wrong size");
            return false;
        }

        UPropertyLite* newCounselorClassProperty = nullptr;
        UField* field = requestFunction->Children;
        int guard = 0;

        while (field && guard++ < 64)
        {
            if (!Memory::IsReadable(
                    field,
                    sizeof(UField)))
            {
                break;
            }

            if (SafeName((UObject*)field) == "NewCounselorClass")
            {
                UObject* fieldClass =
                    (UObject*)field->ClassPrivate;

                if (fieldClass &&
                    Memory::IsReadable(
                        fieldClass,
                        sizeof(UObject)) &&
                    SafeName(fieldClass) == "SoftClassProperty")
                {
                    UPropertyLite* candidate =
                        (UPropertyLite*)field;

                    if (Memory::IsReadable(
                            candidate,
                            sizeof(UPropertyLite)))
                    {
                        newCounselorClassProperty =
                            candidate;
                    }
                }

                break;
            }

            field = field->Next;
        }

        if (!newCounselorClassProperty ||
            newCounselorClassProperty->Offset_Internal < 0 ||
            newCounselorClassProperty->ElementSize != SoftClassSize ||
            newCounselorClassProperty->Offset_Internal +
                SoftClassSize > 0x100)
        {
            Logger::Error(
                "Counselor selection 18K: NewCounselorClass parameter layout unavailable or not 40-byte SoftClassProperty");
            return false;
        }

        uint8_t* pickedSoftClass =
            (uint8_t*)((uintptr_t)playerState +
                pickedProperty->Offset_Internal);

        if (!Memory::IsReadable(
                pickedSoftClass,
                SoftClassSize))
        {
            Logger::Error(
                "Counselor selection 18K: PickedCounselorClass storage unreadable");
            return false;
        }

        std::string oldPickedPath =
            ReadSoftClassAssetPath(
                pickedSoftClass);

        Logger::Success(
            "Counselor selection 18K: local PlayerState object=" +
            std::to_string((uintptr_t)playerState) +
            " class=" +
            SafeName((UObject*)playerState->Class));

        Logger::Success(
            "Counselor selection 18K: RequestCounselorClass found | NewCounselorClass offset=" +
            std::to_string(newCounselorClassProperty->Offset_Internal) +
            " size=" +
            std::to_string(newCounselorClassProperty->ElementSize) +
            " | PickedCounselorClass offset=" +
            std::to_string(pickedProperty->Offset_Internal) +
            " size=" +
            std::to_string(pickedProperty->ElementSize));

        Logger::Debug(
            "Counselor selection 18K: PickedCounselorClass old=" +
            (oldPickedPath.empty() ?
                std::string("<empty>") :
                oldPickedPath));

        Logger::Success(
            "Counselor selection 18K: requested path=" +
            g_SelectedPlayerCounselorPath +
            " | native roster entry[" +
            std::to_string(index) +
            "]=" +
            kPlayerCounselorClassNames[index]);

        alignas(16) uint8_t params[0x100]{};

        memcpy(
            params +
                newCounselorClassProperty->Offset_Internal,
            g_SelectedPlayerCounselorSoftClass.data(),
            SoftClassSize);

        // Cache only after every reflected address has been validated.  The
        // raw monitor below performs no global scans and no ProcessEvent calls.
        g_CounselorPlayerState.store(
            (uintptr_t)playerState);

        g_PickedCounselorClassOffset.store(
            pickedProperty->Offset_Internal);

        g_CounselorSelectionWorld.store(
            (uintptr_t)Engine::GetWorld());

        bool callOK =
            SafeProcessEventCall(
                (uintptr_t)playerState,
                playerState,
                requestFunction,
                params);

        if (!callOK)
        {
            Logger::Error(
                "Counselor selection 18K: RequestCounselorClass call failed");
            return false;
        }

        g_CounselorRequestSubmittedAt.store(
            GetTickCount64());

        std::string newPickedPath =
            ReadSoftClassAssetPath(
                pickedSoftClass);

        Logger::Debug(
            "Counselor selection 18K: PickedCounselorClass new=" +
            (newPickedPath.empty() ?
                std::string("<empty>") :
                newPickedPath));

        if (newPickedPath ==
            g_SelectedPlayerCounselorPath)
        {
            g_CounselorPickedConfirmed.store(true);

            Logger::Success(
                "Counselor selection 18K SUCCESS: PickedCounselorClass=" +
                newPickedPath);
        }
        else
        {
            Logger::Debug(
                "Counselor selection 18K: stock request submitted; monitoring PickedCounselorClass without force-loading the generated UClass");
        }

        return true;
    }

    static void TryResolveCounselorSelectionSaveOnce()
    {
        if (g_SelectionSaveObject.load() ||
            g_CounselorSaveScanAttempted.exchange(true))
        {
            return;
        }

        if (!ResolveSelectionSaveMetadata() ||
            !g_SelectionSaveClass.load())
        {
            Logger::Debug(
                "Counselor selection 18K: selection-save metadata not ready; CounselorPick fallback will remain disabled");
            return;
        }

        Logger::Debug(
            "Counselor selection 18K: one-shot search for live SCCharacterSelectionsSaveGame");

        UObject* live =
            RawFindSelectionSaveObject(
                (UClass*)g_SelectionSaveClass.load());

        if (live)
        {
            g_SelectionSaveObject.store(
                (uintptr_t)live);

            Logger::Success(
                "Counselor selection 18K: live SCCharacterSelectionsSaveGame found");
        }
        else
        {
            Logger::Debug(
                "Counselor selection 18K: live SCCharacterSelectionsSaveGame not found in one-shot search; stock soft selection remains primary");
        }
    }

    static void TickCounselorSelection()
    {
        int stage =
            g_CounselorStage.load();

        if (stage !=
            (int)CounselorSelectStage::Monitoring)
        {
            return;
        }

        UWorld* world =
            Engine::GetWorld();

        uintptr_t originalWorld =
            g_CounselorSelectionWorld.load();

        if (!world ||
            !originalWorld ||
            (uintptr_t)world != originalWorld)
        {
            Logger::Success(
                "Counselor selection 18K: frontend world changed; counselor selection monitor released | softConfirmed=" +
                std::string(g_CounselorPickedConfirmed.load() ? "true" : "false") +
                " classResident=" +
                std::string(g_TargetPlayerCounselorClass.load() ? "true" : "false") +
                " CounselorPickFallback=" +
                std::string(g_CounselorFallbackApplied.load() ? "true" : "false"));

            g_CounselorStage.store(
                (int)CounselorSelectStage::Done);
            return;
        }

        UObject* playerState =
            (UObject*)g_CounselorPlayerState.load();

        int32_t pickedOffset =
            g_PickedCounselorClassOffset.load();

        if (!playerState ||
            !Memory::IsReadable(
                playerState,
                sizeof(UObject)) ||
            pickedOffset < 0)
        {
            Logger::Error(
                "Counselor selection 18K: cached PlayerState/PickedCounselorClass state lost");

            g_CounselorStage.store(
                (int)CounselorSelectStage::Failed);
            return;
        }

        uint8_t* pickedSoftClass =
            (uint8_t*)((uintptr_t)playerState +
                pickedOffset);

        if (!Memory::IsReadable(
                pickedSoftClass,
                SoftClassSize))
        {
            Logger::Error(
                "Counselor selection 18K: PickedCounselorClass storage became unreadable");

            g_CounselorStage.store(
                (int)CounselorSelectStage::Failed);
            return;
        }

        std::string pickedPath =
            ReadSoftClassAssetPath(
                pickedSoftClass);

        bool matches =
            !g_SelectedPlayerCounselorPath.empty() &&
            pickedPath ==
                g_SelectedPlayerCounselorPath;

        bool wasConfirmed =
            g_CounselorPickedConfirmed.load();

        if (matches &&
            !wasConfirmed)
        {
            g_CounselorPickedConfirmed.store(true);

            Logger::Success(
                "Counselor selection 18K SUCCESS: PickedCounselorClass=" +
                pickedPath);
        }
        else if (!matches &&
            wasConfirmed)
        {
            // The stock request succeeded earlier, so a later mismatch is an
            // actual frontend overwrite.  Keep the SOFT CLASS sticky by
            // restoring the exact native 40-byte TSoftClassPtr.  No UClass
            // load is forced and no ProcessEvent/global scan is repeated.
            memcpy(
                pickedSoftClass,
                g_SelectedPlayerCounselorSoftClass.data(),
                SoftClassSize);

            std::string restoredPath =
                ReadSoftClassAssetPath(
                    pickedSoftClass);

            uint32_t n =
                g_CounselorSoftStickyRewriteCount.fetch_add(1) + 1;

            if (n <= 12)
            {
                Logger::Debug(
                    "Counselor selection 18K sticky soft class: stock frontend changed PickedCounselorClass to " +
                    (pickedPath.empty() ?
                        std::string("<empty>") :
                        pickedPath) +
                    " -> restored " +
                    restoredPath);
            }
        }
        else if (!matches &&
            !wasConfirmed)
        {
            ULONGLONG requestedAt =
                g_CounselorRequestSubmittedAt.load();

            ULONGLONG now =
                GetTickCount64();

            if (requestedAt &&
                now - requestedAt >= 5000 &&
                !g_CounselorInitialMismatchLogged.exchange(true))
            {
                Logger::Debug(
                    "Counselor selection 18K: PickedCounselorClass has not matched after 5 seconds; continuing stock frontend without LoadAssetClass or CounselorPick force-write");
            }
        }

        // CounselorPick is only an optional fallback.  Locate the live save
        // object at most once, and only scan GObjects for the generated class
        // once after the stock soft selection has succeeded.  This is setup
        // work only; nothing is added to the frozen Jason AI tick.
        TryResolveCounselorSelectionSaveOnce();

        UObject* saveObject =
            (UObject*)g_SelectionSaveObject.load();

        int32_t counselorPickOffset =
            g_CounselorPickOffset.load();

        UClass** counselorPick = nullptr;

        if (saveObject &&
            counselorPickOffset >= 0 &&
            Memory::IsReadable(
                saveObject,
                sizeof(UObject)))
        {
            counselorPick =
                (UClass**)((uintptr_t)saveObject +
                    counselorPickOffset);

            if (!Memory::IsReadable(
                    counselorPick,
                    sizeof(UClass*)))
            {
                counselorPick = nullptr;
            }
        }

        int32_t index =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        UClass* targetClass =
            (UClass*)g_TargetPlayerCounselorClass.load();

        // If the stock save updated itself, that pointer proves the generated
        // class is naturally resident and avoids any GObjects scan.
        if (!targetClass &&
            counselorPick &&
            *counselorPick &&
            SafeName((UObject*)*counselorPick) ==
                kPlayerCounselorClassNames[index])
        {
            targetClass =
                *counselorPick;

            g_TargetPlayerCounselorClass.store(
                (uintptr_t)targetClass);

            if (!g_CounselorStockPickLogged.exchange(true))
            {
                Logger::Success(
                    "Counselor selection 18K: stock CounselorPick automatically resolved to naturally loaded " +
                    SafeName((UObject*)targetClass));
            }
        }

        // One global class lookup only, after the primary soft-class request
        // is known-good.  If the class is still not resident, we simply let
        // the stock frontend continue and do not keep scanning or extend a
        // timeout.
        ULONGLONG counselorRequestAt =
            g_CounselorRequestSubmittedAt.load();

        ULONGLONG counselorNow =
            GetTickCount64();

        if (!targetClass &&
            g_CounselorPickedConfirmed.load() &&
            counselorRequestAt &&
            counselorNow - counselorRequestAt >= 1000 &&
            !g_CounselorNaturalClassScanDone.exchange(true))
        {
            UClass* found =
                FindClassExact(
                    kPlayerCounselorClassNames[index]);

            if (found &&
                Memory::IsReadable(
                    found,
                    sizeof(UClass)))
            {
                targetClass = found;

                g_TargetPlayerCounselorClass.store(
                    (uintptr_t)found);

                Logger::Success(
                    "Counselor selection 18K: selected generated counselor class is naturally resident: " +
                    SafeName((UObject*)found));
            }
            else
            {
                Logger::Debug(
                    "Counselor selection 18K: selected generated class is not resident yet; no force-load and no recurring class scan");
            }
        }

        // Only after stock PickedCounselorClass success AND natural UClass
        // residency may we use CounselorPick as a fallback.
        if (targetClass &&
            counselorPick &&
            g_CounselorPickedConfirmed.load())
        {
            UClass* current =
                *counselorPick;

            if (current == targetClass)
            {
                if (!g_CounselorStockPickLogged.exchange(true))
                {
                    Logger::Success(
                        "Counselor selection 18K: stock CounselorPick already matches selected counselor; no fallback write needed");
                }
            }
            else if (!g_CounselorFallbackApplied.load())
            {
                std::string before =
                    SafeName((UObject*)current);

                *counselorPick =
                    targetClass;

                if (*counselorPick == targetClass)
                {
                    g_CounselorFallbackApplied.store(true);

                    Logger::Success(
                        "Counselor selection 18K: naturally loaded class available but CounselorPick had not updated; fallback set CounselorPick from " +
                        before +
                        " to " +
                        SafeName((UObject*)targetClass));
                }
            }
            else if (current != targetClass)
            {
                std::string before =
                    SafeName((UObject*)current);

                *counselorPick =
                    targetClass;

                uint32_t n =
                    g_CounselorClassStickyRewriteCount.fetch_add(1) + 1;

                if (n <= 12)
                {
                    Logger::Debug(
                        "Counselor selection 18K sticky CounselorPick fallback: stock frontend changed CounselorPick to " +
                        before +
                        " -> restored " +
                        SafeName((UObject*)targetClass));
                }
            }
        }
    }

    static bool RequestSandboxSelectedCounselorOnGameThread()
    {
        APlayerController* controller = Engine::GetLocalPlayerController();
        if (!controller || !controller->Class ||
            controller->Class->GetName().find("Sandbox") == std::string::npos)
        {
            Logger::Debug("Counselor mode 18K character sync: Sandbox controller not ready yet");
            g_SandboxNextCharacterAt.store(GetTickCount64() + 750);
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Waiting);
            return true;
        }

        AActor* pawn = controller->AcknowledgedPawn;
        if (!pawn || !pawn->Class ||
            !Memory::IsReadable(pawn, sizeof(UObject)) ||
            !Memory::IsReadable(pawn->Class, sizeof(UObject)))
        {
            Logger::Debug("Counselor mode 18K character sync: acknowledged pawn not ready yet");
            g_SandboxNextCharacterAt.store(GetTickCount64() + 750);
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Waiting);
            return true;
        }

        int32_t index = ClampPlayerCounselorIndex(g_SelectedPlayerCounselorIndex.load());
        const std::string targetName = kPlayerCounselorClassNames[index];
        const std::string currentName = SafeName((UObject*)pawn->Class);

        g_SandboxSyncWorld.store((uintptr_t)Engine::GetWorld());

        if (currentName == targetName)
        {
            Logger::Success("Counselor mode 18K CHARACTER READY: actual pawn class=" + currentName);
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Done);
            return true;
        }

        UFunction* nextFunction =
            FindFunctionInHierarchyByName(controller->Class, "SERVER_RequestNextCharacter");
        if (!nextFunction)
        {
            Logger::Error("Counselor mode 18K character sync: SERVER_RequestNextCharacter not found");
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Failed);
            return false;
        }

        uint8_t params[8]{};
        bool ok = SafeProcessEventCall(
            (uintptr_t)controller,
            controller,
            nextFunction,
            params);

        uint32_t attempt = g_SandboxNextCharacterAttempts.fetch_add(1) + 1;
        Logger::Debug(
            "Counselor mode 18K character sync: actualPawn=" +
            (currentName.empty() ? std::string("<none>") : currentName) +
            " target=" + targetName +
            " -> SERVER_RequestNextCharacter attempt=" + std::to_string(attempt) +
            " call=" + std::string(ok ? "true" : "false"));

        if (!ok)
        {
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Failed);
            return false;
        }

        // Sandbox performs a real pawn replacement here.  18J called this every
        // 500 ms, which outran the game's replacement/possession work.  Give the
        // stock Sandbox controller time to finish one character change before
        // deciding whether another request is needed.
        g_SandboxNextCharacterAt.store(GetTickCount64() + 1500);
        g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Waiting);
        return true;
    }

    static void TickSandboxCounselorSync()
    {
        if (g_SandboxCounselorSyncStage.load() !=
            (int)SandboxCounselorSyncStage::Waiting)
            return;

        ULONGLONG now = GetTickCount64();
        ULONGLONG nextAt = g_SandboxNextCharacterAt.load();
        if (nextAt && now < nextAt)
            return;

        UWorld* world = Engine::GetWorld();
        uintptr_t originalWorld = g_SandboxSyncWorld.load();
        if (originalWorld && world && (uintptr_t)world != originalWorld)
        {
            Logger::Error("Counselor mode 18K character sync: Sandbox world changed before selected counselor was reached");
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Failed);
            return;
        }

        APlayerController* controller = Engine::GetLocalPlayerController();
        if (!controller || !controller->Class ||
            controller->Class->GetName().find("Sandbox") == std::string::npos)
        {
            g_SandboxNextCharacterAt.store(now + 750);
            return;
        }

        AActor* pawn = controller->AcknowledgedPawn;
        if (pawn && pawn->Class &&
            Memory::IsReadable(pawn, sizeof(UObject)) &&
            Memory::IsReadable(pawn->Class, sizeof(UObject)))
        {
            int32_t index = ClampPlayerCounselorIndex(g_SelectedPlayerCounselorIndex.load());
            std::string currentName = SafeName((UObject*)pawn->Class);
            if (currentName == kPlayerCounselorClassNames[index])
            {
                Logger::Success("Counselor mode 18K CHARACTER READY: actual pawn class=" + currentName);
                g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Done);
                return;
            }
        }

        if (g_SandboxNextCharacterAttempts.load() >= 20)
        {
            Logger::Error("Counselor mode 18K character sync: exhausted 20 stock Sandbox character-cycle requests");
            g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::Failed);
            return;
        }

        g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::NeedRequest);
    }

    static bool DumpCounselorRosterOnGameThread()
    {
        NativeObjectItem* objects = nullptr;
        int32_t objectCount = 0;

        if (!GetGObjects(&objects, &objectCount))
        {
            Logger::Error(
                "Counselor discovery 18F: GObjects unavailable");
            return false;
        }

        UObject* kismetDefault = nullptr;
        UObject* offlineBotsGameModeDefault = nullptr;
        UObject* sandboxGameModeDefault = nullptr;
        UObject* baseGameModeDefault = nullptr;
        UClass* scWorldSettingsClass = nullptr;

        for (int32_t i = 0; i < objectCount; ++i)
        {
            UObject* obj = objects[i].Object;
            if (!obj || !Memory::IsReadable(obj, sizeof(UObject)))
                continue;

            std::string name = SafeName(obj);

            if (!kismetDefault && name == "Default__KismetSystemLibrary")
                kismetDefault = obj;
            else if (!offlineBotsGameModeDefault && name == "Default__SCGameMode_OfflineBots")
                offlineBotsGameModeDefault = obj;
            else if (!sandboxGameModeDefault && name == "Default__SCGameMode_Sandbox")
                sandboxGameModeDefault = obj;
            else if (!baseGameModeDefault && name == "Default__SCGameMode")
                baseGameModeDefault = obj;
            else if (!scWorldSettingsClass && name == "SCWorldSettings")
            {
                UObject* meta = (UObject*)obj->Class;
                if (meta && SafeName(meta) == "Class")
                    scWorldSettingsClass = (UClass*)obj;
            }
        }

        UObject* gameModeDefault =
            offlineBotsGameModeDefault ? offlineBotsGameModeDefault :
            (sandboxGameModeDefault ? sandboxGameModeDefault : baseGameModeDefault);

        if (!kismetDefault || !kismetDefault->Class ||
            !gameModeDefault || !gameModeDefault->Class)
        {
            Logger::Error(
                "Counselor discovery 18F: required Kismet/GameMode CDOs not ready");
            return false;
        }

        UFunction* convertFunction =
            FindFunctionInHierarchyByName(
                kismetDefault->Class,
                "Conv_SoftClassReferenceToClass");

        UPropertyLite* counselorClassesProperty =
            FindPropertyInHierarchyByName(
                gameModeDefault->Class,
                "CounselorCharacterClasses");

        if (!convertFunction || !counselorClassesProperty ||
            counselorClassesProperty->Offset_Internal <= 0 ||
            counselorClassesProperty->Offset_Internal >= 0x10000)
        {
            Logger::Error(
                "Counselor discovery 18F: converter or CounselorCharacterClasses not resolved");
            return false;
        }

        RawArray* counselorClasses =
            (RawArray*)((uintptr_t)gameModeDefault +
                counselorClassesProperty->Offset_Internal);

        if (!Memory::IsReadable(counselorClasses, sizeof(RawArray)) ||
            !counselorClasses->Data ||
            counselorClasses->Count <= 0 ||
            counselorClasses->Count > 64 ||
            counselorClasses->Max < counselorClasses->Count ||
            counselorClasses->Max > 128 ||
            !Memory::IsReadable(
                counselorClasses->Data,
                (size_t)counselorClasses->Count * SoftClassSize))
        {
            Logger::Error(
                "Counselor discovery 18F: CounselorCharacterClasses array unavailable/empty");
            return false;
        }

        Logger::Success(
            "========== COUNSELOR DISCOVERY 18F BEGIN ==========");
        Logger::Success(
            "Counselor discovery 18F: GameMode=" +
            SafeName(gameModeDefault) +
            " | native counselor entries=" +
            std::to_string(counselorClasses->Count));

        struct ResolveParams
        {
            uint8_t SoftClass[40];
            UClass* ReturnValue;
        };

        static_assert(
            sizeof(ResolveParams) == 48,
            "ResolveParams must be 48 bytes");

        int32_t resolvedCount = 0;

        for (int32_t index = 0;
            index < counselorClasses->Count;
            ++index)
        {
            uint8_t* softClass =
                counselorClasses->Data +
                ((uintptr_t)index * SoftClassSize);

            std::string path =
                ReadSoftClassAssetPath(softClass);

            ResolveParams params{};
            memcpy(params.SoftClass, softClass, SoftClassSize);

            bool resolveOK =
                SafeProcessEventCall(
                    (uintptr_t)kismetDefault,
                    kismetDefault,
                    convertFunction,
                    &params);

            std::string resolvedName =
                (resolveOK && params.ReturnValue &&
                    Memory::IsReadable(params.ReturnValue, sizeof(UClass)))
                ? SafeName((UObject*)params.ReturnValue)
                : std::string("<not-loaded>");

            if (resolvedName != "<not-loaded>")
                ++resolvedCount;

            Logger::Success(
                "COUNSELOR ROSTER [" +
                std::to_string(index) +
                "] path=" +
                (path.empty() ? std::string("<empty>") : path) +
                " | resolved=" + resolvedName);
        }

        Logger::Success(
            "Counselor discovery 18F: resolved=" +
            std::to_string(resolvedCount) +
            "/" +
            std::to_string(counselorClasses->Count));

        // Also enumerate any currently loaded SCWorldSettings-derived objects
        // and print their HeroCharacterClass soft reference.  This is read-only
        // discovery for the next selectable Hero step.
        int32_t heroWorldSettingsCount = 0;

        if (scWorldSettingsClass)
        {
            for (int32_t i = 0;
                i < objectCount && heroWorldSettingsCount < 32;
                ++i)
            {
                UObject* obj = objects[i].Object;
                if (!obj || !obj->Class ||
                    !Memory::IsReadable(obj, sizeof(UObject)) ||
                    !Memory::IsReadable(obj->Class, sizeof(UClass)) ||
                    !ClassIsOrDerivesFrom(obj->Class, scWorldSettingsClass))
                {
                    continue;
                }

                UPropertyLite* heroProperty =
                    FindPropertyInHierarchyByName(
                        obj->Class,
                        "HeroCharacterClass");

                if (!heroProperty ||
                    heroProperty->Offset_Internal <= 0 ||
                    heroProperty->Offset_Internal >= 0x10000)
                {
                    continue;
                }

                uint8_t* heroSoftClass =
                    (uint8_t*)((uintptr_t)obj +
                        heroProperty->Offset_Internal);

                std::string heroPath =
                    ReadSoftClassAssetPath(heroSoftClass);

                if (heroPath.empty())
                    continue;

                ResolveParams heroParams{};
                memcpy(heroParams.SoftClass, heroSoftClass, SoftClassSize);

                bool heroResolveOK =
                    SafeProcessEventCall(
                        (uintptr_t)kismetDefault,
                        kismetDefault,
                        convertFunction,
                        &heroParams);

                std::string heroResolved =
                    (heroResolveOK && heroParams.ReturnValue &&
                        Memory::IsReadable(heroParams.ReturnValue, sizeof(UClass)))
                    ? SafeName((UObject*)heroParams.ReturnValue)
                    : std::string("<not-loaded>");

                Logger::Success(
                    "HERO WORLDSETTINGS [" +
                    std::to_string(heroWorldSettingsCount) +
                    "] object=" +
                    SafeName(obj) +
                    " | class=" +
                    SafeName((UObject*)obj->Class) +
                    " | path=" + heroPath +
                    " | resolved=" + heroResolved);

                ++heroWorldSettingsCount;
            }
        }

        if (heroWorldSettingsCount == 0)
        {
            Logger::Debug(
                "Counselor discovery 18F: no non-empty HeroCharacterClass soft references found in loaded SCWorldSettings objects");
        }

        Logger::Success(
            "========== COUNSELOR DISCOVERY 18F END ==========");
        return true;
    }

    static bool ResolveJason5ClassOnGameThread()
    {
        // Prototype 17C proved the exact stock soft-class path:
        // /Game/Characters/Killers/Jason/J5/Jason_J5.Jason_J5_C
        //
        // LoadAssetClass did not materialize the generated class even after
        // 30 seconds.  The existing counselor-bot code already has a proven
        // path that resolves TSoftClassPtr entries synchronously:
        //
        //   KismetSystemLibrary::Conv_SoftClassReferenceToClass
        //
        // Use that exact stock conversion here instead of the latent loader.

        UObject* kismetDefault = nullptr;
        UObject* gameModeDefault = nullptr;

        // Resolve both required CDOs in ONE GObjects pass.  Prototype 17C
        // performed several full scans and spent ~20 seconds before it even
        // submitted the target request.
        NativeObjectItem* objects = nullptr;
        int32_t objectCount = 0;

        if (!GetGObjects(&objects, &objectCount))
        {
            Logger::Debug(
                "Jason resolve 18B: GObjects unavailable");
            return false;
        }

        for (int32_t i = 0; i < objectCount; ++i)
        {
            UObject* obj = objects[i].Object;
            if (!obj || !Memory::IsReadable(obj, sizeof(UObject)))
                continue;

            std::string name = SafeName(obj);

            if (!kismetDefault &&
                name == "Default__KismetSystemLibrary")
            {
                kismetDefault = obj;
            }

            if (!gameModeDefault)
            {
                if (name == "Default__SCGameMode_OfflineBots" ||
                    name == "Default__SCGameMode_Sandbox" ||
                    name == "Default__SCGameMode")
                {
                    gameModeDefault = obj;
                }
            }

            if (kismetDefault && gameModeDefault)
                break;
        }

        if (!kismetDefault || !kismetDefault->Class ||
            !gameModeDefault || !gameModeDefault->Class)
        {
            Logger::Debug(
                "Jason resolve 18B: required Kismet/GameMode CDOs not ready");
            return false;
        }

        UFunction* convertFunction =
            FindFunctionInHierarchyByName(
                kismetDefault->Class,
                "Conv_SoftClassReferenceToClass");

        UPropertyLite* killerClassesProperty =
            FindPropertyInHierarchyByName(
                gameModeDefault->Class,
                "KillerCharacterClasses");

        if (!convertFunction || !killerClassesProperty ||
            killerClassesProperty->Offset_Internal <= 0 ||
            killerClassesProperty->Offset_Internal >= 0x10000)
        {
            Logger::Debug(
                "Jason resolve 18B: converter or KillerCharacterClasses not resolved");
            return false;
        }

        RawArray* killerClasses =
            (RawArray*)((uintptr_t)gameModeDefault +
                killerClassesProperty->Offset_Internal);

        if (!Memory::IsReadable(killerClasses, sizeof(RawArray)) ||
            !killerClasses->Data ||
            killerClasses->Count <= 0 ||
            killerClasses->Count > 64 ||
            killerClasses->Max < killerClasses->Count ||
            killerClasses->Max > 128 ||
            !Memory::IsReadable(
                killerClasses->Data,
                (size_t)killerClasses->Count * SoftClassSize))
        {
            Logger::Debug(
                "Jason resolve 18B: KillerCharacterClasses array unavailable/empty");
            return false;
        }

        int32_t targetIndex =
            ClampJasonIndex(g_SelectedJasonIndex.load());

        if (targetIndex < 0 ||
            targetIndex >= killerClasses->Count)
        {
            Logger::Error(
                "Jason resolve 18B: selected Jason index is outside KillerCharacterClasses");
            return false;
        }

        uint8_t* selectedSoftClass =
            killerClasses->Data + ((uintptr_t)targetIndex * SoftClassSize);

        std::string targetPath =
            ReadSoftClassAssetPath(selectedSoftClass);

        if (targetPath.empty())
        {
            Logger::Error(
                "Jason resolve 18B: selected Jason soft-class path is empty");
            return false;
        }

        struct ResolveParams
        {
            uint8_t SoftClass[40];
            UClass* ReturnValue;
        };

        static_assert(
            sizeof(ResolveParams) == 48,
            "ResolveParams must be 48 bytes");

        ResolveParams params{};

        memcpy(
            params.SoftClass,
            selectedSoftClass,
            SoftClassSize);

        Logger::Debug(
            "Jason resolve 18B: converting entry[" +
            std::to_string(targetIndex) + "] " + targetPath);

        bool resolveOK =
            SafeProcessEventCall(
                (uintptr_t)kismetDefault,
                kismetDefault,
                convertFunction,
                &params);

        UClass* resolved = params.ReturnValue;

        if (!resolveOK ||
            !resolved ||
            !Memory::IsReadable(resolved, sizeof(UClass)))
        {
            Logger::Error(
                "Jason resolve 18B: Conv_SoftClassReferenceToClass returned null/invalid");
            return false;
        }

        std::string resolvedName = SafeName((UObject*)resolved);

        Logger::Debug(
            "Jason resolve 18B: converter returned " +
            (resolvedName.empty() ? std::string("<unnamed>") : resolvedName) +
            " @ " + std::to_string((uintptr_t)resolved));

        if (resolvedName.empty())
        {
            Logger::Error(
                "Jason resolve 18B: selected Jason converter returned unnamed class");
            return false;
        }

        g_TargetJasonClass.store((uintptr_t)resolved);
        Logger::Success("MenuSelectedJason=" + resolvedName);

        Logger::Success(
            "Jason resolve 18B SUCCESS: selected Jason class resolved synchronously: " +
            resolvedName);
        return true;
    }

    static bool TryResolveTargetJasonClass()
    {
        return g_TargetJasonClass.load() != 0;
    }

    struct LifecycleRawArray
    {
        uint8_t* Data;
        int32_t Count;
        int32_t Max;
    };

    static constexpr int32_t LifecycleSoftClassSize = 40;

#if defined(F13_BASE_GAME_PORT)
    // Exact equivalents in the packed stock executable
    // SHA256 941E249E4CAABF93A22FB57A6EB61D894D224C16A7464CD7451B41205436E62F.
    // Each entry was matched against its Resurrected implementation and is
    // signature-checked before the lifecycle is allowed to run.
    static constexpr uintptr_t RVA_LifecycleLoadSoftClass = 0x29D880;
    static constexpr uintptr_t RVA_LifecycleCopySoftClass = 0x3A0690;
    static constexpr uintptr_t RVA_LifecycleSetActiveCharacter = 0x3A5C00;
    static constexpr uintptr_t RVA_LifecycleSpawnParamsCtor = 0x17D0E10;
    static constexpr uintptr_t RVA_LifecycleSpawnActor = 0x14DA140;
    static constexpr uintptr_t RVA_LifecycleRestartPlayer = 0x396850;
    static constexpr uintptr_t RVA_LifecycleCounselorControllerClass = 0x479A60;
    static constexpr uintptr_t RVA_LifecycleHasFullyTraveled = 0x26A2A0;
    static constexpr uintptr_t RVA_LifecycleCrowdFollowerCtorCall = 0x2F04A0;
    static constexpr uintptr_t RVA_LifecycleKillerControllerClass = 0x4A8140;
    static constexpr uintptr_t RVA_LifecycleBasePossess = 0x110E290;
#else
    static constexpr uintptr_t RVA_LifecycleLoadSoftClass = 0x2A6310;
    static constexpr uintptr_t RVA_LifecycleCopySoftClass = 0x3B16D0;
    static constexpr uintptr_t RVA_LifecycleSetActiveCharacter = 0x3B7B70;
    static constexpr uintptr_t RVA_LifecycleSpawnParamsCtor = 0x17F1160;
    static constexpr uintptr_t RVA_LifecycleSpawnActor = 0x1519D10;
    static constexpr uintptr_t RVA_LifecycleRestartPlayer = 0x3A4B70;
    static constexpr uintptr_t RVA_LifecycleCounselorControllerClass = 0x48E5C0;
    static constexpr uintptr_t RVA_LifecycleHasFullyTraveled = 0x2A3A60;
    static constexpr uintptr_t RVA_LifecycleCrowdFollowerCtorCall = 0x2FE340;
    static constexpr uintptr_t RVA_LifecycleKillerControllerClass = 0x4C8FF0;
    static constexpr uintptr_t RVA_LifecycleBasePossess = 0x112F3F0;
#endif

    static uintptr_t ShippingAddress(uintptr_t rva)
    {
        HMODULE module = GetModuleHandleW(nullptr);
        return module ? reinterpret_cast<uintptr_t>(module) + rva : 0;
    }

    static bool MatchesBytes(
        uintptr_t address,
        const uint8_t* expected,
        size_t size)
    {
        return address &&
            expected &&
            Memory::IsReadable(reinterpret_cast<void*>(address), size) &&
            std::memcmp(reinterpret_cast<void*>(address), expected, size) == 0;
    }

    static bool ValidateCounselorLifecycleNativeSurface()
    {
        static std::atomic<int> state{ 0 };
        const int current = state.load();
        if (current != 0)
            return current > 0;

        const uint8_t spawnSig[] =
        {
            0x40,0x53,0x56,0x57,0x48,0x83,0xEC,0x70,
            0x48,0x8B,0x05
        };
        const uint8_t paramsSig[] =
        {
            0x33,0xC0,0x48,0x89,0x01,0x48,0x89,0x41,
            0x08,0x48,0x89,0x41,0x10,0x48,0x89,0x41
        };
        const uint8_t possessSig[] =
        {
            0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,
            0x24,0x20,0x57,0x48,0x83,0xEC,0x40,0x33
        };
        const uint8_t activeSig[] =
        {
            0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,
            0xEC,0x50,0x33,0xC0,0x48,0x8B,0xDA,0x48
        };
        const uint8_t copySig[] =
        {
            0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,
            0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57
        };

        const bool valid =
            MatchesBytes(ShippingAddress(RVA_LifecycleSpawnActor), spawnSig, sizeof(spawnSig)) &&
            MatchesBytes(ShippingAddress(RVA_LifecycleSpawnParamsCtor), paramsSig, sizeof(paramsSig)) &&
            MatchesBytes(ShippingAddress(RVA_LifecycleBasePossess), possessSig, sizeof(possessSig)) &&
            MatchesBytes(ShippingAddress(RVA_LifecycleSetActiveCharacter), activeSig, sizeof(activeSig)) &&
            MatchesBytes(ShippingAddress(RVA_LifecycleCopySoftClass), copySig, sizeof(copySig));

        state.store(valid ? 1 : -1);
        if (!valid)
        {
            Logger::Error(
                "18L-AD lifecycle native signature mismatch; counselor replacement is disabled");
        }
        return valid;
    }

    static uintptr_t GetLifecycleArrayOffset(
        UObject* gameMode,
        const char* propertyName,
        uintptr_t resurrectedFallback)
    {
        if (gameMode && gameMode->Class && propertyName)
        {
            UPropertyLite* property = FindPropertyInHierarchyByName(
                gameMode->Class,
                propertyName);
            if (property &&
                property->Offset_Internal > 0 &&
                property->Offset_Internal < 0x10000)
            {
                return static_cast<uintptr_t>(property->Offset_Internal);
            }
        }
#if defined(F13_BASE_GAME_PORT)
        return 0;
#else
        return resurrectedFallback;
#endif
    }

    static const uint8_t* GetLifecycleSoftClass(
        UObject* gameMode,
        uintptr_t arrayOffset,
        int32_t index)
    {
        if (!gameMode || index < 0)
            return nullptr;

        LifecycleRawArray* classes =
            reinterpret_cast<LifecycleRawArray*>(
                reinterpret_cast<uintptr_t>(gameMode) + arrayOffset);

        if (!Memory::IsReadable(classes, sizeof(LifecycleRawArray)) ||
            !classes->Data ||
            classes->Count <= index ||
            classes->Count <= 0 ||
            classes->Count > 64 ||
            classes->Max < classes->Count ||
            classes->Max > 128 ||
            !Memory::IsReadable(
                classes->Data,
                static_cast<size_t>(classes->Count) * LifecycleSoftClassSize))
        {
            return nullptr;
        }

        return classes->Data +
            static_cast<uintptr_t>(index) * LifecycleSoftClassSize;
    }

    static int32_t GetLifecycleSoftClassCount(
        UObject* gameMode,
        uintptr_t arrayOffset)
    {
        if (!gameMode)
            return 0;

        LifecycleRawArray* classes =
            reinterpret_cast<LifecycleRawArray*>(
                reinterpret_cast<uintptr_t>(gameMode) + arrayOffset);

        if (!Memory::IsReadable(classes, sizeof(LifecycleRawArray)) ||
            !classes->Data ||
            classes->Count <= 0 ||
            classes->Count > 64 ||
            classes->Max < classes->Count ||
            classes->Max > 128)
        {
            return 0;
        }
        return classes->Count;
    }

    static UClass* LoadLifecycleSoftClass(const uint8_t* softClass)
    {
        if (!softClass ||
            !Memory::IsReadable(softClass, LifecycleSoftClassSize))
        {
            return nullptr;
        }

        using LoadSoftClassFn = UClass* (__fastcall*)(const void*);
        LoadSoftClassFn load = reinterpret_cast<LoadSoftClassFn>(
            ShippingAddress(RVA_LifecycleLoadSoftClass));

        if (!load ||
            !Memory::IsReadable(reinterpret_cast<void*>(load), 1))
        {
            return nullptr;
        }

        return load(softClass);
    }

    static bool ClassDerivesFrom(UClass* cls, const char* baseName)
    {
        if (!cls || !baseName)
            return false;

        for (UStruct* current = reinterpret_cast<UStruct*>(cls);
            current;
            current = current->Super)
        {
            if (!Memory::IsReadable(current, sizeof(UStruct)))
                break;
            if (SafeName(reinterpret_cast<UObject*>(current)) == baseName)
                return true;
        }
        return false;
    }

    static bool ResolveNativeSelectedKiller(
        UObject* gameMode,
        const uint8_t** softClassOut,
        UClass** classOut)
    {
        if (!gameMode || !softClassOut || !classOut)
            return false;

        *softClassOut = nullptr;
        *classOut = nullptr;

        const uintptr_t killerArrayOffset = GetLifecycleArrayOffset(
            gameMode,
            "KillerCharacterClasses",
            0x6B0);
        const int32_t killerCount =
            GetLifecycleSoftClassCount(gameMode, killerArrayOffset);
        if (killerCount <= 0)
            return false;

#if defined(F13_BASE_GAME_PORT)
        // The stock picker can commit KillerPick immediately before world
        // travel, between the frontend's 500-ms profile polls. Read its saved
        // choice once more on the lifecycle game thread before resolving the
        // intro and independent AI pawn; do not rely on the old J5 fallback.
        if (!g_TargetJasonClass.load() && ResolveSelectionSaveMetadata())
        {
            UObject* selections = reinterpret_cast<UObject*>(
                g_SelectionSaveObject.load());
            if (!selections ||
                !Memory::IsReadable(selections, sizeof(UObject)))
            {
                selections = RawFindSelectionSaveObject(
                    reinterpret_cast<UClass*>(g_SelectionSaveClass.load()));
                if (selections)
                    g_SelectionSaveObject.store(
                        reinterpret_cast<uintptr_t>(selections));
            }
            const int32_t pickOffset = g_KillerPickOffset.load();
            if (selections && pickOffset > 0 && pickOffset < 0x10000)
            {
                UClass** savedPick = reinterpret_cast<UClass**>(
                    reinterpret_cast<uintptr_t>(selections) + pickOffset);
                if (Memory::IsReadable(savedPick, sizeof(UClass*)) &&
                    *savedPick && Memory::IsReadable(*savedPick, sizeof(UClass)) &&
                    ClassDerivesFrom(*savedPick, "SCKillerCharacter"))
                {
                    g_TargetJasonClass.store(
                        reinterpret_cast<uintptr_t>(*savedPick));
                    Logger::Success(
                        "STOCK JASON PICK AT LIFECYCLE: " +
                        SafeName(reinterpret_cast<UObject*>(*savedPick)));
                }
            }
        }
#endif

        UClass* nativePick = reinterpret_cast<UClass*>(
            g_TargetJasonClass.load());
        const std::string nativePickName =
            nativePick && Memory::IsReadable(nativePick, sizeof(UClass))
            ? SafeName(reinterpret_cast<UObject*>(nativePick))
            : std::string();

#if defined(F13_BASE_GAME_PORT)
        const int32_t pickerIndex = g_SelectedJasonIndex.load();
        constexpr int32_t stockPickerCount = static_cast<int32_t>(
            sizeof(kStockPickerJasonClasses) /
            sizeof(kStockPickerJasonClasses[0]));
        const std::string stockPickerName =
            g_StockKillerRequestCaptured.load() &&
            pickerIndex >= 0 && pickerIndex < stockPickerCount
            ? kStockPickerJasonClasses[pickerIndex] : std::string();
#endif

        // A resolved live choice is authoritative. Do not accept a second,
        // contradictory default-table class simply because it occurs first
        // in GameMode's class list (Savini previously became Uber this way).
        std::string requestedPickName = nativePickName;
#if defined(F13_BASE_GAME_PORT)
        if (requestedPickName.empty()) requestedPickName = stockPickerName;
#endif
        if (!requestedPickName.empty())
        {
            for (int32_t i = 0; i < killerCount; ++i)
            {
                const uint8_t* candidateSoft =
                    GetLifecycleSoftClass(gameMode, killerArrayOffset, i);
                UClass* candidateClass = LoadLifecycleSoftClass(candidateSoft);
                if (!candidateClass ||
                    !ClassDerivesFrom(candidateClass, "SCKillerCharacter"))
                {
                    continue;
                }

                if (SafeName(reinterpret_cast<UObject*>(candidateClass)) == requestedPickName)
                {
                    *softClassOut = candidateSoft;
                    *classOut = candidateClass;
                    Logger::Success(
                        "IN-GAME JASON SELECTION COMMITTED: " +
                        SafeName(reinterpret_cast<UObject*>(candidateClass)));
                    return true;
                }
            }
            Logger::Error("IN-GAME JASON SELECTION: requested class not available; refusing a different Jason: " +
                requestedPickName);
            return false;
        }

        const int32_t fallbackIndex =
            ClampJasonIndex(g_SelectedJasonIndex.load()) % killerCount;
        const uint8_t* fallbackSoft =
            GetLifecycleSoftClass(gameMode, killerArrayOffset, fallbackIndex);
        UClass* fallbackClass = LoadLifecycleSoftClass(fallbackSoft);
        if (!fallbackClass ||
            !ClassDerivesFrom(fallbackClass, "SCKillerCharacter"))
        {
            return false;
        }

        *softClassOut = fallbackSoft;
        *classOut = fallbackClass;
        return true;
    }

    static bool ResolveNativeSelectedCounselor(
        UObject* gameMode,
        const uint8_t** softClassOut,
        UClass** classOut)
    {
        if (!gameMode || !softClassOut || !classOut)
            return false;

        *softClassOut = nullptr;
        *classOut = nullptr;

        UClass* nativePick = reinterpret_cast<UClass*>(
            g_TargetPlayerCounselorClass.load());
        const std::string nativePickName =
            nativePick && Memory::IsReadable(nativePick, sizeof(UClass))
            ? SafeName(reinterpret_cast<UObject*>(nativePick))
            : std::string();

        if (nativePickName == "Hunter_Counselor_C")
        {
            UPropertyLite* hunterProperty = FindPropertyInHierarchyByName(
                gameMode->Class,
                "HunterCharacterClass");
            if (hunterProperty &&
                hunterProperty->Offset_Internal > 0 &&
                hunterProperty->Offset_Internal < 0x10000 &&
                hunterProperty->ElementSize >= LifecycleSoftClassSize)
            {
                const uint8_t* hunterSoft = reinterpret_cast<const uint8_t*>(
                    reinterpret_cast<uintptr_t>(gameMode) +
                    hunterProperty->Offset_Internal);
                UClass* hunterClass = LoadLifecycleSoftClass(hunterSoft);
                if (hunterClass &&
                    ClassDerivesFrom(hunterClass, "SCCounselorCharacter"))
                {
                    *softClassOut = hunterSoft;
                    *classOut = hunterClass;
                    return true;
                }
            }
        }

        const uintptr_t counselorArrayOffset = GetLifecycleArrayOffset(
            gameMode,
            "CounselorCharacterClasses",
            0x510);
        const int32_t counselorCount =
            GetLifecycleSoftClassCount(gameMode, counselorArrayOffset);
        if (counselorCount <= 0)
            return false;

        if (!nativePickName.empty())
        {
            for (int32_t i = 0; i < counselorCount; ++i)
            {
                const uint8_t* candidateSoft =
                    GetLifecycleSoftClass(gameMode, counselorArrayOffset, i);
                UClass* candidateClass = LoadLifecycleSoftClass(candidateSoft);
                if (!candidateClass ||
                    !ClassDerivesFrom(candidateClass, "SCCounselorCharacter"))
                {
                    continue;
                }

                if (candidateClass == nativePick ||
                    SafeName(reinterpret_cast<UObject*>(candidateClass)) ==
                        nativePickName)
                {
                    *softClassOut = candidateSoft;
                    *classOut = candidateClass;
                    Logger::Success(
                        "IN-GAME COUNSELOR SELECTION COMMITTED: " +
                        nativePickName);
                    return true;
                }
            }
        }

        const int32_t preferred =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());
        for (int32_t i = 0; i < counselorCount; ++i)
        {
            const int32_t index = (preferred + i) % counselorCount;
            const uint8_t* candidateSoft =
                GetLifecycleSoftClass(gameMode, counselorArrayOffset, index);
            UClass* candidateClass = LoadLifecycleSoftClass(candidateSoft);
            if (candidateClass &&
                ClassDerivesFrom(candidateClass, "SCCounselorCharacter"))
            {
                *softClassOut = candidateSoft;
                *classOut = candidateClass;
                return true;
            }
        }

        return false;
    }

    static void ApplyPendingStartingItemOnGameThread()
    {
#if defined(F13_BASE_GAME_PORT)
        if (!g_StartingItemPending.load() || GetTickCount64() < g_NextStartingItemAt.load()) return;
        UWorld* world = Engine::GetWorld();
        if (!world || reinterpret_cast<uintptr_t>(world) != g_StartingItemWorld.load() ||
            !g_CounselorJasonActive.load())
        {
            g_StartingItemPending.store(false);
            return;
        }
        int32_t stage = g_StartingLoadoutStage.load();
        if (stage == 0 && g_CommittedStartingWeapon.load() < 2)
            g_StartingLoadoutStage.store(stage = 1);
        const int32_t choice = stage == 0 ? g_CommittedStartingWeapon.load() : g_CommittedStartingInventory.load();
        const int32_t selected = stage == 0 ? choice + 4 : choice;
        if (stage > 1 || choice < 2 || (stage == 0 ? choice > 4 : choice > 5))
        {
            g_StartingItemPending.store(false);
            return;
        }
        if (g_StartingItemAttempts.fetch_add(1) >= 15)
        {
            Logger::Error("STARTING LOADOUT: slot unavailable after bounded retry | stage=" + std::to_string(stage));
            g_StartingLoadoutStage.store(stage + 1);
            g_StartingItemAttempts.store(0);
            g_StartingItemPending.store(stage == 0 && g_CommittedStartingInventory.load() >= 2);
            return;
        }
        g_NextStartingItemAt.store(GetTickCount64() + 1000);
        static const char* classes[9] = { "", "", "BP_PocketKnife_C", "BP_FirstAid_C",
            "BP_Firecracker_C", "Map_C", "CounselorMachete_C", "CounselorTwoHandedAxe_C", "Shotgun_C" };
        APlayerController* controller = Engine::GetLocalPlayerController();
        APawn* pawn = controller ? controller->AcknowledgedPawn : nullptr;
        if (!pawn || !pawn->Class || !ClassDerivesFrom(pawn->Class, "SCCounselorCharacter")) return;
        UClass* itemClass = FindClassExact(classes[selected]);
        if (!itemClass || !ClassDerivesFrom(itemClass, "SCItem")) return;
        // Native GiveStartingItem owns PickingItem, then calls the character's
        // AddOrSwapPickingItem virtual. It selects small vs large inventory.
        // Wait for any existing pickup to finish, never overwrite that slot.
        UObject** picking = reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(pawn) + 0xF68);
        if (!Memory::IsReadable(picking, sizeof(UObject*)) || *picking) return;
        // Consume this slot before the native call (which can re-enter hooks).
        // The next slot waits at least one second and for PickingItem to clear.
        g_StartingLoadoutStage.store(stage + 1);
        g_StartingItemAttempts.store(0);
        g_StartingItemPending.store(stage == 0 && g_CommittedStartingInventory.load() >= 2);
        if (!FrozenJasonBridge::GrantHumanStartingItem(reinterpret_cast<AActor*>(pawn), itemClass))
        {
            Logger::Error("STARTING ITEM: validated native grant unavailable; inventory unchanged");
            return;
        }
        Logger::Success("STARTING LOADOUT: native grant dispatched | stage=" + std::to_string(stage) +
            " | human=" + SafeName(pawn) + " | class=" + classes[selected] + " | once per slot per match");
#endif
    }

    static bool GrantHunterStartingLoadout(
        UObject* gameMode,
        APawn* bornPawn)
    {
        if (!gameMode ||
            !bornPawn ||
            !Memory::IsReadable(gameMode, 0x6A0) ||
            !Memory::IsReadable(bornPawn, 0x1A19) ||
            !bornPawn->Class)
        {
            return false;
        }

        const std::string pawnClass =
            SafeName(reinterpret_cast<UObject*>(bornPawn->Class));
        const bool isHunter =
            *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x1A18) != 0 ||
            pawnClass == "Hunter_Counselor_C";

        // Tommy/Hunter is the stock hero with this four-item starting kit.
        // Ordinary counselors and Hero_Counselor_C (the female hero class)
        // retain their own normal match inventory.
        if (!isHunter)
            return true;

        APlayerController* localController =
            Engine::GetLocalPlayerController();
        UObject* pawnController =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x3A0);
        UObject* pickingItem =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0xF68);
        UObject* specialInventory =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x1520);
        UObject* smallInventory =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x1528);

        if (!localController ||
            pawnController != localController ||
            pickingItem ||
            !specialInventory ||
            !smallInventory ||
            !Memory::IsReadable(specialInventory, sizeof(UObject)) ||
            !Memory::IsReadable(smallInventory, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AH HERO LOADOUT: born Hunter is not ready for the stock grant path");
            return false;
        }

        UClass* shotgunClass = LoadLifecycleSoftClass(
            reinterpret_cast<uint8_t*>(bornPawn) + 0x1450);
        UClass* mapClass = LoadLifecycleSoftClass(
            reinterpret_cast<uint8_t*>(gameMode) + 0x5E8);
        UClass* sprayClass = LoadLifecycleSoftClass(
            reinterpret_cast<uint8_t*>(gameMode) + 0x660);
        UClass* pocketKnifeClass = LoadLifecycleSoftClass(
            reinterpret_cast<uint8_t*>(gameMode) + 0x638);

        UClass* itemClasses[] =
        {
            shotgunClass,
            mapClass,
            sprayClass,
            pocketKnifeClass
        };
        const char* itemLabels[] =
        {
            "Shotgun",
            "Map",
            "HealthSpray",
            "PocketKnife"
        };
        const bool smallItems[] =
        {
            false,
            false,
            true,
            true
        };

        for (UClass* itemClass : itemClasses)
        {
            if (!itemClass ||
                !Memory::IsReadable(itemClass, sizeof(UClass)) ||
                !ClassDerivesFrom(itemClass, "SCItem"))
            {
                Logger::Error(
                    "18L-AH HERO LOADOUT: a stock item soft class did not resolve as SCItem");
                return false;
            }
        }

        using GiveStartingItemFn = void(__fastcall*)(APawn*, UClass*);
        using IsSmallInventoryFullFn = bool(__fastcall*)(APawn*);
        using CountItemFn = int32_t(__fastcall*)(APawn*, UClass*);

        GiveStartingItemFn giveStartingItem =
            reinterpret_cast<GiveStartingItemFn>(ShippingAddress(0x2EF980));
        IsSmallInventoryFullFn isSmallInventoryFull =
            reinterpret_cast<IsSmallInventoryFullFn>(
                ShippingAddress(0x3D4060));

        uintptr_t* pawnVtable =
            *reinterpret_cast<uintptr_t**>(bornPawn);
        CountItemFn countItem = nullptr;
        if (pawnVtable &&
            Memory::IsReadable(
                pawnVtable,
                0xE98 + sizeof(uintptr_t)))
        {
            countItem = reinterpret_cast<CountItemFn>(
                pawnVtable[0xE98 / sizeof(uintptr_t)]);
        }

        if (!giveStartingItem ||
            !isSmallInventoryFull ||
            !countItem ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(giveStartingItem), 1) ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(isSmallInventoryFull), 1) ||
            !Memory::IsReadable(
                reinterpret_cast<void*>(countItem), 1))
        {
            Logger::Error(
                "18L-AH HERO LOADOUT: verified stock inventory functions are unavailable");
            return false;
        }

        bool complete = true;
        std::string counts;
        for (int32_t i = 0; i < 4; ++i)
        {
            int32_t count = countItem(bornPawn, itemClasses[i]);
            if (count <= 0)
            {
                if (smallItems[i] && isSmallInventoryFull(bornPawn))
                {
                    Logger::Error(
                        std::string("18L-AH HERO LOADOUT: stock small inventory unexpectedly full before ") +
                        itemLabels[i]);
                    complete = false;
                }
                else
                {
                    // This is the exact native helper used by stock Hunter
                    // spawn: spawn, mark picked, and route through
                    // AddOrSwapPickingItem.  No inventory arrays are patched.
                    giveStartingItem(bornPawn, itemClasses[i]);
                    count = countItem(bornPawn, itemClasses[i]);
                    if (count <= 0)
                        complete = false;
                }
            }

            if (!counts.empty())
                counts += ",";
            counts += std::string(itemLabels[i]) + "=" +
                std::to_string(count);
        }

        if (complete)
        {
            Logger::Success(
                "18L-AH HERO LOADOUT COMPLETE: stock Hunter grant path | " +
                counts + " | no refill after consumption");
        }
        else
        {
            Logger::Error(
                "18L-AH HERO LOADOUT INCOMPLETE: " + counts);
        }
        return complete;
    }

    static bool SetLifecycleActiveCharacter(
        UObject* playerState,
        const uint8_t* borrowedSoftClass)
    {
        if (!playerState ||
            !borrowedSoftClass ||
            !Memory::IsReadable(playerState, sizeof(UObject)) ||
            !Memory::IsReadable(borrowedSoftClass, LifecycleSoftClassSize))
        {
            return false;
        }

        using CopySoftClassFn = void* (__fastcall*)(void*, const void*);
        using SetActiveFn = void(__fastcall*)(UObject*, void*);

        CopySoftClassFn copy = reinterpret_cast<CopySoftClassFn>(
            ShippingAddress(RVA_LifecycleCopySoftClass));
        SetActiveFn setActive = reinterpret_cast<SetActiveFn>(
            ShippingAddress(RVA_LifecycleSetActiveCharacter));

        if (!copy || !setActive)
            return false;

        alignas(16) uint8_t ownedSoftClass[LifecycleSoftClassSize]{};
        copy(ownedSoftClass, borrowedSoftClass);
        setActive(playerState, ownedSoftClass);
        return true;
    }

    static void ApplyLifecycleKillerCosmetics(
        UObject* playerState,
        const uint8_t* borrowedSoftClass)
    {
        if (!playerState || !borrowedSoftClass)
            return;

        using CosmeticRpcFn = void(__fastcall*)(UObject*, const void*);
        CosmeticRpcFn grabKills = reinterpret_cast<CosmeticRpcFn>(
            ShippingAddress(0x4DEF90));
        CosmeticRpcFn weapon = reinterpret_cast<CosmeticRpcFn>(
            ShippingAddress(0x4DF0B0));

        if (grabKills)
            grabKills(playerState, borrowedSoftClass);
        if (weapon)
            weapon(playerState, borrowedSoftClass);
    }

    static bool CopyLifecycleKillerPresentation(
        UObject* sourcePlayerState,
        UObject* destinationPlayerState,
        UClass* selectedKillerClass,
        bool useSelectedDefaults = false)
    {
        if (!sourcePlayerState ||
            !destinationPlayerState ||
            !Memory::IsReadable(sourcePlayerState, 0x7E0) ||
            !Memory::IsReadable(destinationPlayerState, 0x7E0))
        {
            return false;
        }

        // SCPlayerState layout and setters verified in this Resurrected EXE:
        //   +0x7A0 PickedKillerSkin (SCJasonSkin class)
        //   +0x7B8 PickedKillerWeapon (40-byte TSoftClassPtr<SCWeapon>)
        // PlayOutro requires both values on GameState.KillerPlayerState.  An
        // AI PlayerState cannot answer CLIENT_RequestLoadPlayerSettings, so
        // transferring ownership before these fields are complete leaves the
        // game in PostMatchOutro forever.
        UClass* skin = useSelectedDefaults ? nullptr : *reinterpret_cast<UClass**>(
            reinterpret_cast<uintptr_t>(sourcePlayerState) + 0x7A0);
        const uint8_t* weapon = reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(sourcePlayerState) + 0x7B8);
        std::string weaponPath = useSelectedDefaults ? std::string{} : ReadSoftClassAssetPath(weapon);
#if defined(F13_BASE_GAME_PORT)
        // Offline profiles may have no loadout inventory. Use the selected
        // killer's authored default weapon, exactly as the stock helper does,
        // instead of issuing a client/backend request to an AI PlayerState.
        // SCJasonSkin is the stock no-override skin (not a fabricated object).
        if (!skin)
            skin = FindClassExact("SCJasonSkin");
        if ((weaponPath.empty() || weaponPath == "None") && selectedKillerClass &&
            ClassDerivesFrom(selectedKillerClass, "SCKillerCharacter") &&
            selectedKillerClass->DefaultObject &&
            Memory::IsReadable(selectedKillerClass->DefaultObject, 0x13A0))
        {
            static const uint8_t defaultWeaponLoad[]{0x48,0x81,0xC2,0x78,0x13,0x00,0x00};
            if (MatchesBytes(ShippingAddress(0x3EEE46), defaultWeaponLoad, sizeof(defaultWeaponLoad)))
            {
                weapon = reinterpret_cast<const uint8_t*>(selectedKillerClass->DefaultObject) + 0x1378;
                weaponPath = ReadSoftClassAssetPath(weapon);
            }
        }
#endif

        // FName/string soft paths serialize an unset weapon as "None".
        // Treat that exactly like an empty path.  Transferring killer
        // ownership to an AI PlayerState with weapon=None prevents the stock
        // CLIENT_RequestLoadPlayerSettings retry and strands ordinary escape
        // endings in PostMatchOutro.
        const bool weaponSelected =
            !weaponPath.empty() && weaponPath != "None";
        if (!skin ||
            !Memory::IsReadable(skin, sizeof(UClass)) ||
            !weaponSelected)
        {
            Logger::Error(
                "18L-AK OUTRO METADATA: human intro owner has no selected Jason skin/weapon to copy");
            return false;
        }

        using SetPickedKillerSkinFn =
            void(__fastcall*)(UObject*, UClass*);
        using SetPickedKillerWeaponFn =
            void(__fastcall*)(UObject*, const void*);

        SetPickedKillerSkinFn setSkin =
            reinterpret_cast<SetPickedKillerSkinFn>(
#if defined(F13_BASE_GAME_PORT)
                ShippingAddress(0x3D2C70));
#else
                ShippingAddress(0x3E8FE0));
#endif
        SetPickedKillerWeaponFn setWeapon =
            reinterpret_cast<SetPickedKillerWeaponFn>(
#if defined(F13_BASE_GAME_PORT)
                ShippingAddress(0x3D2D10));
        // Stock setters: skin writes +0x7A0; weapon deep-copies +0x7B8
        // and calls SCWorldSettings::SettingsLoaded. Never call Res RVAs.
        static const uint8_t skinSig[]{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x18};
        static const uint8_t weaponSig[]{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x40};
        if (!MatchesBytes(reinterpret_cast<uintptr_t>(setSkin), skinSig, sizeof(skinSig)) ||
            !MatchesBytes(reinterpret_cast<uintptr_t>(setWeapon), weaponSig, sizeof(weaponSig)))
            return false;
#else
                ShippingAddress(0x3E9100));
#endif
        if (!setSkin ||
            !setWeapon ||
            !Memory::IsReadable(reinterpret_cast<void*>(setSkin), 1) ||
            !Memory::IsReadable(reinterpret_cast<void*>(setWeapon), 1))
        {
            Logger::Error(
                "18L-AK OUTRO METADATA: native Jason skin/weapon setters are unavailable");
            return false;
        }

        // Skin must be committed first.  The weapon setter deep-copies its
        // soft path and notifies WorldSettings::SettingsLoaded, which is also
        // the stock retry path used by PlayOutro.
        setSkin(destinationPlayerState, skin);
        setWeapon(destinationPlayerState, weapon);

        UClass* committedSkin = *reinterpret_cast<UClass**>(
            reinterpret_cast<uintptr_t>(destinationPlayerState) + 0x7A0);
        const uint8_t* committedWeapon = reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(destinationPlayerState) + 0x7B8);
        const std::string committedWeaponPath =
            ReadSoftClassAssetPath(committedWeapon);
        const bool complete =
            committedSkin == skin &&
            committedWeaponPath == weaponPath &&
            !committedWeaponPath.empty() &&
            committedWeaponPath != "None";

        if (complete)
        {
            Logger::Success(
                "18L-AK OUTRO METADATA READY: AI Jason PlayerState skin=" +
                SafeName(reinterpret_cast<UObject*>(skin)) +
                " | weapon=" + weaponPath);
        }
        else
        {
            Logger::Error(
                "18L-AK OUTRO METADATA INCOMPLETE: refusing an AI killer-owner handoff that would stall PostMatchOutro");
        }
        return complete;
    }

    static bool DeepAssignLifecycleSoftClass(
        uint8_t* destination,
        const uint8_t* source)
    {
        if (!destination ||
            !source ||
            !Memory::IsReadable(destination, LifecycleSoftClassSize) ||
            !Memory::IsReadable(source, LifecycleSoftClassSize))
        {
            return false;
        }

        using AssignSoftPathFn = void(__fastcall*)(void*, const void*);
        AssignSoftPathFn assignPath = reinterpret_cast<AssignSoftPathFn>(
            ShippingAddress(
#if defined(F13_BASE_GAME_PORT)
                0x76AC90
#else
                0x78DCA0
#endif
            ));
        if (!assignPath)
            return false;

        std::memcpy(destination, source, 12);
        assignPath(destination + 0x10, source + 0x10);
        return true;
    }

    static AActor* SpawnLifecycleActor(
        UWorld* world,
        UClass* actorClass,
        const FVector* location,
        const FRotator* rotation,
        UObject* owner,
        uint8_t collisionMode)
    {
        if (!world || !actorClass)
            return nullptr;

        using SpawnParamsCtorFn = void* (__fastcall*)(void*);
        using SpawnActorFn = AActor* (__fastcall*)(
            UWorld*, UClass*, const FVector*, const FRotator*, const void*);

        SpawnParamsCtorFn construct = reinterpret_cast<SpawnParamsCtorFn>(
            ShippingAddress(RVA_LifecycleSpawnParamsCtor));
        SpawnActorFn spawn = reinterpret_cast<SpawnActorFn>(
            ShippingAddress(RVA_LifecycleSpawnActor));
        if (!construct || !spawn)
            return nullptr;

        alignas(16) uint8_t parameters[0x30]{};
        construct(parameters);
        *reinterpret_cast<UObject**>(parameters + 0x10) = owner;
        parameters[0x28] = collisionMode;

        return spawn(world, actorClass, location, rotation, parameters);
    }

    static UObject* GetControllerPlayerState(UObject* controller)
    {
        if (!controller || !Memory::IsReadable(controller, 0x390))
            return nullptr;
        return *reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(controller) + 0x388);
    }

    static bool RestartLifecyclePlayer(UObject* gameMode, UObject* controller)
    {
        using RestartFn = void(__fastcall*)(UObject*, UObject*);
        RestartFn restart = reinterpret_cast<RestartFn>(
            ShippingAddress(RVA_LifecycleRestartPlayer));
        if (!restart || !gameMode || !controller)
            return false;
        restart(gameMode, controller);
        return true;
    }

    static bool SpawnNativeCounselorBots(
        UObject* gameMode,
        UClass* humanCounselorClass)
    {
        using ClassGetterFn = UClass* (__fastcall*)();
        // Native returns readiness. This offline compatibility call retains
        // its existing behavior; hosted code must check the bool result.
        using HasFullyTraveledFn = bool(__fastcall*)(UObject*, bool);

        ClassGetterFn controllerClassGetter =
            reinterpret_cast<ClassGetterFn>(
                ShippingAddress(RVA_LifecycleCounselorControllerClass));
        HasFullyTraveledFn fullyTraveled =
            reinterpret_cast<HasFullyTraveledFn>(
                ShippingAddress(RVA_LifecycleHasFullyTraveled));

        UClass* controllerClass = controllerClassGetter
            ? controllerClassGetter()
            : nullptr;
        UWorld* world = Engine::GetWorld();
        const uintptr_t counselorArrayOffset = GetLifecycleArrayOffset(
            gameMode,
            "CounselorCharacterClasses",
            0x510);
        const int32_t classCount =
            GetLifecycleSoftClassCount(gameMode, counselorArrayOffset);
        const int32_t requested =
            GetRequestedCounselorBotCount();

        if (!world || !controllerClass || classCount <= 0)
            return false;

        int32_t created = 0;
        const int32_t start = static_cast<int32_t>(GetTickCount64() % classCount);

        for (int32_t candidate = 0;
            candidate < classCount && created < requested;
            ++candidate)
        {
            const int32_t index = (start + candidate) % classCount;
            const uint8_t* softClass =
                GetLifecycleSoftClass(gameMode, counselorArrayOffset, index);
            UClass* counselorClass = LoadLifecycleSoftClass(softClass);

            if (!counselorClass ||
                counselorClass == humanCounselorClass ||
                !ClassDerivesFrom(counselorClass, "SCCounselorCharacter"))
            {
                continue;
            }

            AActor* aiController = SpawnLifecycleActor(
                world,
                controllerClass,
                nullptr,
                nullptr,
                gameMode,
                1);
            UObject* playerState = GetControllerPlayerState(aiController);

            if (!aiController ||
                !playerState ||
                !SetLifecycleActiveCharacter(playerState, softClass) ||
                !RestartLifecyclePlayer(gameMode, aiController))
            {
                Logger::Error(
                    "18L-AD PREMATCH: native counselor bot creation failed at roster index " +
                    std::to_string(index));
                continue;
            }

            if (fullyTraveled)
                (void)fullyTraveled(playerState, true);

            int32_t* numBots = reinterpret_cast<int32_t*>(
                reinterpret_cast<uintptr_t>(gameMode) + 0x40C);
            if (Memory::IsReadable(numBots, sizeof(int32_t)))
                ++(*numBots);

            ++created;
            Logger::Success(
                "18L-AD PREMATCH: native counselor bot born | class=" +
                SafeName(reinterpret_cast<UObject*>(counselorClass)) +
                " | count=" + std::to_string(created) + "/" +
                std::to_string(requested));
        }

        g_CounselorBotsCreated.store(created);
        return created == requested;
    }

    static bool GetActorStartTransform(
        AActor* actor,
        FVector& location,
        FRotator& rotation)
    {
        if (!actor || !actor->Class)
            return false;

        UFunction* getLocation = FindFunctionInHierarchyByName(
            actor->Class, "K2_GetActorLocation");
        UFunction* getRotation = FindFunctionInHierarchyByName(
            actor->Class, "K2_GetActorRotation");
        if (!getLocation || !getRotation)
            return false;

        struct LocationParams { FVector ReturnValue; } locationParams{};
        struct RotationParams { FRotator ReturnValue; } rotationParams{};

        if (!SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(actor),
                actor,
                getLocation,
                &locationParams) ||
            !SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(actor),
                actor,
                getRotation,
                &rotationParams))
        {
            return false;
        }

        location = locationParams.ReturnValue;
        rotation = rotationParams.ReturnValue;
        return true;
    }

    static AActor* FindKillerPlayerStart(UWorld* world)
    {
        if (!world)
            return nullptr;

        constexpr uintptr_t Offset_Levels = 0x110;
        TArray<ULevel*>* levels = reinterpret_cast<TArray<ULevel*>*>(
            reinterpret_cast<uintptr_t>(world) + Offset_Levels);

        if (!Memory::IsReadable(levels, sizeof(TArray<ULevel*>)) ||
            !levels->Data ||
            levels->Count <= 0 ||
            levels->Count > 1024 ||
            !Memory::IsReadable(
                levels->Data,
                sizeof(ULevel*) * static_cast<size_t>(levels->Count)))
        {
            return nullptr;
        }

        for (int32_t levelIndex = 0; levelIndex < levels->Count; ++levelIndex)
        {
            ULevel* level = levels->Data[levelIndex];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
                continue;

            TArray<AActor*>& actors = level->Actors;
            if (!actors.Data ||
                actors.Count <= 0 ||
                actors.Count > 100000 ||
                !Memory::IsReadable(
                    actors.Data,
                    sizeof(AActor*) * static_cast<size_t>(actors.Count)))
            {
                continue;
            }

            for (int32_t actorIndex = 0; actorIndex < actors.Count; ++actorIndex)
            {
                AActor* actor = actors.Data[actorIndex];
                if (actor &&
                    Memory::IsReadable(actor, sizeof(UObject)) &&
                    actor->Class &&
                    ClassDerivesFrom(actor->Class, "SCKillerPlayerStart"))
                {
                    return actor;
                }
            }
        }
        return nullptr;
    }

    static AActor* SpawnKillerControllerWithStockCrowdBypass(
        UWorld* world,
        UClass* controllerClass)
    {
        uint8_t* crowdCall = reinterpret_cast<uint8_t*>(
            ShippingAddress(RVA_LifecycleCrowdFollowerCtorCall));
#if defined(F13_BASE_GAME_PORT)
        const uint8_t expected[5] = { 0xE8,0x0B,0x78,0xF6,0xFF };
#else
        const uint8_t expected[5] = { 0xE8,0xEB,0x58,0xF9,0xFF };
#endif

        if (!MatchesBytes(
                reinterpret_cast<uintptr_t>(crowdCall),
                expected,
                sizeof(expected)))
        {
            Logger::Error(
                "18L-AD AI Jason: SCCrowdFollowing constructor signature mismatch");
            return nullptr;
        }

        DWORD oldProtection = 0;
        if (!VirtualProtect(
                crowdCall,
                sizeof(expected),
                PAGE_EXECUTE_READWRITE,
                &oldProtection))
        {
            return nullptr;
        }

        std::memset(crowdCall, 0x90, sizeof(expected));
        FlushInstructionCache(GetCurrentProcess(), crowdCall, sizeof(expected));

        FVector zero{};
        FRotator zeroRotation{};
        AActor* controller = SpawnLifecycleActor(
            world,
            controllerClass,
            &zero,
            &zeroRotation,
            nullptr,
            2);

        std::memcpy(crowdCall, expected, sizeof(expected));
        FlushInstructionCache(GetCurrentProcess(), crowdCall, sizeof(expected));
        DWORD unusedProtection = 0;
        VirtualProtect(
            crowdCall,
            sizeof(expected),
            oldProtection,
            &unusedProtection);

        return controller;
    }
}

namespace
{
    static void* RuntimeScalarAddress(UObject* object, const char* name,
        const char* propertyType, size_t bytes)
    {
        if (!object || !Memory::IsReadable(object, sizeof(UObject)) || !object->Class)
            return nullptr;
        UPropertyLite* property = FindPropertyInHierarchyByName(object->Class, name);
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 || property->ElementSize != bytes ||
            SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) != propertyType)
            return nullptr;
        void* address = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(object) +
            property->Offset_Internal);
        return Memory::IsReadable(address, bytes) ? address : nullptr;
    }


    // First online-port stage is deliberately read-only. The donor's adoption
    // API enables offline solo rules; do not call it on a replicated Hunt yet.
    // F8 captures exactly one game-thread snapshot, with no worker registry scan.
    static void AuditPrivateLobbyOnGameThread()
    {
        UWorld* world = Engine::GetWorld();
        if (!world || !Memory::IsReadable(world, 0x100))
        {
            Logger::Error("PRIVATE LOBBY AUDIT: no current world | NOT ARMED");
            return;
        }
        UObject* mode = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF0);
        UObject* state = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF8);
        const bool host = mode && state && Memory::IsReadable(mode, sizeof(UObject)) &&
            Memory::IsReadable(state, sizeof(UObject)) && mode->Class && state->Class &&
            Memory::IsReadable(mode->Class, sizeof(UClass)) && Memory::IsReadable(state->Class, sizeof(UClass));
        if (!host)
        {
            Logger::Error("PRIVATE LOBBY AUDIT: authoritative GameMode/GameState unavailable; client rejected | NOT ARMED");
            return;
        }
        const std::string modeName = SafeName(reinterpret_cast<UObject*>(mode->Class));
        const bool hunt = ClassDerivesFrom(mode->Class, "SCGameMode_Hunt") &&
            modeName.find("OfflineBots") == std::string::npos && modeName.find("Sandbox") == std::string::npos;
        // Stock repeatedly checks this FURL option through FURL::HasOption.
        // This confirms private intent independently from mere host authority.
        const uint8_t optionSig[]{0x48,0x83,0xEC,0x28,0x45,0x33,0xC0,0xE8,0x74,0xE9,0xFF,0xFF,
            0x48,0x85,0xC0,0x0F,0x95,0xC0,0x48,0x83,0xC4,0x28,0xC3};
        const uintptr_t optionEntry = ShippingAddress(0x17BFBF0);
        int privateOption = -1;
        if (MatchesBytes(optionEntry, optionSig, sizeof(optionSig)) && Memory::IsReadable(world, 0x730))
        {
            using HasOptionFn = bool(__fastcall*)(const void*, const wchar_t*);
            privateOption = reinterpret_cast<HasOptionFn>(optionEntry)(
                reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(world) + 0x6D0),
                L"?bIsPrivateMatch") ? 1 : 0;
        }
        std::string phase = "<unknown>";
        const FName* matchState = reinterpret_cast<const FName*>(
            RuntimeScalarAddress(state, "MatchState", "NameProperty", sizeof(FName)));
        if (matchState && GNames && GNames->IsValidIndex(matchState->ComparisonIndex))
        {
            const FNameEntry* entry = GNames->GetById(matchState->ComparisonIndex);
            if (entry && Memory::IsReadable(entry, sizeof(FNameEntry)) &&
                memchr(entry->AnsiName, 0, sizeof(entry->AnsiName)))
                phase = entry->AnsiName;
        }
        int32_t players = -1;
        RawArray* playerArray = reinterpret_cast<RawArray*>(
            RuntimeScalarAddress(state, "PlayerArray", "ArrayProperty", sizeof(RawArray)));
        if (playerArray && playerArray->Count >= 0 && playerArray->Count <= 64 &&
            playerArray->Max >= playerArray->Count && playerArray->Max <= 128)
            players = playerArray->Count;
        const uintptr_t counselorOffset = GetLifecycleArrayOffset(mode, "CounselorCharacterClasses", 0x510);
        const uintptr_t killerOffset = GetLifecycleArrayOffset(mode, "KillerCharacterClasses", 0x6B0);
        // These are evidence gates, not permission to mutate. In particular,
        // an address/prefix match cannot prove transactional rollback of Hunt
        // RestartPlayer, Logout, role counters, or peer replication.
        UFunction* restart = FindFunctionInHierarchyByName(mode->Class, "RestartPlayer");
        UObject** killerOwner = reinterpret_cast<UObject**>(RuntimeScalarAddress(
            state, "CurrentKillerPlayerState", "ObjectProperty", sizeof(UObject*)));
        UObject** currentKiller = reinterpret_cast<UObject**>(RuntimeScalarAddress(
            state, "CurrentKiller", "ObjectProperty", sizeof(UObject*)));
        int32_t* numBots = reinterpret_cast<int32_t*>(RuntimeScalarAddress(
            mode, "NumBots", "IntProperty", sizeof(int32_t)));
        const uint8_t destroySig[]{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,
            0x24,0x18,0x57,0x48,0x83,0xEC,0x40,0xF6,0x81,0x3C,0x01,0x00,0x00,0x04};
        const uint8_t travelSig[]{0x40,0x53,0x48,0x83,0xEC,0x20,0x8B,0x81,
            0x94,0x03,0x00,0x00,0x44,0x0F,0xB6,0xC2,0x48,0x8B,0xD9};
        const bool destroyMapped = MatchesBytes(ShippingAddress(0x11E1180),
            destroySig, sizeof(destroySig));
        const bool travelMapped = MatchesBytes(ShippingAddress(RVA_LifecycleHasFullyTraveled),
            travelSig, sizeof(travelSig));
        bool ownerRegistered = false;
        if (players > 0 && playerArray->Data &&
            Memory::IsReadable(playerArray->Data, sizeof(UObject*) * players) && killerOwner)
        {
            UObject** entries = reinterpret_cast<UObject**>(playerArray->Data);
            for (int32_t i = 0; i < players; ++i)
                if (*killerOwner && entries[i] == *killerOwner)
                    ownerRegistered = true;
        }
        UObject* killerController = nullptr;
        if (currentKiller && *currentKiller && Memory::IsReadable(*currentKiller, 0x3A8))
            killerController = *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(*currentKiller) + 0x3A0);
        const bool killerControllerOwnerMatches = killerOwner && *killerOwner &&
            GetControllerPlayerState(killerController) == *killerOwner;
        const bool privateMatchBoundary = hunt && privateOption == 1 && phase == "InProgress";
        Logger::Success("PRIVATE LOBBY AUDIT: host=Y | mode=" + modeName +
            " | hunt=" + (hunt ? "Y" : "N") + " | privateURL=" + std::to_string(privateOption) +
            " | phase=" + phase + " | registeredPlayers=" + std::to_string(players) +
            " | counselorClasses=" + std::to_string(counselorOffset ? GetLifecycleSoftClassCount(mode, counselorOffset) : 0) +
            " | killerClasses=" + std::to_string(killerOffset ? GetLifecycleSoftClassCount(mode, killerOffset) : 0) +
            " | lifecycleSignatures=" + (ValidateCounselorLifecycleNativeSurface() ? "OK" : "FAIL") +
            " | NOT ARMED; no pawn/PlayerState/AI ownership changed");
        Logger::Debug(std::string("HOSTED READINESS: privateHostInProgress=") +
            (privateMatchBoundary ? "Y" : "N") +
            " | reflectedRestartPlayer=" + (restart ? "Y" : "N") +
            " | killerOwnerField=" + (killerOwner && *killerOwner ? "Y" : "N") +
            " | currentKillerField=" + (currentKiller && *currentKiller ? "Y" : "N") +
            " | killerOwnerRegistered=" + (ownerRegistered ? "Y" : "N") +
            " | killerControllerOwnerMatches=" + (killerControllerOwnerMatches ? "Y" : "N") +
            " | reflectedNumBots=" + (numBots ? std::to_string(*numBots) : "unavailable") +
            " | nativeDestroySignature=" + (destroyMapped ? "OK" : "FAIL") +
            " | travelQuerySignature=" + (travelMapped ? "OK" : "FAIL") +
            " | auditOnly=Y; hosted transaction validates its own activation and ownership gates");
    }

    struct HostedCounselorTransaction
    {
        UWorld* World = nullptr;
        UObject* Mode = nullptr;
        UObject* State = nullptr;
        ULONGLONG InProgressAt = 0;
        UObject* Controller = nullptr;
        UObject* PlayerState = nullptr;
        AActor* Pawn = nullptr;
        UObject* Killer = nullptr;
        UObject* KillerOwner = nullptr;
        int32_t BeforeBots = 0;
        int32_t BeforePlayers = 0;
        ULONGLONG Deadline = 0;
        bool BotCountAccounted = false;
        bool AdditionalJason = false;
        bool Locked = false;
    };
    static HostedCounselorTransaction g_HostedCounselor;
    static ULONGLONG g_HostedIntentUntil = 0;
    static ULONGLONG g_HostedAutoNextAttempt = 0;
    static int32_t g_HostedAutoAttempts = 0;
    static bool g_HostedIntentEnteredHunt = false;

    // Authority is the server-only GameMode presence. These exact reflected
    // classes distinguish a lobby/loading phase from frontend/offline worlds.
    static int HostedWorldKind(UWorld* world, UObject* mode, UObject* state)
    {
        if (!world || !mode || !state || !Memory::IsReadable(mode, sizeof(UObject)) ||
            !Memory::IsReadable(state, sizeof(UObject))) return 0; // transient/client
        if (ClassDerivesFrom(mode->Class, "SCGameMode_OfflineBots") ||
            ClassDerivesFrom(mode->Class, "SCGameMode_Sandbox")) return -1;
        if (ClassDerivesFrom(mode->Class, "SCGame_Lobby") ||
            ClassDerivesFrom(mode->Class, "SCGameMode_Lobby")) return 1;
        if (ClassDerivesFrom(mode->Class, "SCGameMode_Hunt")) return 2;
        return -1; // confirmed unsupported authoritative frontend/mode
    }

    static bool HostedActorInWorld(UObject* actor, UWorld* world)
    {
        for (int i = 0; actor && i < 8; ++i)
        {
            if (actor == reinterpret_cast<UObject*>(world)) return true;
            if (!Memory::IsReadable(actor, sizeof(UObject))) return false;
            actor = reinterpret_cast<UObject*>(actor->OuterPrivate);
        }
        return false;
    }

    static bool HostedRoster(UObject* state, UObject* wanted, int32_t& count)
    {
        auto* roster = reinterpret_cast<RawArray*>(RuntimeScalarAddress(
            state, "PlayerArray", "ArrayProperty", sizeof(RawArray)));
        count = -1;
        if (!roster || roster->Count < 0 || roster->Count > 64 ||
            roster->Max < roster->Count || roster->Max > 128 ||
            (roster->Count && (!roster->Data || !Memory::IsReadable(
                roster->Data, sizeof(UObject*) * roster->Count)))) return false;
        count = roster->Count;
        if (!wanted) return true;
        auto** entries = reinterpret_cast<UObject**>(roster->Data);
        int matches = 0;
        for (int32_t i = 0; i < count; ++i) if (entries[i] == wanted) ++matches;
        return matches == 1;
    }

    using HostedRoleSelectionFn = void(__fastcall*)(UObject*);
    static HostedRoleSelectionFn g_HostedRoleSelectionOriginal = nullptr;
    static bool g_HostedRoleSelectionHookReady = false;

    static void __fastcall HostedRoleSelectionBridge(UObject* mode)
    {
        // Stock Hunt chooses Jason here, before the intro and before any
        // controller owns a Jason pawn. This one-shot preference adjustment
        // keeps the listen host a native counselor; the later host-pawn
        // conversion repeatedly left that controller without usable input.
        if (g_HostedJasonIntent.load() && mode &&
            Memory::IsReadable(mode, 0x3C8) && mode->Class &&
            ClassDerivesFrom(mode->Class, "SCGameMode_Hunt"))
        {
            UWorld* world = Engine::GetWorld();
            UObject* state = world && Memory::IsReadable(world, 0x148)
                ? *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(world) + 0xF8) : nullptr;
            UObject* actualMode = world && Memory::IsReadable(world, 0xF8)
                ? *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(world) + 0xF0) : nullptr;
            UObject* local = reinterpret_cast<UObject*>(
                Engine::GetLocalPlayerController());
            UObject* hostState = local ? GetControllerPlayerState(local) : nullptr;
            auto* roster = reinterpret_cast<RawArray*>(RuntimeScalarAddress(
                state, "PlayerArray", "ArrayProperty", sizeof(RawArray)));
            UObject* remoteState = nullptr;
            if (actualMode == mode && state &&
                *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(mode) + 0x3C0) == state &&
                hostState && roster && roster->Count >= 2 &&
                roster->Count <= 8 && roster->Max >= roster->Count &&
                roster->Max <= 128 && roster->Data &&
                Memory::IsReadable(roster->Data,
                    sizeof(UObject*) * roster->Count))
            {
                auto** players = reinterpret_cast<UObject**>(roster->Data);
                bool hostFound = false;
                for (int32_t index = 0; index < roster->Count; ++index)
                {
                    UObject* candidate = players[index];
                    if (!candidate || !Memory::IsReadable(candidate, 0x869) ||
                        !candidate->Class ||
                        !ClassDerivesFrom(candidate->Class, "SCPlayerState_Hunt") ||
                        (*reinterpret_cast<uint8_t*>(
                            reinterpret_cast<uintptr_t>(candidate) + 0x394) & 4))
                        continue;
                    if (candidate == hostState) hostFound = true;
                    else if (!remoteState) remoteState = candidate;
                }
                UObject* instance = *reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(world) + 0x140);
                auto* priorName = instance && Memory::IsReadable(instance, 0x314)
                    ? reinterpret_cast<uint8_t*>(instance) + 0x2F0 : nullptr;
                int32_t priorLength = priorName
                    ? *reinterpret_cast<int32_t*>(priorName + 8) : -1;
                int32_t priorCapacity = priorName
                    ? *reinterpret_cast<int32_t*>(priorName + 12) : -1;
                void* priorData = priorName
                    ? *reinterpret_cast<void**>(priorName) : nullptr;
                const uint8_t oldHost = hostFound
                    ? *reinterpret_cast<uint8_t*>(
                        reinterpret_cast<uintptr_t>(hostState) + 0x868) : 255;
                const uint8_t oldRemote = remoteState
                    ? *reinterpret_cast<uint8_t*>(
                        reinterpret_cast<uintptr_t>(remoteState) + 0x868) : 255;
                if (hostFound && remoteState && oldHost <= 2 &&
                    oldRemote <= 2 && priorName &&
                    priorLength >= 0 && priorLength <= 256 &&
                    priorCapacity >= priorLength && priorCapacity <= 512 &&
                    (priorLength == 0 || (priorData &&
                        Memory::IsReadable(priorData,
                            sizeof(wchar_t) * priorLength))))
                {
                    *reinterpret_cast<uint8_t*>(
                        reinterpret_cast<uintptr_t>(hostState) + 0x868) = 1;
                    *reinterpret_cast<uint8_t*>(
                        reinterpret_cast<uintptr_t>(remoteState) + 0x868) = 2;
                    // The native prior-Jason exclusion can discard the only
                    // remote candidate in a two-player lobby. Skip it for
                    // this F8 match. Native selection publishes its new
                    // prior identity; restoring the old FString count after
                    // that copy would corrupt the freshly written string.
                    *reinterpret_cast<int32_t*>(priorName + 8) = 0;
                    Logger::Success("HOSTED F8 role selection: native host Counselor / remote Killer preference set before lottery | oldHost=" +
                        std::to_string(oldHost) + " | oldRemote=" +
                        std::to_string(oldRemote) + " | priorLength=" +
                        std::to_string(priorLength));
                }
                else Logger::Error("HOSTED F8 role selection: guarded host/remote/predecessor preflight failed; stock lottery retained");
            }
            else Logger::Error("HOSTED F8 role selection: Hunt roster unavailable before lottery; stock lottery retained");
        }
        if (g_HostedRoleSelectionOriginal)
            g_HostedRoleSelectionOriginal(mode);
    }

    static bool InstallHostedRoleSelectionBridge()
    {
        if (g_HostedRoleSelectionHookReady) return true;
        const uintptr_t entry = ShippingAddress(0x382110);
        static const uint8_t signature[]{
            0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x56,
            0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
        if (!MatchesBytes(entry, signature, sizeof(signature))) return false;
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            return false;
        if (MH_CreateHook(reinterpret_cast<void*>(entry),
                reinterpret_cast<void*>(&HostedRoleSelectionBridge),
                reinterpret_cast<void**>(&g_HostedRoleSelectionOriginal)) != MH_OK)
            return false;
        if (MH_EnableHook(reinterpret_cast<void*>(entry)) != MH_OK)
        {
            MH_RemoveHook(reinterpret_cast<void*>(entry));
            g_HostedRoleSelectionOriginal = nullptr;
            return false;
        }
        g_HostedRoleSelectionHookReady = true;
        Logger::Success("HOSTED F8: stock Hunt preselection hook installed");
        return true;
    }

    static bool HostedBoundary(UWorld*& world, UObject*& mode, UObject*& state,
        bool* confirmedEnding = nullptr)
    {
        if (confirmedEnding) *confirmedEnding = false;
        world = Engine::GetWorld();
        if (!world || !Memory::IsReadable(world, 0x100)) return false;
        mode = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF0);
        state = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF8);
        if (!mode || !state || !Memory::IsReadable(mode, sizeof(UObject)) ||
            !Memory::IsReadable(state, sizeof(UObject)) ||
            !ClassDerivesFrom(mode->Class, "SCGameMode_Hunt") ||
            ClassDerivesFrom(mode->Class, "SCGameMode_OfflineBots") ||
            ClassDerivesFrom(mode->Class, "SCGameMode_Sandbox")) return false;
        auto* phase = reinterpret_cast<FName*>(RuntimeScalarAddress(
            state, "MatchState", "NameProperty", sizeof(FName)));
        if (!phase || !GNames || !GNames->IsValidIndex(phase->ComparisonIndex)) return false;
        const FNameEntry* entry = GNames->GetById(phase->ComparisonIndex);
        if (!entry || !Memory::IsReadable(entry, sizeof(FNameEntry)) ||
            !memchr(entry->AnsiName, 0, sizeof(entry->AnsiName))) return false;
        if (confirmedEnding)
            *confirmedEnding = std::strcmp(entry->AnsiName, "WaitingPostMatchOutro") == 0 ||
                std::strcmp(entry->AnsiName, "PostMatchOutro") == 0 ||
                std::strcmp(entry->AnsiName, "WaitingPostMatch") == 0 ||
                std::strcmp(entry->AnsiName, "LeavingMap") == 0 ||
                std::strcmp(entry->AnsiName, "Aborted") == 0;
        return std::strcmp(entry->AnsiName, "InProgress") == 0;
    }

    static UObject** HostedObjectField(UObject* object, const char* name)
    {
        return reinterpret_cast<UObject**>(RuntimeScalarAddress(
            object, name, "ObjectProperty", sizeof(UObject*)));
    }

    static bool HostedBotBirthReady(UWorld* world, UObject* state,
        UObject* controller, UObject* playerState, AActor* pawn = nullptr)
    {
        // HasFullyTraveled checks a human client acknowledgement before its
        // bot shortcut. Server-born AI has no client to acknowledge its PS.
        // Validate native bot birth/ownership instead, never forge that RPC,
        // a human identity, or the cached travel-ready byte.
        if (!world || !state || !controller || !playerState ||
            !Memory::IsReadable(controller, 0x390) ||
            !Memory::IsReadable(playerState, 0x398) ||
            (controller->ObjectFlags & 0x30) || (playerState->ObjectFlags & 0x30) ||
            !controller->Class || !playerState->Class ||
            !(ClassDerivesFrom(controller->Class, "SCKillerAIController") ||
              ClassDerivesFrom(controller->Class, "SCCounselorAIController")) ||
            !ClassDerivesFrom(playerState->Class, "SCPlayerState") ||
            !HostedActorInWorld(controller, world) || !HostedActorInWorld(playerState, world) ||
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(controller) + 0x110) != 3 ||
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(playerState) + 0x110) != 3 ||
            !(*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(playerState) + 0x394) & 0x04) ||
            GetControllerPlayerState(controller) != playerState) return false;
        UObject** owner = HostedObjectField(playerState, "Owner");
        int32_t players = -1;
        if (!owner || *owner != controller || !HostedRoster(state, playerState, players)) return false;
        if (!pawn) return true; // pre-possession native PlayerState registration
        UObject** pawnController = HostedObjectField(pawn, "Controller");
        UObject** pawnState = HostedObjectField(pawn, "PlayerState");
        return Memory::IsReadable(pawn, 0x3A8) && !(pawn->ObjectFlags & 0x30) &&
            HostedActorInWorld(pawn, world) &&
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pawn) + 0x110) == 3 &&
            *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(controller) + 0x370) == pawn &&
            pawnController && *pawnController == controller && pawnState && *pawnState == playerState;
    }

    static UFunction* HostedSingleObjectFunction(UObject* object, const char* name, const char* parameter)
    {
        if (!object || !Memory::IsReadable(object, sizeof(UObject))) return nullptr;
        UFunction* function = FindFunctionInHierarchyByName(object->Class, name);
        if (!function || !Memory::IsReadable(function, sizeof(UFunction))) return nullptr;
        // Verify the reflected native parameter rather than borrowing the
        // OfflineBots override (which reads an invalid Hunt +0x900 field).
        int params = 0;
        int fields = 0;
        for (UField* field = function->Children; field && params < 2; field = field->Next)
        {
            if (++fields > 16) return nullptr;
            if (!Memory::IsReadable(field, sizeof(UPropertyLite))) return nullptr;
            auto* property = reinterpret_cast<UPropertyLite*>(field);
            if (!(property->PropertyFlags & 0x80)) continue;
            if (field->GetName() != parameter || property->Offset_Internal != 0 ||
                property->ElementSize != sizeof(UObject*) ||
                SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) != "ObjectProperty") return nullptr;
            ++params;
        }
        return params == 1 ? function : nullptr;
    }

    static UFunction* HostedRestartFunction(UObject* mode)
    {
        return HostedSingleObjectFunction(mode, "RestartPlayer", "NewPlayer");
    }

    struct HostedCounselorViewHandoff
    {
        UWorld* World = nullptr;
        UObject* Controller = nullptr;
        UObject* PlayerState = nullptr;
        AActor* Pawn = nullptr;
        ULONGLONG Deadline = 0;
        ULONGLONG RetryAt = 0;
        ULONGLONG LateRepairAt = 0;
        ULONGLONG FinalRepairAt = 0;
        bool Retried = false;
        bool LensCleared = false;
        bool InitialPublished = false;
        bool LateRepairSent = false;
        bool FinalRepairSent = false;
    };
    static HostedCounselorViewHandoff g_HostedViewHandoff;
    static UObject* g_HostedInputModeLibrary = nullptr;

    static UFunction* HostedClientRestartFunction(UObject* controller)
    {
        UFunction* function = HostedSingleObjectFunction(controller, "ClientRestart", "NewPawn");
        return function && reinterpret_cast<uintptr_t>(function->ExecFunction) == ShippingAddress(0x18D9270)
            ? function : nullptr;
    }

    static bool ApplyHostedHighestCounselorSkill(UObject* mode)
    {
        // Native-only SCGameMode difficulty member (not a reflected field).
        // Stock InitGame's Difficulty=Hard assignment and the AI timing
        // consumer independently identify this byte. Restrict the write to
        // the already-authoritative hosted Hunt, and require both signatures.
        const uint8_t hardAssignment[]{ 0x41,0xC6,0x86,0xCC,0x04,0x00,0x00,0x02 };
        const uint8_t timingConsumer[]{ 0x0F,0xB6,0x91,0xCC,0x04,0x00,0x00,0x85,0xD2 };
        if (!mode || mode != g_HostedCounselor.Mode ||
            !ClassDerivesFrom(mode->Class, "SCGameMode_Hunt") ||
            !ClassDerivesFrom(mode->Class, "SCGameMode") ||
            !MatchesBytes(ShippingAddress(0x3862D3), hardAssignment, sizeof(hardAssignment)) ||
            !MatchesBytes(ShippingAddress(0x37CC60), timingConsumer, sizeof(timingConsumer))) return false;
        auto* difficulty = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mode) + 0x4CC);
        if (!Memory::IsReadable(difficulty, 1) || *difficulty > 2) return false;
        *difficulty = 2; // ESCGameModeDifficulty::Hard, not UI index ordering.
        Logger::Success("HOSTED COUNSELOR AI: native game-mode difficulty=Hard (2); all counselor bots use highest stock skill");
        return true;
    }

    static bool ApplyHostedTestLobbyIdleGrace(UObject* mode)
    {
        // Preserve genuine network disconnects. This changes only the
        // game's reflected AFK limit in an F8-armed Hunt, if this packed
        // build exposes a plausible stock value and type.
        if (!mode || !ClassDerivesFrom(mode->Class, "SCGameMode_Hunt"))
            return false;
        UPropertyLite* property = FindPropertyInHierarchyByName(
            mode->Class, "MaxIdleTimeBeforeKick");
        if (!property || property->ArrayDim != 1 ||
            property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000)
            return false;
        uint8_t* value = reinterpret_cast<uint8_t*>(mode) +
            property->Offset_Internal;
        const std::string type = SafeName(
            reinterpret_cast<UObject*>(property->ClassPrivate));
        if (type == "FloatProperty" && property->ElementSize == 4 &&
            Memory::IsReadable(value, sizeof(float)))
        {
            const float previous = *reinterpret_cast<float*>(value);
            if (previous < 1.0f || previous > 3600.0f) return false;
            *reinterpret_cast<float*>(value) = 7200.0f;
            Logger::Success("HOSTED F8 idle grace: stock MaxIdleTimeBeforeKick " +
                std::to_string(previous) + " -> 7200 seconds");
            return true;
        }
        if (type == "IntProperty" && property->ElementSize == 4 &&
            Memory::IsReadable(value, sizeof(int32_t)))
        {
            const int32_t previous = *reinterpret_cast<int32_t*>(value);
            if (previous < 1 || previous > 3600) return false;
            *reinterpret_cast<int32_t*>(value) = 7200;
            Logger::Success("HOSTED F8 idle grace: stock MaxIdleTimeBeforeKick " +
                std::to_string(previous) + " -> 7200 seconds");
            return true;
        }
        return false;
    }

    static bool RefreshHostedCounselorRootHUD(UObject* controller, bool& invoked)
    {
        invoked = false;
        // UI objects exist only on their owning local client. ClientRestart
        // handles the remote owning client; never borrow the host's HUD.
        if (controller != reinterpret_cast<UObject*>(Engine::GetLocalPlayerController())) return true;
        UObject* hud = ReadRuntimeWidgetObject(controller, "MyHUD");
        if (!hud || !ClassDerivesFrom(hud->Class, "SCInGameHUD") ||
            ReadRuntimeWidgetObject(hud, "PlayerOwner") != controller) return false;
        auto** roleClass = reinterpret_cast<UClass**>(RuntimeScalarAddress(
            hud, "CounselorHUDClass", "ClassProperty", sizeof(UClass*)));
        if (!roleClass || !*roleClass || !Memory::IsReadable(*roleClass, sizeof(UClass)) ||
            !ClassDerivesFrom(*roleClass, "SCHUDWidget")) return false;
        UFunction* change = FindFunctionInHierarchyByName(hud->Class, "ChangeRootMenu");
        UFunction* currentMenu = FindFunctionInHierarchyByName(hud->Class, "GetCurrentMenu");
        if (!change || !Memory::IsReadable(change, sizeof(UFunction)) || change->Size != 0x18 ||
            reinterpret_cast<uintptr_t>(change->ExecFunction) != ShippingAddress(0x4A0B50) ||
            !currentMenu || !Memory::IsReadable(currentMenu, sizeof(UFunction)) || currentMenu->Size != 8 ||
            reinterpret_cast<uintptr_t>(currentMenu->ExecFunction) != ShippingAddress(0x4A12F0)) return false;
        UPropertyLite* currentResult = FindPropertyInStructByName(reinterpret_cast<UStruct*>(currentMenu), "ReturnValue");
        if (!currentResult || currentResult->Offset_Internal != 0 || currentResult->ElementSize != 8 ||
            SafeName(reinterpret_cast<UObject*>(currentResult->ClassPrivate)) != "ObjectProperty") return false;
        UPropertyLite* menu = FindPropertyInStructByName(reinterpret_cast<UStruct*>(change), "MenuClass");
        UPropertyLite* force = FindPropertyInStructByName(reinterpret_cast<UStruct*>(change), "bForce");
        UPropertyLite* result = FindPropertyInStructByName(reinterpret_cast<UStruct*>(change), "ReturnValue");
        if (!menu || !force || !result || menu->Offset_Internal != 0 || menu->ElementSize != 8 ||
            SafeName(reinterpret_cast<UObject*>(menu->ClassPrivate)) != "ClassProperty" ||
            force->Offset_Internal != 8 || force->ElementSize != 1 ||
            SafeName(reinterpret_cast<UObject*>(force->ClassPrivate)) != "BoolProperty" ||
            result->Offset_Internal != 0x10 || result->ElementSize != 8 ||
            SafeName(reinterpret_cast<UObject*>(result->ClassPrivate)) != "ObjectProperty") return false;
        // A successful first publication is already the correct root. The
        // old bounded handoff rebuilt it again at both camera repairs; those
        // UI transitions can reclaim controller input after ClientRestart.
        struct CurrentParams { UObject* ReturnValue; } existing{};
        if (SafeProcessEventCall(reinterpret_cast<uintptr_t>(hud), hud,
                currentMenu, &existing) && existing.ReturnValue &&
            Memory::IsReadable(existing.ReturnValue, sizeof(UObject)) &&
            existing.ReturnValue->Class == *roleClass)
        {
            UFunction* ownerFunction = FindFunctionInHierarchyByName(
                existing.ReturnValue->Class, "GetOwningPlayer");
            if (ownerFunction && ownerFunction->Size == sizeof(UObject*))
            {
                CurrentParams owner{};
                if (SafeProcessEventCall(
                        reinterpret_cast<uintptr_t>(existing.ReturnValue),
                        existing.ReturnValue, ownerFunction, &owner) &&
                    owner.ReturnValue == controller)
                {
                    return true;
                }
            }
        }
        struct Params { UClass* MenuClass; uint8_t Force; uint8_t Pad[7]; UObject* ReturnValue; }
            params{ *roleClass, 1, {}, nullptr };
        invoked = true;
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(hud), hud, change, &params) ||
            !params.ReturnValue || !Memory::IsReadable(params.ReturnValue, sizeof(UObject)) ||
            params.ReturnValue->Class != *roleClass) return false;
        UFunction* owning = FindFunctionInHierarchyByName(params.ReturnValue->Class, "GetOwningPlayer");
        UPropertyLite* owningResult = owning ? FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(owning), "ReturnValue") : nullptr;
        if (!owning || !Memory::IsReadable(owning, sizeof(UFunction)) || owning->Size != 8 ||
            !owningResult || owningResult->Offset_Internal != 0 || owningResult->ElementSize != 8 ||
            SafeName(reinterpret_cast<UObject*>(owningResult->ClassPrivate)) != "ObjectProperty") return false;
        struct OwnerParams { UObject* ReturnValue; } ownerParams{};
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(params.ReturnValue), params.ReturnValue,
                owning, &ownerParams) || ownerParams.ReturnValue != controller) return false;
        OwnerParams currentParams{};
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(hud), hud, currentMenu, &currentParams) ||
            currentParams.ReturnValue != params.ReturnValue) return false;
        Logger::Success("HOSTED COUNSELOR VIEW: stock counselor root HUD installed | widget=" + SafeName(params.ReturnValue));
        return true;
    }

    static bool HostedMoveInputIgnored(UObject* controller, bool& ignored)
    {
        ignored = false;
        if (!controller || !Memory::IsReadable(controller, sizeof(UObject))) return false;
        UFunction* query = FindFunctionInHierarchyByName(controller->Class, "IsMoveInputIgnored");
        UPropertyLite* result = query ? FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(query), "ReturnValue") : nullptr;
        if (!query || query->Size != 1 || !result || result->Offset_Internal != 0 ||
            result->ElementSize != 1 ||
            SafeName(reinterpret_cast<UObject*>(result->ClassPrivate)) != "BoolProperty") return false;
        struct Params { uint8_t ReturnValue; } params{};
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(controller), controller, query, &params)) return false;
        ignored = params.ReturnValue != 0;
        return true;
    }

    static UObject* HostedViewTarget(UObject* controller)
    {
        if (!controller || !Memory::IsReadable(controller, sizeof(UObject))) return nullptr;
        UFunction* get = FindFunctionInHierarchyByName(controller->Class, "GetViewTarget");
        UPropertyLite* result = get ? FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(get), "ReturnValue") : nullptr;
        if (!get || get->Size != sizeof(UObject*) || !result ||
            result->Offset_Internal != 0 || result->ElementSize != sizeof(UObject*) ||
            SafeName(reinterpret_cast<UObject*>(result->ClassPrivate)) != "ObjectProperty") return nullptr;
        struct Params { UObject* ReturnValue; } params{};
        return SafeProcessEventCall(reinterpret_cast<uintptr_t>(controller), controller, get, &params)
            ? params.ReturnValue : nullptr;
    }

    static bool HostedRestoreLocalCounselorView(UObject* controller, AActor* counselor)
    {
        if (!controller || !counselor ||
            controller != reinterpret_cast<UObject*>(Engine::GetLocalPlayerController())) return false;
        UFunction* set = FindFunctionInHierarchyByName(controller->Class, "SetViewTargetWithBlend");
        UFunction* clientSet = FindFunctionInHierarchyByName(controller->Class, "ClientSetViewTarget");
        struct Params
        {
            UObject* NewViewTarget;
            float BlendTime;
            uint8_t BlendFunction;
            uint8_t Padding[3];
            float BlendExp;
            bool LockOutgoing;
            uint8_t TailPadding[3];
        } params{};
        static_assert(sizeof(Params) == 0x18, "view-target transition parameter layout");
        params.NewViewTarget = reinterpret_cast<UObject*>(counselor);
        // Both stock entry points take AActor* plus the 16-byte transition
        // struct (0x18 bytes total). The old 0x20 guard rejected every call,
        // leaving the local host looking through Jason's camera even though
        // RestartPlayer had already possessed a counselor.
        bool clientApplied = false;
        bool localApplied = false;
        if (clientSet && clientSet->Size == sizeof(Params))
            clientApplied = SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller), controller, clientSet, &params);
        if (set && set->Size == sizeof(Params))
            localApplied = SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(controller), controller, set, &params);
        Logger::Debug(std::string("HOSTED COUNSELOR VIEW: stock camera bind") +
            " | clientSchema=" + (clientSet ? std::to_string(clientSet->Size) : "missing") +
            " | localSchema=" + (set ? std::to_string(set->Size) : "missing") +
            " | clientApplied=" + (clientApplied ? "Y" : "N") +
            " | localApplied=" + (localApplied ? "Y" : "N") +
            " | targetOnCounselor=" +
            (HostedViewTarget(controller) == reinterpret_cast<UObject*>(counselor) ? "Y" : "N"));
        return clientApplied || localApplied;
    }

    static bool HostedRestoreLocalGameInput(UObject* controller)
    {
        if (controller != reinterpret_cast<UObject*>(
                Engine::GetLocalPlayerController()) ||
            !g_HostedInputModeLibrary ||
            !Memory::IsReadable(g_HostedInputModeLibrary, sizeof(UObject)) ||
            SafeName(reinterpret_cast<UObject*>(
                g_HostedInputModeLibrary->Class)) !=
                    "WidgetBlueprintLibrary") return false;
        UFunction* function = FindFunctionInHierarchyByName(
            g_HostedInputModeLibrary->Class, "SetInputMode_GameOnly");
        if (!function || !Memory::IsReadable(function, sizeof(UFunction)) ||
            (function->Size != 8 && function->Size != 16)) return false;
        bool controllerParam = false;
        int parameterCount = 0;
        alignas(16) uint8_t params[16]{};
        for (UField* field = function->Children, *next = nullptr;
             field; field = next)
        {
            if (++parameterCount > 4 ||
                !Memory::IsReadable(field, sizeof(UPropertyLite))) return false;
            next = field->Next;
            auto* property = reinterpret_cast<UPropertyLite*>(field);
            if (!(property->PropertyFlags & 0x80)) continue;
            const std::string name = field->GetName();
            if (name == "PlayerController" &&
                property->Offset_Internal == 0 &&
                property->ElementSize == sizeof(UObject*) &&
                SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) ==
                    "ObjectProperty")
            {
                *reinterpret_cast<UObject**>(params) = controller;
                controllerParam = true;
            }
            else if (name == "bFlushInput" &&
                property->Offset_Internal == 8 &&
                property->ElementSize == 1 && function->Size >= 9 &&
                SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) ==
                    "BoolProperty")
            {
                params[8] = 1;
            }
            else return false;
        }
        return controllerParam && SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(g_HostedInputModeLibrary),
            g_HostedInputModeLibrary, function, params);
    }

    static void PumpHostedCounselorView(UWorld* world, ULONGLONG now)
    {
        auto& view = g_HostedViewHandoff;
        if (!view.Controller) return;
        if (view.World != world || now >= view.Deadline ||
            !HostedActorInWorld(view.Controller, world) || !HostedActorInWorld(view.Pawn, world) ||
            GetControllerPlayerState(view.Controller) != view.PlayerState ||
            !Memory::IsReadable(view.Controller, 0x3E8) ||
            *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(view.Controller) + 0x370) != view.Pawn)
        {
            Logger::Debug("HOSTED COUNSELOR VIEW: handoff retired on timeout/ownership change; no raw camera or acknowledgement writes");
            view = {};
            return;
        }
        const bool localHost = view.Controller ==
            reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
        const bool firstHostRepair = localHost && view.InitialPublished &&
            !view.LateRepairSent;
        const bool finalHostRepair = localHost && view.InitialPublished &&
            view.LateRepairSent && !view.FinalRepairSent;
        if (firstHostRepair || finalHostRepair)
        {
            if (now < (firstHostRepair ? view.LateRepairAt :
                    view.FinalRepairAt)) return;
            // The Hunt opening cinematic can republish the former Jason
            // camera/input after an immediately acknowledged counselor pawn.
            // The host transaction itself waits until the native opening
            // grace has elapsed. Re-run stock client methods shortly after
            // RestartPlayer, rather than leaving the host on Jason's camera
            // for another 20 seconds.
            if (firstHostRepair) view.LateRepairSent = true;
            else view.FinalRepairSent = true;
            view.Retried = true;
            UObject* cameraBefore = HostedViewTarget(view.Controller);
            bool ignoredBefore = false;
            const bool inputKnown = HostedMoveInputIgnored(view.Controller, ignoredBefore);
            UFunction* restart = HostedClientRestartFunction(view.Controller);
            struct Params { AActor* NewPawn; } params{ view.Pawn };
            const bool restarted = restart && SafeProcessEventCall(
                reinterpret_cast<uintptr_t>(view.Controller), view.Controller, restart, &params);
            // GetViewTarget can correctly report the counselor while its
            // internal camera is still uninitialized. The stock log showed
            // repeated OutdoorCamera fallback on this exact host handoff.
            const bool cameraSeeded = restarted &&
                InitializeCounselorActiveCamera(view.Pawn);
            bool resetInput = false;
            if (inputKnown && ignoredBefore)
            {
                UFunction* reset = FindFunctionInHierarchyByName(
                    view.Controller->Class, "ResetIgnoreInputFlags");
                resetInput = reset && reset->Size == 0 && SafeProcessEventCall(
                    reinterpret_cast<uintptr_t>(view.Controller), view.Controller, reset, nullptr);
            }
            // GetViewTarget can name the counselor while its previous Jason
            // camera manager still owns the rendered POV. Rebind the view
            // once after the native opening grace, even when the reported
            // target is already the counselor.
            const bool restoredCamera = HostedRestoreLocalCounselorView(
                view.Controller, view.Pawn);
            view.LensCleared = false;
            bool ignoredAfter = false;
            const bool afterKnown = HostedMoveInputIgnored(view.Controller, ignoredAfter);
            Logger::Debug(std::string(firstHostRepair
                    ? "HOSTED COUNSELOR VIEW: early native control repair"
                    : "HOSTED COUNSELOR VIEW: final native control repair") +
                " | restart=" + (restarted ? "Y" : "N") +
                " | counselorCameraSeeded=" + (cameraSeeded ? "Y" : "N") +
                " | oldCameraOnCounselor=" + (cameraBefore == reinterpret_cast<UObject*>(view.Pawn) ? "Y" : "N") +
                " | cameraRepair=" + (restoredCamera ? "Y" : "N") +
                " | ignoredMoveBefore=" + (inputKnown ? (ignoredBefore ? "Y" : "N") : "unknown") +
                " | inputReset=" + (resetInput ? "Y" : "N") +
                " | ignoredMoveAfter=" + (afterKnown ? (ignoredAfter ? "Y" : "N") : "unknown"));
        }
        UObject** acknowledged = HostedObjectField(view.Controller, "AcknowledgedPawn");
        if (!acknowledged || *acknowledged != reinterpret_cast<UObject*>(view.Pawn))
        {
            if (!view.Retried && now >= view.RetryAt)
            {
                view.Retried = true;
                UFunction* restart = HostedClientRestartFunction(view.Controller);
                struct Params { AActor* NewPawn; } params{ view.Pawn };
                if (restart) SafeProcessEventCall(reinterpret_cast<uintptr_t>(view.Controller), view.Controller, restart, &params);
            }
            return;
        }
        if (!view.LensCleared)
        {
            UFunction* clear = FindFunctionInHierarchyByName(view.Controller->Class, "ClientClearCameraLensEffects");
            if (!clear || !Memory::IsReadable(clear, sizeof(UFunction)) || clear->Size != 0 ||
                reinterpret_cast<uintptr_t>(clear->ExecFunction) != ShippingAddress(0x18D8020) ||
                !SafeProcessEventCall(reinterpret_cast<uintptr_t>(view.Controller), view.Controller, clear, nullptr)) return;
            view.LensCleared = true;
        }
        bool hudInvoked = false;
        if (!RefreshHostedCounselorRootHUD(view.Controller, hudInvoked))
        {
            if (hudInvoked)
            {
                Logger::Error("HOSTED COUNSELOR VIEW: role-widget publication failed verification; not rebuilding UI repeatedly");
                view = {};
            }
            return;
        }
        if (localHost && !view.InitialPublished)
        {
            view.InitialPublished = true;
            Logger::Debug("HOSTED COUNSELOR VIEW: initial counselor pawn/HUD acknowledged; waiting for post-intro input/camera check");
            return;
        }
        // Keep only this bounded host handoff alive through the tail of the
        // stock intro. It may republish Jason camera/input after the first
        // restart; the final repair is one shot, not a frame-by-frame write.
        if (localHost && !view.FinalRepairSent)
            return;
        if (localHost)
            Logger::Debug(std::string(
                "HOSTED COUNSELOR VIEW: post-intro stock game input mode | applied=") +
                (HostedRestoreLocalGameInput(view.Controller) ? "Y" : "N"));
        Logger::Success(localHost
            ? "HOSTED COUNSELOR VIEW: post-intro counselor control/camera repair sent; confirm input and HUD in play"
            : "HOSTED COUNSELOR VIEW: remote pawn acknowledgement and lens RPC complete; remote role-widget display requires tester verification");
        view = {};
    }

    static bool HostedNativeSurface()
    {
        const uint8_t destroy[]{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
            0x57,0x48,0x83,0xEC,0x40,0xF6,0x81,0x3C,0x01,0x00,0x00,0x04};
        const uint8_t getter[]{0x4C,0x8B,0xDC,0x48,0x83,0xEC,0x78,0x48,0x8B,0x05,0xBA,0x13,0xB1,0x02,0x48,0x85,0xC0};
        const uint8_t travel[]{0x40,0x53,0x48,0x83,0xEC,0x20,0x8B,0x81,0x94,0x03,0x00,0x00,0x44,0x0F,0xB6,0xC2,0x48,0x8B,0xD9};
        const uint8_t tree[]{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57};
        const uint8_t load[]{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9};
        return ValidateCounselorLifecycleNativeSurface() &&
            MatchesBytes(ShippingAddress(0x11E1180), destroy, sizeof(destroy)) &&
            MatchesBytes(ShippingAddress(RVA_LifecycleCounselorControllerClass), getter, sizeof(getter)) &&
            MatchesBytes(ShippingAddress(RVA_LifecycleHasFullyTraveled), travel, sizeof(travel)) &&
            MatchesBytes(ShippingAddress(RVA_LifecycleLoadSoftClass), load, sizeof(load)) &&
            MatchesBytes(ShippingAddress(0x31EED0), tree, sizeof(tree));
    }

    static bool HostedKillerUnchanged()
    {
        auto& tx = g_HostedCounselor;
        UObject** killer = HostedObjectField(tx.State, "CurrentKiller");
        UObject** owner = HostedObjectField(tx.State, "CurrentKillerPlayerState");
        return killer && owner && *killer == tx.Killer && *owner == tx.KillerOwner;
    }

    static void HostedRollback(const char* reason)
    {
        auto& tx = g_HostedCounselor;
        using DestroyFn = bool(__fastcall*)(UObject*, bool, bool);
        const auto destroy = reinterpret_cast<DestroyFn>(ShippingAddress(0x11E1180));
        bool pawnDestroyed = !tx.Pawn;
        bool controllerDestroyed = !tx.Controller;
        // One native destruction per owned actor. Controller destruction owns
        // PlayerState logout/removal; never separately destroy its PlayerState.
        if (tx.Pawn && HostedActorInWorld(tx.Pawn, tx.World))
            pawnDestroyed = destroy(tx.Pawn, false, true);
        if (tx.Controller && HostedActorInWorld(tx.Controller, tx.World))
            controllerDestroyed = destroy(tx.Controller, false, true);
        int32_t count = -1;
        const bool stillRegistered = tx.PlayerState && HostedRoster(tx.State, tx.PlayerState, count);
        const bool rosterValid = HostedRoster(tx.State, nullptr, count);
        auto* bots = reinterpret_cast<int32_t*>(RuntimeScalarAddress(tx.Mode, "NumBots", "IntProperty", sizeof(int32_t)));
        // Hunt Logout does not decrement NumBots for non-PlayerController AI.
        // Remove exactly this transaction's contribution only after native
        // destruction and roster removal are proven. Never overwrite a count
        // changed by a different roster operation.
        if (tx.BotCountAccounted && pawnDestroyed && controllerDestroyed &&
            rosterValid && !stillRegistered && count == tx.BeforePlayers && bots &&
            *bots == tx.BeforeBots + 1)
            --*bots;
        const bool restored = pawnDestroyed && controllerDestroyed && rosterValid &&
            !stillRegistered && count == tx.BeforePlayers && bots && *bots == tx.BeforeBots && HostedKillerUnchanged();
        Logger::Error(std::string(tx.AdditionalJason ? "HOSTED F1 rollback: " : "HOSTED F3 rollback: ") + reason +
            " | verified=" + (restored ? "Y" : "N") + " | spawning locked until next match");
        tx.Controller = nullptr; tx.PlayerState = nullptr; tx.Pawn = nullptr;
        tx.BotCountAccounted = false;
        tx.Locked = true;
        g_HostedJasonIntent.store(false);
        g_HostedF1Pending.store(false);
        g_HostedF3Armed.store(false);
        g_HostedF3Pending.store(false);
        g_HostedF3Busy.store(false);
    }

    static bool TakeOverHostedJason()
    {
        auto& session = g_HostedCounselor;
        if (FrozenJasonBridge::IsHostedLobbyAIActive()) return true;
        UWorld* world = nullptr; UObject* mode = nullptr; UObject* state = nullptr;
        if (!HostedBoundary(world, mode, state) || world != session.World ||
            mode != session.Mode || state != session.State || session.Locked ||
            g_HostedF3Busy.load() || !HostedNativeSurface()) return false;
        if (!FrozenJasonBridge::GetHostedCounselorBehaviorTree())
        {
            Logger::Error("HOSTED F8 rejected before spawn: OfflineBotsCounselorBehaviorTree is not loaded; both F8/F3 require the stock counselor tree");
            return false;
        }
        UObject** current = HostedObjectField(state, "CurrentKiller");
        UObject** owner = HostedObjectField(state, "CurrentKillerPlayerState");
        auto* bots = reinterpret_cast<int32_t*>(RuntimeScalarAddress(mode, "NumBots", "IntProperty", sizeof(int32_t)));
        if (!current || !owner || !*current || !*owner || !bots || *bots < 0 || *bots > 7 ||
            !Memory::IsReadable(*current, 0x3A8) || !ClassDerivesFrom((*current)->Class, "SCKillerCharacter")) return false;
        AActor* jason = reinterpret_cast<AActor*>(*current);
        UObject* humanPS = *owner;
        UObject* human = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(jason) + 0x3A0);
        int32_t beforePlayers = -1;
        if (!human || !Memory::IsReadable(human, 0x390) ||
            !ClassDerivesFrom(human->Class, "PlayerController") ||
            GetControllerPlayerState(human) != humanPS ||
            *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(human) + 0x370) != jason ||
            !HostedActorInWorld(human, world) || !HostedActorInWorld(jason, world) ||
            !HostedRoster(state, humanPS, beforePlayers) || beforePlayers >= 8) return false;
        // Resurrected's stock handoff also displaces a remote-selected
        // Jason. The host must already have a valid counselor in that case:
        // the AI's local target and the host HUD must never be bound to the
        // remote player's newly restarted pawn.
        UObject* localController = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
        if (!localController || !HostedActorInWorld(localController, world)) return false;
        // Repeated live tests proved this branch leaves the listen host with
        // a non-controllable counselor and Jason HUD despite a valid pawn.
        // A late ClientRestart does not repair it. Keep the stock match
        // playable if the lobby preference did not make the host counselor;
        // only the remotely selected Jason handoff is currently verified.
        if (human == localController)
        {
            session.Locked = true;
            g_HostedJasonIntent.store(false);
            Logger::Error("HOSTED F8 skipped: stock lottery chose host Jason; "
                "unsafe host pawn handoff disabled, original Jason retained");
            return false;
        }
        if (human != localController)
        {
            if (!Memory::IsReadable(localController, 0x378)) return false;
            AActor* localPawn = *reinterpret_cast<AActor**>(
                reinterpret_cast<uintptr_t>(localController) + 0x370);
            if (!localPawn || !HostedActorInWorld(localPawn, world) ||
                !Memory::IsReadable(localPawn, 0x3A8) ||
                !ClassDerivesFrom(localPawn->Class, "SCCounselorCharacter") ||
                *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(localPawn) + 0x3A0) != localController)
                return false; // native host counselor not ready: retry pre-birth
        }
        using TravelReadyFn = bool(__fastcall*)(UObject*, bool);
        if (!reinterpret_cast<TravelReadyFn>(ShippingAddress(RVA_LifecycleHasFullyTraveled))(humanPS, true))
            return false; // pre-birth retry: selected human has not fully traveled
        UFunction* restart = HostedRestartFunction(mode);
        UFunction* possessHuman = HostedSingleObjectFunction(human, "Possess", "InPawn");
        UFunction* clientRestart = HostedClientRestartFunction(human);
        if (!restart || !possessHuman || !clientRestart) return false;
        const uintptr_t killers = GetLifecycleArrayOffset(mode, "KillerCharacterClasses", 0);
        const uintptr_t counselors = GetLifecycleArrayOffset(mode, "CounselorCharacterClasses", 0);
        const int32_t killerCount = GetLifecycleSoftClassCount(mode, killers);
        const int32_t counselorCount = GetLifecycleSoftClassCount(mode, counselors);
        const uint8_t* killerSoft = nullptr;
        for (int32_t i = 0; i < killerCount; ++i)
        {
            const uint8_t* candidate = GetLifecycleSoftClass(mode, killers, i);
            if (LoadLifecycleSoftClass(candidate) == jason->Class) { killerSoft = candidate; break; }
        }
        // RestartPlayer consumes ActiveCharacterClass, not the remote
        // player's profile UI. Honor the class that the selected Jason's
        // PlayerState already published before falling back to the stock
        // counselor roster. A random choice here silently changed the
        // displaced tester's preferred counselor every time F8 adopted them.
        const uint8_t* counselorSoft = nullptr;
        std::string counselorChoice = "fallback-random";
        UPropertyLite* picked = FindPropertyInHierarchyByName(
            humanPS->Class, "PickedCounselorClass");
        if (picked && picked->Offset_Internal > 0 &&
            picked->Offset_Internal < 0x10000 &&
            picked->ElementSize == LifecycleSoftClassSize)
        {
            const uint8_t* selected = reinterpret_cast<const uint8_t*>(
                reinterpret_cast<uintptr_t>(humanPS) + picked->Offset_Internal);
            const std::string selectedPath = ReadSoftClassAssetPath(selected);
            if (!selectedPath.empty() && selectedPath != "None")
            {
                counselorChoice = "preferred-unmatched";
                for (int32_t i = 0; i < counselorCount; ++i)
                {
                    const uint8_t* candidate = GetLifecycleSoftClass(mode, counselors, i);
                    if (candidate && ReadSoftClassAssetPath(candidate) == selectedPath)
                    {
                        counselorSoft = candidate;
                        counselorChoice = "preferred-matched";
                        break;
                    }
                }
            }
            else counselorChoice = "preferred-empty";
        }
        else counselorChoice = "preferred-unavailable";
        if (!counselorSoft && counselorCount > 0)
            counselorSoft = GetLifecycleSoftClass(mode, counselors,
                static_cast<int32_t>(GetTickCount64() % counselorCount));
        Logger::Debug("HOSTED F8 counselor class selection | source=" + counselorChoice +
            " | class=" + ReadSoftClassAssetPath(counselorSoft));
        UClass* counselorClass = LoadLifecycleSoftClass(counselorSoft);
        if (!killerSoft || !counselorClass || !ClassDerivesFrom(counselorClass, "SCCounselorCharacter")) return false;
        const uint8_t getterSig[]{0x4C,0x8B,0xDC,0x48,0x83,0xEC,0x78,0x48,0x8B,0x05,0x82,0x59,0xAE,0x02,0x48,0x85,0xC0};
        if (!MatchesBytes(ShippingAddress(RVA_LifecycleKillerControllerClass), getterSig, sizeof(getterSig))) return false;
        using GetterFn = UClass*(__fastcall*)();
        UClass* aiClass = reinterpret_cast<GetterFn>(ShippingAddress(RVA_LifecycleKillerControllerClass))();
        if (!aiClass || !ClassDerivesFrom(aiClass, "SCKillerAIController")) return false;
        const int32_t beforeBots = *bots;
        UObject* ai = SpawnKillerControllerWithStockCrowdBypass(world, aiClass);
        UObject* aiPS = GetControllerPlayerState(ai);
        AActor* counselor = nullptr;
        bool humanChanged = false;
        bool adoptionAttempted = false;
        bool botCountAccounted = false;
        auto rollback = [&](const char* reason)
        {
            if (adoptionAttempted) FrozenJasonBridge::ResetCounselorModeJason();
            bool restored = true;
            if (humanChanged)
            {
                // Restore original native ownership before re-possessing. A
                // failed restoration retains AI ownership; never destroy the
                // controller that may still own the stock Jason pawn.
                *current = jason; *owner = humanPS;
                restored = SetLifecycleActiveCharacter(humanPS, killerSoft);
                struct PossessParams { AActor* InPawn; } params{ jason };
                restored = SafeProcessEventCall(reinterpret_cast<uintptr_t>(human), human, possessHuman, &params) && restored;
                restored = restored && *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(jason) + 0x3A0) == human &&
                    *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(human) + 0x370) == jason &&
                    (!ai || (Memory::IsReadable(ai, 0x378) &&
                        *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(ai) + 0x370) != jason));
            }
            using DestroyFn = bool(__fastcall*)(UObject*, bool, bool);
            const auto destroy = reinterpret_cast<DestroyFn>(ShippingAddress(0x11E1180));
            if (restored && counselor && counselor != jason)
                restored = HostedActorInWorld(counselor, world) && destroy(counselor, false, true);
            if (restored && ai)
                restored = HostedActorInWorld(ai, world) && destroy(ai, false, true);
            int32_t count = -1;
            const bool registered = aiPS && HostedRoster(state, aiPS, count);
            const bool rosterRestored = !registered && HostedRoster(state, nullptr, count) && count == beforePlayers;
            if (restored && rosterRestored && botCountAccounted && *bots == beforeBots + 1)
                --*bots;
            restored = restored && rosterRestored && *bots == beforeBots && *current == jason && *owner == humanPS;
            session.Locked = true;
            g_HostedJasonIntent.store(false);
            g_HostedF1Pending.store(false);
            g_HostedF3Armed.store(false);
            g_HostedF3Pending.store(false);
            Logger::Error(std::string("HOSTED F8 rollback: ") + reason + " | verified=" +
                (restored ? "Y" : "N") + " | locked until next match");
            return false;
        };
        if (!ai || !aiPS || !HostedActorInWorld(ai, world) || !Memory::IsReadable(aiPS, 0x398) ||
            !(*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(aiPS) + 0x394) & 0x04))
            return rollback("native killer controller/PlayerState bot flag");
        const int32_t birthDelta = *bots - beforeBots;
        if (birthDelta == 0) ++*bots;
        else if (birthDelta != 1) return rollback("unexpected killer constructor NumBots delta");
        botCountAccounted = true;
        int32_t players = -1;
        if (!HostedRoster(state, aiPS, players) || players != beforePlayers + 1 ||
            !SetLifecycleActiveCharacter(aiPS, killerSoft) ||
            !CopyLifecycleKillerPresentation(humanPS, aiPS, jason->Class))
            return rollback("AI roster/class/presentation preflight");
        if (!HostedBotBirthReady(world, state, ai, aiPS))
            return rollback("AI native bot birth readiness before human handoff");
        UFunction* possessAI = HostedSingleObjectFunction(ai, "Possess", "InPawn");
        if (!possessAI) return rollback("AI reflected Possess schema");
        // Match the proven Resurrected role handoff: the displaced player's
        // active character becomes a counselor before AI Possess unpossesses
        // their Jason. Otherwise the local host's unpossession/intro callbacks
        // can still publish Jason input and HUD state after RestartPlayer.
        humanChanged = true;
        if (!SetLifecycleActiveCharacter(humanPS, counselorSoft))
            return rollback("human counselor class before AI possession");
        struct PossessParams { AActor* InPawn; } possess{ jason };
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(ai), ai, possessAI, &possess) ||
            *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(jason) + 0x3A0) != ai ||
            *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(ai) + 0x370) != jason ||
            !HostedBotBirthReady(world, state, ai, aiPS, jason))
            return rollback("AI possession");
        // Resurrected leaves the original killer PlayerState in GameState
        // until the human has a real counselor pawn. Publish AI ownership
        // only after Hunt RestartPlayer passes all native ownership checks.
        struct RestartParams { UObject* NewPlayer; } restartParams{ human };
        const bool restarted = SafeProcessEventCall(reinterpret_cast<uintptr_t>(mode), mode, restart, &restartParams);
        counselor = *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(human) + 0x370);
        if (counselor == jason) counselor = nullptr;
        if (!restarted || !counselor || !HostedActorInWorld(counselor, world) ||
            !Memory::IsReadable(counselor, 0x3A8) || !ClassDerivesFrom(counselor->Class, "SCCounselorCharacter") ||
            *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(counselor) + 0x3A0) != human ||
            GetControllerPlayerState(human) != humanPS || *current != jason ||
            (*owner != humanPS && *owner != aiPS) ||
            *bots != beforeBots + 1 || !HostedRoster(state, aiPS, players) || players != beforePlayers + 1)
            return rollback("human Hunt RestartPlayer ownership/roster postconditions");
        *owner = aiPS;
        adoptionAttempted = true;
        if (!FrozenJasonBridge::AdoptHostedLobbyJason(world, jason, ai))
            return rollback("hosted behavior adoption");
        FrozenJasonBridge::RegisterHostedLobbyCounselor(counselor);
        if (!ApplyHostedHighestCounselorSkill(mode))
            Logger::Error("HOSTED COUNSELOR AI: highest-skill native signatures unavailable; stock difficulty retained");
        if (human == localController)
            InitializeCounselorActiveCamera(counselor);
        // Hunt RestartPlayer sends its own ClientRestart. Do not send a
        // second one during the opening cinematic; the bounded handoff below
        // retries it only if native acknowledgement has not arrived.
        const ULONGLONG handoffAt = GetTickCount64();
        const ULONGLONG lateRepairDelay = 2000;
        g_HostedViewHandoff = { world, human, humanPS, counselor,
            handoffAt + 30000, handoffAt + 2000,
            handoffAt + lateRepairDelay, handoffAt + 20000,
            false, false, false, false, false };
        Logger::Success(std::string("HOSTED F8: stock Jason adopted by AI; selected human restarted as counselor; new AI PlayerState owns killer presentation | displaced=") +
            (human == localController ? "host" : "remote") + " | verify peer client role view and helper replication");
        return true;
    }

    static bool RestoreHostedPrimaryAfterAdditionalBirth()
    {
        auto& tx = g_HostedCounselor;
        UObject** killer = HostedObjectField(tx.State, "CurrentKiller");
        UObject** owner = HostedObjectField(tx.State, "CurrentKillerPlayerState");
        if (!killer || !owner) return false;
        // Native killer BeginPlay can publish the newly born pawn before it
        // has a PlayerState. Restore only this birth's recognized values, never
        // overwrite an unrelated ownership change.
        if (*killer != tx.Killer && *killer != tx.Pawn) return false;
        if (*owner != tx.KillerOwner && *owner != tx.PlayerState &&
            !(*killer == tx.Pawn && *owner == nullptr)) return false;
        *killer = tx.Killer;
        *owner = tx.KillerOwner;
        return HostedKillerUnchanged();
    }

    static void BeginHostedAdditionalJason(ULONGLONG now)
    {
        auto& tx = g_HostedCounselor;
        UWorld* world = nullptr; UObject* mode = nullptr; UObject* state = nullptr;
        auto reject = [](const char* reason)
        { Logger::Error(std::string("HOSTED F1 rejected before spawn: ") + reason); g_HostedF3Busy.store(false); };
        if (!HostedBoundary(world, mode, state) || world != tx.World || mode != tx.Mode ||
            state != tx.State || tx.Locked || !HostedNativeSurface() ||
            !FrozenJasonBridge::CanRegisterHostedLobbyJason())
        { reject("authority/gameplay/driver capacity"); return; }
        auto* bots = reinterpret_cast<int32_t*>(RuntimeScalarAddress(mode, "NumBots", "IntProperty", sizeof(int32_t)));
        UObject** killer = HostedObjectField(state, "CurrentKiller");
        UObject** owner = HostedObjectField(state, "CurrentKillerPlayerState");
        int32_t players = -1;
        if (!bots || *bots < 0 || *bots > 7 || !killer || !*killer || !owner || !*owner ||
            !HostedRoster(state, *owner, players) || players >= 8)
        { reject("native roster capacity/current killer"); return; }
        const uintptr_t offset = GetLifecycleArrayOffset(mode, "KillerCharacterClasses", 0);
        const int32_t count = GetLifecycleSoftClassCount(mode, offset);
        const uint8_t* soft = count > 0 ? GetLifecycleSoftClass(mode, offset, static_cast<int32_t>(now % count)) : nullptr;
        UClass* pawnClass = LoadLifecycleSoftClass(soft);
        AActor* start = FindKillerPlayerStart(world);
        FVector location{}; FRotator rotation{};
        if (!pawnClass || !ClassDerivesFrom(pawnClass, "SCKillerCharacter") ||
            !start || !GetActorStartTransform(start, location, rotation))
        { reject("native killer class/start transform"); return; }
        const uint8_t getterSig[]{0x4C,0x8B,0xDC,0x48,0x83,0xEC,0x78,0x48,0x8B,0x05,0x82,0x59,0xAE,0x02,0x48,0x85,0xC0};
        if (!MatchesBytes(ShippingAddress(RVA_LifecycleKillerControllerClass), getterSig, sizeof(getterSig)))
        { reject("killer controller getter signature"); return; }
        using GetterFn = UClass*(__fastcall*)();
        UClass* controllerClass = reinterpret_cast<GetterFn>(ShippingAddress(RVA_LifecycleKillerControllerClass))();
        if (!controllerClass || !ClassDerivesFrom(controllerClass, "SCKillerAIController"))
        { reject("native killer controller class"); return; }
        tx.Killer = *killer; tx.KillerOwner = *owner;
        tx.BeforeBots = *bots; tx.BeforePlayers = players; tx.BotCountAccounted = false;
        tx.AdditionalJason = true; tx.Deadline = now + 5000;
        tx.Controller = SpawnKillerControllerWithStockCrowdBypass(world, controllerClass);
        tx.PlayerState = GetControllerPlayerState(tx.Controller);
        if (!tx.Controller || !tx.PlayerState || !HostedActorInWorld(tx.Controller, world) ||
            !Memory::IsReadable(tx.PlayerState, 0x398) ||
            !(*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(tx.PlayerState) + 0x394) & 0x04))
        { HostedRollback("native additional killer controller/PlayerState"); return; }
        const int32_t delta = *bots - tx.BeforeBots;
        if (delta == 0) ++*bots;
        else if (delta != 1) { HostedRollback("additional killer constructor NumBots delta"); return; }
        tx.BotCountAccounted = true;
        if (!HostedRoster(state, tx.PlayerState, players) || players != tx.BeforePlayers + 1 ||
            !SetLifecycleActiveCharacter(tx.PlayerState, soft) ||
            !CopyLifecycleKillerPresentation(tx.KillerOwner, tx.PlayerState, pawnClass, true))
        { HostedRollback("additional killer roster/class/presentation"); return; }
        UFunction* possess = HostedSingleObjectFunction(tx.Controller, "Possess", "InPawn");
        if (!possess) { HostedRollback("additional killer Possess schema"); return; }
        tx.Pawn = SpawnLifecycleActor(world, pawnClass, &location, &rotation, mode, 2);
        const bool ownerRestored = RestoreHostedPrimaryAfterAdditionalBirth();
        if (!tx.Pawn || !ownerRestored || !HostedActorInWorld(tx.Pawn, world) ||
            !Memory::IsReadable(tx.Pawn, 0x3A8))
        { HostedRollback("additional killer native pawn/primary owner"); return; }
        struct PossessParams { AActor* InPawn; } params{tx.Pawn};
        const bool possessed = SafeProcessEventCall(reinterpret_cast<uintptr_t>(tx.Controller), tx.Controller, possess, &params);
        if (!RestoreHostedPrimaryAfterAdditionalBirth() || !possessed ||
            *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(tx.Pawn) + 0x3A0) != tx.Controller ||
            *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(tx.Controller) + 0x370) != tx.Pawn ||
            *bots != tx.BeforeBots + 1)
        { HostedRollback("additional killer possession/primary invariant"); return; }
        Logger::Debug("HOSTED F1: native additional Jason awaiting travel readiness before driver registration");
    }

    static void PumpHostedCounselor()
    {
        const ULONGLONG now = GetTickCount64();
        g_HostedNextPump.store(now + 250);
        const bool arm = g_HostedArmPending.exchange(false);
        UWorld* world = nullptr; UObject* mode = nullptr; UObject* state = nullptr;
        auto& tx = g_HostedCounselor;
        bool confirmedEnding = false;
        const bool inProgress = HostedBoundary(world, mode, state, &confirmedEnding);
        const int worldKind = HostedWorldKind(world, mode, state);
        static UWorld* idleGraceWorld = nullptr;
        if (worldKind != 2 || !g_HostedJasonIntent.load())
            idleGraceWorld = nullptr;
        else if (idleGraceWorld != world)
        {
            idleGraceWorld = world;
            if (!ApplyHostedTestLobbyIdleGrace(mode))
                Logger::Debug("HOSTED F8 idle grace unavailable; stock AFK setting retained");
        }
        // The OfflineBots +0x818 hook does not run for online Hunt. Prepare
        // the hosted timer on Hunt entry, then update the replicated
        // RemainingTime once at the start if stock initialization already
        // copied its former value. Never touch an unarmed or late match.
        static UWorld* timerWorld = nullptr;
        static bool timerEligible = false;
        static bool timerApplied = false;
        static ULONGLONG timerInProgressAt = 0;
        if (worldKind != 2 || !g_HostedJasonIntent.load())
        {
            timerWorld = nullptr;
            timerEligible = false;
            timerApplied = false;
            timerInProgressAt = 0;
        }
        else
        {
            if (timerWorld != world)
            {
                timerWorld = world;
                // Lobby-armed intent has not entered Hunt yet. F8 pressed
                // during an existing match must not reset its clock.
                timerEligible = !g_HostedIntentEnteredHunt;
                timerApplied = false;
                timerInProgressAt = 0;
                if (timerEligible)
                    OfflineSetup::ApplyCommittedMatchDurationBeforeStart(mode);
            }
            if (timerEligible && inProgress && !timerApplied)
            {
                if (!timerInProgressAt) timerInProgressAt = now;
                if (now - timerInProgressAt < 10000)
                {
                    auto* remaining = reinterpret_cast<int32_t*>(
                        RuntimeScalarAddress(state, "RemainingTime",
                            "IntProperty", sizeof(int32_t)));
                    if (remaining && *remaining > 0 && *remaining <= 7200)
                    {
                        const int32_t previous = *remaining;
                        *remaining = 1800;
                        timerApplied = true;
                        Logger::Success("HOSTED MATCH LENGTH: replicated RemainingTime " +
                            std::to_string(previous) + " -> 1800 seconds");
                    }
                }
            }
        }
        if (arm && (worldKind == 1 || (worldKind == 2 && !confirmedEnding)))
        {
            if (worldKind == 1 && !InstallHostedRoleSelectionBridge())
                Logger::Error("HOSTED F8: preselection hook unavailable; "
                    "unsafe host handoff remains disabled");
            // Resolve the stock UMG helper while F8 is pressed, not during
            // the role handoff or gameplay hot path.
            if (!g_HostedInputModeLibrary)
            {
                UObject* candidate = FindObjectExact(
                    "Default__WidgetBlueprintLibrary");
                if (candidate && Memory::IsReadable(candidate, sizeof(UObject)) &&
                    SafeName(reinterpret_cast<UObject*>(candidate->Class)) ==
                        "WidgetBlueprintLibrary")
                    g_HostedInputModeLibrary = candidate;
            }
            // Lobby intent creates no actors. One normal transition into Hunt
            // consumes it through bounded pre-birth attempts after InProgress.
            if (!(tx.Locked && tx.World == world && tx.Mode == mode))
            {
                g_HostedJasonIntent.store(true);
                g_HostedIntentUntil = now + (worldKind == 1 ? 1800000 : 180000);
                g_HostedIntentEnteredHunt = worldKind == 2;
                g_HostedAutoAttempts = 0;
                g_HostedAutoNextAttempt = now;
                Logger::Success(worldKind == 1 ?
                    "HOSTED F8 ARMED in lobby; waiting for native InProgress and human travel readiness" :
                    "HOSTED F8 ARMED for current Hunt; waiting for native gameplay readiness");
            }
            else Logger::Error("HOSTED F8 rejected: transaction lockout until next match");
        }
        else if (arm)
            Logger::Error("HOSTED F8 rejected: authoritative online lobby/Hunt required");
        const bool returnedToLobby = worldKind == 1 && g_HostedIntentEnteredHunt && !arm;
        if (worldKind == -1 || returnedToLobby || confirmedEnding ||
            (g_HostedJasonIntent.load() && !FrozenJasonBridge::IsHostedLobbyAIActive() && now >= g_HostedIntentUntil))
        {
            g_HostedJasonIntent.store(false);
            g_HostedF1Pending.store(false);
            g_HostedF3Armed.store(false);
            g_HostedF3Pending.store(false);
            if ((worldKind == -1 || returnedToLobby) && FrozenJasonBridge::IsHostedLobbyAIActive())
                FrozenJasonBridge::ResetCounselorModeJason();
            if (worldKind == -1 || returnedToLobby)
            {
                g_HostedViewHandoff = {};
                tx = HostedCounselorTransaction{};
                g_HostedF3Busy.store(false);
                g_HostedIntentEnteredHunt = false;
            }
        }
        if (worldKind == 2 && g_HostedJasonIntent.load() && !g_HostedIntentEnteredHunt)
        {
            g_HostedIntentEnteredHunt = true;
            g_HostedIntentUntil = now + 180000;
        }
        if (!inProgress)
        {
            if (confirmedEnding && tx.World == world && tx.Mode == mode && tx.State == state)
            {
                g_HostedViewHandoff = {};
                if (FrozenJasonBridge::IsHostedLobbyAIActive())
                    FrozenJasonBridge::RetireHostedGameplayForNativeEnding();
                // Native ending owns actor teardown. Retire only our driver
                // and requests. Retain death/outro identity until actual travel;
                // resetting here discarded the cinematic's delayed completion.
                tx.Controller = nullptr; tx.PlayerState = nullptr; tx.Pawn = nullptr;
                tx.BotCountAccounted = false;
                tx.Locked = true;
                g_HostedF3Armed.store(false); g_HostedF3Pending.store(false); g_HostedF1Pending.store(false); g_HostedF3Busy.store(false);
                return;
            }
            // GWorld may briefly alternate with an empty transition world.
            // Keep the in-flight transaction during its bounded readiness
            // window, but perform no mutations against a mismatched world.
            if (tx.Controller)
            {
                if (now >= tx.Deadline && !tx.Locked)
                {
                    tx.Locked = true;
                    g_HostedJasonIntent.store(false);
                    g_HostedF1Pending.store(false);
                    g_HostedF3Armed.store(false);
                    g_HostedF3Pending.store(false);
                    Logger::Error("HOSTED transaction paused/locked: authoritative world unavailable; owned actors retained until matching world returns or native travel replaces it");
                }
                return;
            }
            g_HostedF3Armed.store(false); g_HostedF3Pending.store(false); g_HostedF1Pending.store(false); g_HostedF3Busy.store(false);
            // Native world teardown owns old actors. Do not touch stale objects.
            tx.Controller = nullptr; tx.PlayerState = nullptr; tx.Pawn = nullptr;
            tx.BotCountAccounted = false;
            return;
        }
        if (tx.World != world || tx.Mode != mode || tx.State != state)
        {
            // Positively validated authoritative replacement, not an empty
            // transient GWorld: retire the prior world's Active flag.
            if (FrozenJasonBridge::IsHostedLobbyAIActive())
                FrozenJasonBridge::ResetCounselorModeJason();
            tx = HostedCounselorTransaction{};
            tx.World = world; tx.Mode = mode; tx.State = state;
            tx.InProgressAt = now;
            g_HostedF3Armed.store(false); g_HostedF3Pending.store(false); g_HostedF1Pending.store(false); g_HostedF3Busy.store(false);
        }
        if (tx.Controller && tx.Locked)
        { HostedRollback("authoritative world returned after readiness timeout"); return; }
        PumpHostedCounselorView(world, now);
        if (g_HostedJasonIntent.load() && !FrozenJasonBridge::IsHostedLobbyAIActive() &&
            now >= g_HostedAutoNextAttempt)
        {
            g_HostedAutoNextAttempt = now + 1000;
            if (++g_HostedAutoAttempts > 60)
            {
                g_HostedJasonIntent.store(false);
                Logger::Error("HOSTED automatic takeover timed out before mutation; press F8 to explicitly retry");
                return;
            }
            if (g_HostedAutoAttempts == 1)
                AuditPrivateLobbyOnGameThread(); // Includes privacy; no session identifiers.
            if (tx.Locked || !HostedNativeSurface() || !HostedRestartFunction(mode))
            {
                g_HostedJasonIntent.store(false);
                Logger::Error("HOSTED F8 rejected: lifecycle signature/schema or prior rollback lockout");
            }
            else
            {
                if (TakeOverHostedJason())
                {
                    g_HostedF3Armed.store(true);
                    Logger::Success("HOSTED F8 ACTIVE: F1 additional Jason and F3 counselors enabled");
                }
                else if (tx.Locked)
                    Logger::Error("HOSTED F8: native transaction rolled back; automatic retries disabled");
            }
        }
        if (g_HostedJasonIntent.load() && FrozenJasonBridge::IsHostedLobbyAIActive())
            g_HostedF3Armed.store(true);
        if (!g_HostedF3Armed.load()) return;
        if (tx.Controller)
        {
            int32_t count = -1;
            auto* bots = reinterpret_cast<int32_t*>(RuntimeScalarAddress(mode, "NumBots", "IntProperty", sizeof(int32_t)));
            const bool links = HostedActorInWorld(tx.Controller, world) && HostedActorInWorld(tx.Pawn, world) &&
                Memory::IsReadable(tx.Controller, 0x390) && Memory::IsReadable(tx.Pawn, 0x3A8) &&
                *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(tx.Controller) + 0x370) == tx.Pawn &&
                *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(tx.Pawn) + 0x3A0) == tx.Controller &&
                GetControllerPlayerState(tx.Controller) == tx.PlayerState;
            if (!links || !HostedKillerUnchanged() || !bots || *bots != tx.BeforeBots + 1 ||
                !HostedRoster(state, tx.PlayerState, count) || count != tx.BeforePlayers + 1)
            { HostedRollback("post-spawn ownership/roster/counter invariant"); return; }
            const bool ready = HostedBotBirthReady(world, state, tx.Controller, tx.PlayerState, tx.Pawn);
            if (!ready)
            {
                if (now >= tx.Deadline) HostedRollback("native bot birth readiness timeout");
                return;
            }
            if (tx.AdditionalJason)
            {
                if (!FrozenJasonBridge::RegisterHostedLobbyJason(tx.Pawn, tx.Controller))
                { HostedRollback("additional killer final driver registration"); return; }
                Logger::Success("HOSTED F1: additional random Jason registered with independent combat driver; primary killer owner unchanged; verify helper replication");
            }
            else
            {
                Logger::Success("HOSTED F3: native counselor registered, possessed, behavior started and bot lifecycle ready; verify helper replication");
                FrozenJasonBridge::RegisterHostedLobbyCounselor(tx.Pawn);
            }
            tx.Controller = nullptr; tx.PlayerState = nullptr; tx.Pawn = nullptr;
            tx.BotCountAccounted = false; // Committed bot is now native match-owned.
            g_HostedF3Busy.store(false);
            return;
        }
        // The host's opening cinematic can republish the former Jason
        // input/HUD state until the one-shot post-intro ClientRestart. Defer
        // additional bot births during that handoff; this match's live trace
        // showed five F3 births before the counselor control repair ran.
        if (g_HostedViewHandoff.Controller ==
                reinterpret_cast<UObject*>(Engine::GetLocalPlayerController()) &&
            !g_HostedViewHandoff.LateRepairSent &&
            now < g_HostedViewHandoff.Deadline)
            return;
        if (g_HostedF1Pending.exchange(false)) { BeginHostedAdditionalJason(now); return; }
        if (!g_HostedF3Pending.exchange(false)) return;
        auto reject = [](const char* reason)
        { Logger::Error(std::string("HOSTED F3 rejected before spawn: ") + reason); g_HostedF3Busy.store(false); };
        int32_t players = -1;
        auto* bots = reinterpret_cast<int32_t*>(RuntimeScalarAddress(mode, "NumBots", "IntProperty", sizeof(int32_t)));
        UObject** killer = HostedObjectField(state, "CurrentKiller");
        UObject** owner = HostedObjectField(state, "CurrentKillerPlayerState");
        UFunction* restart = HostedRestartFunction(mode);
        UObject* tree = reinterpret_cast<UObject*>(FrozenJasonBridge::GetHostedCounselorBehaviorTree());
        if (!HostedNativeSurface() || !restart || !tree || !bots || *bots < 0 || *bots > 7 ||
            !killer || !*killer || !owner || !*owner || !HostedRoster(state, *owner, players) || players >= 8)
        { reject("capacity/native/schema/tree/killer-owner preflight"); return; }
        const uintptr_t offset = GetLifecycleArrayOffset(mode, "CounselorCharacterClasses", 0);
        const int32_t classes = GetLifecycleSoftClassCount(mode, offset);
        if (classes <= 0) { reject("counselor class list"); return; }
        const uint8_t* soft = GetLifecycleSoftClass(mode, offset, static_cast<int32_t>(now % classes));
        UClass* pawnClass = LoadLifecycleSoftClass(soft);
        using GetterFn = UClass*(__fastcall*)();
        UClass* controllerClass = reinterpret_cast<GetterFn>(ShippingAddress(RVA_LifecycleCounselorControllerClass))();
        if (!pawnClass || !ClassDerivesFrom(pawnClass, "SCCounselorCharacter") ||
            !controllerClass || !ClassDerivesFrom(controllerClass, "SCCounselorAIController"))
        { reject("native counselor classes"); return; }
        tx.Killer = *killer; tx.KillerOwner = *owner; tx.BeforeBots = *bots; tx.BeforePlayers = players;
        tx.AdditionalJason = false;
        tx.BotCountAccounted = false;
        tx.Deadline = now + 5000;
        tx.Controller = SpawnLifecycleActor(world, controllerClass, nullptr, nullptr, mode, 1);
        tx.PlayerState = GetControllerPlayerState(tx.Controller);
        // Track this owned contribution before class/restart work. Native Hunt
        // Logout skips NumBots for AI, so verified rollback removes it itself.
        if (tx.PlayerState && Memory::IsReadable(tx.PlayerState, 0x398) &&
            (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(tx.PlayerState) + 0x394) & 0x04))
        {
            const int32_t birthDelta = *bots - tx.BeforeBots;
            if (birthDelta == 0) ++*bots;
            else if (birthDelta != 1) { HostedRollback("unexpected constructor NumBots delta"); return; }
            tx.BotCountAccounted = true;
        }
        else if (tx.Controller)
        { HostedRollback("native AI PlayerState bot flag unavailable"); return; }
        if (!tx.Controller || !tx.PlayerState || !HostedActorInWorld(tx.Controller, world) ||
            !SetLifecycleActiveCharacter(tx.PlayerState, soft))
        { HostedRollback("controller/PlayerState/active class"); return; }
        struct RestartParams { UObject* NewPlayer; } params{tx.Controller};
        const bool dispatched = SafeProcessEventCall(reinterpret_cast<uintptr_t>(mode), mode, restart, &params);
        if (Memory::IsReadable(tx.Controller, 0x378))
            tx.Pawn = *reinterpret_cast<AActor**>(reinterpret_cast<uintptr_t>(tx.Controller) + 0x370);
        if (!dispatched || !tx.Pawn || !Memory::IsReadable(tx.Pawn, 0x3A8) ||
            !HostedActorInWorld(tx.Pawn, world) || !ClassDerivesFrom(tx.Pawn->Class, "SCCounselorCharacter") ||
            *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(tx.Pawn) + 0x3A0) != tx.Controller ||
            !HostedRoster(state, tx.PlayerState, players) || players != tx.BeforePlayers + 1 || !HostedKillerUnchanged())
        { HostedRollback("reflected Hunt RestartPlayer postconditions"); return; }
        if (*bots != tx.BeforeBots + 1) { HostedRollback("unexpected RestartPlayer NumBots delta"); return; }
        auto** vtable = *reinterpret_cast<void***>(tx.Controller);
        if (!vtable || !Memory::IsReadable(vtable + 0x758 / sizeof(void*), sizeof(void*)) ||
            reinterpret_cast<uintptr_t>(vtable[0x758 / sizeof(void*)]) != ShippingAddress(0x31EED0))
        { HostedRollback("counselor behavior override identity"); return; }
        using StartTreeFn = bool(__fastcall*)(UObject*, UObject*);
        if (!reinterpret_cast<StartTreeFn>(vtable[0x758 / sizeof(void*)])(tx.Controller, tree))
        { HostedRollback("stock counselor behavior start"); return; }
        tx.Deadline = now + 5000;
        Logger::Debug("HOSTED F3: counselor transaction awaiting native bot lifecycle validation");
    }

    static bool ApplyNativeOfflineDamageSettings(UObject* object)
    {
        const int32_t difficulty = g_CommittedJasonDifficulty.load();
        if (difficulty < 0 || difficulty > 2)
            return false;
        int32_t* attack = reinterpret_cast<int32_t*>(RuntimeScalarAddress(
            object, "iJasonAttackDamage", "IntProperty", sizeof(int32_t)));
        int32_t* knife = reinterpret_cast<int32_t*>(RuntimeScalarAddress(
            object, "iJasonThrowingKnifeDamage", "IntProperty", sizeof(int32_t)));
#if defined(F13_BASE_GAME_PORT)
        // Cooked native custom-match fields are not always present in the
        // runtime UProperty chain. The supported stock copier proves their
        // layout; require its exact instructions before using native storage.
        static const uint8_t nativeDamageCopy[]{
            0x8B,0x83,0x4C,0x03,0x00,0x00,0x89,0x87,0x9C,0x04,0x00,0x00,
            0x0F,0xB6,0x83,0x50,0x03,0x00,0x00,0x88,0x87,0xA0,0x04,0x00,0x00,
            0x8B,0x83,0x54,0x03,0x00,0x00,0x89,0x87,0xA4,0x04,0x00,0x00};
        if ((!attack || !knife) && object && object->Class &&
            MatchesBytes(ShippingAddress(0x3A9FA2), nativeDamageCopy, sizeof(nativeDamageCopy)))
        {
            const bool mode = ClassDerivesFrom(object->Class, "SCGameMode_OfflineBots");
            if (mode)
            {
                int32_t* nativeAttack = reinterpret_cast<int32_t*>(
                    reinterpret_cast<uintptr_t>(object) + 0x4A4);
                int32_t* nativeKnife = reinterpret_cast<int32_t*>(
                    reinterpret_cast<uintptr_t>(object) + 0x49C);
                if (Memory::IsReadable(nativeAttack, sizeof(int32_t)) &&
                    Memory::IsReadable(nativeKnife, sizeof(int32_t)) &&
                    *nativeAttack >= 0 && *nativeAttack <= 2 &&
                    *nativeKnife >= 0 && *nativeKnife <= 2)
                {
                    attack = nativeAttack;
                    knife = nativeKnife;
                }
            }
        }
#endif
        if (!attack || !knife)
        {
            Logger::Error("OFFLINE JASON DIFFICULTY: stock damage properties missing on " + SafeName(object));
            return false;
        }
        const int32_t previousAttack = *attack;
        const int32_t previousKnife = *knife;
        // Stock custom-match combo indices: Half / Normal / 2X. Restrict
        // mutation to the current offline instance, never a CDO/profile.
        *attack = 1;
        *knife = 1;
        // Supported EXE native SetBit helpers write full bytes at +0x499
        // (SCGameMode) and +0x5D5 (SCGameState), not shared bitfields.
        UPropertyLite* rageProperty = FindPropertyInHierarchyByName(object->Class, "bJasonRageOnStart");
        auto* rageAtStart = reinterpret_cast<uint8_t*>(RuntimeScalarAddress(
            object, "bJasonRageOnStart", "BoolProperty", sizeof(uint8_t)));
        if (rageAtStart && rageProperty &&
            ((ClassDerivesFrom(object->Class, "SCGameMode") && rageProperty->Offset_Internal == 0x499) ||
             (ClassDerivesFrom(object->Class, "SCGameState") && rageProperty->Offset_Internal == 0x5D5)))
            *rageAtStart = 0;
        Logger::Success("OFFLINE DAMAGE SETTINGS APPLIED: object=" + SafeName(object) +
            " | attack=" + std::to_string(previousAttack) + "->" + std::to_string(*attack) +
            " | knife=" + std::to_string(previousKnife) + "->" + std::to_string(*knife) +
            " | multiplier=" + (difficulty == 0 ? "0.5" : difficulty == 1 ? "1" : "2"));
        return true;
    }

    static UObject* FindCurrentWorldSettings(UWorld* world)
    {
        if (!world || !Memory::IsReadable(world, sizeof(UWorld)) ||
            !world->PersistentLevel || !Memory::IsReadable(world->PersistentLevel, sizeof(ULevel)))
            return nullptr;
        const auto& actors = world->PersistentLevel->Actors;
        if (!actors.Data || actors.Count <= 0 || actors.Count > 20000 ||
            !Memory::IsReadable(actors.Data, sizeof(AActor*) * actors.Count))
            return nullptr;
        for (int32_t i = 0; i < actors.Count; ++i)
        {
            UObject* actor = reinterpret_cast<UObject*>(actors.Data[i]);
            if (actor && Memory::IsReadable(actor, sizeof(UObject)) && actor->Class &&
                actor->OuterPrivate == reinterpret_cast<UObject*>(world->PersistentLevel) &&
                ClassDerivesFrom(actor->Class, "SCWorldSettings"))
                return actor;
        }
        return nullptr;
    }

    static void PumpChallengeStartIntroSkip(UWorld* world)
    {
        // The shared WorldSettings function can also skip an outro. Never
        // sample Start or invoke it outside the exact Challenge intro phase.
        static UWorld* lastWorld = nullptr;
        static bool previouslyPressed = false;
        static bool dispatched = false;
        static ULONGLONG nextPollAt = 0;
        if (world != lastWorld)
        {
            lastWorld = world;
            // A held Start from the menu must not auto-skip the first frame.
            previouslyPressed = true;
            dispatched = false;
            nextPollAt = 0;
        }
        const ULONGLONG now = GetTickCount64();
        if (dispatched || now < nextPollAt)
            return;
        nextPollAt = now + 50;

        bool pressed = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        static XInputGetStateFn readGamepad = []() -> XInputGetStateFn
        {
            HMODULE module = GetModuleHandleW(L"xinput1_4.dll");
            if (!module) module = LoadLibraryW(L"xinput1_4.dll");
            return module ? reinterpret_cast<XInputGetStateFn>(
                GetProcAddress(module, "XInputGetState")) : nullptr;
        }();
        XINPUT_STATE gamepad{};
        if (readGamepad && readGamepad(0, &gamepad) == ERROR_SUCCESS)
            pressed |= (gamepad.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
        const bool edge = pressed && !previouslyPressed;
        previouslyPressed = pressed;
        if (!edge)
            return;

        UObject* settings = FindCurrentWorldSettings(world);
        UFunction* skip = settings && settings->Class
            ? FindFunctionInHierarchyByName(settings->Class,
                "SkipLevelIntroOutro") : nullptr;
        if (!skip)
        {
            Logger::Error("CHALLENGE INTRO START SKIP: native WorldSettings skip unavailable; intro preserved");
            return;
        }
        alignas(16) uint8_t params[0x40]{};
        dispatched = SafeProcessEventCall(
            reinterpret_cast<uintptr_t>(settings), settings, skip, params);
        Logger::Success(std::string("CHALLENGE INTRO START SKIP: native intro skip ") +
            (dispatched ? "dispatched" : "rejected"));
    }

    static bool SafeNativeSetOfflineRain(uintptr_t function, UObject* settings, bool rain)
    {
        __try
        {
            reinterpret_cast<void(__fastcall*)(UObject*, bool)>(function)(settings, rain);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ApplyNativeOfflineWeather(UWorld* world)
    {
        const int32_t rain = g_CommittedOfflineRain.load();
        if (rain < 0)
            return true; // No weather choice: keep native behavior.
        UObject* settings = FindCurrentWorldSettings(world);
        if (!settings)
            return false;
        auto* enabled = reinterpret_cast<FName*>(RuntimeScalarAddress(
            settings, "RainEnabledLevel", "NameProperty", sizeof(FName)));
        auto* disabled = reinterpret_cast<FName*>(RuntimeScalarAddress(
            settings, "RainDisabledLevel", "NameProperty", sizeof(FName)));
        auto* raining = reinterpret_cast<uint8_t*>(RuntimeScalarAddress(
            settings, "bIsRaining", "BoolProperty", sizeof(uint8_t)));
        if (!enabled || !disabled || !raining ||
            (enabled->ComparisonIndex == 0 && enabled->Number == 0) ||
            (disabled->ComparisonIndex == 0 && disabled->Number == 0))
            return false;
        // Disassembled supported Shipping native SCWorldSettings::SetRaining:
        // streams the map's own RainOn/RainOff levels, updates materials/audio,
        // and calls OnRep_IsRaining. Do not substitute a raw bool write.
        const uintptr_t function = ShippingAddress(0x4431C0);
        const uint8_t signature[] = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x7C,
            0x24,0x10,0x55,0x48,0x8D,0x6C,0x24,0xA9,0x48,0x81,0xEC,0x90,0,0,0 };
        UPropertyLite* enabledProperty = FindPropertyInHierarchyByName(settings->Class, "RainEnabledLevel");
        UPropertyLite* disabledProperty = FindPropertyInHierarchyByName(settings->Class, "RainDisabledLevel");
        UPropertyLite* rainProperty = FindPropertyInHierarchyByName(settings->Class, "bIsRaining");
        if (!MatchesBytes(function, signature, sizeof(signature)) ||
            enabledProperty->Offset_Internal != 0x880 || disabledProperty->Offset_Internal != 0x888 ||
            rainProperty->Offset_Internal != 0x8B0)
            return false;
        // Keep GameState's selection deterministic too. Its setter alone
        // does not trigger level streaming; native SetRaining below does.
        UObject* gameState = reinterpret_cast<UObject*>(world->GameState);
        UFunction* setter = gameState && gameState->Class ?
            FindFunctionInHierarchyByName(gameState->Class, "SetRainSetting") : nullptr;
        if (setter)
        {
            alignas(16) uint8_t params[0x200]{};
            const uint8_t value = rain ? 1 : 2; // ESCRainSetting On / Off.
            if (WriteRuntimeCallArgument(setter, params, "NewRainSetting", &value, sizeof(value)))
                SafeProcessEventCall(reinterpret_cast<uintptr_t>(gameState), gameState, setter, params);
        }
        if (!SafeNativeSetOfflineRain(function, settings, rain != 0))
            return false;
        Logger::Success(std::string("OFFLINE WEATHER APPLIED: ") + (rain ? "Rain" : "Off") +
            " | native world setting=" + std::to_string(*raining) +
            " | map RainOn/RainOff streaming requested");
        return *raining == static_cast<uint8_t>(rain);
    }

    static void ApplyPendingOfflineSettingsOnGameThread()
    {
        if (!g_OfflineWeatherApplyPending.load())
            return;
        const ULONGLONG now = GetTickCount64();
        if (now < g_NextWeatherApplyAt.load())
            return;
        g_NextWeatherApplyAt.store(now + 500);
        UWorld* world = Engine::GetWorld();
        if (!world || reinterpret_cast<uintptr_t>(world) != g_CommittedSettingsWorld.load())
        {
            g_OfflineWeatherApplyPending.store(false);
            return;
        }
        if (ApplyNativeOfflineWeather(world))
            g_OfflineWeatherApplyPending.store(false);
        else if (now - g_WeatherApplyStartedAt.load() > 15000)
        {
            g_OfflineWeatherApplyPending.store(false);
            Logger::Error("OFFLINE WEATHER NOT APPLIED: native map rain surface unavailable after bounded retry; stock weather retained");
        }
    }

    struct OfflineJasonExtraState
    {
        uintptr_t world = 0;
        UObject* pawn = nullptr;
        uint64_t identity = 0;
        uint64_t movementIdentity = 0;
        int32_t initialKnives = 0;
        int32_t appliedKnives = 0;
        bool knivesCaptured = false;
        std::array<float, 4> initialRecharge{};
        std::array<float, 4> appliedRecharge{};
        bool rechargeCaptured = false;
        std::array<float, 2> initialMovement{};
        std::array<float, 2> appliedMovement{};
        bool movementCaptured = false;
        ULONGLONG nextPump = 0;
        bool reportedFailure = false;
    };
    OfflineJasonExtraState g_OfflineJasonExtra{};

    static void* OfflineJasonExtraField(UObject* pawn, const char* name,
        const char* type, int32_t offset, int32_t dimension, uint64_t* ownerIdentity = nullptr)
    {
        if (ownerIdentity) *ownerIdentity = 0;
        if (!pawn || !Memory::IsReadable(pawn, sizeof(UObject)) || !pawn->Class) return nullptr;
        UObject* owner = pawn;
        if (ownerIdentity)
        {
            owner = ReadRuntimeWidgetObject(pawn, "CharacterMovement");
            if (!owner || !Memory::IsReadable(owner, sizeof(UObject)) || !owner->Class ||
                (owner->ObjectFlags & 0x30) || owner->OuterPrivate != pawn ||
                !ClassDerivesFrom(owner->Class, "SCCharacterMovement")) return nullptr;
        }
        UPropertyLite* property = FindPropertyInHierarchyByName(owner->Class, name);
        if (!property || property->Offset_Internal != offset || property->ElementSize != 4 ||
            property->ArrayDim != dimension ||
            SafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) != type) return nullptr;
        const uint64_t identity = ChallengeObjectIdentity(owner);
        void* address = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(owner) + offset);
        if (!identity || !ChallengeWritable(address, 4 * static_cast<size_t>(dimension))) return nullptr;
        if (ownerIdentity) *ownerIdentity = identity;
        return address;
    }

    static void ResetOfflineJasonExtras()
    {
        auto& state = g_OfflineJasonExtra;
        if (state.pawn && state.identity && ChallengeObjectIdentity(state.pawn) == state.identity &&
            ChallengeIsCurrentJason(state.pawn, state.world,
                reinterpret_cast<uintptr_t>(Engine::GetLocalPlayerController())))
        {
            if (state.knivesCaptured)
            {
                auto* knives = reinterpret_cast<int32_t*>(OfflineJasonExtraField(
                    state.pawn, "NumKnives", "IntProperty", 0x15BC, 1));
                if (knives && *knives == state.appliedKnives) *knives = state.initialKnives;
            }
            if (state.rechargeCaptured)
            {
                auto* recharge = reinterpret_cast<float*>(OfflineJasonExtraField(
                    state.pawn, "AbilityRechargeTime", "FloatProperty", 0x1218, 4));
                if (recharge && std::equal(recharge, recharge + 4, state.appliedRecharge.begin()))
                    std::copy(state.initialRecharge.begin(), state.initialRecharge.end(), recharge);
            }
            if (state.movementCaptured)
            {
                constexpr const char* names[]{ "MaxRunSpeed", "MaxSprintSpeed" };
                constexpr int32_t offsets[]{ 0x724, 0x720 };
                for (int i = 0; i < 2; ++i)
                {
                    uint64_t ownerIdentity = 0;
                    auto* speed = reinterpret_cast<float*>(OfflineJasonExtraField(
                        state.pawn, names[i], "FloatProperty", offsets[i], 1, &ownerIdentity));
                    if (speed && ownerIdentity == state.movementIdentity &&
                        *speed == state.appliedMovement[i]) *speed = state.initialMovement[i];
                }
            }
        }
        state = {};
    }

    static void PumpOfflineJasonExtras()
    {
        if (!g_CommittedJasonUnlimitedKnives.load() && !g_CommittedJasonFastRecharge.load() &&
            !g_CommittedJasonFastMovement.load())
        {
            if (g_OfflineJasonExtra.pawn) ResetOfflineJasonExtras();
            return;
        }
        static ULONGLONG nextScopeAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (now < nextScopeAt) return;
        nextScopeAt = now + 100;
        UWorld* world = Engine::GetWorld();
        UObject* mode = ChallengeCurrentGameMode();
        UObject* state = world && Memory::IsReadable(world, sizeof(UWorld))
            ? reinterpret_cast<UObject*>(world->GameState) : nullptr;
        UObject* controller = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
        UObject* pawn = state ? ReadRuntimeWidgetObject(state, "CurrentKiller") : nullptr;
        UObject* netDriver = nullptr;
        UObject* netConnection = nullptr;
        const bool offlineNetwork = world && controller &&
            ChallengeReadObjectField(reinterpret_cast<UObject*>(world), "NetDriver", &netDriver) &&
            ChallengeReadObjectField(controller, "NetConnection", &netConnection) &&
            !netDriver && !netConnection;
        const uintptr_t worldValue = reinterpret_cast<uintptr_t>(world);
        if (!world || worldValue != g_CommittedSettingsWorld.load() ||
            g_CounselorMenuRouteLatched.load() || g_ChallengeRouteActive.load() ||
            !mode || !mode->Class || !ClassDerivesFrom(mode->Class, "SCGameMode_OfflineBots") ||
            !controller || !offlineNetwork || !ChallengeIsCurrentJason(pawn, worldValue,
                reinterpret_cast<uintptr_t>(controller)))
        {
            if (g_OfflineJasonExtra.pawn) ResetOfflineJasonExtras();
            return;
        }
        const uint64_t identity = ChallengeObjectIdentity(pawn);
        if (!identity) return;
        if (g_OfflineJasonExtra.pawn != pawn || g_OfflineJasonExtra.identity != identity ||
            g_OfflineJasonExtra.world != worldValue)
        {
            ResetOfflineJasonExtras();
            g_OfflineJasonExtra.world = worldValue;
            g_OfflineJasonExtra.pawn = pawn;
            g_OfflineJasonExtra.identity = identity;
        }
        auto& owned = g_OfflineJasonExtra;
        if (now < owned.nextPump) return;
        owned.nextPump = now + 100;
        if (g_CommittedJasonUnlimitedKnives.load())
        {
            auto* knives = reinterpret_cast<int32_t*>(OfflineJasonExtraField(
                pawn, "NumKnives", "IntProperty", 0x15BC, 1));
            if (knives && *knives >= 0 && *knives <= 9999)
            {
                if (!owned.knivesCaptured)
                {
                    owned.initialKnives = *knives;
                    owned.knivesCaptured = true;
                    Logger::Success("OFFLINE JASON EXTRA: unlimited knives on local Jason");
                }
                owned.appliedKnives = std::max(*knives, 99);
                *knives = owned.appliedKnives;
            }
        }
        if (g_CommittedJasonFastRecharge.load() && !owned.rechargeCaptured)
        {
            auto* recharge = reinterpret_cast<float*>(OfflineJasonExtraField(
                pawn, "AbilityRechargeTime", "FloatProperty", 0x1218, 4));
            if (recharge && std::all_of(recharge, recharge + 4, [](float value) {
                return std::isfinite(value) && value > 0.0f && value <= 3600.0f;
            }))
            {
                std::copy(recharge, recharge + 4, owned.initialRecharge.begin());
                for (int i = 0; i < 4; ++i) owned.appliedRecharge[i] = recharge[i] * 0.5f;
                std::copy(owned.appliedRecharge.begin(), owned.appliedRecharge.end(), recharge);
                owned.rechargeCaptured = true;
                Logger::Success("OFFLINE JASON EXTRA: native ability recharge time halved");
            }
        }
        if (g_CommittedJasonFastMovement.load() && !owned.movementCaptured)
        {
            uint64_t runIdentity = 0, sprintIdentity = 0;
            auto* run = reinterpret_cast<float*>(OfflineJasonExtraField(
                pawn, "MaxRunSpeed", "FloatProperty", 0x724, 1, &runIdentity));
            auto* sprint = reinterpret_cast<float*>(OfflineJasonExtraField(
                pawn, "MaxSprintSpeed", "FloatProperty", 0x720, 1, &sprintIdentity));
            if (run && sprint && runIdentity && runIdentity == sprintIdentity &&
                std::isfinite(*run) && *run > 0.0f && *run <= 5000.0f &&
                std::isfinite(*sprint) && *sprint > 0.0f && *sprint <= 5000.0f)
            {
                owned.initialMovement = { *run, *sprint };
                owned.appliedMovement = { *run * 2.0f, *sprint * 2.0f };
                *run = owned.appliedMovement[0];
                *sprint = owned.appliedMovement[1];
                owned.movementIdentity = runIdentity;
                owned.movementCaptured = true;
                Logger::Success("OFFLINE JASON EXTRA: native run and sprint speed doubled");
            }
        }
        if (!owned.reportedFailure &&
            (g_CommittedJasonUnlimitedKnives.load() && !owned.knivesCaptured ||
             g_CommittedJasonFastRecharge.load() && !owned.rechargeCaptured ||
             g_CommittedJasonFastMovement.load() && !owned.movementCaptured))
        {
            owned.reportedFailure = true;
            Logger::Error("OFFLINE JASON EXTRA: native field not ready; bounded retry without changing unsupported fields");
        }
    }
}

namespace OfflineSetup
{
    bool HasActiveChallengeRoute() { return g_ChallengeRouteActive.load(); }

    static void RepairMissingChallengeWeapon(const f13::challenges::runtime::Scope& scope)
    {
#if defined(F13_BASE_GAME_PORT)
        if (!scope.inProgress || !scope.localAuthority || scope.networkSession ||
            !ChallengeIsCurrentJason(scope.primaryJason, scope.world, scope.controller)) return;
        UObject* pawn = reinterpret_cast<UObject*>(scope.primaryJason);
        UObject* currentKiller = nullptr;
        if (!ChallengeReadObjectField(reinterpret_cast<UObject*>(scope.gameState), "CurrentKiller", &currentKiller) ||
            currentKiller != pawn || !Memory::IsReadable(pawn, 0x13A0)) return;
        const uint64_t identity = ChallengeObjectIdentity(pawn);
        if (!identity) return;
        struct Attempt {
            uintptr_t world = 0;
            uint64_t pawnIdentity = 0;
            ULONGLONG firstSeen = 0;
            bool attempted = false;
            bool reported = false;
        };
        static Attempt attempt{};
        const ULONGLONG now = GetTickCount64();
        if (attempt.world != scope.world || attempt.pawnIdentity != identity)
            attempt = {scope.world, identity, now, false, false};
        if (attempt.reported) return;
        UObject* equipped = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(pawn) + 0xF78);
        UObject* pending = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(pawn) + 0xF68);
        if (equipped && Memory::IsReadable(equipped, sizeof(UObject)) && equipped->Class &&
            ClassDerivesFrom(equipped->Class, "SCWeapon"))
        {
            Logger::Success(std::string("CHALLENGE WEAPON: native equipped weapon confirmed | class=") +
                SafeName(reinterpret_cast<UObject*>(equipped->Class)) +
                " | repaired=" + (attempt.attempted ? "true" : "false"));
            attempt.reported = true;
            return;
        }
        // Never compete with a native pending equip or replenish weapons
        // during combat/death. This is a bounded initial-birth repair only.
        if (now - attempt.firstSeen > 10000)
        {
            Logger::Error("CHALLENGE WEAPON: initial equipment not confirmed after grace; native pending=" +
                std::to_string(pending != nullptr) + " | repairAttempted=" + std::to_string(attempt.attempted));
            attempt.reported = true;
            return;
        }
        if (now - attempt.firstSeen < 2000 || pending || equipped || attempt.attempted) return;
        attempt.attempted = true;
        const uint8_t* authored = reinterpret_cast<const uint8_t*>(pawn) + 0x1378;
        static const uint8_t authoredSig[]{0x48,0x81,0xC2,0x78,0x13,0x00,0x00};
        static const uint8_t loadSig[]{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20};
        if (!MatchesBytes(ShippingAddress(0x3EEE46), authoredSig, sizeof(authoredSig)) ||
            !MatchesBytes(ShippingAddress(0x29D880), loadSig, sizeof(loadSig))) return;
        // Prefer a valid selected native weapon; otherwise use this Jason's
        // authored default, exactly like the game's normal killer initializer.
        const uint8_t* selected = authored;
        UObject* playerState = nullptr;
        if (ChallengeReadObjectField(pawn, "PlayerState", &playerState) && playerState &&
            playerState->Class && ClassDerivesFrom(playerState->Class, "SCPlayerState") &&
            Memory::IsReadable(playerState, 0x7E0))
        {
            const uint8_t* choice = reinterpret_cast<const uint8_t*>(playerState) + 0x7B8;
            const std::string path = ReadSoftClassAssetPath(choice);
            if (!path.empty() && path != "None") selected = choice;
        }
        UClass* weapon = LoadLifecycleSoftClass(selected);
        if ((!weapon || !ClassDerivesFrom(weapon, "SCWeapon")) && selected != authored)
            weapon = LoadLifecycleSoftClass(authored);
        const bool requested = weapon && ClassDerivesFrom(weapon, "SCWeapon") &&
            FrozenJasonBridge::GrantChallengeMissingWeapon(reinterpret_cast<AActor*>(pawn), weapon);
        Logger::Success("CHALLENGE WEAPON: bounded missing-equipment repair | requested=" + std::to_string(requested) +
            " | native slash gates preserved");
#endif
    }

    static bool IsPotentialChallengeUiEvent(UObject* function, bool after, bool active)
    {
        // This global ProcessEvent gate performs no allocations/VirtualQuery.
        // Inactive routes inspect only the exact challenge click before dispatch.
        if (after && !active) return false;
        __try
        {
            if (!function || !GNames) return false;
            const FNameEntry* entry = GNames->GetById(function->NameIndex);
            if (!entry) return false;
            const char* name = entry->AnsiName;
            if (after) return strcmp(name, "Construct") == 0;
            if (!active) return strcmp(name,
                "BndEvt__SinglePlayerChallengesButton_K2Node_ComponentBoundEvent_20_OnClicked__DelegateSignature") == 0;
            return strcmp(name, "OnClicked_Start") == 0 || strcmp(name, "OnStartPressed") == 0 || strcmp(name, "OnClicked_Back") == 0 ||
                (strncmp(name, "BndEvt__", 8) == 0 && strstr(name, "OnClicked__DelegateSignature") != nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void ObserveChallengeSetupEvent(void* objectValue, void* functionValue, bool afterOriginal)
    {
        UObject* object = reinterpret_cast<UObject*>(objectValue);
        UObject* function = reinterpret_cast<UObject*>(functionValue);
        if (!IsPotentialChallengeUiEvent(function, afterOriginal, g_ChallengeRouteActive.load())) return;
        if (!object || !function || !Memory::IsReadable(object, sizeof(UObject)) || !object->Class) return;
        const bool offlinePlay = SafeNameEquals(reinterpret_cast<UObject*>(object->Class), "OfflinePlayMenuWidget_C");
        const bool challengeSettings = SafeNameEquals(reinterpret_cast<UObject*>(object->Class), "SPChallengesSettingsMenuWidget_C");
        const bool challengePicker = SafeNameEquals(reinterpret_cast<UObject*>(object->Class), "PickSPChallengesMapMenuWidget_C");
        const bool challengeLobby = SafeNameEquals(reinterpret_cast<UObject*>(object->Class), "SP_LobbyWidget_C");
        if (!offlinePlay && !challengeSettings && !challengePicker && !challengeLobby) return;
        static const char* click = "BndEvt__SinglePlayerChallengesButton_K2Node_ComponentBoundEvent_20_OnClicked__DelegateSignature";
        const bool actualClick = offlinePlay && SafeNameEquals(function, click);
        if (!afterOriginal && actualClick && ChallengeIsLiveFrontend(object))
        {
            ActivateChallengeRoute(object);
            return;
        }
        if (!g_ChallengeRouteActive.load()) return;
        const std::string className = SafeName(reinterpret_cast<UObject*>(object->Class));
        const std::string eventName = SafeName(function);
        if (afterOriginal)
        {
            if (challengePicker && eventName == "Construct") InstallChallengeSettingsHooksAfterClick();
            if (challengeLobby && eventName == "Construct") InstallChallengePickerHooksAfterClick();
            f13::challenges::nativeui::AfterEvent(object, className, eventName);
        }
        else
        {
            if (challengeSettings && eventName == "OnClicked_Back" &&
                object == g_ChallengeSetupPage && ChallengeObjectIdentity(object) == g_ChallengeSetupPageIdentity)
                return; // This Back returns to the challenge lobby, not out of the route.
            const bool back = (challengeSettings || challengePicker) && eventName == "OnClicked_Back";
            const bool otherRoute = offlinePlay && !actualClick &&
                eventName.find("OnClicked__DelegateSignature") != std::string::npos;
            if (back || otherRoute)
            {
                f13::challenges::runtime::Reset();
                g_ChallengeRouteActive.store(false);
            }
            f13::challenges::nativeui::BeforeEvent(object, className, eventName);
        }
    }

    void PumpChallengeRuntimeOnGameThread()
    {
        if (!g_ChallengeRouteActive.load())
        {
            // Ordinary actor ReceiveTick events need only the route flag; no
            // repeated restore/reset work once the challenge state is retired.
            if (g_ChallengeJasonBoundary.WorldIdentity) RestoreChallengeJasonBoundary();
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (now < g_ChallengeNextPumpAt) return;
        g_ChallengeNextPumpAt = now + 100;
        UObject* mode = ChallengeCurrentGameMode();
        if (!mode || !mode->Class) { RestoreChallengeJasonBoundary(); return; } // Travel.
        if (GetActiveFrontendMenu() == mode) { RestoreChallengeJasonBoundary(); return; }
        if (!ClassDerivesFrom(mode->Class, "SCGameMode_SPChallenges"))
        {
            RestoreChallengeJasonBoundary();
            // Invalidate the physical escape roster on exit as well as the
            // runtime flag. It must never survive into offline bot worlds.
            g_ChallengeBoundaryWorld = 0;
            f13::challenges::runtime::Reset();
            f13::challenges::nativeui::Reset();
            g_ChallengeRouteActive.store(false);
            Logger::Success("CHALLENGE ADDON: route disarmed on nonchallenge world");
            return;
        }
        using namespace f13::challenges;
        runtime::Scope scope{};
        UWorld* world = Engine::GetWorld();
        UObject* controller = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
        scope.world = reinterpret_cast<uintptr_t>(world);
        scope.controller = reinterpret_cast<uintptr_t>(controller);
        scope.stockChallengeMission = true;
        UObject* pawn = nullptr;
        if (controller && ChallengeReadObjectField(controller, "Pawn", &pawn)) scope.primaryJason = pawn;
        UObject* netDriver = nullptr;
        UObject* netConnection = nullptr;
        const bool networkFieldsVerified = ChallengeReadObjectField(reinterpret_cast<UObject*>(world), "NetDriver", &netDriver) &&
            ChallengeReadObjectField(controller, "NetConnection", &netConnection);
        scope.networkSession = !networkFieldsVerified || netDriver || netConnection;
        // Role at 0x110 is proved by the exact native refill signature before
        // configuring this addon; no client/non-authority pawn can be refilled.
        scope.localAuthority = pawn && Memory::IsReadable(pawn, 0x111) &&
            *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(pawn) + 0x110) >= 3;
        UObject* state = world && Memory::IsReadable(world, 0x100)
            ? *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0xF8) : nullptr;
        scope.gameState = state;
        bool preMatchIntro = false;
        if (state && Memory::IsReadable(state, sizeof(UObject)) && state->Class &&
            ClassDerivesFrom(state->Class, "SCGameState_SPChallenges"))
        {
            UPropertyLite* phase = FindPropertyInHierarchyByName(state->Class, "MatchState");
            if (phase && phase->Offset_Internal > 0 && phase->Offset_Internal < 0x10000 &&
                phase->ElementSize == sizeof(FName) && phase->ArrayDim == 1 &&
                SafeName(reinterpret_cast<UObject*>(phase->ClassPrivate)) == "NameProperty")
            {
                const FName* name = reinterpret_cast<const FName*>(reinterpret_cast<uintptr_t>(state) + phase->Offset_Internal);
                if (Memory::IsReadable(name, sizeof(FName)) && GNames && GNames->IsValidIndex(name->ComparisonIndex))
                {
                    const FNameEntry* entry = GNames->GetById(name->ComparisonIndex);
                    scope.inProgress = entry && Memory::IsReadable(entry, sizeof(FNameEntry)) &&
                        memchr(entry->AnsiName, 0, sizeof(entry->AnsiName)) && strcmp(entry->AnsiName, "InProgress") == 0;
                    preMatchIntro = entry && Memory::IsReadable(entry, sizeof(FNameEntry)) &&
                        memchr(entry->AnsiName, 0, sizeof(entry->AnsiName)) && strcmp(entry->AnsiName, "PreMatchIntro") == 0;
                }
            }
        }
        if (preMatchIntro && !scope.networkSession && world && controller)
            PumpChallengeStartIntroSkip(world);
        static uintptr_t loggedScopeWorld = 0;
        if (scope.inProgress && loggedScopeWorld != scope.world)
        {
            loggedScopeWorld = scope.world;
            Logger::Success("CHALLENGE ADDON: mission scope | nativeChallenge=Y | primaryJason=" +
                std::to_string(scope.primaryJason != nullptr) + " | localAuthority=" + std::to_string(scope.localAuthority) +
                " | networkFieldsVerified=" + std::to_string(networkFieldsVerified) +
                " | networkSession=" + std::to_string(scope.networkSession) +
                " | committed=" + std::to_string(nativeui::HasCommittedSettings()));
        }
        if (scope.inProgress && !AuthorizeChallengeMissionSettings(mode))
        {
            RestoreChallengeJasonBoundary();
            runtime::Reset();
            g_ChallengeBoundaryWorld = 0;
            g_ChallengeCombatPawn.store(0);
            return;
        }
        runtime::Tick(scope, nativeui::GetCommittedSettings(), nativeui::HasCommittedSettings());
        PumpChallengeJasonBoundary(scope);
        PumpChallengeEscapeBoundary(scope);
        if (scope.inProgress && scope.localAuthority && !scope.networkSession &&
            ChallengeIsCurrentJason(pawn, scope.world, scope.controller))
        {
            const uint64_t identity = ChallengeObjectIdentity(pawn);
            if (identity && (identity != g_ChallengeCombatIdentity || scope.world != g_ChallengeCombatWorld))
            {
                g_ChallengeCombatPawn.store(reinterpret_cast<uintptr_t>(pawn));
                g_ChallengeCombatIdentity = identity;
                g_ChallengeCombatWorld = scope.world;
                g_ChallengeCombatReports = 0;
                g_ChallengeAttackInputs = 0;
                g_ChallengeCombatNextReport = 0;
                g_ChallengeSlashCooldownBefore = 0;
                LogChallengeCombatGates(pawn, "initial native gate snapshot", -1);
                RecordChallengeReplayReady(mode);
            }
        }
        else g_ChallengeCombatPawn.store(0);
        RepairMissingChallengeWeapon(scope);
    }

    void ApplyCommittedOfflineDifficultyBeforeStart(void* gameModeValue)
    {
#if defined(F13_BASE_GAME_PORT)
        ApplyNativeOfflineDamageSettings(reinterpret_cast<UObject*>(gameModeValue));
#endif
    }

    void ApplyCommittedJasonDifficultyToPawn(void* pawnValue)
    {
#if defined(F13_BASE_GAME_PORT)
        UWorld* world = Engine::GetWorld();
        const uintptr_t worldValue = reinterpret_cast<uintptr_t>(world);
        const int32_t difficulty = g_CommittedJasonDifficulty.load();
        UObject* pawn = reinterpret_cast<UObject*>(pawnValue);
        if (!worldValue || worldValue != g_CommittedSettingsWorld.load() ||
            difficulty < 0 || difficulty > 2 || !pawn ||
            !Memory::IsReadable(pawn, sizeof(UObject)) || !pawn->Class ||
            !ClassDerivesFrom(pawn->Class, "SCKillerCharacter"))
            return;
        if (g_DifficultyPawnWorld != worldValue)
        {
            g_DifficultyPawnWorld = worldValue;
            g_DifficultyPawns.clear();
        }
        const uintptr_t pawnAddress = reinterpret_cast<uintptr_t>(pawn);
        if (std::find(g_DifficultyPawns.begin(), g_DifficultyPawns.end(), pawnAddress) != g_DifficultyPawns.end())
            return;
        float* threshold = reinterpret_cast<float*>(RuntimeScalarAddress(
            pawn, "RageUnlockTime", "FloatProperty", sizeof(float)));
        if (!threshold || !std::isfinite(*threshold) || *threshold <= 0 || *threshold > 86400)
        {
            Logger::Error("OFFLINE JASON DIFFICULTY: native Rage threshold unavailable on " + SafeName(pawn));
            return;
        }
        const float original = *threshold;
        const float scale = difficulty == 0 ? 2.0f : difficulty == 2 ? 0.5f : 1.0f;
        *threshold = original * scale;
        g_DifficultyPawns.push_back(pawnAddress);
        Logger::Success("OFFLINE RAGE THRESHOLD APPLIED: pawn=" + SafeName(pawn) +
            " | " + std::to_string(original) + "->" + std::to_string(*threshold) +
            " | timer/starting Rage state unchanged | same AI");
#endif
    }

    float GetCommittedJasonDamageScale(void* world)
    {
        if (!world || reinterpret_cast<uintptr_t>(world) != g_CommittedSettingsWorld.load() ||
            !g_OfflineModifiersApplied.load()) return 0.0f;
        const int32_t difficulty = g_CommittedJasonDifficulty.load();
        return difficulty == 0 ? 0.5f : difficulty == 1 ? 1.0f : difficulty == 2 ? 2.0f : 0.0f;
    }

    int32_t GetCommittedAdditionalStartingCounselors()
    {
        const int32_t total = g_NativeSelectedCounselorTotal.load();
        return g_CounselorMenuRouteLatched.load() && total > 7 && total <= 10 ? total - 7 : 0;
    }

    void BeginCommittedOfflineSettingsAfterStart(void* gameModeValue)
    {
#if defined(F13_BASE_GAME_PORT)
        UWorld* world = Engine::GetWorld();
        if (!gameModeValue || !world || !Memory::IsReadable(world, sizeof(UWorld)) ||
            reinterpret_cast<uintptr_t>(world) == g_WeatherTravelSourceWorld.load())
            return;
        g_CommittedSettingsWorld.store(reinterpret_cast<uintptr_t>(world));
        g_DifficultyPawnWorld = reinterpret_cast<uintptr_t>(world);
        g_DifficultyPawns.clear();
        UObject* gameState = reinterpret_cast<UObject*>(world->GameState);
        const bool stockNormalized = ApplyNativeOfflineDamageSettings(reinterpret_cast<UObject*>(gameModeValue));
        g_OfflineModifiersApplied.store(stockNormalized && FrozenJasonBridge::InstallOfflineDamageBridge());
        ApplyCommittedJasonDifficultyToPawn(ReadRuntimeWidgetObject(gameState, "CurrentKiller"));
        g_WeatherApplyStartedAt.store(GetTickCount64());
        g_NextWeatherApplyAt.store(0);
        g_OfflineWeatherApplyPending.store(g_CommittedOfflineRain.load() >= 0);
        ApplyPendingOfflineSettingsOnGameThread();
#endif
    }

    void ApplyCommittedMatchDurationBeforeStart(void* gameModeValue)
    {
#if defined(F13_BASE_GAME_PORT)
        UObject* gameMode = reinterpret_cast<UObject*>(gameModeValue);
        if (!gameMode || !Memory::IsReadable(gameMode, sizeof(UObject)) ||
            !gameMode->Class)
            return;
        // The hosted online Hunt mode has no Offline Bots setup screen.
        // Set its authoritative round timer before GameState initialization;
        // do not consume or alter the offline 30/60/90-minute selection.
        const bool hostedHunt =
            ClassDerivesFrom(gameMode->Class, "SCGameMode_Hunt") &&
            !ClassDerivesFrom(gameMode->Class, "SCGameMode_OfflineBots") &&
            !ClassDerivesFrom(gameMode->Class, "SCGameMode_Sandbox");
        const int32_t selectedSeconds = g_CommittedMatchSeconds.load();
        // Preserve the established counselor fallback if setup construction
        // failed; never replace an untouched stock Jason timer in that case.
        const int32_t seconds = hostedHunt ? 1800 :
            selectedSeconds == -2 ? -1 : selectedSeconds >= 0 ? selectedSeconds :
            (g_CounselorMenuRouteLatched.load() ? 3600 : -1);
        if (seconds != -1 && seconds != 1800 && seconds != 3600 && seconds != 5400)
            return;
        if (seconds == -1 && selectedSeconds != -2)
            return; // No selection: preserve the stock Jason match timer.
        UPropertyLite* property = FindPropertyInHierarchyByName(
            gameMode->Class, "RoundTime");
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 ||
            property->ElementSize != sizeof(int32_t) ||
            SafeName(reinterpret_cast<UObject*>(reinterpret_cast<UObject*>(property)->Class)) != "IntProperty")
        {
            Logger::Error("OFFLINE MATCH LENGTH: native RoundTime property unavailable; timer unchanged");
            return;
        }
        int32_t* roundTime = reinterpret_cast<int32_t*>(
            reinterpret_cast<uintptr_t>(gameMode) + property->Offset_Internal);
        if (!Memory::IsReadable(roundTime, sizeof(int32_t)))
            return;
        const int32_t previous = *roundTime;
        *roundTime = seconds;
        Logger::Success(std::string(hostedHunt
            ? "HOSTED MATCH LENGTH APPLIED: "
            : "OFFLINE MATCH LENGTH APPLIED: ") +
            std::to_string(previous) + " -> " + std::to_string(seconds) +
            " seconds before native GameState initialization | route=" +
            (hostedHunt ? "Online Hunt" :
                g_CounselorMenuRouteLatched.load() ? "Counselor" : "Jason"));
#endif
    }

    void PumpCounselorSkillDisplayOnGameThread()
    {
#if defined(F13_BASE_GAME_PORT)
        if (g_GameSetupSelectionLocked.load())
            return;
        UObject* combo = reinterpret_cast<UObject*>(
            g_ActiveSkillCombo.load());
        if (!combo || !Memory::IsReadable(combo, sizeof(UObject)))
            return;
        const ULONGLONG now = GetTickCount64();
        if (now < g_NextSkillDisplayPumpAt.load())
            return;
        g_NextSkillDisplayPumpAt.store(now + 50);
        RefreshCounselorSkillValue(combo);
#endif
    }

    void VerifyPrimaryKillerOwnerAfterReinforcement()
    {
#if defined(F13_BASE_GAME_PORT)
        if (!IsCounselorMatchInProgress())
            return;
        UObject* primary = reinterpret_cast<UObject*>(
            g_PrimaryKillerPlayerState.load());
        UWorld* world = Engine::GetWorld();
        UObject* gameState = world
            ? reinterpret_cast<UObject*>(world->GameState)
            : nullptr;
        if (!primary || !Memory::IsReadable(primary, sizeof(UObject)) ||
            !gameState ||
            !Memory::IsReadable(gameState, sizeof(UObject)) ||
            !gameState->Class)
        {
            Logger::Error(
                "F1 primary killer owner invariant unavailable after reinforcement");
            return;
        }
        UPropertyLite* property = FindPropertyInHierarchyByName(
            gameState->Class, "CurrentKillerPlayerState");
        if (!property || property->Offset_Internal <= 0 ||
            property->Offset_Internal >= 0x10000 ||
            property->ElementSize != sizeof(UObject*))
        {
            Logger::Error(
                "F1 primary killer owner property unavailable after reinforcement");
            return;
        }
        UObject** owner = reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(gameState) +
            property->Offset_Internal);
        if (!Memory::IsReadable(owner, sizeof(UObject*)))
            return;
        const bool displaced = *owner != primary;
        if (displaced)
            *owner = primary;
        Logger::Success(
            std::string("F1 primary killer owner after reinforcement | displaced=") +
            (displaced ? "true" : "false") +
            " | restored=" +
            ((*owner == primary) ? "true" : "false"));
#endif
    }

    void TranslateCounselorSkillDisplayText(
        void* objectValue, void* functionValue, void* params)
    {
#if defined(F13_BASE_GAME_PORT)
        if (!objectValue || !functionValue || !params ||
            g_GameSetupSelectionLocked.load() ||
            reinterpret_cast<uintptr_t>(objectValue) !=
                g_ActiveSkillValueText.load())
            return;

        auto* function = reinterpret_cast<UFunction*>(functionValue);
        UObject* combo = reinterpret_cast<UObject*>(
            g_ActiveSkillCombo.load());
        if (!combo || !Memory::IsReadable(combo, sizeof(UObject)) ||
            !combo->Class ||
            !Memory::IsReadable(function, sizeof(UFunction)) ||
            SafeName(reinterpret_cast<UObject*>(function)) != "SetText")
            return;

        UPropertyLite* indexProperty = FindPropertyInHierarchyByName(
            combo->Class, "CurrentOptionIndex");
        if (!indexProperty || indexProperty->Offset_Internal <= 0 ||
            indexProperty->Offset_Internal >= 0x10000 ||
            indexProperty->ElementSize != sizeof(int32_t))
            return;
        const int32_t* index = reinterpret_cast<const int32_t*>(
            reinterpret_cast<uintptr_t>(combo) +
            indexProperty->Offset_Internal);
        if (!Memory::IsReadable(index, sizeof(int32_t)) ||
            *index < 0 || *index > 2)
            return;

        UPropertyLite* textProperty = FindPropertyInStructByName(
            reinterpret_cast<UStruct*>(function), "InText");
        if (!textProperty)
            textProperty = FindFunctionPropertyByType(
                function, "TextProperty", false);
        if (!textProperty || textProperty->Offset_Internal < 0 ||
            textProperty->ElementSize <= 0 ||
            textProperty->Offset_Internal +
                textProperty->ElementSize > function->Size ||
            function->Size > 0x200)
            return;

        static PersistentRuntimeText values[3]{};
        static constexpr const wchar_t* labels[3] = {
            L"Low", L"Medium", L"High" };
        if (!values[*index].Value &&
            !CreatePersistentRuntimeText(labels[*index], &values[*index]))
            return;
        if (!values[*index].Value ||
            values[*index].Size != textProperty->ElementSize)
            return;

        // Replace only the text argument of the native ValueText update.
        // The stock OptionList and CurrentOptionIndex stay untouched, so
        // left/right input and game difficulty continue using indices 0/1/2.
        memcpy(reinterpret_cast<uint8_t*>(params) +
                textProperty->Offset_Internal,
            values[*index].Value, values[*index].Size);
        static int32_t lastIndex = -1;
        if (lastIndex != *index)
        {
            lastIndex = *index;
            Logger::Success("Game Setup Counselor Bot Skill selected=" +
                std::string(*index == 0 ? "Low" :
                    *index == 1 ? "Medium" : "High") +
                " | nativeIndex=" + std::to_string(*index));
        }
#else
        (void)objectValue;
        (void)functionValue;
        (void)params;
#endif
    }

    void* StockJasonSelectionSaveFallback(void* requestedClass)
    {
#if !defined(F13_BASE_GAME_PORT)
        (void)requestedClass;
        return nullptr;
#else
        if (g_CounselorMenuRouteLatched.load() ||
            !ResolveSelectionSaveMetadata())
            return nullptr;

        UClass* selectionClass = reinterpret_cast<UClass*>(
            g_SelectionSaveClass.load());
        if (!selectionClass || requestedClass != selectionClass ||
            !Memory::IsReadable(selectionClass, sizeof(UClass)))
            return nullptr;

        UObject* fallback = selectionClass->DefaultObject;
        if (!fallback ||
            !Memory::IsReadable(fallback, sizeof(UObject)) ||
            fallback->Class != selectionClass ||
            SafeName(fallback) != "Default__SCCharacterSelectionsSaveGame")
        {
            fallback = FindObjectExact(
                "Default__SCCharacterSelectionsSaveGame");
            if (!fallback ||
                !Memory::IsReadable(fallback, sizeof(UObject)) ||
                fallback->Class != selectionClass)
                return nullptr;
        }

        Logger::Success(
            "STOCK JASON INTRO: missing selection save; using the game's own class default for this one lookup");
        return fallback;
#endif
    }

    void ObserveStockKillerClassRequest(
        void* objectValue,
        void* functionValue,
        void* paramsValue)
    {
#if defined(F13_BASE_GAME_PORT)
        if (!g_CounselorMenuRouteLatched.load() ||
            g_CounselorBirthComplete.load() || !paramsValue)
            return;
        UObject* object = reinterpret_cast<UObject*>(objectValue);
        UFunction* function = reinterpret_cast<UFunction*>(functionValue);
        if (!object || !function ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !Memory::IsReadable(function, sizeof(UFunction)) ||
            !object->Class ||
            !Memory::IsReadable(object->Class, sizeof(UClass)))
            return;

        const std::string functionName = SafeName(
            reinterpret_cast<UObject*>(function));
        if (functionName != "RequestKillerClass" &&
            functionName != "RequestKiller")
            return;

        constexpr uint64_t CPF_Parm = 0x80ull;
        constexpr uint64_t CPF_ReturnParm = 0x400ull;
        uint8_t* params = reinterpret_cast<uint8_t*>(paramsValue);
        for (UField* field = function->Children;
             field && Memory::IsReadable(field, sizeof(UField));
             field = field->Next)
        {
            UPropertyLite* property = reinterpret_cast<UPropertyLite*>(field);
            if (!Memory::IsReadable(property, sizeof(UPropertyLite)) ||
                (property->PropertyFlags & CPF_Parm) == 0 ||
                (property->PropertyFlags & CPF_ReturnParm) != 0 ||
                property->Offset_Internal < 0 ||
                property->Offset_Internal + property->ElementSize >
                    static_cast<int32_t>(function->Size))
                continue;

            UObject* typeObject = reinterpret_cast<UObject*>(
                field->ClassPrivate);
            const std::string typeName = typeObject &&
                Memory::IsReadable(typeObject, sizeof(UObject))
                ? SafeName(typeObject) : "<unknown>";
            UClass* chosen = nullptr;
            if (property->ElementSize == LifecycleSoftClassSize &&
                (typeName == "SoftClassProperty" ||
                 typeName == "SoftObjectProperty"))
                chosen = LoadLifecycleSoftClass(
                    params + property->Offset_Internal);
            else if (property->ElementSize == sizeof(UClass*) &&
                     (typeName == "ClassProperty" ||
                      typeName == "ObjectProperty"))
                std::memcpy(&chosen,
                    params + property->Offset_Internal, sizeof(chosen));

            const std::string chosenName = chosen &&
                Memory::IsReadable(chosen, sizeof(UClass))
                ? SafeName(reinterpret_cast<UObject*>(chosen))
                : "<unresolved>";
            Logger::Success(
                "STOCK KILLER REQUEST: " + functionName +
                " parameter=" + SafeName(
                    reinterpret_cast<UObject*>(field)) +
                " type=" + typeName +
                " class=" + chosenName);
            if (chosen && Memory::IsReadable(chosen, sizeof(UClass)) &&
                ClassDerivesFrom(chosen, "SCKillerCharacter"))
            {
                g_TargetJasonClass.store(
                    reinterpret_cast<uintptr_t>(chosen));
                g_StockKillerRequestCaptured.store(true);
            }
        }
#else
        (void)objectValue;
        (void)functionValue;
        (void)paramsValue;
#endif
    }

    void TraceStockJasonPickerEvent(
        void* objectValue,
        void* functionValue)
    {
#if defined(F13_BASE_GAME_PORT)
        if (!g_CounselorMenuRouteLatched.load() ||
            g_CounselorBirthComplete.load())
            return;

        UObject* object = reinterpret_cast<UObject*>(objectValue);
        UFunction* function = reinterpret_cast<UFunction*>(functionValue);
        if (!object || !function ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !Memory::IsReadable(function, sizeof(UFunction)) ||
            !object->Class ||
            !Memory::IsReadable(object->Class, sizeof(UClass)) ||
            SafeName(reinterpret_cast<UObject*>(object->Class)) !=
                "Jason_Select_Widget_C")
            return;

        const std::string eventName =
            SafeName(reinterpret_cast<UObject*>(function));
        if (eventName == "ReceiveTick")
            return;
        static std::string seen[32];
        static int32_t seenCount = 0;
        for (int32_t i = 0; i < seenCount; ++i)
            if (seen[i] == eventName)
                return;
        if (seenCount >= 32)
            return;
        seen[seenCount++] = eventName;

        std::string savedPickName = "<unavailable>";
        UObject* selections = reinterpret_cast<UObject*>(
            g_SelectionSaveObject.load());
        const int32_t pickOffset = g_KillerPickOffset.load();
        if (selections && pickOffset > 0 && pickOffset < 0x10000 &&
            Memory::IsReadable(selections, sizeof(UObject)))
        {
            UClass** savedPick = reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(selections) + pickOffset);
            if (Memory::IsReadable(savedPick, sizeof(UClass*)) &&
                *savedPick && Memory::IsReadable(*savedPick, sizeof(UClass)))
                savedPickName = SafeName(
                    reinterpret_cast<UObject*>(*savedPick));
        }
        Logger::Debug(
            "STOCK JASON PICKER EVENT: " + eventName +
            " | savedKillerPick=" + savedPickName);
#else
        (void)objectValue;
        (void)functionValue;
#endif
    }

    bool SetMapIndex(int32_t value)
    {
        g_SelectedMapIndex.store(ClampMapIndex(value));
        return true;
    }

    bool SetJasonIndex(int32_t value)
    {
        g_SelectedJasonIndex.store(ClampJasonIndex(value));
        return true;
    }

    bool SetPlayerCounselorIndex(int32_t value)
    {
        g_SelectedPlayerCounselorIndex.store(
            ClampPlayerCounselorIndex(value));
        return true;
    }

    bool SetDifficulty(int32_t value)
    {
        g_SelectedDifficulty.store(ClampDifficulty(value));
        return true;
    }

    bool SetCounselorCount(int32_t value)
    {
        g_SelectedCounselorCount.store(ClampCounselorCount(value));
        return true;
    }

    bool SetWeather(int32_t value)
    {
        g_SelectedWeather.store(ClampWeather(value));
        return true;
    }

    bool QueueArmSelectedSetup()
    {
        if (g_ArmPresetRequested.exchange(true))
        {
            Logger::Debug(
                "Counselor mode 18K: setup arm already queued");
            return false;
        }

        Logger::Debug(
            "Counselor mode 18K: selected map + Sandbox counselor mode arm queued");
        return true;
    }

    bool QueueApplyGameSetupPreset()
    {
#if defined(F13_BASE_GAME_PORT)
        // This legacy F5 preset assumes a Resurrected-only WeatherComboBox
        // at a fixed offset. The packed stock widget has no such child.
        // Never write that foreign layout while native Game Setup is live.
        Logger::Error(
            "Stock Game Setup preset unavailable: weather control is absent from this cooked widget; native selections are preserved");
        return false;
#else
        if (g_ApplyGameSetupRequested.exchange(true))
        {
            Logger::Debug(
                "Unified setup 18B: Game Setup apply already queued");
            return false;
        }

        Logger::Debug(
            "Unified setup 18B: selected Game Setup apply queued");
        return true;
#endif
    }

    bool QueueSelectedCounselor()
    {
        int stage =
            g_CounselorStage.load();

        if (stage != (int)CounselorSelectStage::Idle &&
            stage != (int)CounselorSelectStage::Done &&
            stage != (int)CounselorSelectStage::Failed)
        {
            Logger::Debug(
                "Counselor selection 18K: request already active");
            return false;
        }

        g_TargetPlayerCounselorClass.store(0);
        g_CounselorSelectionWorld.store(0);
        g_CounselorPlayerState.store(0);
        g_PickedCounselorClassOffset.store(-1);
        g_CounselorRequestSubmittedAt.store(0);
        g_CounselorPickedConfirmed.store(false);
        g_CounselorInitialMismatchLogged.store(false);
        g_CounselorNaturalClassScanDone.store(false);
        g_CounselorSaveScanAttempted.store(false);
        g_CounselorFallbackApplied.store(false);
        g_CounselorStockPickLogged.store(false);
        g_CounselorSoftStickyRewriteCount.store(0);
        g_CounselorClassStickyRewriteCount.store(0);
        g_SelectedPlayerCounselorSoftClass.fill(0);
        g_SelectedPlayerCounselorPath.clear();

        g_CounselorStage.store(
            (int)CounselorSelectStage::NeedRequest);

        int32_t index =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        Logger::Debug(
            "Counselor selection 18K: selected counselor queued | index=" +
            std::to_string(index) +
            " | class=" +
            kPlayerCounselorClassNames[index] +
            " | next=SCPlayerState::RequestCounselorClass");

        return true;
    }

    bool QueueSelectedJason()
    {
        int expected = (int)SetupStage::Idle;
        if (!g_Stage.compare_exchange_strong(
            expected,
            (int)SetupStage::NeedPreload))
        {
            if (expected == (int)SetupStage::Done ||
                expected == (int)SetupStage::Failed)
            {
                g_SelectionSaveObject.store(0);
                g_TargetJasonClass.store(0);
                g_LastTargetScanAt.store(0);
                g_PreloadAt.store(0);
                g_SelectionWorld.store(0);
                g_StickyRewriteCount.store(0);
                g_BackgroundScanStarted.store(false);
                g_Stage.store((int)SetupStage::NeedPreload);
            }
            else
            {
                Logger::Debug(
                    "Jason resolve 18B request already active");
                return false;
            }
        }

        Logger::Debug(
            "Jason resolve 18B: selected Jason queued for game-thread conversion | index=" +
            std::to_string(ClampJasonIndex(g_SelectedJasonIndex.load())));
        return true;
    }

    bool QueueDumpCounselorRoster()
    {
        bool expected = false;
        if (!g_DumpCounselorRosterRequested.compare_exchange_strong(
            expected,
            true))
        {
            Logger::Debug(
                "Counselor discovery 18F: roster dump already queued");
            return false;
        }

        Logger::Debug(
            "Counselor discovery 18F: roster dump queued");
        return true;
    }

    bool QueueSandboxCounselorSync()
    {
        int stage = g_SandboxCounselorSyncStage.load();
        if (stage == (int)SandboxCounselorSyncStage::NeedRequest ||
            stage == (int)SandboxCounselorSyncStage::Waiting)
            return true;

        g_SandboxSyncWorld.store(0);
        g_SandboxSyncPlayerState.store(0);
        g_SandboxSpawnedClassOffset.store(-1);
        g_SandboxNextCharacterAttempts.store(0);
        g_SandboxNextCharacterAt.store(0);
        g_SandboxCounselorSyncStage.store((int)SandboxCounselorSyncStage::NeedRequest);

        int32_t index = ClampPlayerCounselorIndex(g_SelectedPlayerCounselorIndex.load());
        Logger::Debug(
            "Counselor mode 18K: Sandbox local-character sync queued | target=" +
            std::string(kPlayerCounselorClassNames[index]));
        return true;
    }

    bool IsCounselorMenuRouteLatched()
    {
        return g_CounselorMenuRouteLatched.load();
    }

    static bool BirthSelectedCounselorForPreMatchLegacy(void* gameModeValue)
    {
        if (!g_CounselorMenuRouteLatched.load())
            return false;

        if (g_CounselorBirthComplete.load())
            return true;

        UObject* gameMode = reinterpret_cast<UObject*>(gameModeValue);
        UWorld* world = Engine::GetWorld();
        APlayerController* controller = Engine::GetLocalPlayerController();

        if (!gameMode ||
            !Memory::IsReadable(gameMode, sizeof(UObject)) ||
            !gameMode->Class ||
            !world ||
            !controller ||
            !Memory::IsReadable(controller, sizeof(UObject)) ||
            !controller->Class)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: GameMode/world/local controller is unavailable");
            return false;
        }

        const std::string gameModeClass = SafeName((UObject*)gameMode->Class);
        const std::string controllerClass = SafeName((UObject*)controller->Class);

        if (gameModeClass != "SCGameMode_OfflineBots" ||
            controllerClass.find("OfflineBots") == std::string::npos)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: unexpected infrastructure | GameMode=" +
                gameModeClass + " | Controller=" + controllerClass);
            return false;
        }

        const int32_t counselorIndex =
            ClampPlayerCounselorIndex(
                g_SelectedPlayerCounselorIndex.load());

        UClass* counselorClass = reinterpret_cast<UClass*>(
            g_TargetPlayerCounselorClass.load());

        if (!counselorClass ||
            !Memory::IsReadable(counselorClass, sizeof(UClass)) ||
            SafeName((UObject*)counselorClass) !=
                kPlayerCounselorClassNames[counselorIndex])
        {
            counselorClass = FindClassExact(
                kPlayerCounselorClassNames[counselorIndex]);
        }

        // Tommy and uncached roster entries may still be represented only by
        // the 40-byte soft class selected in the frontend. Resolve that exact
        // native value synchronously before RestartPlayer.
        if ((!counselorClass ||
                !Memory::IsReadable(counselorClass, sizeof(UClass))) &&
            !g_SelectedPlayerCounselorPath.empty())
        {
            UClass* kismetClass = FindClassExact("KismetSystemLibrary");
            UFunction* convertFunction = kismetClass
                ? FindFunctionInHierarchyByName(
                    kismetClass,
                    "Conv_SoftClassReferenceToClass")
                : nullptr;

            if (kismetClass &&
                kismetClass->DefaultObject &&
                convertFunction)
            {
                struct ResolveParams
                {
                    uint8_t SoftClass[SoftClassSize];
                    UClass* ReturnValue;
                };

                static_assert(sizeof(ResolveParams) == 48,
                    "Counselor ResolveParams must be 48 bytes");

                ResolveParams params{};
                memcpy(
                    params.SoftClass,
                    g_SelectedPlayerCounselorSoftClass.data(),
                    SoftClassSize);

                if (SafeProcessEventCall(
                        (uintptr_t)kismetClass->DefaultObject,
                        kismetClass->DefaultObject,
                        convertFunction,
                        &params))
                {
                    counselorClass = params.ReturnValue;
                }
            }
        }

        if (!counselorClass ||
            !Memory::IsReadable(counselorClass, sizeof(UClass)))
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: selected counselor class is not resident/resolvable | target=" +
                std::string(kPlayerCounselorClassNames[counselorIndex]));
            return false;
        }

        g_TargetPlayerCounselorClass.store((uintptr_t)counselorClass);

        // Match the donor's HandlePreMatchIntro ordering: make the selected
        // counselor the pawn class first, then RestartPlayer while no human
        // Jason pawn has ever been created.
        UPropertyLite* defaultPawnProperty =
            FindPropertyInHierarchyByName(
                gameMode->Class,
                "DefaultPawnClass");

        if (!defaultPawnProperty ||
            defaultPawnProperty->Offset_Internal <= 0 ||
            defaultPawnProperty->Offset_Internal >= 0x10000 ||
            defaultPawnProperty->ElementSize < (int32_t)sizeof(UClass*))
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: DefaultPawnClass property is unavailable");
            return false;
        }

        UClass** defaultPawnSlot = reinterpret_cast<UClass**>(
            reinterpret_cast<uintptr_t>(gameMode) +
            defaultPawnProperty->Offset_Internal);

        if (!Memory::IsReadable(defaultPawnSlot, sizeof(UClass*)))
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: DefaultPawnClass slot is unreadable");
            return false;
        }

        *defaultPawnSlot = counselorClass;

        UPropertyLite* playerStateProperty =
            FindPropertyInHierarchyByName(
                controller->Class,
                "PlayerState");

        UObject* playerState = nullptr;

        if (playerStateProperty &&
            playerStateProperty->Offset_Internal > 0 &&
            playerStateProperty->Offset_Internal < 0x10000)
        {
            UObject** playerStateSlot = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(controller) +
                playerStateProperty->Offset_Internal);

            if (Memory::IsReadable(playerStateSlot, sizeof(UObject*)))
                playerState = *playerStateSlot;
        }

        if (!playerState ||
            !Memory::IsReadable(playerState, sizeof(UObject)) ||
            !playerState->Class)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: local SCPlayerState_OfflineBots is unavailable");
            return false;
        }

        auto writeSelectedSoftClass =
            [&](const char* propertyName) -> bool
        {
            UPropertyLite* property =
                FindPropertyInHierarchyByName(
                    playerState->Class,
                    propertyName);

            if (!property ||
                property->Offset_Internal <= 0 ||
                property->Offset_Internal >= 0x10000 ||
                property->ElementSize != SoftClassSize)
            {
                return false;
            }

            uint8_t* destination = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(playerState) +
                property->Offset_Internal);

            if (!Memory::IsReadable(destination, SoftClassSize))
                return false;

            memcpy(
                destination,
                g_SelectedPlayerCounselorSoftClass.data(),
                SoftClassSize);
            return true;
        };

        const bool pickedWritten =
            writeSelectedSoftClass("PickedCounselorClass");
        const bool activeWritten =
            writeSelectedSoftClass("ActiveCharacterClass");

        UFunction* restartPlayer =
            FindFunctionInHierarchyByName(
                gameMode->Class,
                "RestartPlayer");

        if (!restartPlayer)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: RestartPlayer UFunction is unavailable");
            return false;
        }

        alignas(16) uint8_t restartParams[0x40]{};
        int32_t newPlayerOffset = 0;

        for (UField* field = restartPlayer->Children;
            field;
            field = field->Next)
        {
            if (!Memory::IsReadable(field, sizeof(UField)))
                break;

            if (SafeName((UObject*)field) == "NewPlayer")
            {
                UPropertyLite* property =
                    reinterpret_cast<UPropertyLite*>(field);

                if (Memory::IsReadable(property, sizeof(UPropertyLite)) &&
                    property->Offset_Internal >= 0 &&
                    property->Offset_Internal <=
                        (int32_t)(sizeof(restartParams) - sizeof(void*)))
                {
                    newPlayerOffset = property->Offset_Internal;
                }
                break;
            }
        }

        memcpy(
            restartParams + newPlayerOffset,
            &controller,
            sizeof(controller));

        Logger::Success(
            "18L-AD PREMATCH COUNSELOR BIRTH: calling native RestartPlayer before PreMatchIntro completes | Counselor=" +
            SafeName((UObject*)counselorClass) +
            " | PickedSoft=" + (pickedWritten ? "true" : "false") +
            " | ActiveSoft=" + (activeWritten ? "true" : "false"));

        if (!SafeProcessEventCall(
                (uintptr_t)gameMode,
                gameMode,
                restartPlayer,
                restartParams))
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: RestartPlayer ProcessEvent failed");
            return false;
        }

        APawn* bornPawn = controller->AcknowledgedPawn;

        if (!bornPawn ||
            !Memory::IsReadable(bornPawn, sizeof(UObject)) ||
            !bornPawn->Class)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: RestartPlayer returned without an acknowledged pawn");
            return false;
        }

        const std::string bornClass =
            SafeName((UObject*)bornPawn->Class);

        if (bornClass.find("_Counselor_C") == std::string::npos)
        {
            Logger::Error(
                "18L-AD PREMATCH COUNSELOR BIRTH: RestartPlayer produced non-counselor pawn=" +
                bornClass);
            return false;
        }

        InitializeCounselorActiveCamera(
            reinterpret_cast<AActor*>(bornPawn));

        UPropertyLite* spawnedClassProperty =
            FindPropertyInHierarchyByName(
                playerState->Class,
                "SpawnedCharacterClass");

        bool spawnedClassWritten = false;

        if (spawnedClassProperty &&
            spawnedClassProperty->Offset_Internal > 0 &&
            spawnedClassProperty->Offset_Internal < 0x10000 &&
            spawnedClassProperty->ElementSize >= (int32_t)sizeof(UClass*))
        {
            UClass** spawnedClassSlot = reinterpret_cast<UClass**>(
                reinterpret_cast<uintptr_t>(playerState) +
                spawnedClassProperty->Offset_Internal);

            if (Memory::IsReadable(spawnedClassSlot, sizeof(UClass*)))
            {
                *spawnedClassSlot = counselorClass;
                spawnedClassWritten = true;
            }
        }

        // The donor records the human PlayerState as the intro metadata owner
        // even though the human pawn is already a counselor. Preserve that
        // relationship without changing ActiveCharacterClass back to killer.
        UObject* gameState = reinterpret_cast<UObject*>(world->GameState);
        bool introOwnerWritten = false;

        if (gameState &&
            Memory::IsReadable(gameState, sizeof(UObject)) &&
            gameState->Class &&
            SafeName((UObject*)gameState->Class) ==
                "SCGameState_OfflineBots")
        {
            UObject** introOwner = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(gameState) + 0x490);

            if (Memory::IsReadable(introOwner, sizeof(UObject*)))
            {
                *introOwner = playerState;
                introOwnerWritten = true;
            }
        }

        g_CounselorBirthPawn.store((uintptr_t)bornPawn);
        g_CounselorBirthComplete.store(true);

        Logger::Success("SpawnedHumanCounselor=" + bornClass);

        Logger::Success(
            "18L-AD COUNSELOR BIRTH COMPLETE: local player was born as " +
            bornClass +
            " before intro | no human Jason pawn created | SpawnedClass=" +
            (spawnedClassWritten ? "true" : "false") +
            " | IntroOwner=" + (introOwnerWritten ? "true" : "false"));
        return true;
    }

    bool BirthSelectedCounselorForPreMatch(void* gameModeValue)
    {
        if (!g_CounselorMenuRouteLatched.load())
            return false;
        if (g_CounselorBirthComplete.load())
            return true;
        if (!ValidateCounselorLifecycleNativeSurface())
            return false;

        UObject* gameMode = reinterpret_cast<UObject*>(gameModeValue);
        APlayerController* humanController = Engine::GetLocalPlayerController();

        if (!gameMode ||
            !Memory::IsReadable(gameMode, 0x940) ||
            !gameMode->Class ||
            SafeName(reinterpret_cast<UObject*>(gameMode->Class)) !=
                "SCGameMode_OfflineBots" ||
            !humanController ||
            !Memory::IsReadable(humanController, 0x390))
        {
            Logger::Error(
                "18L-AD PREMATCH: dedicated OfflineBots host or human controller is unavailable");
            return false;
        }

        UObject* humanPlayerState = GetControllerPlayerState(humanController);
        if (!humanPlayerState ||
            !Memory::IsReadable(humanPlayerState, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD PREMATCH: human SCPlayerState_OfflineBots is unavailable");
            return false;
        }

#if defined(F13_BASE_GAME_PORT)
        // Offline Bots' second setup screen selects Jason on the lobby
        // PlayerState, not in the Customize save object. Capture that native
        // soft class before replacing ActiveCharacterClass with a counselor.
        if (humanPlayerState->Class)
        {
            for (const char* propertyName : {
                    "ActiveCharacterClass", "PickedKillerClass" })
            {
                UPropertyLite* property = FindPropertyInHierarchyByName(
                    humanPlayerState->Class, propertyName);
                if (!property || property->Offset_Internal <= 0 ||
                    property->Offset_Internal >= 0x10000 ||
                    property->ElementSize != LifecycleSoftClassSize)
                    continue;
                const uint8_t* selectedSoft = reinterpret_cast<uint8_t*>(
                    humanPlayerState) + property->Offset_Internal;
                if (!Memory::IsReadable(selectedSoft, LifecycleSoftClassSize))
                    continue;
                UClass* selectedClass = LoadLifecycleSoftClass(selectedSoft);
                const std::string selectedName = selectedClass &&
                    Memory::IsReadable(selectedClass, sizeof(UClass))
                    ? SafeName(reinterpret_cast<UObject*>(selectedClass))
                    : std::string("<unresolved>");
                Logger::Success(
                    std::string("STOCK PRE-BIRTH JASON PICK: ") +
                    propertyName + "=" + selectedName);
                if (selectedClass &&
                    !g_StockKillerRequestCaptured.load() &&
                    ClassDerivesFrom(selectedClass, "SCKillerCharacter"))
                {
                    g_TargetJasonClass.store(
                        reinterpret_cast<uintptr_t>(selectedClass));
                    break;
                }
            }
        }
#endif

        const uint8_t* counselorSoftClass = nullptr;
        UClass* counselorClass = nullptr;

        if (!g_SelectedPlayerCounselorPath.empty())
        {
            UClass* selected = LoadLifecycleSoftClass(
                g_SelectedPlayerCounselorSoftClass.data());
            if (selected &&
                ClassDerivesFrom(selected, "SCCounselorCharacter"))
            {
                counselorSoftClass =
                    g_SelectedPlayerCounselorSoftClass.data();
                counselorClass = selected;
            }
        }

        if (!counselorSoftClass)
        {
            ResolveNativeSelectedCounselor(
                gameMode,
                &counselorSoftClass,
                &counselorClass);
        }

        if (!counselorSoftClass || !counselorClass)
        {
            Logger::Error(
                "18L-AD PREMATCH: selected counselor soft class is unavailable");
            return false;
        }

        // Donor order: counselor bots first, then the human counselor.
        Logger::Success("PendingMatchCounselor=" + SafeName(reinterpret_cast<UObject*>(counselorClass)));
        const bool allBotsCreated =
            SpawnNativeCounselorBots(gameMode, counselorClass);
        if (!SetLifecycleActiveCharacter(
                humanPlayerState,
                counselorSoftClass))
        {
            Logger::Error("18L-AD PREMATCH: native human counselor class setter failed");
            return false;
        }
        // Apply after native character-selection callbacks, but before birth.
        // Bot births above never receive the human's selected loadout.
        if (!RestartLifecyclePlayer(gameMode, humanController))
        {
            Logger::Error(
                "18L-AD PREMATCH: native human counselor RestartPlayer failed");
            return false;
        }

        APawn* bornPawn = humanController->AcknowledgedPawn;
        if (!bornPawn ||
            !Memory::IsReadable(bornPawn, 0x3A8) ||
            !bornPawn->Class ||
            !ClassDerivesFrom(bornPawn->Class, "SCCounselorCharacter"))
        {
            Logger::Error(
                "18L-AD PREMATCH: human RestartPlayer did not produce a counselor pawn");
            return false;
        }

        InitializeCounselorActiveCamera(
            reinterpret_cast<AActor*>(bornPawn));

        // Stock Hunter spawning normally grants these before the match intro.
        // Our counselor-specific lifecycle supplies the same native loadout
        // immediately after RestartPlayer, once only; failure is logged but
        // does not discard an otherwise valid counselor birth.
#if !defined(F13_BASE_GAME_PORT)
        GrantHunterStartingLoadout(gameMode, bornPawn);
#endif

        UObject* pawnControllerBefore =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(bornPawn) + 0x3A0);

        const uint8_t* killerSoftClass = nullptr;
        UClass* killerClass = nullptr;
        ResolveNativeSelectedKiller(
            gameMode,
            &killerSoftClass,
            &killerClass);

        if (!killerSoftClass ||
            !killerClass ||
            !ClassDerivesFrom(killerClass, "SCKillerCharacter"))
        {
            Logger::Error(
                "18L-AD PREMATCH: selected killer soft class is unavailable for intro metadata");
            return false;
        }

        g_TargetJasonClass.store(reinterpret_cast<uintptr_t>(killerClass));

        bool killerClassAssigned = true;
#if !defined(F13_BASE_GAME_PORT)
        UObject* gameState =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(gameMode) + 0x3C0);
        if (!gameState || !Memory::IsReadable(gameState, 0x560))
        {
            Logger::Error(
                "18L-AD PREMATCH: SCGameState_OfflineBots is unavailable");
            return false;
        }

        *reinterpret_cast<UObject**>(
            reinterpret_cast<uintptr_t>(gameState) + 0x490) =
            humanPlayerState;

        if (!SetLifecycleActiveCharacter(
                humanPlayerState,
                killerSoftClass))
        {
            Logger::Error(
                "18L-AD PREMATCH: killer intro metadata setter failed");
            return false;
        }

        ApplyLifecycleKillerCosmetics(humanPlayerState, killerSoftClass);

        killerClassAssigned =
            DeepAssignLifecycleSoftClass(
                reinterpret_cast<uint8_t*>(gameState) + 0x538,
                killerSoftClass);
#else
        // The stock class reflects the same two intro fields as Resurrected.
        // Resolve them by name and require their exact sizes before borrowing
        // the human killer metadata for the intro. The human pawn remains a
        // counselor; its PlayerState is restored after the AI killer spawns.
        killerClassAssigned = false;
        UObject* stockGameState = reinterpret_cast<UObject*>(
            Engine::GetWorld() ? Engine::GetWorld()->GameState : nullptr);
        UPropertyLite* ownerProperty = stockGameState &&
            stockGameState->Class
            ? FindPropertyInHierarchyByName(
                stockGameState->Class, "CurrentKillerPlayerState")
            : nullptr;
        UPropertyLite* classProperty = stockGameState &&
            stockGameState->Class
            ? FindPropertyInHierarchyByName(
                stockGameState->Class, "KillerClass")
            : nullptr;
        if (stockGameState &&
            Memory::IsReadable(stockGameState, sizeof(UObject)) &&
            ownerProperty && classProperty &&
            ownerProperty->Offset_Internal > 0 &&
            ownerProperty->Offset_Internal < 0x10000 &&
            ownerProperty->ElementSize == sizeof(UObject*) &&
            classProperty->Offset_Internal > 0 &&
            classProperty->Offset_Internal < 0x10000 &&
            classProperty->ElementSize == LifecycleSoftClassSize)
        {
            UObject** introOwner = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(stockGameState) +
                ownerProperty->Offset_Internal);
            uint8_t* introClass = reinterpret_cast<uint8_t*>(
                stockGameState) + classProperty->Offset_Internal;
            if (Memory::IsReadable(introOwner, sizeof(UObject*)) &&
                Memory::IsReadable(introClass, LifecycleSoftClassSize))
            {
                *introOwner = humanPlayerState;
                const bool classWritten = DeepAssignLifecycleSoftClass(
                    introClass, killerSoftClass);
                const bool playerStateWritten =
                    SetLifecycleActiveCharacter(
                        humanPlayerState, killerSoftClass);
                killerClassAssigned = classWritten && playerStateWritten;
                Logger::Success(
                    "STOCK PREMATCH JASON HANDOFF: killer=" +
                    SafeName(reinterpret_cast<UObject*>(killerClass)) +
                    " | class=" + (classWritten ? "true" : "false") +
                    " | playerState=" +
                    (playerStateWritten ? "true" : "false"));
            }
        }
        if (!killerClassAssigned)
            Logger::Error("STOCK PREMATCH JASON HANDOFF: metadata unavailable");
#endif

        APawn* pawnAfterMetadata = humanController->AcknowledgedPawn;
        UObject* pawnControllerAfter =
            pawnAfterMetadata && Memory::IsReadable(pawnAfterMetadata, 0x3A8)
            ? *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(pawnAfterMetadata) + 0x3A0)
            : nullptr;

        if (pawnAfterMetadata != bornPawn ||
            pawnControllerAfter != pawnControllerBefore ||
            pawnControllerAfter != humanController)
        {
            Logger::Error(
                "18L-AD PREMATCH: intro metadata unexpectedly changed human counselor possession");
            return false;
        }

        g_CounselorBirthPawn.store(reinterpret_cast<uintptr_t>(bornPawn));
        g_CounselorBirthComplete.store(true);

        Logger::Success(
            "18L-AD COUNSELOR BIRTH COMPLETE: donor order reproduced | human=" +
            SafeName(reinterpret_cast<UObject*>(bornPawn->Class)) +
            " | bots=" + std::to_string(g_CounselorBotsCreated.load()) +
            " | requestedBots=" +
            std::to_string(GetRequestedCounselorBotCount()) +
            " | selectedCounselors=" +
            std::to_string(g_NativeSelectedCounselorTotal.load()) +
            " | allBots=" + (allBotsCreated ? "true" : "false") +
            " | introKiller=" +
            SafeName(reinterpret_cast<UObject*>(killerClass)) +
            " | GameStateKillerClass=" +
            (killerClassAssigned ? "true" : "false") +
            " | possessionPreserved=true");

        // HandlePreMatchIntro runs on the game thread after the OfflineBots
        // world/controller and selected mode are fully established. This is
        // the first safe point to restore the temporary CDO alias.
        if (g_CounselorAliasRestorePending.load())
            RestoreDedicatedCounselorAlias();
        return true;
    }

    bool IsCounselorBirthComplete()
    {
        if (!g_CounselorBirthComplete.load())
            return false;

        APlayerController* controller = Engine::GetLocalPlayerController();
        return controller &&
            controller->AcknowledgedPawn &&
            (uintptr_t)controller->AcknowledgedPawn ==
                g_CounselorBirthPawn.load();
    }

    void MarkCounselorMatchInProgress()
    {
        if (g_CounselorMenuRouteLatched.load())
        {
            if (!g_CounselorMatchInProgress.exchange(true))
            {
                const int32_t difficulty =
                    g_NativeSelectedCounselorSkill.load();
                const int32_t weather = g_NativeSelectedWeather.load();
                static constexpr const char* difficultyNames[] = {
                    "Low", "Medium", "High"
                };
                static constexpr const char* weatherNames[] = {
                    "Off", "Rain", "Random"
                };
                static constexpr const char* jasonDifficultyNames[] = {
                    "Easy", "Normal", "Hard"
                };
                const int32_t jasonDifficulty = g_CommittedJasonDifficulty.load();
                UWorld* world = Engine::GetWorld();
                UClass* selectedCounselor = reinterpret_cast<UClass*>(
                    g_TargetPlayerCounselorClass.load());
                UClass* selectedJason = reinterpret_cast<UClass*>(
                    g_TargetJasonClass.load());
                Logger::Success(
                    "OFFLINE BOTS COUNSELOR APPLIED: world=" +
                    (world ? SafeName(reinterpret_cast<UObject*>(world))
                           : std::string("NULL")) +
                    " | counselor=" +
                    (selectedCounselor &&
                     Memory::IsReadable(selectedCounselor, sizeof(UClass))
                        ? SafeName(reinterpret_cast<UObject*>(selectedCounselor))
                        : std::string("unavailable")) +
                    " | Jason=" +
                    (selectedJason &&
                     Memory::IsReadable(selectedJason, sizeof(UClass))
                        ? SafeName(reinterpret_cast<UObject*>(selectedJason))
                        : std::string("unavailable")) +
                    " | counselors=" +
                    std::to_string(g_NativeSelectedCounselorTotal.load()) +
                    " | counselorBotSkill=" +
                    (difficulty >= 0 && difficulty < 3
                        ? difficultyNames[difficulty] : "unavailable") +
                    " | requestedWeather=" +
                    (weather >= 0 && weather < 3
                        ? weatherNames[weather] : "unavailable") +
                    " | requestedJasonDifficulty=" +
                    (jasonDifficulty >= 0 && jasonDifficulty < 3
                        ? jasonDifficultyNames[jasonDifficulty] : "stock"));
            }
            ArmObjectivesWidgetGameStatePatch();
        }
    }

    bool IsCounselorMatchInProgress()
    {
        return g_CounselorMatchInProgress.load();
    }

    bool SpawnCounselorModeJasonAfterMatch(void* gameModeValue)
    {
        if (g_CounselorJasonActive.load())
            return true;
        if (!g_CounselorMenuRouteLatched.load() ||
            !g_CounselorBirthComplete.load() ||
            !g_CounselorMatchInProgress.load() ||
            !ValidateCounselorLifecycleNativeSurface())
        {
            return false;
        }

        UObject* gameMode = reinterpret_cast<UObject*>(gameModeValue);
        UWorld* world = Engine::GetWorld();
        AActor* localCounselor = reinterpret_cast<AActor*>(
            g_CounselorBirthPawn.load());

        if (!gameMode ||
            !world ||
            !localCounselor ||
            !Memory::IsReadable(gameMode, 0x940) ||
            !Memory::IsReadable(world, sizeof(UWorld)) ||
            !Memory::IsReadable(localCounselor, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD AI Jason: match infrastructure is unavailable");
            return false;
        }

        const uint8_t* killerSoftClass = nullptr;
        UClass* killerClass = nullptr;
        ResolveNativeSelectedKiller(
            gameMode,
            &killerSoftClass,
            &killerClass);

        if (!killerSoftClass ||
            !killerClass ||
            !ClassDerivesFrom(killerClass, "SCKillerCharacter"))
        {
            Logger::Error(
                "18L-AD AI Jason: selected killer class did not resolve");
            return false;
        }

        AActor* killerStart = FindKillerPlayerStart(world);
        FVector startLocation{};
        FRotator startRotation{};

        if (!killerStart ||
            !GetActorStartTransform(
                killerStart,
                startLocation,
                startRotation))
        {
            Logger::Error(
                "18L-AD AI Jason: no usable SCKillerPlayerStart transform; failing closed");
            return false;
        }

        Logger::Success("PendingMatchJason=" + SafeName(reinterpret_cast<UObject*>(killerClass)));
        AActor* jason = SpawnLifecycleActor(
            world,
            killerClass,
            &startLocation,
            &startRotation,
            nullptr,
            2);

        if (!jason ||
            !Memory::IsReadable(jason, 0x3A8))
        {
            Logger::Error(
                "18L-AD AI Jason: selected killer pawn SpawnActor failed");
            return false;
        }

        using ClassGetterFn = UClass* (__fastcall*)();
        ClassGetterFn killerControllerGetter =
            reinterpret_cast<ClassGetterFn>(
                ShippingAddress(RVA_LifecycleKillerControllerClass));
        UClass* killerControllerClass = killerControllerGetter
            ? killerControllerGetter()
            : nullptr;

        if (!killerControllerClass)
        {
            Logger::Error(
                "18L-AD AI Jason: SCKillerAIController class getter failed");
            return false;
        }

        AActor* killerControllerActor =
            SpawnKillerControllerWithStockCrowdBypass(
                world,
                killerControllerClass);
        UObject* killerController =
            reinterpret_cast<UObject*>(killerControllerActor);

        if (!killerController ||
            !Memory::IsReadable(killerController, 0x410))
        {
            Logger::Error(
                "18L-AD AI Jason: SCKillerAIController SpawnActor failed");
            return false;
        }

        using PossessFn = void(__fastcall*)(UObject*, AActor*);
        PossessFn basePossess = reinterpret_cast<PossessFn>(
            ShippingAddress(RVA_LifecycleBasePossess));
        basePossess(killerController, jason);

        UObject* pawnController =
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(jason) + 0x3A0);
        UObject* killerPlayerState =
            GetControllerPlayerState(killerController);

        if (pawnController != killerController ||
            !killerPlayerState ||
            !Memory::IsReadable(killerPlayerState, sizeof(UObject)))
        {
            Logger::Error(
                "18L-AD AI Jason: base AAIController possession did not establish pawn/PlayerState links");
            return false;
        }

        if (!SetLifecycleActiveCharacter(
                killerPlayerState,
                killerSoftClass))
        {
            Logger::Error(
                "18L-AD AI Jason: PlayerState killer metadata setter failed");
            return false;
        }

#if !defined(F13_BASE_GAME_PORT)
        ApplyLifecycleKillerCosmetics(
            killerPlayerState,
            killerSoftClass);
#endif

        UObject* humanPlayerState = GetControllerPlayerState(
            Engine::GetLocalPlayerController());
        const bool killerPresentationReady =
            CopyLifecycleKillerPresentation(
                humanPlayerState,
                killerPlayerState,
                killerClass);

        if (!FrozenJasonBridge::AdoptCounselorModeJason(
                jason,
                killerController,
                localCounselor))
        {
            Logger::Error(
                "18L-AD AI Jason: frozen behavior adapter rejected the native pawn/controller pair");
            return false;
        }

        // The pre-match intro has to borrow the human PlayerState as its
        Logger::Success("SpawnedJason=" + SafeName(reinterpret_cast<UObject*>(jason->Class)));
        // killer metadata owner because stock OfflineBots assumes that the
        // local player is Jason.  Once the independent AI Jason exists, hand
        // that ownership to its real PlayerState and restore the human's
        // ActiveCharacterClass to the counselor that is still possessed.
        // Leaving the borrowed killer metadata in place makes the HUD/result
        // path report Jason's "You killed" summary to the counselor player.
        const uint8_t* activeCounselorSoftClass = nullptr;
        const uintptr_t counselorArrayOffset = GetLifecycleArrayOffset(
            gameMode,
            "CounselorCharacterClasses",
            0x510);
        const int32_t counselorClassCount =
            GetLifecycleSoftClassCount(gameMode, counselorArrayOffset);

        for (int32_t i = 0; i < counselorClassCount; ++i)
        {
            const uint8_t* candidate =
                GetLifecycleSoftClass(gameMode, counselorArrayOffset, i);
            UClass* loaded = LoadLifecycleSoftClass(candidate);
            if (loaded == localCounselor->Class)
            {
                activeCounselorSoftClass = candidate;
                break;
            }
        }

        UObject* gameState =
#if defined(F13_BASE_GAME_PORT)
            reinterpret_cast<UObject*>(world->GameState);
#else
            *reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(gameMode) + 0x3C0);
#endif
        bool killerOwnerTransferred = false;
#if defined(F13_BASE_GAME_PORT)
        if (killerPresentationReady &&
            gameState &&
            Memory::IsReadable(gameState, sizeof(UObject)) &&
            gameState->Class)
        {
            UPropertyLite* killerOwnerProperty =
                FindPropertyInHierarchyByName(
                    gameState->Class,
                    "CurrentKillerPlayerState");
            if (killerOwnerProperty &&
                killerOwnerProperty->Offset_Internal > 0 &&
                killerOwnerProperty->Offset_Internal < 0x10000 &&
                killerOwnerProperty->ElementSize == sizeof(UObject*))
            {
                UObject** killerOwner = reinterpret_cast<UObject**>(
                    reinterpret_cast<uintptr_t>(gameState) +
                    killerOwnerProperty->Offset_Internal);
                if (Memory::IsReadable(killerOwner, sizeof(UObject*)))
                {
                    *killerOwner = killerPlayerState;
                    killerOwnerTransferred =
                        *killerOwner == killerPlayerState;
                }
            }
        }
#else
        if (killerPresentationReady &&
            gameState &&
            Memory::IsReadable(gameState, 0x498))
        {
            UObject** killerOwner = reinterpret_cast<UObject**>(
                reinterpret_cast<uintptr_t>(gameState) + 0x490);
            if (Memory::IsReadable(killerOwner, sizeof(UObject*)))
            {
                *killerOwner = killerPlayerState;
                killerOwnerTransferred = *killerOwner == killerPlayerState;
            }
        }
#endif

        const bool humanCounselorMetadataRestored =
            humanPlayerState &&
            activeCounselorSoftClass &&
            SetLifecycleActiveCharacter(
                humanPlayerState,
                activeCounselorSoftClass);

        if (killerOwnerTransferred && humanCounselorMetadataRestored)
        {
            g_PrimaryKillerPlayerState.store(
                reinterpret_cast<uintptr_t>(killerPlayerState));
            Logger::Success(
                "18L-AI ROLE HANDOFF COMPLETE: GameState killer owner=AI Jason PlayerState | human ActiveCharacterClass=" +
                SafeName(reinterpret_cast<UObject*>(localCounselor->Class)));
        }
        else
        {
            Logger::Error(
                "18L-AI ROLE HANDOFF INCOMPLETE: killerOwnerTransferred=" +
                std::string(killerOwnerTransferred ? "true" : "false") +
                " | humanCounselorMetadataRestored=" +
                (humanCounselorMetadataRestored ? "true" : "false"));
        }

        g_CounselorJasonActive.store(true);
        g_StartingItemAttempts.store(0);
        g_StartingItemWorld.store(reinterpret_cast<uintptr_t>(world));
        g_NextStartingItemAt.store(GetTickCount64() + 3000);
        g_StartingLoadoutStage.store(0);
        g_StartingItemPending.store(g_CommittedStartingWeapon.load() >= 2 || g_CommittedStartingInventory.load() >= 2);
        ApplyPendingStartingItemOnGameThread();

        Logger::Success(
            "18L-AD AI JASON COMPLETE: independent " +
            SafeName(reinterpret_cast<UObject*>(killerClass)) +
            " spawned at SCKillerPlayerStart and possessed by SCKillerAIController | human counselor untouched");
        return true;
    }

    bool BeginFrozenAICompatibilityShim()
    {
        if (g_AISpoofObject.load() &&
            g_AISpoofController.load())
        {
            return true;
        }

        if (!g_CounselorMenuRouteLatched.load() ||
            !g_CounselorBirthComplete.load() ||
            !g_CounselorMatchInProgress.load() ||
            !Engine::IsInGame())
        {
            return false;
        }

        UWorld* world = Engine::GetWorld();
        if (!world)
            return false;

        uintptr_t gameModeSlot =
            (uintptr_t)world + 0xF0;

        if (!Memory::IsReadable(
                (void*)gameModeSlot,
                sizeof(UObject*)))
        {
            return false;
        }

        UObject* gameMode =
            *(UObject**)gameModeSlot;

        if (!gameMode ||
            !Memory::IsReadable(
                gameMode,
                sizeof(UObject)) ||
            !gameMode->Class ||
            !Memory::IsReadable(
                gameMode->Class,
                sizeof(UClass)))
        {
            return false;
        }

        std::string className =
            SafeName((UObject*)gameMode->Class);

        if (className != "SCGameMode_OfflineBots")
            return false;

        UClass* sandboxClass =
            FindClassExact("SCGameMode_Sandbox");

        APlayerController* controller =
            Engine::GetLocalPlayerController();

        UClass* sandboxControllerClass =
            FindClassExact("SCPlayerController_Sandbox");

        if (!sandboxClass ||
            !Memory::IsReadable(
                sandboxClass,
                sizeof(UClass)) ||
            !controller ||
            !Memory::IsReadable(
                controller,
                sizeof(UObject)) ||
            !controller->Class ||
            !sandboxControllerClass ||
            !Memory::IsReadable(
                sandboxControllerClass,
                sizeof(UClass)))
        {
            return false;
        }

        const std::string controllerClass =
            SafeName((UObject*)controller->Class);

        if (controllerClass.find("OfflineBots") ==
            std::string::npos)
        {
            return false;
        }

        g_AISpoofObject.store(
            (uintptr_t)gameMode);

        g_AISpoofOriginalClass.store(
            (uintptr_t)gameMode->Class);

        g_AISpoofController.store(
            (uintptr_t)controller);

        g_AISpoofControllerOriginalClass.store(
            (uintptr_t)controller->Class);

        gameMode->Class = sandboxClass;
        controller->Class = sandboxControllerClass;

        Logger::Success(
            "18L-AD frozen-AI compatibility shim: dedicated OFLBC host GameMode/controller temporarily exposed as Sandbox for frozen resource/spawn APIs");
        return true;
    }

    void EndFrozenAICompatibilityShim()
    {
        UObject* gameMode =
            (UObject*)g_AISpoofObject.exchange(0);

        UClass* originalClass =
            (UClass*)g_AISpoofOriginalClass.exchange(0);

        UObject* controller =
            (UObject*)g_AISpoofController.exchange(0);

        UClass* controllerOriginalClass =
            (UClass*)g_AISpoofControllerOriginalClass.exchange(0);

        if (gameMode &&
            originalClass &&
            Memory::IsReadable(
                gameMode,
                sizeof(UObject)))
        {
            gameMode->Class = originalClass;
        }

        if (controller &&
            controllerOriginalClass &&
            Memory::IsReadable(
                controller,
                sizeof(UObject)))
        {
            controller->Class = controllerOriginalClass;
        }

        Logger::Success(
            "18L-AD frozen-AI compatibility shim restored native OfflineBots GameMode/controller classes");
    }

    bool IsCounselorModeAutoStartArmed()
    {
        return g_CounselorModeAutoStartArmed.load();
    }

    bool IsSandboxCounselorReady()
    {
        return g_SandboxCounselorSyncStage.load() ==
            (int)SandboxCounselorSyncStage::Done;
    }

    int32_t GetCounselorModeBotCount()
    {
        return GetRequestedCounselorBotCount();
    }

    void MarkCounselorModeAutoStartFinished()
    {
        g_CounselorModeAutoStartArmed.store(false);
        g_CounselorMenuRouteEnabled.store(false);
        g_CounselorMenuRouteLatched.store(false);
        EndFrozenAICompatibilityShim();
    }

    bool HasPendingRequest()
    {
#if defined(F13_BASE_GAME_PORT)
        if (g_HostedArmPending.load() || g_HostedF3Pending.load() || g_HostedF1Pending.load() ||
            ((g_HostedJasonIntent.load() || g_HostedF3Armed.load() || g_HostedF3Busy.load()) &&
                GetTickCount64() >= g_HostedNextPump.load())) return true;
        if (g_PrivateLobbyAuditPending.load()) return true;
        if (g_StartingItemPending.load() && GetTickCount64() >= g_NextStartingItemAt.load())
            return true;
        if (g_OfflineWeatherApplyPending.load() && GetTickCount64() >= g_NextWeatherApplyAt.load())
            return true;
        if (g_StockOfflinePlayProbePending.load())
            return true;
#endif

        if (g_StartSplashAcceptRequested.load())
            return true;

        if (g_RuntimeMenuOrderRequested.load())
            return true;

        if (g_DumpCounselorRosterRequested.load())
            return true;

        if (g_SandboxCounselorSyncStage.load() ==
            (int)SandboxCounselorSyncStage::NeedRequest)
            return true;

        int counselorStage =
            g_CounselorStage.load();

        if (counselorStage == (int)CounselorSelectStage::NeedRequest)
        {
            return true;
        }

        int stage = g_Stage.load();
        return stage == (int)SetupStage::NeedPreload ||
            stage == (int)SetupStage::WaitingForAssets ||
            stage == (int)SetupStage::ReadyToApply ||
            stage == (int)SetupStage::Sticky;
    }

    void QueuePrivateLobbyAudit()
    {
        g_PrivateLobbyAuditPending.store(true);
    }

    void QueueHostedCounselorArm()
    {
        g_HostedArmPending.store(true);
    }

    bool IsHostedCounselorArmed()
    {
        return g_HostedF3Armed.load();
    }

    bool IsHostedJasonArmed()
    {
        return g_HostedJasonIntent.load();
    }

    bool QueueHostedAdditionalJason()
    {
        if (!g_HostedF3Armed.load()) return false;
        bool idle = false;
        if (!g_HostedF3Busy.compare_exchange_strong(idle, true)) return false;
        g_HostedF1Pending.store(true);
        return true;
    }

    bool QueueHostedCounselor()
    {
        if (!g_HostedF3Armed.load()) return false;
        bool idle = false;
        if (!g_HostedF3Busy.compare_exchange_strong(idle, true)) return false;
        g_HostedF3Pending.store(true);
        return true;
    }

    bool IsStockMainMenuReadyForHook()
    {
#if defined(F13_BASE_GAME_PORT)
        // A genuine Offline Play transition is a usable frontend on both the
        // authenticated and native offline paths. Do not require a login
        // success callback: the backend may be unavailable, and that callback
        // may run before our ProcessEvent bridge is installed.
        return (g_StockMainMenuVisible.load() ||
                g_StockOfflinePlayVisible.load()) &&
            !g_CounselorMatchInProgress.load();
#else
        return false;
#endif
    }

    void ObserveMainMenuVisibilityEvent(
        void* objectValue,
        void* functionValue,
        void* params)
    {
#if defined(F13_BASE_GAME_PORT)
        if (!IsStockFrontendMenuEventObject(reinterpret_cast<UObject*>(objectValue)) ||
            !functionValue ||
            !Memory::IsReadable(objectValue, sizeof(UObject)) ||
            !Memory::IsReadable(functionValue, sizeof(UFunction)))
        {
            return;
        }

        auto* object = reinterpret_cast<UObject*>(objectValue);
        auto* function = reinterpret_cast<UFunction*>(functionValue);
        if (!object->Class)
        {
            return;
        }

        const std::string functionName = SafeName((UObject*)function);
        const std::string className = SafeName((UObject*)object->Class);

        if (g_CounselorMatchInProgress.load() &&
            (className == "OfflinePlayMenuWidget_C" || className == "MainMenuWidget_C"))
        {
            // Queue lifecycle work once; the worker confirms the actual
            // frontend world before retiring any match state. A preloaded
            // menu object alone must never reset a live match.
            g_FrontendReturnEventPending.store(true);
        }

        if (className == "OfflinePlayMenuWidget_C" &&
            !g_StockOfflinePlayVisible.load())
        {
            // Blueprint transition callbacks are not reliable on this stock
            // build. Any event merely caches the instance; the queued probe
            // still requires the native menu stack to name it as top/current.
            g_StockOfflinePlayWidget.store(
                reinterpret_cast<uintptr_t>(object));
            const ULONGLONG now = GetTickCount64();
            if (now >= g_StockOfflinePlayProbeNextAt.load())
            {
                g_StockOfflinePlayProbeNextAt.store(now + 500);
                g_StockOfflinePlayProbePending.store(true);
            }
        }

        if (className == "LoginMenuWidget_C")
        {
            // PerformLogin can run before the ProcessEvent bridge is ready.
            // The first event from the live login widget is therefore the
            // reliable start of our bounded, failure-only timeout.
            g_StockLoginWidget.store(reinterpret_cast<uintptr_t>(object));
            ULONGLONG firstSeen = 0;
            if (g_StockLoginStartedAt.compare_exchange_strong(
                    firstSeen,
                    GetTickCount64()))
            {
                Logger::Debug(
                    "Stock login widget active; native sign-in remains in control");
            }

            if (functionName == "PerformLogin")
            {
                Logger::Debug(
                    "Stock PerformLogin observed; waiting for the normal OnlineFix/game callback");
            }
            else if (functionName ==
                "OnSuccess_5A74E96A4BAE3DC53B687384D1D6CDDF")
            {
                g_StockBackendLoginSucceeded.store(true);
                g_StockLoginTimeoutPending.store(false);
                Logger::Success(
                    "Stock backend login success observed; preserving authenticated profile and progression path");
            }
            else if (functionName ==
                    "OnFailure_5A74E96A4BAE3DC53B687384D1D6CDDF" ||
                functionName ==
                    "OnGeneralFailure_5A74E96A4BAE3DC53B687384D1D6CDDF" ||
                functionName == "OnLoginFailed_6DAE067F4264CDBCB91EE68013776F3B" ||
                functionName == "LoginFailed")
            {
                g_StockLoginFailureObserved.store(true);
                g_StockLoginTimeoutPending.store(false);
                Logger::Debug(
                    "Stock login failure UI observed; native offline choice remains available");
            }
        }

        if (g_StockMainMenuVisible.load() || !params)
            return;

        if (functionName != "Get_MainMenu_Visibility" &&
            functionName != "Get_MainMenu_Visibility_0")
        {
            return;
        }

        if (className != "MainMenuWidget_C" ||
            !Memory::IsReadable(params, sizeof(uint8_t)))
        {
            return;
        }

        // ESlateVisibility::Visible is the zero-valued entry. This callback is
        // invoked after the original ProcessEvent, so the return byte is final.
        if (*reinterpret_cast<uint8_t*>(params) != 0)
            return;

        if (!g_StockMainMenuVisible.exchange(true))
        {
            Logger::Success(
                "Stock main menu confirmed visible; offline-bots menu activation is now permitted");
        }
#else
        (void)objectValue;
        (void)functionValue;
        (void)params;
#endif
    }

    void TickWorker()
    {
#if !defined(F13_BASE_GAME_PORT)
        TickStartSplashAutoAccept();
#else
        // This backend is attached only after the external OnlineFix sign-in
        // has completed. Never rediscover a stale LoginMenuWidget or
        // manufacture a login failure from inside the mod. The late attach
        // can miss Offline Play's first Blueprint transition, so verify its
        // current menu stack at a bounded rate on the game thread instead.
        if (!g_StockOfflinePlayVisible.load() &&
            !g_CounselorMatchInProgress.load())
        {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG nextProbe =
                g_StockOfflinePlayProbeNextAt.load();
            if (!nextProbe || now >= nextProbe)
            {
                g_StockOfflinePlayProbeNextAt.store(now + 3000);
                if (GetActiveFrontendMenu() != nullptr)
                    g_StockOfflinePlayProbePending.store(true);
            }
        }
#endif

        // A completed/abandoned counselor match can return to a fresh frontend
        // without traversing the older auto-start-finished callback. Retire the
        // stale route here so the same process can immediately arm a brand-new
        // counselor picker. GetActiveFrontendMenu is deliberately narrow and
        // cannot match an in-world pause menu.
        // Native menu events handle visibility immediately. This fallback is
        // only for a missed return-to-frontend event, not a per-50-ms scan.
        static ULONGLONG nextReplayFrontendProbeAt = 0;
        UObject* activeFrontend = nullptr;
        const ULONGLONG workerNow = GetTickCount64();
        const bool frontendReturnEvent = g_FrontendReturnEventPending.exchange(false);
        if (g_CounselorMatchInProgress.load() &&
            (frontendReturnEvent || workerNow >= nextReplayFrontendProbeAt))
        {
            nextReplayFrontendProbeAt = workerNow + 500;
            activeFrontend = GetActiveFrontendMenu();
        }
        if (activeFrontend && g_CounselorMatchInProgress.exchange(false))
        {
            const uintptr_t activeFrontendAddress =
                reinterpret_cast<uintptr_t>(activeFrontend);
            // Rebuilding the VerticalBox a second time in one process is
            // unsafe after the post-match frontend reconstructs its Slate
            // resources. Keep replay widgets in native order instead of
            // clearing/re-adding their children. A fresh process still receives
            // the requested Jason/Counselor-first order.
            const bool suppressingReplayMenuRebuild =
                g_RuntimeMenuOrderedWidget.load() != 0;

            RestoreCounselorPickerPlayerStateCast();
            EndFrozenAICompatibilityShim();

            g_CounselorModeAutoStartArmed.store(false);
            g_CounselorBirthComplete.store(false);
            g_CounselorJasonActive.store(false);
            g_CounselorBotsCreated.store(0);
            g_CounselorBirthPawn.store(0);
            g_CounselorMenuRouteEnabled.store(false);
            g_CounselorMenuRouteLatched.store(false);
            g_CounselorMenuWorld.store(0);
            g_CounselorMenuObject.store(0);
            g_CounselorMenuRewriteCount.store(0);
            g_RequestOfflineModeHookHits.store(0);
            g_CounselorEntryClickPending.store(false);
            g_CounselorEntryClickedAt.store(0);
            g_CounselorPickerActive.store(false);
            g_CounselorPickerAccepted.store(false);
            g_CounselorPickerWidget.store(0);
            g_CounselorPickerSourceMenu.store(0);
            g_NativeSelectedCounselorTotal.store(0);
            g_NativeSelectedCounselorSkill.store(-1);
            g_NativeSelectedWeather.store(-1);
            g_GameSetupSelectionLocked.store(false);
            g_WeatherSettingsWidget.store(0);
            g_ActiveWeatherCombo.store(0);
            g_ActiveJasonDifficultyCombo.store(0);
            g_ActiveMatchLengthCombo.store(0);
            g_ActiveJasonUnlimitedKnivesCombo.store(0);
            g_ActiveJasonFastRechargeCombo.store(0);
            g_ActiveJasonFastMovementCombo.store(0);
            g_CommittedMatchSeconds.store(-1);
            g_CommittedJasonUnlimitedKnives.store(false);
            g_CommittedJasonFastRecharge.store(false);
            g_CommittedJasonFastMovement.store(false);
            g_ActiveStartingWeaponCombo.store(0);
            g_ActiveStartingInventoryCombo.store(0);
            g_CommittedStartingWeapon.store(0);
            g_CommittedStartingInventory.store(0);
            g_StartingLoadoutStage.store(0);
            g_StartingItemPending.store(false);
            g_CommittedJasonDifficulty.store(-1);
            g_CommittedOfflineRain.store(-1);
            g_OfflineWeatherApplyPending.store(false);
            g_CommittedSettingsWorld.store(0);
            g_ActiveSkillCombo.store(0);
            g_ActiveSkillValueText.store(0);
            g_LastSkillDisplayedIndex.store(-1);
            g_NextSkillDisplayPumpAt.store(0);
            g_PrimaryKillerPlayerState.store(0);
#if defined(F13_BASE_GAME_PORT)
            g_StockKillerRequestCaptured.store(false);
            g_StockCounselorPickerCaptured.store(false);
            g_StockJasonPickerLoggedIndex.store(-1);
            g_TargetJasonClass.store(0);
            g_TargetPlayerCounselorClass.store(0);
#endif
            g_RuntimeMenuLabeledWidget.store(0);
            // These widget classes can be reconstructed on return from a
            // match. A readable address alone does not prove it belongs to
            // the live replay menu or its current click delegates.
            g_OfflinePlayMenuClass.store(0);
            g_CounselorEntryHookTarget.store(0);
            g_VC3VisibilityHookTarget.store(0);
            g_RuntimeMenuLabelLastAttempt.store(0);
            g_RuntimeMenuLabelFailureLogged.store(false);
            g_RuntimeMenuOrderedWidget.store(
                suppressingReplayMenuRebuild
                    ? activeFrontendAddress
                    : 0);
            g_RuntimeMenuOrderFailureLogged.store(false);
            g_RuntimeMenuOrderRequested.store(false);
            g_RuntimeMenuOrderWidget.store(0);
            g_RuntimeMenuOrderFailureCode.store(0);
            g_NextAutomaticRouteArmAt.store(0);
            g_ObjectivesGameStateCastPatchPending.store(false);
            g_ObjectivesGameStateCastNextAttempt.store(0);

            Logger::Success(
                suppressingReplayMenuRebuild
                    ? "18L-AC replay lifecycle: counselor match returned to frontend; destructive menu reorder suppressed and fresh character picker re-armed"
                    : "18L-AC replay lifecycle: counselor match returned to frontend; stale route retired and fresh character picker re-armed");
        }

        // The base-game port must remain dormant during OnlineFix sign-in.
        // MainMenuWidget_C is preloaded during that screen, so object
        // existence is not sufficient.  ObserveMainMenuVisibilityEvent marks
        // readiness only after its native visibility binding actually returns
        // Visible on the game thread.
        // The stock ProcessEvent bridge can come online just after the first
        // MainMenuWidget visibility evaluation, so that single callback cannot
        // be a hard prerequisite.  The exact live frontend lookup is the
        // authoritative fallback: it matches SCGame_Menu/EntryGame_C but not
        // an in-world pause menu.  Engine::GetWorld alone is much too broad and
        // previously retried the frontend armer during gameplay.
        const bool frontendArmAllowed =
#if defined(F13_BASE_GAME_PORT)
            (g_StockMainMenuVisible.load() ||
             g_StockOfflinePlayVisible.load());
#else
            g_StockMainMenuVisible.load();
#endif
        if (!g_CounselorMenuRouteEnabled.load() &&
            !g_CounselorMenuRouteLatched.load() &&
            frontendArmAllowed)
        {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG next = g_NextAutomaticRouteArmAt.load();
            if (!next || now >= next)
            {
                g_NextAutomaticRouteArmAt.store(now + 1500);
                if (GetActiveFrontendMenu() != nullptr && ArmSelectedPreset())
                {
                    Logger::Success(
                        "CONTROLLER-FREE MODE READY: Offline Bots - Counselor is armed automatically; use the native game menus only");
                }
            }
        }

        // Route installation must always win the frontend race.  The native
        // profile lookup can perform a full live-object scan the first time it
        // resolves the selection save.  Running that scan before ArmSelectedPreset
        // allowed a fast user to enter the stock Jason picker while this worker
        // was still busy, and a late injection could scan during map teardown.
        // Only sync the profile once the dedicated frontend route is armed.
        if (g_CounselorMenuRouteEnabled.load() ||
            g_CounselorMenuRouteLatched.load())
        {
            TickNativeProfileSelection();
        }

        // The unified setup preset is independent from the frozen AI tick and
        // from the Jason-selection state machine.  Service only explicit
        // one-shot requests here, then maintain the already-validated map
        // pointer while still in the frontend world.
        if (g_ArmPresetRequested.exchange(false))
        {
            ArmSelectedPreset();
        }

        if (g_ApplyGameSetupRequested.exchange(false))
        {
            ApplyGameSetupPreset();
        }

        TickMapSticky();
        TickCounselorSelection();
        TickSandboxCounselorSync();

        int stage = g_Stage.load();
        if (stage == (int)SetupStage::Idle ||
            stage == (int)SetupStage::Done ||
            stage == (int)SetupStage::Failed)
        {
            return;
        }

        ResolveSelectionSaveMetadata();

        if (!g_SelectionSaveObject.load() &&
            g_SelectionSaveClass.load() &&
            !g_BackgroundScanStarted.exchange(true))
        {
            Logger::Debug(
                "Jason resolve 18B: background search for live SCCharacterSelectionsSaveGame started");

            UObject* live = RawFindSelectionSaveObject(
                (UClass*)g_SelectionSaveClass.load());

            if (live)
            {
                g_SelectionSaveObject.store((uintptr_t)live);
                Logger::Success(
                    "Jason resolve 18B: live SCCharacterSelectionsSaveGame found");
            }
            else
            {
                Logger::Error(
                    "Jason resolve 18B: live SCCharacterSelectionsSaveGame not found");
            }
        }

        if (stage == (int)SetupStage::Sticky)
        {
            UWorld* world = Engine::GetWorld();
            uintptr_t originalWorld = g_SelectionWorld.load();

            // Prototype 16 proved the stock frontend can overwrite KillerPick
            // after our initial write. Keep it sticky ONLY while we remain in
            // the same frontend world. Stop immediately when travel/teardown
            // swaps the UWorld pointer.
            if (!world || !originalWorld ||
                (uintptr_t)world != originalWorld)
            {
                Logger::Success(
                    "Jason resolve 18B: frontend world changed; sticky KillerPick override released");
                g_Stage.store((int)SetupStage::Done);
                return;
            }

            UObject* saveObject =
                (UObject*)g_SelectionSaveObject.load();
            UClass* jasonClass =
                (UClass*)g_TargetJasonClass.load();
            int32_t killerPickOffset =
                g_KillerPickOffset.load();

            if (!saveObject || !jasonClass ||
                killerPickOffset < 0 ||
                !Memory::IsReadable(saveObject, sizeof(UObject)))
            {
                Logger::Error(
                    "Jason resolve 18B: sticky state lost required pointers");
                g_Stage.store((int)SetupStage::Failed);
                return;
            }

            UClass** killerPick =
                (UClass**)((uintptr_t)saveObject + killerPickOffset);

            if (!Memory::IsReadable(killerPick, sizeof(UClass*)))
            {
                Logger::Error(
                    "Jason resolve 18B: sticky KillerPick address unreadable");
                g_Stage.store((int)SetupStage::Failed);
                return;
            }

            UClass* current = *killerPick;

            if (current != jasonClass)
            {
                std::string beforeName =
                    SafeName((UObject*)current);

                *killerPick = jasonClass;

                uint32_t n =
                    g_StickyRewriteCount.fetch_add(1) + 1;

                if (n <= 12)
                {
                    Logger::Debug(
                        "Jason resolve 18B sticky: stock frontend changed KillerPick to " +
                        beforeName +
                        " -> restoring selected Jason");
                }
            }

            return;
        }

        if (!g_TargetJasonClass.load() &&
            stage == (int)SetupStage::WaitingForAssets)
        {
            ULONGLONG now = GetTickCount64();
            ULONGLONG lastScan = g_LastTargetScanAt.load();

            if (!lastScan || now - lastScan >= 500)
            {
                g_LastTargetScanAt.store(now);

                if (TryResolveTargetJasonClass())
                {
                    Logger::Success(
                        "Jason resolve 18B: selected Jason class is available");
                }
            }

        }

        if (g_TargetJasonClass.load() &&
            g_SelectionSaveObject.load() &&
            g_KillerPickOffset.load() >= 0 &&
            stage == (int)SetupStage::WaitingForAssets)
        {
            g_Stage.store((int)SetupStage::ReadyToApply);
        }
    }

    bool ConsumePendingRequestOnGameThread()
    {
#if defined(F13_BASE_GAME_PORT)
        if (g_HostedArmPending.load() || g_HostedF3Pending.load() || g_HostedF1Pending.load() ||
            ((g_HostedJasonIntent.load() || g_HostedF3Armed.load() || g_HostedF3Busy.load()) &&
                GetTickCount64() >= g_HostedNextPump.load()))
            PumpHostedCounselor();
        if (g_PrivateLobbyAuditPending.exchange(false))
            AuditPrivateLobbyOnGameThread();
        ApplyPendingStartingItemOnGameThread();
        ApplyPendingOfflineSettingsOnGameThread();
        PumpOfflineJasonExtras();
        if (g_StockOfflinePlayProbePending.exchange(false))
        {
            if (ConfirmVisibleStockOfflinePlayOnGameThread())
                return true;
        }
#endif

        if (g_StartSplashAcceptRequested.exchange(false))
        {
            if (!AcceptStartSplashOnGameThread())
            {
                // Retry discovery rather than retaining a possibly stale
                // widget pointer across a frontend transition.
                g_StartSplashWidget.store(0);
                g_NextStartSplashScanAt.store(GetTickCount64() + 250);
                return false;
            }
            return true;
        }

        if (g_RuntimeMenuOrderRequested.exchange(false))
        {
            UObject* menuWidget = reinterpret_cast<UObject*>(
                g_RuntimeMenuOrderWidget.load());
            const bool ordered =
                ReorderRuntimeOfflinePlayMenu(menuWidget);
            if (!ordered &&
                !g_RuntimeMenuOrderFailureLogged.exchange(true))
            {
                Logger::Error(
                    "Packed menu game-thread reorder failed | Code=" +
                    std::to_string(
                        g_RuntimeMenuOrderFailureCode.load()));
            }
            return ordered;
        }

        if (g_SandboxCounselorSyncStage.load() ==
            (int)SandboxCounselorSyncStage::NeedRequest)
        {
            return RequestSandboxSelectedCounselorOnGameThread();
        }

        int counselorStage =
            g_CounselorStage.load();

        if (counselorStage ==
            (int)CounselorSelectStage::NeedRequest)
        {
            Logger::Success(
                "Counselor selection 18K executing SCPlayerState::RequestCounselorClass on game thread | thread=" +
                std::to_string(GetCurrentThreadId()));

            if (!RequestSelectedCounselorSoftClassOnGameThread())
            {
                Logger::Error(
                    "Counselor selection 18K: native counselor soft-class request failed");

                g_CounselorStage.store(
                    (int)CounselorSelectStage::Failed);

                return false;
            }

            g_CounselorStage.store(
                (int)CounselorSelectStage::Monitoring);

            Logger::Success(
                "Counselor selection 18K: native request submitted; monitoring PickedCounselorClass until frontend travel");

            return true;
        }

        if (g_DumpCounselorRosterRequested.exchange(false))
        {
            Logger::Success(
                "Counselor discovery 18F executing on game thread | thread=" +
                std::to_string(GetCurrentThreadId()));
            return DumpCounselorRosterOnGameThread();
        }

        int stage = g_Stage.load();

        if (stage == (int)SetupStage::NeedPreload)
        {
            Logger::Success(
                "Jason resolve 18B executing synchronous soft-class conversion on game thread | thread=" +
                std::to_string(GetCurrentThreadId()));

            if (!ResolveJason5ClassOnGameThread())
            {
                Logger::Error(
                    "Jason resolve 18B: selected Jason soft-class conversion failed");
                g_Stage.store((int)SetupStage::Failed);
                return false;
            }

            g_Stage.store((int)SetupStage::WaitingForAssets);
            return true;
        }

        if (stage == (int)SetupStage::WaitingForAssets)
        {
            // Worker thread is resolving the live save object and loaded class.
            // Keep this game-thread path intentionally cheap.
            return true;
        }

        if (stage == (int)SetupStage::ReadyToApply)
        {
            UObject* saveObject =
                (UObject*)g_SelectionSaveObject.load();
            UClass* jasonClass =
                (UClass*)g_TargetJasonClass.load();
            int32_t killerPickOffset =
                g_KillerPickOffset.load();

            if (!saveObject || !jasonClass || killerPickOffset < 0 ||
                !Memory::IsReadable(saveObject, sizeof(UObject)))
            {
                Logger::Error(
                    "Jason resolve 18B: ready state lost required pointers");
                g_Stage.store((int)SetupStage::Failed);
                return false;
            }

            UClass** killerPick =
                (UClass**)((uintptr_t)saveObject + killerPickOffset);

            if (!Memory::IsReadable(killerPick, sizeof(UClass*)))
            {
                Logger::Error(
                    "Jason resolve 18B: KillerPick address unreadable");
                g_Stage.store((int)SetupStage::Failed);
                return false;
            }

            UClass* before = *killerPick;
            *killerPick = jasonClass;
            UClass* after = *killerPick;

            Logger::Debug(
                "Jason resolve 18B: KillerPick before=" + SafeName((UObject*)before) +
                " after=" + SafeName((UObject*)after));

            if (after == jasonClass)
            {
                Logger::Success(
                    "Jason resolve 18B SUCCESS: selected Jason KillerPick set to " +
                    SafeName((UObject*)after));

                UWorld* world = Engine::GetWorld();
                g_SelectionWorld.store((uintptr_t)world);
                g_StickyRewriteCount.store(0);

                if (world)
                {
                    Logger::Success(
                        "Jason resolve 18B: sticky KillerPick override armed until frontend world travel");
                    g_Stage.store((int)SetupStage::Sticky);
                }
                else
                {
                    Logger::Error(
                        "Jason resolve 18B: could not capture frontend world for sticky override");
                    g_Stage.store((int)SetupStage::Failed);
                }

                return true;
            }

            Logger::Error(
                "Jason resolve 18B: KillerPick write did not persist");
            g_Stage.store((int)SetupStage::Failed);
            return false;
        }

        return false;
    }

    bool HasPendingObjectivesPatch()
    {
        return g_ObjectivesGameStateCastPatchPending.load() &&
            GetTickCount64() >=
                g_ObjectivesGameStateCastNextAttempt.load();
    }

    bool TryPatchObjectivesWidgetOnGameThread()
    {
        if (!HasPendingObjectivesPatch())
            return false;

        if (PatchObjectivesWidgetGameStateCast())
        {
            if (g_ObjectivesGameStateCastPatched.load())
                g_ObjectivesGameStateCastPatchPending.store(false);
            return true;
        }

        // ObjectivesWidget is first loaded by the pause/map UI. That UI also
        // suspends gameplay ReceiveTick, so Engine's ProcessEvent bridge calls
        // this helper from the first confirmed game-thread UI event as well.
        g_ObjectivesGameStateCastNextAttempt.store(
            GetTickCount64() + 250);
        return false;
    }

    bool TryPatchObjectivesWidgetForEventOnGameThread(void* objectValue)
    {
        if (!g_ObjectivesGameStateCastPatchPending.load())
            return false;

        UObject* object = reinterpret_cast<UObject*>(objectValue);
        if (!object ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !object->Class ||
            !Memory::IsReadable(object->Class, sizeof(UClass)))
        {
            return false;
        }

        const std::string eventClassName =
            SafeName(reinterpret_cast<UObject*>(object->Class));
        bool isObjectiveWidget = false;
        for (const ObjectiveWidgetPatchSpec& spec :
             g_ObjectiveWidgetPatchSpecs)
        {
            if (eventClassName == spec.ClassName)
            {
                isObjectiveWidget = true;
                break;
            }
        }
        if (!isObjectiveWidget)
            return false;

        // This event proves the class is resident. Patch before forwarding it
        // so Construct and the first ExecuteUbergraph call see the widened
        // SCGameState cast. No recurring class scan is needed during play.
        g_ObjectivesGameStateCastNextAttempt.store(0);
        const bool patched =
            PatchObjectivesWidgetGameStateCast(eventClassName.c_str());
        if (g_ObjectivesGameStateCastPatched.load())
            g_ObjectivesGameStateCastPatchPending.store(false);
        return patched;
    }

    bool InterceptPackedCounselorMenuClick(
        void* objectValue,
        void* functionValue)
    {
#if !defined(F13_BASE_GAME_PORT)
        (void)objectValue;
        (void)functionValue;
        return false;
#else
        // Birth occurs only after the picker has committed travel. Replay
        // clears this latch before rearming the frontend, so menu recovery
        // remains available while normal gameplay takes the cheap exit.
        if (!g_CounselorMenuRouteEnabled.load() ||
            g_CounselorBirthComplete.load())
        {
            return false;
        }

        // This is a global ProcessEvent observer. Match the received event
        // first with guarded, allocation-free name access; do not walk object
        // metadata or query page mappings for every unrelated actor event.
        if (!SafeNameEquals(reinterpret_cast<UObject*>(functionValue),
                "BndEvt__HostSandboxMatchButton_K2Node_ComponentBoundEvent_34_OnClicked__DelegateSignature"))
            return false;

        UObject* object = reinterpret_cast<UObject*>(objectValue);
        UFunction* function = reinterpret_cast<UFunction*>(functionValue);
        if (!object || !function ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !Memory::IsReadable(function, sizeof(UFunction)) ||
            !object->Class ||
            !Memory::IsReadable(object->Class, sizeof(UClass)))
        {
            return false;
        }

        if (SafeName(reinterpret_cast<UObject*>(object->Class)) !=
            "OfflinePlayMenuWidget_C")
        {
            return false;
        }

        // The exact packed click identifies the Counselor surface even if
        // a replay's visibility callback has not restored the cached flags.
        g_UsingStockBaseSandboxRow.store(true);
        g_UsingStockBaseCounselorRow.store(false);

        Logger::Success(
            "STOCK PACKED COUNSELOR CLICK: ProcessEvent bridge intercepted the relabeled Sandbox row before Sandbox travel");
        CounselorEntryExecHook(object, nullptr, nullptr);
        return true;
#endif
    }

    bool SuppressEmptyStockKillerLoadoutEvent(
        void* objectValue,
        void* functionValue)
    {
#if !defined(F13_BASE_GAME_PORT)
        (void)objectValue;
        (void)functionValue;
        return false;
#else
        // OnlineFix can reach the stock frontend without a backend inventory.
        // In that state SCKillerCharacterProfile.GrabKills has length zero.
        // Jason_Select_Widget nevertheless indexes slots 0..3 and its
        // SetKillerWeapon event subsequently follows null loadout data.  The
        // weapon/grab-kill choice is presentation-only for counselor mode: the
        // selected Jason class itself is recorded through KillerPick and the
        // AI spawns with that class's native defaults.  Avoid manufacturing
        // UObject references or mutating the disconnected player profile.
        if (!g_CounselorMenuRouteLatched.load() ||
            g_CounselorBirthComplete.load() ||
            g_StockBackendLoginSucceeded.load())
            return false;

        if (!SafeNameEquals(reinterpret_cast<UObject*>(functionValue),
                "SetKillerWeapon"))
            return false;

        UObject* object = reinterpret_cast<UObject*>(objectValue);
        UFunction* function = reinterpret_cast<UFunction*>(functionValue);
        if (!object || !function ||
            !Memory::IsReadable(object, sizeof(UObject)) ||
            !Memory::IsReadable(function, sizeof(UFunction)) ||
            !object->Class ||
            !Memory::IsReadable(object->Class, sizeof(UClass)))
        {
            return false;
        }

        if (SafeName(reinterpret_cast<UObject*>(object->Class)) !=
            "Jason_Select_Widget_C")
        {
            return false;
        }

        static std::atomic<bool> logged{ false };
        if (!logged.exchange(true))
        {
            Logger::Success(
                "STOCK JASON PICKER SAFETY: suppressed SetKillerWeapon because the offline profile has no grab-kill inventory; native Jason class selection remains active");
        }
        return true;
#endif
    }

}
