#pragma once

class UObject;
class AActor;
class UFunction;
class UClass;

namespace FrozenJasonBridge
{
    bool InstallOfflineDamageBridge();
    void PrepareCounselorSpawnResources();
    // One-shot hosted spawn preflight, game thread only. No offline adoption.
    void* GetHostedCounselorBehaviorTree();
    bool GrantHumanStartingItem(AActor* pawn, UClass* itemClass);
    // Game thread only; caller bounds retries and verifies CurrentKiller.
    // Restore only an absent challenge weapon through native equipment handling.
    bool GrantChallengeMissingWeapon(AActor* pawn, UClass* weaponClass);
    // Hosted Hunt reuses primary AI and authority-scoped counselor death
    // setup. Solo roster/stat/loot and local-only ending shortcuts stay out.
    bool AdoptHostedLobbyJason(void* world, AActor* jason, UObject* controller);
    bool RegisterHostedLobbyJason(AActor* jason, UObject* controller);
    bool IsHostedLobbyAIActive();
    // Retire gameplay only. Keep native death/outro observer state until
    // completed scene accounting or world teardown; do not reset early.
    void RetireHostedGameplayForNativeEnding();
    bool CanRegisterHostedLobbyJason();
    void RegisterHostedLobbyCounselor(AActor* counselor);
    void PreparePamelaTargetBeforeUse(UObject* object, UFunction* function);
    void PrepareNativeOutroOwner(UObject* object, UFunction* function);
    void PrepareSandboxHotkeyResources();
    // Observe only the exact manager's final-action RPC during a scoped
    // synchronous attempt; no interaction or authority state is changed.
    void TraceFinalInteractionLockEvent(
        UObject* object, UFunction* function, void* params, bool afterOriginal);

    // F6 queues one local-inventory diagnostic for the game thread. The
    // worker thread never dereferences game objects.
    void QueueMaskPickupDiagnostic();

    // Runs immediately before a cabinet's construction script. Replace only
    // tape/walkie class entries in its native item-spawner list, so drawer
    // loot is born as a useful pickup and no attached actor is destroyed.
    void ReplaceOfflineCabinetLootBeforeSpawn(
        UObject* object,
        UFunction* function);

    // Adopt the donor-style AI Jason created by the dedicated counselor
    // lifecycle.  The frozen Features.cpp implementation remains byte-for-byte
    // unchanged; this translation-unit adapter is the only integration seam.
    bool AdoptCounselorModeJason(
        AActor* jason,
        UObject* killerController,
        AActor* localCounselor);

    void ResetCounselorModeJason();

    // Called after the stock BlueprintNativeEvent has evaluated CanSpectate.
    // Counselor mode keeps every stock spectator rule, widening only the
    // active AI Jason PlayerState so next/previous cycling can reach him.
    bool AllowCounselorToSpectateJason(
        UFunction* function,
        void* params);
    void MirrorHumanCounselorEmote(
        UObject* object,
        UFunction* function,
        void* params);
    // Local-only edge dispatch, paced at 16 ms from the shared game-thread
    // ReceiveTick lane; independent of Jason AI or online host authority.
    void PumpOfflineCounselorRBEmoteOnGameThread();

    // The stock native CanSpectate path does not dispatch through the local
    // controller's ProcessEvent vtable in this build.  Observe the actual
    // next/previous spectator RPC instead and insert the active AI Jason into
    // that otherwise-stock cycle.  F4 remains a direct toggle fallback.
    bool HandleCounselorSpectatorCycleEvent(
        UObject* object,
        UFunction* function);

    // Observe a real damage event against Jason during the earned Pamela
    // trance window. This is a cheap ProcessEvent name/pointer gate and avoids
    // relying solely on Jason's clamped health value to recognize the hit.
    bool ObservePamelaTranceDamageEvent(
        UObject* object,
        UFunction* function);

    // Stock's seven-byte pawn block is absent in this executable. Widen the
    // local counselor's validated native stat getter result instead.
    void OverrideStockCounselorStatResult(
        UObject* object,
        UFunction* function,
        void* params);

    // Preserve the authored Jason kill stance between the first and second
    // melee hits. This is a pointer/name gate on ProcessEvent, active only for
    // the bounded Pamela kneel window.
    bool SuppressPamelaKneelEndStun(
        UObject* object,
        UFunction* function);

    // Keep ClientsPlayOutro's stock completion path, but skip its cabin
    // sequence shortly after it starts when Jason's death was confirmed.
    void SkipDeadJasonCabinOutroAfterStart(
        UObject* object,
        UFunction* function);

    // Reuse the stock spectator-camera update, but replace only its pose while
    // F4 is locked to the active AI Jason. This avoids competing camera calls.
    bool RewriteJasonSpectatorCameraUpdate(
        UFunction* function,
        void* params);

    // BlueprintUpdateCamera is the final camera-manager handoff. Override its
    // returned pose after stock evaluation so Jason spectating stays stable.
    bool OverrideJasonSpectatorCameraResult(
        UFunction* function,
        void* params);
}
