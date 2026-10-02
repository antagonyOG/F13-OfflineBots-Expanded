// Included inside FrozenJasonBridge after the existing property/call helpers.
// Wave replacement, loaded on first use; F2 is an optional retry.
// Never discovers a montage by scanning GObjects.
namespace
{
    UObject* g_UrinationWaveDefault = nullptr;
    UObject* g_UrinationOriginalMaleMontage = nullptr;
    UObject* g_UrinationOriginalFemaleMontage = nullptr;
    UObject* g_UrinationLoadedMontage = nullptr;
    void* g_UrinationPendingAction = nullptr;
    DWORD g_UrinationRequestThread = 0;
    UObject* g_UrinationRequestController = nullptr;
    UWorld* g_UrinationRequestWorld = nullptr;
    ULONGLONG g_UrinationRequestedAt = 0;
    bool g_UrinationPatched = false;
    bool g_UrinationBothGenders = false;
    bool g_UrinationReplacementSuspended = false;
    bool g_UrinationHooksInstalled = false;
    bool g_UrinationWaitingForWave = false;
    UWorld* g_UrinationPreparationWorld = nullptr;
    ULONGLONG g_UrinationPreparationAt = 0;
    UWorld* g_UrinationAutomaticAttemptWorld = nullptr;
    bool g_UrinationMoveInput = false;
    ULONGLONG g_UrinationMoveInputAt = 0;
    bool g_RBEmoteSelectionObserved = false;
    struct UrinationPlayback
    {
        AActor* Owner = nullptr;
        UClass* Class = nullptr;
        UWorld* World = nullptr;
        ULONGLONG StartedAt = 0;
        int32_t CurrentEmoteOffset = 0;
    };
    UrinationPlayback g_UrinationPlayback[JasonAITargetCapacity]{};
    thread_local bool g_UrinationRequestScope = false;
    using UrinationLoadCtorFn = void*(__fastcall*)(void*, void*, void*);
    using UrinationLoadedDelegateFn = bool(__fastcall*)(void*, UObject*);
    UrinationLoadCtorFn g_UrinationLoadCtorOriginal = nullptr;
    UrinationLoadedDelegateFn g_UrinationLoadedDelegateOriginal = nullptr;

    UObject* GetUrinationLoaderDefault()
    {
        // The native spawn lane need not initialize the archived AI's helper
        // cache. Read the engine's existing KismetSystemLibrary class singleton
        // instead. Exact executable gates + class/CDO identity; no object scan,
        // native class creation, possession change, or startup asset loading.
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
        const uint8_t getterBytes[]{0x4c,0x8b,0xdc,0x48,0x83,0xec,0x78,
            0x48,0x8b,0x05,0xa2,0x11,0x9b,0x01};
        const uint8_t nameReferenceBytes[]{0x48,0x8d,0x15,0x6c,0xdc,0xf3,0x00};
        const auto* getter = reinterpret_cast<const void*>(base + 0x187EB80);
        const auto* nameReference = reinterpret_cast<const void*>(base + 0x187EBBF);
        auto** singleton = reinterpret_cast<UClass**>(base + 0x0322FD30);
        if (!base || !Memory::IsReadable(getter, sizeof(getterBytes)) ||
            !Memory::IsReadable(nameReference, sizeof(nameReferenceBytes)) ||
            std::memcmp(getter, getterBytes, sizeof(getterBytes)) ||
            std::memcmp(nameReference, nameReferenceBytes, sizeof(nameReferenceBytes)) ||
            !Memory::IsReadable(singleton, sizeof(UClass*))) return nullptr;
        UClass* library = *singleton;
        if (!library || !Memory::IsReadable(library, sizeof(UClass)) ||
            JasonAISafeName(reinterpret_cast<UObject*>(library)) != "KismetSystemLibrary") return nullptr;
        UObject* defaults = library->DefaultObject;
        if (!defaults || !Memory::IsReadable(defaults, sizeof(UObject)) ||
            defaults->Class != library ||
            JasonAISafeName(defaults) != "Default__KismetSystemLibrary") return nullptr;
        return defaults;
    }

    void RememberWaveDefaultForUrination(UObject* emoteClass)
    {
        if (!emoteClass || !Memory::IsReadable(emoteClass, sizeof(UClass))) return;
        UObject* defaults = reinterpret_cast<UClass*>(emoteClass)->DefaultObject;
        if (!defaults || !Memory::IsReadable(defaults, sizeof(UObject)) ||
            JasonAISafeName(defaults) != "Default__Emote_Wave_C") return;
        if (defaults != g_UrinationWaveDefault && !g_UrinationPendingAction)
        {
            g_UrinationWaveDefault = defaults;
            g_UrinationPatched = false;
            g_UrinationBothGenders = false;
            g_UrinationReplacementSuspended = false;
            g_UrinationOriginalMaleMontage = nullptr;
            g_UrinationOriginalFemaleMontage = nullptr;
            g_UrinationLoadedMontage = nullptr;
            Logger::Debug("18L-EMOTE Wave template validated for F2 preparation");
        }
        if (!g_UrinationPatched && !g_UrinationRequestedAt &&
            g_UrinationAutomaticAttemptWorld != Engine::GetWorld())
        {
            g_UrinationAutomaticAttemptWorld = Engine::GetWorld();
            g_UrinationWaitingForWave = true;
            g_UrinationPreparationWorld = Engine::GetWorld();
            g_UrinationPreparationAt = GetTickCount64();
            Logger::Debug("18L-EMOTE automatic Wave replacement queued; F2 is optional retry only");
        }
    }

