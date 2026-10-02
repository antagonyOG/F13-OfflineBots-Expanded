// Reversible challenge-only script entry override. Direct Blueprint calls can
// bypass UFunction::ExecFunction; no thunk interception or generic Reset hook.
struct ChallengeBoundaryScriptOverride {
    UFunction* Function = nullptr;
    uint64_t Identity = 0;
    uint8_t* Data = nullptr;
    int32_t Count = 0;
    uint8_t Original[2]{};
    bool Applied = false;
};
struct ChallengeJasonBoundaryState {
    uint64_t WorldIdentity = 0, ModeIdentity = 0, PawnIdentity = 0, StateIdentity = 0;
    ULONGLONG NextInstall = 0;
    unsigned Attempts = 0;
    bool Ready = false;
    ChallengeBoundaryScriptOverride Outside{}, Restart{};
};
static ChallengeJasonBoundaryState g_ChallengeJasonBoundary{};
static constexpr auto& ChallengeBoundaryReturn = f13::challenges::boundaryscript::VoidReturn;

static bool ChallengeJasonBoundarySelected()
{
    return f13::challenges::nativeui::HasCommittedSettings() &&
        f13::challenges::nativeui::GetCommittedSettings().enabled[
            static_cast<size_t>(f13::challenges::Option::DisableJasonBoundary)];
}

static UPropertyLite* ChallengeJasonPawnParameter(UFunction* function)
{
    // FName interning can retain either cooked display spelling.
    auto* input = ChallengeBoundaryParam(function, "ScKillerCharacter", "ObjectProperty", sizeof(UObject*));
    if (!input) input = ChallengeBoundaryParam(function, "SCKillerCharacter", "ObjectProperty", sizeof(UObject*));
    return input && (input->PropertyFlags & 0x80) ? input : nullptr;
}

static bool WriteChallengeBoundaryPrefix(uint8_t* address, const uint8_t* expected, const uint8_t* replacement)
{
    if (!address || !Memory::IsReadable(address, 2) || memcmp(address, expected, 2) != 0) return false;
    DWORD protection = 0;
    if (!VirtualProtect(address, 2, PAGE_READWRITE, &protection)) return false;
    memcpy(address, replacement, 2);
    DWORD unused = 0;
    VirtualProtect(address, 2, protection, &unused);
    return memcmp(address, replacement, 2) == 0;
}

static void RestoreChallengeBoundaryScript(ChallengeBoundaryScriptOverride& owned)
{
    if (!owned.Applied) { owned = {}; return; }
    // No stale function/buffer dereference; restore only our exact owned edit.
    if (ChallengeBoundaryLive(reinterpret_cast<UObject*>(owned.Function), owned.Identity))
    {
        const auto* script = reinterpret_cast<const NativeScriptArray*>(reinterpret_cast<uintptr_t>(owned.Function) + 0x48);
        if (Memory::IsReadable(script, sizeof(*script)) && script->Data == owned.Data && script->Count == owned.Count)
            WriteChallengeBoundaryPrefix(owned.Data, ChallengeBoundaryReturn, owned.Original);
    }
    owned = {};
}

static void RestoreChallengeJasonBoundary()
{
    RestoreChallengeBoundaryScript(g_ChallengeJasonBoundary.Outside);
    RestoreChallengeBoundaryScript(g_ChallengeJasonBoundary.Restart);
    g_ChallengeJasonBoundary = {};
}

static bool PrepareChallengeBoundaryScript(UFunction* function, f13::challenges::boundaryscript::Kind kind,
    ChallengeBoundaryScriptOverride& owned)
{
    if (!function || !Memory::IsReadable(function, sizeof(UFunction)) || (function->FunctionFlags & 0x400)) return false;
    const auto* script = reinterpret_cast<const NativeScriptArray*>(reinterpret_cast<uintptr_t>(function) + 0x48);
    if (!Memory::IsReadable(script, sizeof(*script)) || script->Count < 3 || script->Count > 0x10000 || script->Max < script->Count ||
        !script->Data || !Memory::IsReadable(script->Data, script->Count) ||
        !f13::challenges::boundaryscript::MatchesStock(script->Data, script->Count, script->Max, kind)) return false;
    owned.Function = function;
    owned.Identity = ChallengeObjectIdentity(function);
    owned.Data = script->Data;
    owned.Count = script->Count;
    memcpy(owned.Original, owned.Data, 2);
    return owned.Identity != 0;
}

