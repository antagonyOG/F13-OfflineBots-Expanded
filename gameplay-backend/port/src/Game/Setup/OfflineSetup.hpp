#pragma once
#include <cstdint>

namespace OfflineSetup
{
    bool SetMapIndex(int32_t value);
    bool SetJasonIndex(int32_t value);
    bool SetPlayerCounselorIndex(int32_t value);
    bool SetDifficulty(int32_t value);
    bool SetCounselorCount(int32_t value);
    bool SetWeather(int32_t value);

    bool QueueArmSelectedSetup();
    bool QueueSelectedJason();
    bool QueueSelectedCounselor();
    bool QueueApplyGameSetupPreset();
    bool QueueDumpCounselorRoster();

    // Counselor-mode coordinator. The temporary Sandbox menu row is redirected
    // into OfflineBots synchronously; the frozen AI implementation remains
    // untouched.
    bool QueueSandboxCounselorSync();
    bool IsCounselorMenuRouteLatched();
    // Stock Offline Bots can arrive at PreMatchIntro without the legacy
    // selection save after the disconnected-login fallback.  Return only the
    // game's own class default for that exact missing lookup.
    void* StockJasonSelectionSaveFallback(void* requestedClass);
    bool BirthSelectedCounselorForPreMatch(void* gameMode);
    bool IsCounselorBirthComplete();
    void MarkCounselorMatchInProgress();
    // Called before either native OfflineBots match-start path initializes
    // GameState timers. A selected duration replaces the legacy fixed hour.
    void ApplyCommittedMatchDurationBeforeStart(void* gameMode);
    void ApplyCommittedOfflineDifficultyBeforeStart(void* gameMode);
    void BeginCommittedOfflineSettingsAfterStart(void* gameMode);
    float GetCommittedJasonDamageScale(void* world);
    int32_t GetCommittedAdditionalStartingCounselors();
    // Reuse the committed difficulty for F1 actors without replacing their AI.
    void ApplyCommittedJasonDifficultyToPawn(void* pawn);
    bool IsCounselorMatchInProgress();
    bool SpawnCounselorModeJasonAfterMatch(void* gameMode);
    bool BeginFrozenAICompatibilityShim();
    void EndFrozenAICompatibilityShim();
    bool IsCounselorModeAutoStartArmed();
    bool IsSandboxCounselorReady();
    int32_t GetCounselorModeBotCount();
    void MarkCounselorModeAutoStartFinished();

    bool HasPendingObjectivesPatch();
    bool TryPatchObjectivesWidgetOnGameThread();
    bool TryPatchObjectivesWidgetForEventOnGameThread(void* objectValue);
    bool HasPendingRequest();
    // Read-only online readiness probe, including private-host/InProgress and
    // lifecycle surfaces. The separate F8 request executes the hosted transaction.
    void QueuePrivateLobbyAudit();
    // Hosted experimental counselor lifecycle; all mutations run on game thread.
    void QueueHostedCounselorArm();
    bool IsHostedCounselorArmed();
    bool IsHostedJasonArmed();
    bool QueueHostedAdditionalJason();
    bool QueueHostedCounselor();
    void TickWorker();
    bool ConsumePendingRequestOnGameThread();
    void ObserveMainMenuVisibilityEvent(
        void* objectValue,
        void* functionValue,
        void* params);
    bool IsStockMainMenuReadyForHook();
    // The untouched packed base menu can dispatch its repurposed Sandbox row
    // directly through ProcessEvent without entering the UFunction native
    // thunk.  Intercept that one exact click before its Sandbox bytecode runs.
    bool InterceptPackedCounselorMenuClick(
        void* objectValue,
        void* functionValue);
    // The disconnected stock profile has no owned grab-kill loadout.  Its
    // Jason picker still invokes SetKillerWeapon and dereferences that empty
    // profile during the counselor route.  Suppress only that cosmetic setter;
    // KillerPick remains owned by the native picker and is synchronized
    // separately for the gameplay lifecycle.
    bool SuppressEmptyStockKillerLoadoutEvent(
        void* objectValue,
        void* functionValue);
    void TraceStockJasonPickerEvent(
        void* objectValue,
        void* functionValue);
    void ObserveStockKillerClassRequest(
        void* objectValue,
        void* functionValue,
        void* params);
    // Keep the stock difficulty indices, but translate only the counselor
    // setup row's visible ValueText during its native SetText event.
    void TranslateCounselorSkillDisplayText(
        void* objectValue,
        void* functionValue,
        void* params);
    // The stock combo changes its native index even when its Blueprint value
    // update bypasses the SetText/ubergraph hooks. Refresh the visible text
    // after game-thread menu events, only when the index actually changed.
    void PumpCounselorSkillDisplayOnGameThread();
    // Lazy stock challenge UI/effects; these never initialize on startup.
    void ObserveChallengeSetupEvent(void* objectValue, void* functionValue, bool afterOriginal);
    bool HasActiveChallengeRoute();
    void PumpChallengeRuntimeOnGameThread();
    // F1 reinforcements must not replace the primary Jason owner used by
    // stock kill interactions and match outro completion.
    void VerifyPrimaryKillerOwnerAfterReinforcement();
}