    bool CancelOnlyUrinationEmote(AActor* owner, const char* reason)
    {
        if (!g_UrinationPatched || !owner || !IsValidatedLiveCounselorPawn(owner) ||
            !owner->Class || IsPamelaExclusiveKillLaneActive(GetTickCount64()) ||
            ReadReflectedObjectProperty(owner, "CurrentEmote") != g_UrinationLoadedMontage) return false;
        // Use the same native server/multicast cancellation as ordinary emotes.
        // Stopping an arbitrary montage would leave CurrentEmote/input state
        // behind and could interrupt Pamela or a paired Jason kill animation.
        UFunction* cancel = FindFunctionInHierarchyByName(owner->Class, "SERVER_CancelEmote");
        if (!cancel || cancel->Size != 0 || cancel->Children)
        { Logger::Error("18L-EMOTE native cancellation layout unavailable; no montage/input reset forced"); return false; }
        const bool called = SafeProcessEventCall(reinterpret_cast<uintptr_t>(owner), owner, cancel, nullptr);
        if (called) Logger::Debug(std::string("18L-EMOTE urination native cancel | reason=") + reason);
        return called;
    }

    void TrackUrinationPlayback(AActor* owner)
    {
        if (!g_UrinationPatched || !owner || !IsValidatedLiveCounselorPawn(owner) || !owner->Class ||
            ReadReflectedObjectProperty(owner, "CurrentEmote") != g_UrinationLoadedMontage) return;
        UPropertyLite* current = FindPropertyInHierarchyByName(owner->Class, "CurrentEmote");
        if (!current || current->ArrayDim != 1 || current->ElementSize != sizeof(UObject*) ||
            current->Offset_Internal <= 0 || current->Offset_Internal + sizeof(UObject*) > owner->Class->Size ||
            JasonAISafeName(reinterpret_cast<UObject*>(current->ClassPrivate)) != "ObjectProperty") return;
        UrinationPlayback* empty = nullptr;
        for (auto& slot : g_UrinationPlayback)
        {
            if (slot.Owner == owner)
            {
                slot = { owner, owner->Class, Engine::GetWorld(), GetTickCount64(), current->Offset_Internal };
                return;
            }
            if (!empty && !slot.Owner) empty = &slot;
        }
        if (empty)
        {
            *empty = { owner, owner->Class, Engine::GetWorld(), GetTickCount64(), current->Offset_Internal };
            Logger::Debug("18L-EMOTE urination playback tracked with 25-second native cancellation bound");
        }
    }

    void PumpUrinationPlayback(ULONGLONG now)
    {
        if (!g_UrinationPatched) return;
        AActor* local = nullptr;
        bool localResolved = false;
        bool stoppedBot = false;
        for (auto& slot : g_UrinationPlayback)
        {
            if (!slot.Owner) continue;
            if (!g_JasonAIState.Active || slot.World != Engine::GetWorld() ||
                !Memory::IsReadable(slot.Owner, sizeof(UObject)) || slot.Owner->Class != slot.Class)
            { slot = {}; continue; }
            // No reflected calls per bot per frame. Expiry is staggered to
            // one native bot cancellation per 50ms tick.
            if (!localResolved)
            { local = ReadLivePossessedCounselor(Engine::GetLocalPlayerController()); localResolved = true; }
            const bool isLocal = slot.Owner == local;
            const bool expired = now - slot.StartedAt >= 25000;
            if ((!isLocal && !expired) || (!isLocal && stoppedBot)) continue;
            const auto* field = reinterpret_cast<UObject* const*>(
                reinterpret_cast<const uint8_t*>(slot.Owner) + slot.CurrentEmoteOffset);
            if (!Memory::IsReadable(field, sizeof(UObject*)) || *field != g_UrinationLoadedMontage)
            { slot = {}; continue; } // crouch/hit/new montage already canceled it
            DWORD foregroundPid = 0;
            if (isLocal) GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
            const bool move = isLocal && foregroundPid == GetCurrentProcessId() &&
                now - slot.StartedAt >= 250 &&
                ((g_UrinationMoveInput && now - g_UrinationMoveInputAt < 100) ||
                 (GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState('A') & 0x8000) ||
                 (GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState('D') & 0x8000));
            if (!move && !expired) continue;
            AActor* owner = slot.Owner;
            slot = {}; // clear before native cancellation can reenter
            if (!isLocal) stoppedBot = true;
            CancelOnlyUrinationEmote(owner, move ? "movement" : "25-second-duration");
        }
    }