static void PumpChallengeJasonBoundary(const f13::challenges::runtime::Scope& scope)
{
    if (!g_ChallengeRouteActive.load() || !g_ChallengeMissionAuthorized || !scope.stockChallengeMission ||
        !scope.localAuthority || scope.networkSession || !scope.inProgress || !ChallengeJasonBoundarySelected() ||
        !ChallengeIsCurrentJason(scope.primaryJason, scope.world, scope.controller))
    {
        RestoreChallengeJasonBoundary();
        return;
    }
    const auto worldIdentity = ChallengeObjectIdentity(reinterpret_cast<void*>(scope.world));
    const auto modeIdentity = ChallengeObjectIdentity(ChallengeCurrentGameMode());
    const auto pawnIdentity = ChallengeObjectIdentity(scope.primaryJason);
    const auto stateIdentity = ChallengeObjectIdentity(scope.gameState);
    if (!worldIdentity || !modeIdentity || !pawnIdentity || !stateIdentity) { RestoreChallengeJasonBoundary(); return; }
    auto& bound = g_ChallengeJasonBoundary;
    if (bound.WorldIdentity != worldIdentity || bound.ModeIdentity != modeIdentity ||
        bound.PawnIdentity != pawnIdentity || bound.StateIdentity != stateIdentity)
        RestoreChallengeJasonBoundary();
    bound.WorldIdentity = worldIdentity;
    bound.ModeIdentity = modeIdentity;
    bound.PawnIdentity = pawnIdentity;
    bound.StateIdentity = stateIdentity;
    const auto now = GetTickCount64();
    if (bound.Ready || bound.Attempts >= 10 || now < bound.NextInstall) return;
    bound.NextInstall = now + 1000;
    ++bound.Attempts;
    const auto reject = [&](const char* reason) {
        if (bound.Attempts == 1 || bound.Attempts == 10)
            Logger::Error(std::string("CHALLENGE JASON BOUNDARY: activation rejected | stage=") + reason +
                " | attempt=" + std::to_string(bound.Attempts));
    };
    auto* klass = FindClassExact("SinglePlayer_Dispatcher_C");
    if (!klass) { reject("dispatcher class not loaded"); return; }
    auto* inside = FindFunctionInHierarchyByName(klass, "SP_Level_Boundary_Inside");
    auto* outside = FindFunctionInHierarchyByName(klass, "SP_Level_Boundary_Outside");
    auto* restart = FindFunctionInHierarchyByName(klass, "Restart_Mission_Out_Of_Bounds");
    if (!inside || !outside || !restart ||
        reinterpret_cast<void*>(inside->OuterPrivate) != reinterpret_cast<void*>(klass) ||
        reinterpret_cast<void*>(outside->OuterPrivate) != reinterpret_cast<void*>(klass) ||
        reinterpret_cast<void*>(restart->OuterPrivate) != reinterpret_cast<void*>(klass) ||
        !ChallengeJasonPawnParameter(inside) || !ChallengeJasonPawnParameter(outside))
        { reject("class-owned functions/pawn ABI"); return; }
    UField* field = restart->Children;
    unsigned fields = 0;
    while (field && fields++ < 64)
    {
        if (!Memory::IsReadable(field, sizeof(UPropertyLite)) ||
            (reinterpret_cast<UPropertyLite*>(field)->PropertyFlags & 0x80))
            { reject("restart field ABI"); return; }
        field = field->Next;
    }
    if (field) { reject("restart field bound"); return; }
    // Audited stock runtime-expanded sizes and entry expressions. Outside
    // begins PushExecutionFlow to its return; Restart begins LetObj LocalVariable.
    using f13::challenges::boundaryscript::Kind;
    if (!PrepareChallengeBoundaryScript(outside, Kind::Outside, bound.Outside) ||
        !PrepareChallengeBoundaryScript(restart, Kind::Restart, bound.Restart))
    {
        reject("stock script signature");
        if (bound.Attempts == 1 || bound.Attempts == 10)
        {
            const auto* a = reinterpret_cast<const NativeScriptArray*>(reinterpret_cast<uintptr_t>(outside) + 0x48);
            const auto* b = reinterpret_cast<const NativeScriptArray*>(reinterpret_cast<uintptr_t>(restart) + 0x48);
            Logger::Error("CHALLENGE JASON BOUNDARY: script counts | outside=" +
                std::to_string(a->Count) + " | restart=" + std::to_string(b->Count));
        }
        return;
    }
    bound.Outside.Applied = WriteChallengeBoundaryPrefix(bound.Outside.Data, bound.Outside.Original, ChallengeBoundaryReturn);
    if (bound.Outside.Applied)
        bound.Restart.Applied = WriteChallengeBoundaryPrefix(bound.Restart.Data, bound.Restart.Original, ChallengeBoundaryReturn);
    if (!bound.Outside.Applied || !bound.Restart.Applied)
    {
        reject("script write; restoring partial override");
        RestoreChallengeBoundaryScript(bound.Outside);
        RestoreChallengeBoundaryScript(bound.Restart);
        bound.Attempts = 10; // Do not retry a failed write every pump.
        return;
    }
    bound.Ready = true;
    Logger::Success("CHALLENGE JASON BOUNDARY: stock Outside and boundary-only Restart scripts disabled; direct Blueprint calls covered; normal mission endings unchanged");
}