    bool StockCounselorCanConfirmEmote(AActor* owner)
    {
        // Same native eligibility used by OnRequestShowEmoteSelect: excludes
        // death, paired interactions, current emotes and active sweater montage.
        // This is NOT the ability's consumed/ready bit; a used sweater should
        // not prevent ordinary emotes once its action has actually finished.
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
        const uint8_t signature[]{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x01};
        const auto* native = reinterpret_cast<const void*>(base + 0x3A7600);
        if (!owner || !base || !Memory::IsReadable(native, sizeof(signature)) ||
            std::memcmp(native, signature, sizeof(signature))) return false;
        __try
        {
            return reinterpret_cast<bool(__fastcall*)(AActor*)>(base + 0x3A7600)(owner);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void RestoreWaveAfterFailedSelection()
    {
        if (!g_UrinationPatched || g_UrinationReplacementSuspended ||
            !g_UrinationWaveDefault || !Memory::IsReadable(g_UrinationWaveDefault, sizeof(UObject)) ||
            !g_UrinationWaveDefault->Class || !g_UrinationOriginalMaleMontage ||
            !Memory::IsReadable(g_UrinationOriginalMaleMontage, sizeof(UObject)) ||
            JasonAISafeName(g_UrinationOriginalMaleMontage) != "AM_m_normal_stand_emote_wave") return;
        UPropertyLite* male = FindPropertyInHierarchyByName(g_UrinationWaveDefault->Class, "MaleMontage");
        if (!male || male->ArrayDim != 1 || male->ElementSize != sizeof(UObject*) ||
            male->Offset_Internal <= 0 ||
            male->Offset_Internal + sizeof(UObject*) > g_UrinationWaveDefault->Class->Size ||
            JasonAISafeName(reinterpret_cast<UObject*>(male->ClassPrivate)) != "ObjectProperty") return;
        auto** field = reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(g_UrinationWaveDefault) + male->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(UObject*)) || *field != g_UrinationLoadedMontage) return;
        UObject** femaleField = nullptr;
        if (g_UrinationBothGenders)
        {
            if (!g_UrinationOriginalFemaleMontage ||
                !Memory::IsReadable(g_UrinationOriginalFemaleMontage, sizeof(UObject)) ||
                JasonAISafeName(g_UrinationOriginalFemaleMontage) != "AM_f_normal_stand_emote_wave") return;
            UPropertyLite* female = FindPropertyInHierarchyByName(g_UrinationWaveDefault->Class, "FemaleMontage");
            if (!female || female->ArrayDim != 1 || female->ElementSize != sizeof(UObject*) ||
                female->Offset_Internal <= 0 ||
                female->Offset_Internal + sizeof(UObject*) > g_UrinationWaveDefault->Class->Size ||
                JasonAISafeName(reinterpret_cast<UObject*>(female->ClassPrivate)) != "ObjectProperty") return;
            femaleField = reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(g_UrinationWaveDefault) + female->Offset_Internal);
            if (!Memory::IsReadable(femaleField, sizeof(UObject*)) || *femaleField != g_UrinationLoadedMontage) return;
        }
        // Both identities preflight before restoring either field. The retained
        // shared urination montage is left intact for already-playing bots.
        *field = g_UrinationOriginalMaleMontage;
        if (femaleField) *femaleField = g_UrinationOriginalFemaleMontage;
        g_UrinationReplacementSuspended = true;
        // Keep g_UrinationPatched/playback tracking alive so bots already using
        // the retained urination montage still get native expiry/cancellation.
        Logger::Error("18L-EMOTE failed native selection: restored ordinary Wave before wheel hide; urination retry disabled until restart");
    }

    bool CommitLocalRBEmote(AActor* owner)
    {
        // RB is not the stock Y release binding. Native OnHideEmoteSelect
        // removes the widget but does NOT confirm its highlighted selection.
        // This fallback is needed without a sweater too, not just after pickup.
        // Confirm the actual highlighted emote through the native widget's
        // own profile lookup, not a hardcoded Wave, pawn/ability flag reset,
        // synthetic Y press, or direct multicast animation.
        if (g_RBEmoteSelectionObserved || !owner ||
            owner != ReadLivePossessedCounselor(Engine::GetLocalPlayerController()) ||
            !IsValidatedLiveCounselorPawn(owner)) return false;
        auto unavailable = [](const char* stage) -> bool
        {
            static UWorld* world = nullptr;
            static const char* reportedStages[16]{};
            if (world != Engine::GetWorld())
            { world = Engine::GetWorld(); std::memset(reportedStages, 0, sizeof(reportedStages)); }
            for (auto& previous : reportedStages)
            {
                if (previous && !std::strcmp(previous, stage)) return false;
                if (!previous)
                {
                    previous = stage;
                    Logger::Debug(std::string("18L-EMOTE RB radial confirmation unavailable | stage=") + stage);
                    break;
                }
            }
            return false;
        };
        if (IsPamelaExclusiveKillLaneActive(GetTickCount64())) return unavailable("kill-sequence-active");
        if (!StockCounselorCanConfirmEmote(owner))
        {
            // Distinguish a leftover playing emote from active sweater/death
            // eligibility without resetting any pawn, ability or input flags.
            UObject* current = ReadReflectedObjectProperty(owner, "CurrentEmote");
            return unavailable(current ? "stock-pawn-current-emote" : "stock-pawn-not-emote-eligible");
        }
        UObject* controller = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
        UObject* hud = ReadReflectedObjectProperty(controller, "MyHUD");
        if (!hud || !ObjectClassDerivesFromExact(hud, "SCInGameHUD") ||
            ReadReflectedObjectProperty(hud, "PlayerOwner") != controller) return unavailable("local-hud-ownership");
        UObject* widget = ReadReflectedObjectProperty(hud, "EmoteWidgetInstance");
        if (!widget || !ObjectClassDerivesFromExact(widget, "SCCounselorEmoteSelectWidget"))
            return unavailable("active-radial-class");
        // The live HUD's active radial is the authoritative in-match context.
        // bIsInGame also controls the profile-menu Blueprint selection path;
        // it rejected live wheels, so validate actual owning pawn instead.
        UFunction* owningPawn = FindFunctionInHierarchyByName(widget->Class, "GetOwningPlayerPawn");
        if (!owningPawn || owningPawn->Size != sizeof(UObject*) || !owningPawn->Children ||
            reinterpret_cast<uintptr_t>(owningPawn->ExecFunction) !=
                reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)) + 0x10658D0 ||
            !Memory::IsReadable(owningPawn->Children, sizeof(UPropertyLite)))
            return unavailable("radial-owner-getter-layout");
        const auto* result = reinterpret_cast<const UPropertyLite*>(owningPawn->Children);
        if (result->ArrayDim != 1 || result->ElementSize != sizeof(UObject*) ||
            result->Offset_Internal != 0 || result->Next ||
            JasonAISafeName(reinterpret_cast<UObject*>(result->ClassPrivate)) != "ObjectProperty" ||
            JasonAISafeName(reinterpret_cast<UObject*>(owningPawn->Children)) != "ReturnValue")
            return unavailable("radial-owner-result-layout");
        UObject* widgetOwner = nullptr;
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(widget), widget, owningPawn, &widgetOwner) ||
            widgetOwner != owner) return unavailable("radial-pawn-ownership");
        UPropertyLite* highlight = FindPropertyInHierarchyByName(widget->Class, "CurrentHighlightSection");
        if (!highlight || highlight->ArrayDim != 1 || highlight->ElementSize != sizeof(int32_t) ||
            highlight->Offset_Internal <= 0 || highlight->Offset_Internal + sizeof(int32_t) > widget->Class->Size ||
            JasonAISafeName(reinterpret_cast<UObject*>(highlight->ClassPrivate)) != "IntProperty")
            return unavailable("highlight-layout");
        const auto* indexField = reinterpret_cast<const int32_t*>(
            reinterpret_cast<const uint8_t*>(widget) + highlight->Offset_Internal);
        if (!Memory::IsReadable(indexField, sizeof(int32_t))) return unavailable("highlight-unreadable");
        const int32_t selected = *indexField;
        if (selected < 0 || selected >= 8) return unavailable("no-highlighted-slot");
        UFunction* play = FindFunctionInHierarchyByName(widget->Class, "PlayEmote");
        if (!play || play->Size != sizeof(int32_t) || !play->Children ||
            reinterpret_cast<uintptr_t>(play->ExecFunction) !=
                reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)) + 0x489630 ||
            !Memory::IsReadable(play->Children, sizeof(UPropertyLite))) return unavailable("play-function-layout");
        const auto* parameter = reinterpret_cast<const UPropertyLite*>(play->Children);
        if (parameter->ArrayDim != 1 || parameter->ElementSize != sizeof(int32_t) ||
            parameter->Offset_Internal != 0 || parameter->Next ||
            JasonAISafeName(reinterpret_cast<UObject*>(parameter->ClassPrivate)) != "IntProperty" ||
            JasonAISafeName(reinterpret_cast<UObject*>(play->Children)) != "Index")
            return unavailable("play-parameter-layout");
        int32_t params = selected;
        const bool called = SafeProcessEventCall(reinterpret_cast<uintptr_t>(widget), widget, play, &params);
        if (!called) RestoreWaveAfterFailedSelection();
        Logger::Debug("18L-EMOTE RB local-owner native radial confirmation | index=" +
            std::to_string(selected) + " | dispatched=" + (called ? "true" : "false") +
            " | serverSelectionObserved=" + (g_RBEmoteSelectionObserved ? "true" : "false") +
            " | currentMontage=" + JasonAISafeName(ReadReflectedObjectProperty(owner, "CurrentEmote")));
        return called && g_RBEmoteSelectionObserved;
    }

    void* __fastcall UrinationLoadCtorBridge(void* action, void* path, void* latent)
    {
        void* result = g_UrinationLoadCtorOriginal(action, path, latent);
        // Only the synchronous constructor reached from OUR F2 LoadAsset call.
        // Other engine asset requests always pass through untouched.
        if (g_UrinationRequestScope && GetCurrentThreadId() == g_UrinationRequestThread &&
            result == action && Memory::IsReadable(action, 0x128) &&
            Memory::IsReadable(latent, 24) &&
            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(latent) + 4) == 0x18E030 &&
            *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(latent) + 16) == g_UrinationRequestController)
            g_UrinationPendingAction = action;
        return result;
    }

    bool RetainUrinationAssets(UObject* waveClass, UObject* montage, UObject* original,
        UObject* originalFemale = nullptr)
    {
        // A CDO property retains dependencies ONLY while its UClass itself is
        // reachable. Profile soft-class lookup does not provide that lifetime.
        // Pin these bounded process-lifetime resources using the stock RootSet
        // operation, with exact executable and individual registry identity
        // proof. No registry traversal, actor roots or per-frame GC work.
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
        const uint8_t nativeRootBytes[]{0x48,0x63,0x48,0x0c,0x3b,0x0d,0xcb,0xa8,0xde,0x02,
            0x7d,0x0f,0x48,0x8d,0x14,0x49,0x48,0x8b,0x0d,0xb2,0xa8,0xde,0x02,
            0x48,0x8d,0x34,0xd1,0x81,0x4e,0x08,0x00,0x00,0x00,0x40};
        const auto* code = reinterpret_cast<const void*>(base + 0x2EEFA7);
        const uintptr_t registry = base + 0x030D9870;
        if (!base || !Memory::IsReadable(code, sizeof(nativeRootBytes)) ||
            std::memcmp(code, nativeRootBytes, sizeof(nativeRootBytes)) ||
            !Memory::IsReadable(reinterpret_cast<void*>(registry), 0x10)) return false;
        const uintptr_t items = *reinterpret_cast<const uintptr_t*>(registry);
        const int count = *reinterpret_cast<const int32_t*>(registry + 0x0C);
        if (!items || count <= 0 || count > 500000) return false;
        UObject* resources[]{waveClass, montage, original, originalFemale};
        const int resourceCount = originalFemale ? 4 : 3;
        volatile LONG* flags[4]{};
        for (int i = 0; i < resourceCount; ++i)
        {
            UObject* object = resources[i];
            if (!object || !Memory::IsReadable(object, sizeof(UObject)) ||
                object->InternalIndex < 0 || object->InternalIndex >= count) return false;
            const uintptr_t item = items + static_cast<uintptr_t>(object->InternalIndex) * 24;
            if (!Memory::IsReadable(reinterpret_cast<void*>(item), 24) ||
                *reinterpret_cast<UObject* const*>(item) != object ||
                (*reinterpret_cast<const uint32_t*>(item + 8) & 0x30000000u)) return false;
            flags[i] = reinterpret_cast<volatile LONG*>(item + 8);
        }
        // All identities preflight before any write. Atomic bit updates preserve
        // unrelated flags and use the same bit/stride as native AddToRoot.
        for (int i = 0; i < resourceCount; ++i) InterlockedOr(flags[i], 0x40000000L);
        Logger::Debug(originalFemale
            ? "18L-EMOTE Wave class, urination montage and both original Waves retained for process lifetime"
            : "18L-EMOTE Wave class, urination montage and original Wave retained for process lifetime");
        return true;
    }

    struct UrinationSlotTrack
    {
        FName SlotName;
        TArray<uint8_t> AnimSegments; // opaque; never read, resize or copy segments
    };
    struct UrinationSlotGroup
    {
        FName GroupName;
        TArray<FName> SlotNames;
    };
    static_assert(sizeof(UrinationSlotTrack) == 24, "Stock SlotAnimationTrack ABI");
    static_assert(sizeof(UrinationSlotGroup) == 24, "Stock AnimSlotGroup ABI");

    FName* FindUrinationSharedSlot(UObject* montage, UObject* skeleton, FName& cinematic)
    {
        auto unavailable = [](const char* stage) -> FName*
        {
            Logger::Debug(std::string("18L-EMOTE female shared slot unavailable | stage=") + stage);
            return nullptr; // this helper runs only on the one completed load
        };
        // Stock cooked Base_Counselor_AnimBP places Cinematic above the gender
        // blend; Male/FemaleNonCinematic are below it. Both Wave montages and
        // this animation use the SAME skeleton. Change only this retained
        // montage's slot, never a pawn's gender, mesh, input or kill montage.
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
        // File-image descriptor proof failed in the live process. Validate the
        // actual registered script structs instead of assuming file constants
        // remain a readable/unchanged runtime representation.
        const uint8_t trackGetter[]{0x48,0x83,0xec,0x28,0x48,0x8b,0x05,0x9d,0xd0,0xa2,0x01};
        const uint8_t groupGetter[]{0x48,0x83,0xec,0x28,0x48,0x8b,0x05,0x25,0xaf,0x93,0x01};
        auto schema = [&](uintptr_t getter, const uint8_t* bytes, uintptr_t singleton,
            const char* name, const char* member, const char* type, int offset, int size) -> bool
        {
            if (!base || !Memory::IsReadable(reinterpret_cast<void*>(base + getter), 11) ||
                std::memcmp(reinterpret_cast<void*>(base + getter), bytes, 11) ||
                !Memory::IsReadable(reinterpret_cast<void*>(base + singleton), sizeof(void*))) return false;
            UStruct* definition = *reinterpret_cast<UStruct* const*>(base + singleton);
            if (!definition || !Memory::IsReadable(definition, sizeof(UStruct)) || definition->Size != 24 ||
                JasonAISafeName(reinterpret_cast<UObject*>(definition)) != name) return false;
            int inspected = 0;
            for (UField* field = definition->Children; field && inspected++ < 64; field = field->Next)
            {
                if (!Memory::IsReadable(field, sizeof(UPropertyLite))) return false;
                const auto* property = reinterpret_cast<const UPropertyLite*>(field);
                if (JasonAISafeName(reinterpret_cast<UObject*>(field)) == member)
                    return property->ArrayDim == 1 && property->ElementSize == size &&
                        property->Offset_Internal == offset &&
                        JasonAISafeName(reinterpret_cast<UObject*>(property->ClassPrivate)) == type;
            }
            return false;
        };
        if (!schema(0x17FC4D0, trackGetter, 0x3229578, "SlotAnimationTrack", "SlotName", "NameProperty", 0, 8) ||
            !schema(0x18FA080, groupGetter, 0x3234FB0, "AnimSlotGroup", "SlotNames", "ArrayProperty", 8, 16))
            return unavailable("live-script-struct-proof");
        auto arrayField = [](UObject* object, const char* name, int offset) -> const TArray<uint8_t>*
        {
            if (!object || !Memory::IsReadable(object, sizeof(UObject)) || !object->Class ||
                !Memory::IsReadable(object->Class, sizeof(UClass))) return nullptr;
            UPropertyLite* field = FindPropertyInHierarchyByName(object->Class, name);
            if (!field || field->ArrayDim != 1 || field->ElementSize != sizeof(TArray<uint8_t>) ||
                field->Offset_Internal != offset || offset + sizeof(TArray<uint8_t>) > object->Class->Size ||
                JasonAISafeName(reinterpret_cast<UObject*>(field->ClassPrivate)) != "ArrayProperty") return nullptr;
            const auto* value = reinterpret_cast<const TArray<uint8_t>*>(
                reinterpret_cast<const uint8_t*>(object) + offset);
            return Memory::IsReadable(value, sizeof(*value)) ? value : nullptr;
        };
        const auto* tracks = arrayField(montage, "SlotAnimTracks", 0x160);
        const auto* groups = arrayField(skeleton, "SlotGroups", 0x258);
        if (!tracks) return unavailable("montage-array-property");
        if (!groups) return unavailable("skeleton-array-property");
        if (tracks->Count != 1 || tracks->Max < 1 || tracks->Max > 4 ||
            !Memory::IsReadable(tracks->Data, sizeof(UrinationSlotTrack)) ||
            groups->Count < 1 || groups->Count > 32 || groups->Max < groups->Count || groups->Max > 64 ||
            !Memory::IsReadable(groups->Data, groups->Count * sizeof(UrinationSlotGroup)))
            return unavailable("array-counts-or-storage");
        auto* track = reinterpret_cast<UrinationSlotTrack*>(tracks->Data);
        if (track->SlotName.Number != 0 ||
            !BridgeFNameEntryEquals(GNames, track->SlotName.ComparisonIndex, "MaleNonCinematic"))
            return unavailable("original-montage-slot-name");
        const auto* slotGroups = reinterpret_cast<const UrinationSlotGroup*>(groups->Data);
        // Bounded existing skeleton names only, not a whole GNames/GObjects scan.
        for (int group = 0; group < groups->Count; ++group)
        {
            const auto& names = slotGroups[group].SlotNames;
            if (names.Count < 0 || names.Count > 64 || names.Max < names.Count || names.Max > 128 ||
                (names.Count && !Memory::IsReadable(names.Data, names.Count * sizeof(FName))))
                return unavailable("skeleton-slot-names-storage");
            for (int slot = 0; slot < names.Count; ++slot)
                if (names.Data[slot].Number == 0 &&
                    BridgeFNameEntryEquals(GNames, names.Data[slot].ComparisonIndex, "Cinematic"))
                {
                    cinematic = names.Data[slot];
                    return &track->SlotName;
                }
        }
        return unavailable("shared-slot-not-registered");
    }

    bool PublishCompletedUrinationMontage(UObject* montage)
    {
        if (!g_JasonAIState.Active || Engine::GetWorld() != g_UrinationRequestWorld ||
            !montage || !Memory::IsReadable(montage, sizeof(UObject)) || !montage->Class ||
            JasonAISafeName(montage) != "m_normal_stand_piss_Montage" ||
            JasonAISafeName(reinterpret_cast<UObject*>(montage->Class)) != "AnimMontage") return false;
        UObject* defaults = g_UrinationWaveDefault;
        if (!defaults || !Memory::IsReadable(defaults, sizeof(UObject)) || !defaults->Class ||
            !Memory::IsReadable(defaults->Class, sizeof(UClass)) ||
            JasonAISafeName(defaults) != "Default__Emote_Wave_C" ||
            defaults->Class->DefaultObject != defaults) return false;
        UPropertyLite* male = FindPropertyInHierarchyByName(defaults->Class, "MaleMontage");
        if (!male || male->ArrayDim != 1 || male->ElementSize != sizeof(UObject*) ||
            male->Offset_Internal <= 0 || male->Offset_Internal + sizeof(UObject*) > defaults->Class->Size ||
            JasonAISafeName(reinterpret_cast<UObject*>(male->ClassPrivate)) != "ObjectProperty") return false;
        auto** field = reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(defaults) + male->Offset_Internal);
        if (!Memory::IsReadable(field, sizeof(UObject*))) return false;
        UObject* original = *field;
        if (!original || !Memory::IsReadable(original, sizeof(UObject)) || !original->Class ||
            JasonAISafeName(original) != "AM_m_normal_stand_emote_wave" ||
            JasonAISafeName(reinterpret_cast<UObject*>(original->Class)) != "AnimMontage") return false;
        UObject* originalSkeleton = ReadReflectedObjectProperty(original, "Skeleton");
        if (!originalSkeleton || ReadReflectedObjectProperty(montage, "Skeleton") != originalSkeleton) return false;
        // Female support is optional/fail-closed; a mismatched layout leaves
        // female Wave untouched and still publishes the proven male version.
        UPropertyLite* female = FindPropertyInHierarchyByName(defaults->Class, "FemaleMontage");
        UObject** femaleField = nullptr;
        FName cinematic;
        FName* sharedSlot = nullptr;
        if (female && female->ArrayDim == 1 && female->ElementSize == sizeof(UObject*) &&
            female->Offset_Internal > 0 && female->Offset_Internal + sizeof(UObject*) <= defaults->Class->Size &&
            JasonAISafeName(reinterpret_cast<UObject*>(female->ClassPrivate)) == "ObjectProperty")
        {
            auto** candidate = reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(defaults) + female->Offset_Internal);
            if (Memory::IsReadable(candidate, sizeof(UObject*)) && *candidate &&
                Memory::IsReadable(*candidate, sizeof(UObject)) &&
                JasonAISafeName(*candidate) == "AM_f_normal_stand_emote_wave" &&
                ObjectClassDerivesFromExact(*candidate, "AnimMontage") &&
                ReadReflectedObjectProperty(*candidate, "Skeleton") == originalSkeleton)
            {
                sharedSlot = FindUrinationSharedSlot(montage, originalSkeleton, cinematic);
                if (sharedSlot) femaleField = candidate;
            }
            else
                Logger::Debug("18L-EMOTE female Wave montage/skeleton proof unavailable; Wave unchanged");
        }
        else
            Logger::Debug("18L-EMOTE female Wave native property proof unavailable; Wave unchanged");
        // The engine invokes OnLoaded AFTER its handle completes, while the
        // latent action/streamable manager still owns the loaded asset. Publish
        // into the native ObjectProperty here, BEFORE that action is retired.
        // The Wave CDO's GC-visible hard reference retains montage/dependencies;
        // a raw global pointer or a soft-reference lookup is not the owner.
        // Male repeat use and sweater coexistence were verified with retention;
        // live shared-slot proof also passed in that same Sept 30 run.
        constexpr bool enableSharedGenderSlotTrial = true;
        UObject* originalFemale = femaleField && sharedSlot && enableSharedGenderSlotTrial
            ? *femaleField : nullptr;
        if (!RetainUrinationAssets(reinterpret_cast<UObject*>(defaults->Class), montage, original, originalFemale))
        {
            Logger::Error("18L-EMOTE asset retention proof failed; Wave unchanged");
            return false;
        }
        g_UrinationOriginalMaleMontage = original;
        g_UrinationOriginalFemaleMontage = originalFemale;
        if (femaleField && sharedSlot && enableSharedGenderSlotTrial)
        {
            *sharedSlot = cinematic;
            *femaleField = montage; // native GC-visible reference, same as male
            g_UrinationBothGenders = true;
        }
        else if (femaleField && sharedSlot)
            Logger::Debug("18L-EMOTE female shared-slot proof passed; publication held for male repeat-use stability test");
        *field = montage;
        g_UrinationLoadedMontage = montage;
        g_UrinationPatched = true;
        return true;
    }

    bool __fastcall UrinationLoadedDelegateBridge(void* delegate, UObject* loaded)
    {
        void* action = g_UrinationPendingAction;
        if (action && delegate == reinterpret_cast<uint8_t*>(action) + 0x118 &&
            GetCurrentThreadId() == g_UrinationRequestThread)
        {
            // Clear before any helper can reenter a reflected/native call.
            g_UrinationPendingAction = nullptr;
            g_UrinationRequestedAt = 0;
            // UpdateOperation also invokes this callback on cancellation.
            // Accept only its verified completed (not canceled) handle state.
            void* handle = Memory::IsReadable(action, 0x128)
                ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(action) + 0xF0) : nullptr;
            const bool completed = handle && Memory::IsReadable(handle, 0x13) &&
                reinterpret_cast<uint8_t*>(handle)[0x10] != 0 &&
                reinterpret_cast<uint8_t*>(handle)[0x12] == 0;
            const bool published = completed && PublishCompletedUrinationMontage(loaded);
            Logger::Debug(published
                ? (g_UrinationBothGenders
                    ? "18L-EMOTE completed-load accepted: Wave uses urination for both genders via validated shared Cinematic slot"
                    : "18L-EMOTE completed-load accepted: male Wave uses urination; female/shared-slot proof unavailable, female Wave unchanged")
                : "18L-EMOTE F2 completed-load rejected: identity/property/skeleton proof failed; Wave unchanged");
        }
        return g_UrinationLoadedDelegateOriginal(delegate, loaded);
    }

    bool InstallUrinationCompletionHooks()
    {
        if (g_UrinationHooksInstalled) return true;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
        const uint8_t ctorBytes[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18};
        const uint8_t delegateBytes[]{0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20};
        auto* ctor = reinterpret_cast<void*>(base + 0x14AF000);
        auto* delegate = reinterpret_cast<void*>(base + 0x4243E0);
        if (!base || !Memory::IsReadable(ctor, sizeof(ctorBytes)) ||
            !Memory::IsReadable(delegate, sizeof(delegateBytes)) ||
            std::memcmp(ctor, ctorBytes, sizeof(ctorBytes)) ||
            std::memcmp(delegate, delegateBytes, sizeof(delegateBytes))) return false;
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
        if (MH_CreateHook(ctor, reinterpret_cast<void*>(&UrinationLoadCtorBridge),
                reinterpret_cast<void**>(&g_UrinationLoadCtorOriginal)) != MH_OK) return false;
        if (MH_CreateHook(delegate, reinterpret_cast<void*>(&UrinationLoadedDelegateBridge),
                reinterpret_cast<void**>(&g_UrinationLoadedDelegateOriginal)) != MH_OK)
        { MH_RemoveHook(ctor); return false; }
        // Batch installation to suspend/resume game threads only once.
        if (MH_QueueEnableHook(delegate) != MH_OK || MH_QueueEnableHook(ctor) != MH_OK ||
            MH_ApplyQueued() != MH_OK)
        {
            MH_DisableHook(delegate); MH_DisableHook(ctor);
            MH_RemoveHook(delegate); MH_RemoveHook(ctor);
            return false;
        }
        g_UrinationHooksInstalled = true;
        return true;
    }

    UPropertyLite* UrinationParameter(UFunction* function, const char* wanted)
    {
        if (!function) return nullptr;
        for (UField* field = function->Children; field; field = field->Next)
        {
            if (!Memory::IsReadable(field, sizeof(UPropertyLite))) return nullptr;
            if (JasonAISafeName(reinterpret_cast<UObject*>(field)) == wanted)
                return reinterpret_cast<UPropertyLite*>(field);
        }
        return nullptr;
    }
    bool UrinationParameterFits(UFunction* function, UPropertyLite* field, int size, int limit)
    {
        return function && field && function->Size > 0 && function->Size <= limit &&
            field->ArrayDim == 1 && field->ElementSize == size && field->Offset_Internal >= 0 &&
            field->Offset_Internal + size <= function->Size;
    }
}

void UpdateUrinationEmoteOnGameThread()
{
    static ULONGLONG nextPoll = 0;
    static bool lastF2 = false;
    const ULONGLONG now = GetTickCount64();
    if (now < nextPoll) return;
    nextPoll = now + 50;
    PumpUrinationPlayback(now);
    // F2 may precede the first Wave. Remember only this world's explicit
    // request, then start the load on a later AI tick (never inside an emote
    // ProcessEvent callback). No menu/startup loading or repeated retries.
    if (g_UrinationWaitingForWave &&
        (Engine::GetWorld() != g_UrinationPreparationWorld ||
         now - g_UrinationPreparationAt > 120000))
    {
        g_UrinationWaitingForWave = false;
        Logger::Debug("18L-EMOTE F2 preparation expired or world changed; Wave unchanged");
    }
    if (g_UrinationRequestedAt && now - g_UrinationRequestedAt > 30000)
    {
        g_UrinationPendingAction = nullptr;
        g_UrinationRequestedAt = 0;
        Logger::Error("18L-EMOTE F2 load timed out; Wave unchanged");
    }
    const bool down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    const bool press = down && !lastF2;
    lastF2 = down;
    const bool prepared = g_UrinationWaitingForWave && g_UrinationWaveDefault;
    if (!press && !prepared) return;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    if (foregroundPid != GetCurrentProcessId() || !g_JasonAIState.Active ||
        !ReadLivePossessedCounselor(Engine::GetLocalPlayerController()) ||
        (g_HostedOnlineBridge ? !IsHostedAuthorityWorld() : !OfflineSetup::IsCounselorMatchInProgress())) return;
    if (g_UrinationPatched)
    { Logger::Debug(g_UrinationReplacementSuspended
        ? "18L-EMOTE Wave reverted after failed selection; restart before another urination trial"
        : g_UrinationBothGenders
        ? "18L-EMOTE F2 already enabled: both genders use Wave slot"
        : "18L-EMOTE F2 already enabled: male counselors use Wave slot; female Wave unchanged"); return; }
    if (g_UrinationRequestedAt)
    { Logger::Debug("18L-EMOTE F2 load already pending"); return; }
    if (!g_UrinationWaveDefault)
    {
        g_UrinationWaitingForWave = true;
        g_UrinationPreparationWorld = Engine::GetWorld();
        g_UrinationPreparationAt = now;
        Logger::Debug("18L-EMOTE F2 armed: perform Wave once; load will start automatically afterward");
        return;
    }
    // Complete the first ordinary-Wave group before swapping its shared CDO.
    // Otherwise asynchronous completion can make half the staggered group
    // urinate while the human/earlier bots are still doing ordinary Wave.
    if (g_PendingEmoteMirrorIndex < g_PendingEmoteMirrorCount)
    {
        g_UrinationWaitingForWave = true;
        g_UrinationPreparationWorld = Engine::GetWorld();
        g_UrinationPreparationAt = now;
        return;
    }
    g_UrinationWaitingForWave = false;
    UObject* kismet = GetUrinationLoaderDefault();
    if (!kismet || !Memory::IsReadable(kismet, sizeof(UObject)) || !kismet->Class)
    { Logger::Error("18L-EMOTE F2 loader context unavailable; Wave unchanged"); return; }
    UFunction* makePath = FindFunctionInHierarchyByName(kismet->Class, "MakeSoftObjectPath");
    UFunction* load = FindFunctionInHierarchyByName(kismet->Class, "LoadAsset");
    UPropertyLite* pathInput = UrinationParameter(makePath, "PathString");
    if (!pathInput) pathInput = UrinationParameter(makePath, "Path");
    UPropertyLite* pathOutput = UrinationParameter(makePath, "ReturnValue");
    UPropertyLite* assetInput = UrinationParameter(load, "Asset");
    UPropertyLite* worldInput = UrinationParameter(load, "WorldContextObject");
    UPropertyLite* latentInput = UrinationParameter(load, "LatentInfo");
    UPropertyLite* delegateInput = UrinationParameter(load, "OnLoaded");
    const bool layoutValid = UrinationParameterFits(makePath, pathInput, sizeof(FString), 0x100) &&
        UrinationParameterFits(makePath, pathOutput, 24, 0x100) &&
        UrinationParameterFits(load, assetInput, 40, 0x200) &&
        UrinationParameterFits(load, worldInput, sizeof(UObject*), 0x200) &&
        UrinationParameterFits(load, latentInput, 24, 0x200) &&
        UrinationParameterFits(load, delegateInput, 16, 0x200);
    if (!layoutValid)
    {
        auto describe = [](UPropertyLite* field) -> std::string
        {
            return field ? std::to_string(field->Offset_Internal) + "/" +
                std::to_string(field->ElementSize) + "/" + std::to_string(field->ArrayDim) : "missing";
        };
        Logger::Error("18L-EMOTE F2 loader parameter layout rejected | path=" + describe(pathInput) +
            " | result=" + describe(pathOutput) + " | asset=" + describe(assetInput) +
            " | world=" + describe(worldInput) + " | latent=" + describe(latentInput) +
            " | delegate=" + describe(delegateInput));
        return;
    }
    if (!InstallUrinationCompletionHooks())
    { Logger::Error("18L-EMOTE F2 loader native signature/hook proof failed; Wave unchanged"); return; }
    static wchar_t assetPath[] = L"/Game/Cinematics/SP_Testing/m_normal_stand_piss_Montage.m_normal_stand_piss_Montage";
    alignas(16) uint8_t pathParams[0x100]{};
    FString input{};
    input.Data = assetPath;
    input.NumElements = static_cast<int32_t>(sizeof(assetPath) / sizeof(wchar_t));
    input.MaxElements = input.NumElements;
    std::memcpy(pathParams + pathInput->Offset_Internal, &input, sizeof(input));
    if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(kismet), kismet, makePath, pathParams))
    { Logger::Error("18L-EMOTE F2 path construction failed; Wave unchanged"); return; }
    const uint8_t* softPath = pathParams + pathOutput->Offset_Internal;
    if (!GNames || !GNames->IsValidIndex(*reinterpret_cast<const int32_t*>(softPath)))
    { Logger::Error("18L-EMOTE F2 asset path invalid; Wave unchanged"); return; }
    alignas(16) uint8_t loadParams[0x200]{};
    *reinterpret_cast<int32_t*>(loadParams + assetInput->Offset_Internal) = -1;
    std::memcpy(loadParams + assetInput->Offset_Internal + 16, softPath, 24);
    *reinterpret_cast<UObject**>(loadParams + worldInput->Offset_Internal) = reinterpret_cast<UObject*>(Engine::GetWorld());
    *reinterpret_cast<int32_t*>(loadParams + latentInput->Offset_Internal) = -1;
    *reinterpret_cast<int32_t*>(loadParams + latentInput->Offset_Internal + 4) = 0x18E030;
    *reinterpret_cast<UObject**>(loadParams + latentInput->Offset_Internal + 16) = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
    // Empty OnLoaded delegate: our narrow native completion observer captures
    // the argument before the stock delegate's harmless empty-binding return.
    g_UrinationRequestThread = GetCurrentThreadId();
    g_UrinationRequestController = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
    g_UrinationRequestWorld = Engine::GetWorld();
    g_UrinationRequestedAt = now;
    g_UrinationRequestScope = true;
    const bool requested = SafeProcessEventCall(reinterpret_cast<uintptr_t>(kismet), kismet, load, loadParams);
    g_UrinationRequestScope = false;
    if (!requested || !g_UrinationPendingAction)
    {
        g_UrinationRequestedAt = 0;
        g_UrinationPendingAction = nullptr;
        Logger::Error("18L-EMOTE F2 request did not capture its own latent action; Wave unchanged");
        return;
    }
    Logger::Debug("18L-EMOTE F2 signed stock urination requested; awaiting native completed-load callback");
}
