// Included inside OfflineSetup's private namespace after challenge helpers.
// See research/CHALLENGE_ESCAPE_PRECOMMIT_20260927.md for native ABI evidence.
using ChallengeEscapeOverlapFn = void(__fastcall*)(UObject*, UObject*, UObject*, UObject*, int32_t, bool, const void*);
static ChallengeEscapeOverlapFn g_ChallengeEscapeOriginal = nullptr;
// The character transition is shared by native escape paths, including callers
// which do not enter the overlap callback. Block before its first state write.
using ChallengeEscapeStartFn = void(__fastcall*)(UObject*, UObject*, float);
static ChallengeEscapeStartFn g_ChallengeEscapeStartOriginal = nullptr;
using ChallengeEscapePresentationFn = void(__fastcall*)(UObject*, UObject*);
using ChallengeEscapeCompleteFn = void(__fastcall*)(UObject*, UObject*, UObject*);
static ChallengeEscapePresentationFn g_ChallengeEscapePresentationOriginal = nullptr;
static ChallengeEscapeCompleteFn g_ChallengeEscapeCompleteOriginal = nullptr;
using ChallengeVehicleEscapeStartFn = void(__fastcall*)(UObject*, UObject*, UObject*, float);
static ChallengeVehicleEscapeStartFn g_ChallengeVehicleEscapeStartOriginal = nullptr;
static bool g_ChallengeEscapeVerified = false;
struct ChallengeBoundaryActor {
    UObject* Object = nullptr;
    uint64_t Identity = 0;
    FVector SafeLocation{};
    FRotator SafeRotation{};
    bool HasSafePoint = false;
    ULONGLONG SampledAt = 0;
    ULONGLONG RecoveredAt = 0;
    float EscapeSphereRadius = 0; // Only used by cached SCEscapeVolume records.
};
static std::array<ChallengeBoundaryActor, 24> g_ChallengeBoundaryPawns{};
static std::array<ChallengeBoundaryActor, 48> g_ChallengeBoundaryVolumes{};
static std::array<ChallengeBoundaryActor, 24> g_ChallengeBoundaryVehicles{};
static uintptr_t g_ChallengeBoundaryWorld = 0;
static uint64_t g_ChallengeBoundaryWorldIdentity = 0;
static uint64_t g_ChallengeBoundaryModeIdentity = 0;
static UObject* g_ChallengeBoundaryState = nullptr;
static uint64_t g_ChallengeBoundaryStateIdentity = 0;
static DWORD g_ChallengeBoundaryThread = 0;
static ULONGLONG g_ChallengeBoundaryNextRoster = 0;
static ULONGLONG g_ChallengeBoundaryNextSlice = 0;
static ULONGLONG g_ChallengeBoundaryNextSample = 0;
static std::array<ChallengeBoundaryActor, 24> g_ChallengeBoundaryStagingPawns{};
static std::array<ChallengeBoundaryActor, 48> g_ChallengeBoundaryStagingVolumes{};
static std::array<ChallengeBoundaryActor, 24> g_ChallengeBoundaryStagingVehicles{};
static size_t g_ChallengeBoundaryPawnCount = 0, g_ChallengeBoundaryVolumeCount = 0, g_ChallengeBoundaryVehicleCount = 0;
static int32_t g_ChallengeBoundaryLevelCursor = 0, g_ChallengeBoundaryActorCursor = 0;
static uint64_t g_ChallengeBoundaryRosterSignature = 0;
static bool g_ChallengeBoundaryBuilding = false;
static bool g_ChallengeBoundaryBuildValid = false;
static bool g_ChallengeBoundaryRosterComplete = false;
static bool g_ChallengeBoundaryRecovering = false;
static unsigned g_ChallengeBoundaryReports = 0;
static unsigned g_ChallengeBoundaryRosterReports = 0;
static unsigned g_ChallengeBoundarySampleReports = 0;
static unsigned g_ChallengeBoundaryForwardReports = 0;
static unsigned g_ChallengeBoundaryRejectedReports = 0;
static unsigned g_ChallengeBoundaryVehicleReports = 0;
static unsigned g_ChallengeBoundaryVehicleGateReports = 0;
static unsigned g_ChallengeBoundaryGeometryReports = 0;

static bool ChallengeBoundaryLive(UObject* value, uint64_t identity = 0)
{
    if (!value || !Memory::IsReadable(value, sizeof(UObject)) || !value->Class ||
        (value->ObjectFlags & 0x30) || !Memory::IsReadable(value->Class, sizeof(UClass))) return false;
    const uint64_t current = ChallengeObjectIdentity(value);
    return current && (!identity || current == identity);
}

static bool ChallengeBoundaryAi(UObject* actor)
{
    if (!ChallengeBoundaryLive(actor) || !ClassDerivesFrom(actor->Class, "SCCounselorCharacter") ||
        !Memory::IsReadable(actor, 0x111) ||
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(actor) + 0x110) != 3) return false;
    UObject* controller = nullptr;
    UObject* pawn = nullptr;
    return ChallengeReadObjectField(actor, "Controller", &controller) && ChallengeBoundaryLive(controller) &&
        ClassDerivesFrom(controller->Class, "AIController") &&
        ChallengeReadObjectField(controller, "Pawn", &pawn) && pawn == actor;
}

static bool ChallengeBoundaryInWorld(UObject* actor)
{
    for (int depth = 0; actor && depth < 8; ++depth)
    {
        if (reinterpret_cast<uintptr_t>(actor) == g_ChallengeBoundaryWorld) return true;
        if (!ChallengeBoundaryLive(actor)) return false;
        // A streamed ULevel's Outer is its asset UWorld, not necessarily the
        // persistent play UWorld. Membership in the play world's Levels array
        // establishes ownership without rejecting placed sublevel counselors.
        if (ClassDerivesFrom(actor->Class, "Level"))
        {
            const auto* levels = reinterpret_cast<const TArray<ULevel*>*>(g_ChallengeBoundaryWorld + 0x110);
            if (!g_ChallengeBoundaryWorld || !Memory::IsReadable(levels, sizeof(*levels)) ||
                !levels->Data || levels->Count <= 0 || levels->Count > 128 || levels->Max < levels->Count ||
                !Memory::IsReadable(levels->Data, static_cast<size_t>(levels->Count) * sizeof(ULevel*))) return false;
            for (int32_t n = 0; n < levels->Count; ++n)
                if (reinterpret_cast<UObject*>(levels->Data[n]) == actor) return true;
            return false;
        }
        actor = reinterpret_cast<UObject*>(actor->OuterPrivate);
    }
    return false;
}

static UPropertyLite* ChallengeBoundaryParam(UFunction* fn, const char* name, const char* type, size_t bytes)
{
    if (!fn || fn->Size < 0 || fn->Size > 0x200) return nullptr;
    auto* p = FindPropertyInStructByName(reinterpret_cast<UStruct*>(fn), name);
    if (!p || p->ArrayDim != 1 || p->ElementSize != bytes || p->Offset_Internal < 0 ||
        static_cast<size_t>(p->Offset_Internal) + bytes > static_cast<size_t>(fn->Size) ||
        SafeName(reinterpret_cast<UObject*>(p->ClassPrivate)) != type) return nullptr;
    return p;
}

static bool ChallengeBoundaryReadTransform(UObject* actor, FVector& position, FRotator& rotation)
{
    auto* locationFn = FindFunctionInHierarchyByName(actor->Class, "K2_GetActorLocation");
    auto* rotationFn = FindFunctionInHierarchyByName(actor->Class, "K2_GetActorRotation");
    auto* locationResult = ChallengeBoundaryParam(locationFn, "ReturnValue", "StructProperty", sizeof(position));
    auto* rotationResult = ChallengeBoundaryParam(rotationFn, "ReturnValue", "StructProperty", sizeof(rotation));
    if (!locationResult || !rotationResult) return false;
    uint8_t data[0x200]{};
    if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(actor), actor, locationFn, data)) return false;
    memcpy(&position, data + locationResult->Offset_Internal, sizeof(position));
    memset(data, 0, sizeof(data));
    if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(actor), actor, rotationFn, data)) return false;
    memcpy(&rotation, data + rotationResult->Offset_Internal, sizeof(rotation));
    return std::isfinite(position.X) && std::isfinite(position.Y) && std::isfinite(position.Z) &&
        std::isfinite(rotation.Pitch) && std::isfinite(rotation.Yaw) && std::isfinite(rotation.Roll);
}

static bool ChallengeBoundaryCacheEscapeSphere(ChallengeBoundaryActor& volume)
{
    UObject* sphere = nullptr;
    if (!ChallengeBoundaryLive(volume.Object, volume.Identity) ||
        !ChallengeReadObjectField(volume.Object, "EscapeVolume", &sphere) || !ChallengeBoundaryLive(sphere) ||
        !ClassDerivesFrom(sphere->Class, "SphereComponent")) return false;
    auto* location = FindFunctionInHierarchyByName(sphere->Class, "K2_GetComponentLocation");
    auto* radius = FindFunctionInHierarchyByName(sphere->Class, "GetScaledSphereRadius");
    auto* locationResult = ChallengeBoundaryParam(location, "ReturnValue", "StructProperty", sizeof(FVector));
    auto* radiusResult = ChallengeBoundaryParam(radius, "ReturnValue", "FloatProperty", sizeof(float));
    if (!locationResult || !radiusResult) return false;
    uint8_t data[0x200]{};
    if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(sphere), sphere, location, data)) return false;
    FVector center{};
    memcpy(&center, data + locationResult->Offset_Internal, sizeof(center));
    memset(data, 0, sizeof(data));
    if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(sphere), sphere, radius, data)) return false;
    float scaledRadius = 0;
    memcpy(&scaledRadius, data + radiusResult->Offset_Internal, sizeof(scaledRadius));
    if (!std::isfinite(center.X) || !std::isfinite(center.Y) || !std::isfinite(center.Z) ||
        !std::isfinite(scaledRadius) || scaledRadius <= 0 || scaledRadius > 100000) return false;
    volume.SafeLocation = center;
    volume.EscapeSphereRadius = scaledRadius;
    volume.HasSafePoint = true;
    return true;
}

static bool ChallengeBoundaryVehiclePathHitsEscape(const FVector& from, const FVector& to, bool& complete)
{
    using namespace f13::challenges::boundary;
    complete = g_ChallengeBoundaryRosterComplete;
    bool hit = false, any = false;
    for (const auto& volume : g_ChallengeBoundaryVolumes)
    {
        if (!volume.Object) continue;
        any = true;
        if (!volume.HasSafePoint || !ChallengeBoundaryLive(volume.Object, volume.Identity))
        {
            complete = false;
            continue;
        }
        hit = hit || SegmentTouchesSphere(Point{from.X,from.Y,from.Z}, Point{to.X,to.Y,to.Z},
            Point{volume.SafeLocation.X,volume.SafeLocation.Y,volume.SafeLocation.Z}, volume.EscapeSphereRadius);
    }
    complete = complete && any;
    return hit;
}

static bool ChallengeBoundaryOutsideAll(UObject* actor)
{
    if (!g_ChallengeBoundaryRosterComplete) return false;
    bool any = false;
    for (const auto& volume : g_ChallengeBoundaryVolumes)
    {
        if (!volume.Object) continue;
        if (!ChallengeBoundaryLive(volume.Object, volume.Identity)) return false;
        any = true;
        auto* fn = FindFunctionInHierarchyByName(volume.Object->Class, "IsOverlappingActor");
        auto* input = ChallengeBoundaryParam(fn, "Other", "ObjectProperty", sizeof(UObject*));
        auto* output = ChallengeBoundaryParam(fn, "ReturnValue", "BoolProperty", 1);
        if (!input || !output) return false;
        uint8_t data[0x200]{};
        memcpy(data + input->Offset_Internal, &actor, sizeof(actor));
        if (!SafeProcessEventCall(reinterpret_cast<uintptr_t>(volume.Object), volume.Object, fn, data) ||
            data[output->Offset_Internal] != 0) return false;
    }
    return any;
}

static bool ChallengeBoundaryEnabled()
{
    if (!g_ChallengeEscapeVerified || !g_ChallengeRouteActive.load() || !g_ChallengeMissionAuthorized ||
        GetCurrentThreadId() != g_ChallengeBoundaryThread ||
        !f13::challenges::nativeui::HasCommittedSettings() ||
        !f13::challenges::nativeui::GetCommittedSettings().enabled[
            static_cast<size_t>(f13::challenges::Option::PreventEscapes)] ||
        ChallengeCurrentWorld() != g_ChallengeBoundaryWorld ||
        !ChallengeBoundaryLive(reinterpret_cast<UObject*>(g_ChallengeBoundaryWorld), g_ChallengeBoundaryWorldIdentity)) return false;
    auto* mode = ChallengeCurrentGameMode();
    if (!ChallengeBoundaryLive(mode, g_ChallengeBoundaryModeIdentity) ||
        !ClassDerivesFrom(mode->Class, "SCGameMode_SPChallenges")) return false;
    if (!ChallengeBoundaryLive(g_ChallengeBoundaryState, g_ChallengeBoundaryStateIdentity) ||
        !ClassDerivesFrom(g_ChallengeBoundaryState->Class, "SCGameState_SPChallenges") ||
        !Memory::IsReadable(reinterpret_cast<void*>(g_ChallengeBoundaryWorld + 0xF8), sizeof(UObject*)) ||
        *reinterpret_cast<UObject**>(g_ChallengeBoundaryWorld + 0xF8) != g_ChallengeBoundaryState) return false;
    auto* match = FindPropertyInHierarchyByName(g_ChallengeBoundaryState->Class, "MatchState");
    if (!match || match->ArrayDim != 1 || match->ElementSize != 8 || match->Offset_Internal < 0 ||
        match->Offset_Internal > 0x10000 ||
        SafeName(reinterpret_cast<UObject*>(match->ClassPrivate)) != "NameProperty") return false;
    const auto* state = reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(g_ChallengeBoundaryState) + match->Offset_Internal);
    if (!Memory::IsReadable(state, 8) || !GNames || !GNames->IsValidIndex(*state)) return false;
    const auto* name = GNames->GetById(*state);
    if (!name || !Memory::IsReadable(name, sizeof(FNameEntry)) ||
        !memchr(name->AnsiName, 0, sizeof(name->AnsiName)) || strcmp(name->AnsiName, "InProgress") != 0) return false;
    UObject* netDriver = nullptr;
    UObject* connection = nullptr;
    auto* local = reinterpret_cast<UObject*>(Engine::GetLocalPlayerController());
    return ChallengeReadObjectField(reinterpret_cast<UObject*>(g_ChallengeBoundaryWorld), "NetDriver", &netDriver) && !netDriver &&
        ChallengeReadObjectField(local, "NetConnection", &connection) && !connection;
}

static bool ChallengeBoundaryRecover(ChallengeBoundaryActor& record)
{
    // Reuse a verified position from this very actor, never a guessed offset.
    const ULONGLONG now = GetTickCount64();
    if (!record.HasSafePoint || now - record.SampledAt > 1500 ||
        !ChallengeBoundaryLive(record.Object, record.Identity) || !ChallengeBoundaryAi(record.Object)) return false;
    auto* teleport = FindFunctionInHierarchyByName(record.Object->Class, "K2_TeleportTo");
    auto* dest = ChallengeBoundaryParam(teleport, "DestLocation", "StructProperty", sizeof(FVector));
    auto* rotation = ChallengeBoundaryParam(teleport, "DestRotation", "StructProperty", sizeof(FRotator));
    auto* result = ChallengeBoundaryParam(teleport, "ReturnValue", "BoolProperty", 1);
    UObject* controller = nullptr;
    UObject* movement = nullptr;
    if (!dest || !rotation || !result ||
        !ChallengeReadObjectField(record.Object, "Controller", &controller) || !ChallengeBoundaryLive(controller) ||
        !ChallengeReadObjectField(record.Object, "CharacterMovement", &movement) || !ChallengeBoundaryLive(movement)) return false;
    auto* stopAi = FindFunctionInHierarchyByName(controller->Class, "StopMovement");
    auto* stopVelocity = FindFunctionInHierarchyByName(movement->Class, "StopMovementImmediately");
    if (!stopAi || !stopVelocity || stopAi->Size != 0 || stopVelocity->Size != 0) return false;
    uint8_t data[0x200]{};
    memcpy(data + dest->Offset_Internal, &record.SafeLocation, sizeof(FVector));
    memcpy(data + rotation->Offset_Internal, &record.SafeRotation, sizeof(FRotator));
    g_ChallengeBoundaryRecovering = true;
    const bool called = SafeProcessEventCall(reinterpret_cast<uintptr_t>(record.Object), record.Object, teleport, data);
    bool recovered = called && data[result->Offset_Internal] != 0 && ChallengeBoundaryOutsideAll(record.Object);
    if (recovered)
    {
        SafeProcessEventCall(reinterpret_cast<uintptr_t>(controller), controller, stopAi, nullptr);
        SafeProcessEventCall(reinterpret_cast<uintptr_t>(movement), movement, stopVelocity, nullptr);
        record.RecoveredAt = now;
    }
    g_ChallengeBoundaryRecovering = false;
    return recovered;
}

template<size_t N>
static ChallengeBoundaryActor* ChallengeBoundaryFind(std::array<ChallengeBoundaryActor, N>& roster, UObject* object)
{
    for (auto& entry : roster)
        if (entry.Object == object && ChallengeBoundaryLive(object, entry.Identity)) return &entry;
    return nullptr;
}

static bool ChallengeBoundaryKnownAi(UObject* actor);

static bool ChallengeBoundaryAiVehicle(UObject* vehicle)
{
    if (!ChallengeBoundaryLive(vehicle) || !ChallengeBoundaryInWorld(vehicle) ||
        !Memory::IsReadable(vehicle, 0x4F8) || !ClassDerivesFrom(vehicle->Class, "SCDriveableVehicle") ||
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(vehicle) + 0x110) != 3) return false;
    // Native OnOverlap reads these exact seats before committing vehicle escape.
    const auto* seats = reinterpret_cast<const TArray<UObject*>*>(reinterpret_cast<uintptr_t>(vehicle) + 0x4E8);
    if (!seats->Data || seats->Count <= 0 || seats->Count > 8 || seats->Max < seats->Count ||
        !Memory::IsReadable(seats->Data, static_cast<size_t>(seats->Count) * sizeof(UObject*))) return false;
    bool occupied = false;
    for (int32_t n = 0; n < seats->Count; ++n)
    {
        auto* seat = seats->Data[n];
        if (!ChallengeBoundaryLive(seat) || !ClassDerivesFrom(seat->Class, "SCVehicleSeatComponent") ||
            !Memory::IsReadable(seat, 0x508)) return false;
        auto* pawn = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(seat) + 0x500);
        if (!pawn) continue;
        // Scripted exit can unpossess the driver before the car crosses a
        // trigger. Keep the exact verified AI incarnation, never a human pawn.
        if (!ChallengeBoundaryInWorld(pawn) || !ChallengeBoundaryKnownAi(pawn)) return false;
        occupied = true;
    }
    return occupied;
}

static bool ChallengeBoundaryRecoverVehicle(ChallengeBoundaryActor& record)
{
    const ULONGLONG now = GetTickCount64();
    if (!ChallengeBoundaryEnabled() || g_ChallengeBoundaryRecovering || !record.HasSafePoint ||
        now - record.SampledAt > 1500 || !ChallengeBoundaryLive(record.Object, record.Identity) ||
        !ChallengeBoundaryAiVehicle(record.Object)) return false;
    bool geometryComplete = false;
    if (ChallengeBoundaryVehiclePathHitsEscape(record.SafeLocation, record.SafeLocation, geometryComplete) ||
        !geometryComplete) return false;
    // Move the vehicle as a unit, never detach or individually teleport seated
    // counselors. Teleport preserves physics momentum, so explicitly stop it.
    UObject* root = nullptr;
    UObject* movement = nullptr;
    if (!ChallengeReadObjectField(record.Object, "RootComponent", &root) || !ChallengeBoundaryLive(root) ||
        !ClassDerivesFrom(root->Class, "PrimitiveComponent") ||
        !ChallengeReadObjectField(record.Object, "VehicleMovement", &movement) || !ChallengeBoundaryLive(movement))
    {
        if (g_ChallengeBoundaryVehicleReports++ < 8)
            Logger::Debug("CHALLENGE ESCAPE: vehicle recovery components unavailable | root=" + SafeName(root) +
                " | movement=" + SafeName(movement));
        return false;
    }
    auto* teleport = FindFunctionInHierarchyByName(record.Object->Class, "K2_TeleportTo");
    auto* dest = ChallengeBoundaryParam(teleport, "DestLocation", "StructProperty", sizeof(FVector));
    auto* rotation = ChallengeBoundaryParam(teleport, "DestRotation", "StructProperty", sizeof(FRotator));
    auto* result = ChallengeBoundaryParam(teleport, "ReturnValue", "BoolProperty", 1);
    auto* linear = FindFunctionInHierarchyByName(root->Class, "SetPhysicsLinearVelocity");
    auto* linearValue = ChallengeBoundaryParam(linear, "NewVel", "StructProperty", sizeof(FVector));
    auto* linearAdd = ChallengeBoundaryParam(linear, "bAddToCurrent", "BoolProperty", 1);
    auto* linearBone = ChallengeBoundaryParam(linear, "BoneName", "NameProperty", 8);
    auto* angular = FindFunctionInHierarchyByName(root->Class, "SetPhysicsAngularVelocity");
    auto* angularValue = ChallengeBoundaryParam(angular, "NewAngVel", "StructProperty", sizeof(FVector));
    auto* angularAdd = ChallengeBoundaryParam(angular, "bAddToCurrent", "BoolProperty", 1);
    auto* angularBone = ChallengeBoundaryParam(angular, "BoneName", "NameProperty", 8);
    auto* throttle = FindFunctionInHierarchyByName(movement->Class, "SetThrottleInput");
    auto* throttleValue = ChallengeBoundaryParam(throttle, "Throttle", "FloatProperty", sizeof(float));
    auto* brake = FindFunctionInHierarchyByName(movement->Class, "SetBrakeInput");
    auto* brakeValue = ChallengeBoundaryParam(brake, "Brake", "FloatProperty", sizeof(float));
    if (!dest || !rotation || !result || !linearValue || !linearAdd || !linearBone ||
        !angularValue || !angularAdd || !angularBone || !throttleValue || !brakeValue)
    {
        if (g_ChallengeBoundaryVehicleReports++ < 8)
            Logger::Debug("CHALLENGE ESCAPE: vehicle recovery API unavailable | teleport=" + std::to_string(dest && rotation && result) +
                " | linear=" + std::to_string(linearValue && linearAdd && linearBone) +
                " | angular=" + std::to_string(angularValue && angularAdd && angularBone) +
                " | inputs=" + std::to_string(throttleValue && brakeValue));
        return false;
    }
    alignas(16) uint8_t data[0x200]{};
    g_ChallengeBoundaryRecovering = true;
    // Zero vectors, bAddToCurrent=false and BoneName=None are all-zero params.
    const bool throttleStopped = SafeProcessEventCall(reinterpret_cast<uintptr_t>(movement), movement, throttle, data);
    const float fullBrake = 1.0f;
    memcpy(data + brakeValue->Offset_Internal, &fullBrake, sizeof(fullBrake));
    const bool brakeApplied = SafeProcessEventCall(reinterpret_cast<uintptr_t>(movement), movement, brake, data);
    memset(data, 0, sizeof(data));
    const bool linearStopped = SafeProcessEventCall(reinterpret_cast<uintptr_t>(root), root, linear, data);
    const bool angularStopped = SafeProcessEventCall(reinterpret_cast<uintptr_t>(root), root, angular, data);
    memcpy(data + dest->Offset_Internal, &record.SafeLocation, sizeof(FVector));
    memcpy(data + rotation->Offset_Internal, &record.SafeRotation, sizeof(FRotator));
    const bool called = SafeProcessEventCall(reinterpret_cast<uintptr_t>(record.Object), record.Object, teleport, data);
    const bool moved = called && data[result->Offset_Internal] != 0;
    memset(data, 0, sizeof(data));
    const bool linearRestopped = SafeProcessEventCall(reinterpret_cast<uintptr_t>(root), root, linear, data);
    const bool angularRestopped = SafeProcessEventCall(reinterpret_cast<uintptr_t>(root), root, angular, data);
    FVector actual{};
    FRotator actualRotation{};
    bool actualGeometryComplete = false;
    const bool recovered = moved && throttleStopped && brakeApplied && linearStopped && angularStopped &&
        linearRestopped && ChallengeBoundaryReadTransform(record.Object, actual, actualRotation) &&
        !ChallengeBoundaryVehiclePathHitsEscape(actual, actual, actualGeometryComplete) && actualGeometryComplete &&
        ChallengeBoundaryOutsideAll(record.Object);
    if (recovered) record.RecoveredAt = now;
    if (g_ChallengeBoundaryVehicleReports++ < 24)
        Logger::Debug("CHALLENGE ESCAPE: vehicle recovery attempted | moved=" + std::to_string(moved) +
            " | recovered=" + std::to_string(recovered) + " | actor=" + SafeName(record.Object));
    g_ChallengeBoundaryRecovering = false;
    return recovered;
}

static void __fastcall ChallengeEscapeBoundaryOverlap(UObject* volume, UObject* component, UObject* other,
    UObject* otherComponent, int32_t bodyIndex, bool swept, const void* hit)
{
    // Escape commitment must not wait for a full staged roster discovery.
    // The roster is required for safe physical recovery, not native blocking.
    if (ChallengeBoundaryEnabled() && ChallengeBoundaryLive(volume) &&
        ChallengeBoundaryInWorld(volume) && ClassDerivesFrom(volume->Class, "SCEscapeVolume"))
    {
        auto* pawn = ChallengeBoundaryFind(g_ChallengeBoundaryPawns, other);
        // Match the native on-foot eligibility tests, so seated/grabbed/already
        // escaping actors are never teleported out of a scripted interaction.
        const bool onFoot = ChallengeBoundaryInWorld(other) && ChallengeBoundaryAi(other) && Memory::IsReadable(other, 0x15CD) &&
            !*reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(other) + 0x14C8) &&
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(other) + 0x1031) == 0 &&
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(other) + 0x15CC) == 0;
        const bool vehicle = !onFoot && ChallengeBoundaryAiVehicle(other);
        if (onFoot || vehicle)
        {
            auto* car = vehicle ? ChallengeBoundaryFind(g_ChallengeBoundaryVehicles, other) : nullptr;
            const bool recovered = !g_ChallengeBoundaryRecovering &&
                (onFoot ? (pawn && ChallengeBoundaryRecover(*pawn)) : (car && ChallengeBoundaryRecoverVehicle(*car)));
            if (g_ChallengeBoundaryReports++ < 12)
                Logger::Success("CHALLENGE ESCAPE: native attempt blocked before commit | actor=" + SafeName(other) +
                    " | physicalRecovery=" + std::to_string(recovered) + " | vehicle=" + std::to_string(vehicle));
            return;
        }
    }
    if (g_ChallengeEscapeOriginal)
        g_ChallengeEscapeOriginal(volume, component, other, otherComponent, bodyIndex, swept, hit);
}

static void __fastcall ChallengeEscapeStart(UObject* actor, UObject* exitPath, float delay)
{
    const bool enabled = ChallengeBoundaryEnabled();
    if (enabled && ChallengeBoundaryInWorld(actor) && ChallengeBoundaryAi(actor) &&
        Memory::IsReadable(actor, 0x15CD) &&
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(actor) + 0x1031) == 0)
    {
        auto* record = ChallengeBoundaryFind(g_ChallengeBoundaryPawns, actor);
        const bool free = !*reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(actor) + 0x14C8) &&
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(actor) + 0x15CC) == 0;
        const bool recovered = free && record && !g_ChallengeBoundaryRecovering && ChallengeBoundaryRecover(*record);
        if (g_ChallengeBoundaryReports++ < 12)
            Logger::Success("CHALLENGE ESCAPE: character escape start blocked before state write | actor=" + SafeName(actor) +
                " | physicalRecovery=" + std::to_string(recovered));
        return;
    }
    if (g_ChallengeBoundaryWorld && ChallengeBoundaryInWorld(actor) && ChallengeBoundaryAi(actor) &&
        g_ChallengeBoundaryReports++ < 12)
        Logger::Debug("CHALLENGE ESCAPE: character escape start forwarded | scopeEnabled=" + std::to_string(enabled) +
            " | sameThread=" + std::to_string(GetCurrentThreadId() == g_ChallengeBoundaryThread) +
            " | actor=" + SafeName(actor));
    if (g_ChallengeEscapeStartOriginal) g_ChallengeEscapeStartOriginal(actor, exitPath, delay);
}

static void __fastcall ChallengeVehicleEscapeStart(UObject* vehicle, UObject* volume, UObject* exitPath, float delay)
{
    // Native vehicle transition changes engine/escaped flags before moving the
    // car onto its exit spline. Block at entry, including non-overlap callers.
    if (ChallengeBoundaryEnabled() && ChallengeBoundaryAiVehicle(vehicle))
    {
        auto* record = ChallengeBoundaryFind(g_ChallengeBoundaryVehicles, vehicle);
        const bool recovered = record && ChallengeBoundaryRecoverVehicle(*record);
        if (g_ChallengeBoundaryVehicleReports++ < 16)
            Logger::Success("CHALLENGE ESCAPE: vehicle escape start blocked before state writes | actor=" + SafeName(vehicle) +
                " | physicalRecovery=" + std::to_string(recovered));
        return;
    }
    if (g_ChallengeVehicleEscapeStartOriginal) g_ChallengeVehicleEscapeStartOriginal(vehicle, volume, exitPath, delay);
}

static bool ChallengeBoundaryKnownAi(UObject* actor)
{
    if (ChallengeBoundaryAi(actor)) return true;
    // Completion can arrive after the AI has unpossessed its pawn. Only accept
    // that case if this exact incarnation was previously verified as AI here.
    auto* record = ChallengeBoundaryFind(g_ChallengeBoundaryPawns, actor);
    UObject* controller = nullptr;
    return record && ChallengeBoundaryLive(actor, record->Identity) &&
        ChallengeBoundaryInWorld(actor) && ClassDerivesFrom(actor->Class, "SCCounselorCharacter") &&
        Memory::IsReadable(actor, 0x111) &&
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(actor) + 0x110) == 3 &&
        ChallengeReadObjectField(actor, "Controller", &controller) && !controller;
}

static bool ChallengeBoundaryBlockCompletion(UObject* actor, const char* seam)
{
    if (!ChallengeBoundaryEnabled()) return false;
    const bool inWorld = ChallengeBoundaryInWorld(actor);
    const bool knownAi = ChallengeBoundaryKnownAi(actor);
    const bool readable = actor && Memory::IsReadable(actor, 0x15CD);
    const int dead = readable ? *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(actor) + 0x1031) : -1;
    if (!inWorld || !knownAi || !readable || dead != 0)
    {
        if (g_ChallengeBoundaryRejectedReports++ < 8)
            Logger::Debug(std::string("CHALLENGE ESCAPE: completion gate rejected | seam=") + seam +
                " | inWorld=" + std::to_string(inWorld) + " | knownAi=" + std::to_string(knownAi) +
                " | readable=" + std::to_string(readable) + " | deadByte=" + std::to_string(dead) +
                " | actor=" + SafeName(actor));
        return false;
    }
    auto* record = ChallengeBoundaryFind(g_ChallengeBoundaryPawns, actor);
    const bool free = !*reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(actor) + 0x14C8) &&
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(actor) + 0x15CC) == 0;
    const bool recovered = free && record && !g_ChallengeBoundaryRecovering && ChallengeBoundaryRecover(*record);
    if (g_ChallengeBoundaryReports++ < 32)
        Logger::Success(std::string("CHALLENGE ESCAPE: completion blocked before challenge accounting | seam=") + seam +
            " | actor=" + SafeName(actor) + " | physicalRecovery=" + std::to_string(recovered));
    return true;
}

static void __fastcall ChallengeEscapePresentation(UObject* volume, UObject* actor)
{
    if (ChallengeBoundaryEnabled() && ChallengeBoundaryLive(volume) && ChallengeBoundaryInWorld(volume) &&
        ClassDerivesFrom(volume->Class, "SCEscapeVolume") &&
        ChallengeBoundaryBlockCompletion(actor, "volume-presentation")) return;
    if (g_ChallengeEscapePresentationOriginal) g_ChallengeEscapePresentationOriginal(volume, actor);
}

static void __fastcall ChallengeEscapeComplete(UObject* mode, UObject* controller, UObject* actor)
{
    // This is the SP challenge game-mode override, before the dossier increment
    // and base escape accounting. It is NOT the global SCDossier notification.
    if (g_ChallengeRouteActive.load() && mode == ChallengeCurrentGameMode() && ChallengeBoundaryEnabled() &&
        ChallengeBoundaryLive(mode, g_ChallengeBoundaryModeIdentity) && ClassDerivesFrom(mode->Class, "SCGameMode_SPChallenges"))
    {
        // R8 is independently verified as a living current-world AI counselor.
        // KnownAi already validates both sides of its current possession (or
        // the exact cached unpossessed incarnation). Native RDX is not required
        // to be its current controller at completion; do not gate on that again.
        if (ChallengeBoundaryBlockCompletion(actor, "challenge-game-mode")) return;
    }
    if (g_ChallengeRouteActive.load() && mode == ChallengeCurrentGameMode() &&
        g_ChallengeBoundaryForwardReports++ < 8)
        Logger::Debug("CHALLENGE ESCAPE: native completion forwarded | scopeEnabled=" + std::to_string(ChallengeBoundaryEnabled()) +
            " | controller=" + SafeName(controller) +
            " | knownAi=" + std::to_string(ChallengeBoundaryKnownAi(actor)) +
            " | actor=" + SafeName(actor));
    if (g_ChallengeEscapeCompleteOriginal) g_ChallengeEscapeCompleteOriginal(mode, controller, actor);
}

static bool InstallChallengeEscapeBoundary()
{
#if defined(F13_BASE_GAME_PORT)
    if (g_ChallengeEscapeVerified) return true;
    static const uint8_t entry[]{0x40,0x55,0x53,0x56,0x41,0x56,0x48,0x8D,0xAC,0x24,0x28,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0xD8,0x01,0,0};
    // Entry and exec->native dispatch anchor are sufficient for the hook ABI;
    // seat offsets are separately checked against their native read sequence.
    static const uint8_t seats[]{0x48,0x8B,0xBE,0xE8,0x04,0,0,0x48,0x63,0x86,0xF0,0x04,0,0};
    static const uint8_t eligible[]{0x48,0x83,0xBE,0xC8,0x14,0,0,0};
    static const uint8_t startEntry[]{0x40,0x55,0x53,0x57,0x41,0x57,0x48,0x8D,0xAC,0x24,0x68,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0x98,0x01,0,0};
    static const uint8_t firstWrite[]{0xC6,0x81,0xD5,0x14,0,0,0x01};
    void* startAddress = reinterpret_cast<void*>(ShippingAddress(0x3D1F90));
    static const uint8_t presentationEntry[]{0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7C,0x24,0x18,0x41,0x56,0x48,0x83,0xEC,0x60};
    static const uint8_t completeEntry[]{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x49,0x8B,0xF8};
    static const uint8_t dossierCall[]{0xE8,0xC0,0xEA,0x08,0x00};
    static const uint8_t vehicleEntry[]{0x48,0x8B,0xC4,0x55,0x56,0x48,0x8D,0x68,0xA1,0x48,0x81,0xEC,0xB8,0,0,0};
    static const uint8_t vehicleFirstWrite[]{0x83,0xA1,0x28,0x06,0,0,0xFE};
    static const uint8_t vehicleEscapedWrite[]{0xC6,0x81,0x7C,0x04,0,0,0x01};
    void* vehicleAddress = reinterpret_cast<void*>(ShippingAddress(0x440FE0));
    void* presentationAddress = reinterpret_cast<void*>(ShippingAddress(0x4439F0));
    void* completeAddress = reinterpret_cast<void*>(ShippingAddress(0x3A7E40));
    void* address = reinterpret_cast<void*>(ShippingAddress(0x439220));
    static const uint8_t dispatch[]{0xE8,0x2B,0x20,0xFA,0xFF};
    if (!MatchesBytes(reinterpret_cast<uintptr_t>(address), entry, sizeof(entry)) ||
        !MatchesBytes(ShippingAddress(0x4971F0), dispatch, sizeof(dispatch)) ||
        !MatchesBytes(ShippingAddress(0x4397E3), seats, sizeof(seats)) ||
        !MatchesBytes(ShippingAddress(0x4396DA), eligible, sizeof(eligible)) ||
        !MatchesBytes(reinterpret_cast<uintptr_t>(startAddress), startEntry, sizeof(startEntry)) ||
        !MatchesBytes(ShippingAddress(0x3D1FBE), firstWrite, sizeof(firstWrite)) ||
        !MatchesBytes(reinterpret_cast<uintptr_t>(presentationAddress), presentationEntry, sizeof(presentationEntry)) ||
        !MatchesBytes(reinterpret_cast<uintptr_t>(completeAddress), completeEntry, sizeof(completeEntry)) ||
        !MatchesBytes(ShippingAddress(0x3A7E6B), dossierCall, sizeof(dossierCall)) ||
        !MatchesBytes(reinterpret_cast<uintptr_t>(vehicleAddress), vehicleEntry, sizeof(vehicleEntry)) ||
        !MatchesBytes(ShippingAddress(0x440FF0), vehicleFirstWrite, sizeof(vehicleFirstWrite)) ||
        !MatchesBytes(ShippingAddress(0x44101E), vehicleEscapedWrite, sizeof(vehicleEscapedWrite))) return false;
    if (!g_ChallengeVehicleEscapeStartOriginal && MH_CreateHook(vehicleAddress, reinterpret_cast<void*>(&ChallengeVehicleEscapeStart),
        reinterpret_cast<void**>(&g_ChallengeVehicleEscapeStartOriginal)) != MH_OK) return false;
    const auto vehicleEnabled = MH_EnableHook(vehicleAddress);
    if (vehicleEnabled != MH_OK && vehicleEnabled != MH_ERROR_ENABLED) return false;
    if (!g_ChallengeEscapePresentationOriginal && MH_CreateHook(presentationAddress, reinterpret_cast<void*>(&ChallengeEscapePresentation),
        reinterpret_cast<void**>(&g_ChallengeEscapePresentationOriginal)) != MH_OK) return false;
    if (!g_ChallengeEscapeCompleteOriginal && MH_CreateHook(completeAddress, reinterpret_cast<void*>(&ChallengeEscapeComplete),
        reinterpret_cast<void**>(&g_ChallengeEscapeCompleteOriginal)) != MH_OK) return false;
    const auto presentationEnabled = MH_EnableHook(presentationAddress);
    const auto completeEnabled = MH_EnableHook(completeAddress);
    if ((presentationEnabled != MH_OK && presentationEnabled != MH_ERROR_ENABLED) ||
        (completeEnabled != MH_OK && completeEnabled != MH_ERROR_ENABLED)) return false;
    if (!g_ChallengeEscapeStartOriginal && MH_CreateHook(startAddress, reinterpret_cast<void*>(&ChallengeEscapeStart),
        reinterpret_cast<void**>(&g_ChallengeEscapeStartOriginal)) != MH_OK) return false;
    const auto startEnabled = MH_EnableHook(startAddress);
    if (startEnabled != MH_OK && startEnabled != MH_ERROR_ENABLED) return false;
    if (!g_ChallengeEscapeOriginal && MH_CreateHook(address, reinterpret_cast<void*>(&ChallengeEscapeBoundaryOverlap),
        reinterpret_cast<void**>(&g_ChallengeEscapeOriginal)) != MH_OK) return false;
    const auto enabled = MH_EnableHook(address);
    g_ChallengeEscapeVerified = enabled == MH_OK || enabled == MH_ERROR_ENABLED;
    Logger::Success("CHALLENGE ESCAPE: native overlap/start/presentation/completion/vehicle seams ready=" + std::to_string(g_ChallengeEscapeVerified));
#endif
    return g_ChallengeEscapeVerified;
}

static void PumpChallengeEscapeBoundary(const f13::challenges::runtime::Scope& scope)
{
    if (!g_ChallengeEscapeVerified || !scope.stockChallengeMission || !scope.localAuthority || scope.networkSession ||
        !scope.inProgress || !f13::challenges::nativeui::HasCommittedSettings() ||
        !f13::challenges::nativeui::GetCommittedSettings().enabled[static_cast<size_t>(f13::challenges::Option::PreventEscapes)])
    {
        g_ChallengeBoundaryWorld = 0;
        return;
    }
    auto* world = reinterpret_cast<UObject*>(scope.world);
    const uint64_t worldIdentity = ChallengeObjectIdentity(world);
    const uint64_t modeIdentity = ChallengeObjectIdentity(ChallengeCurrentGameMode());
    if (!worldIdentity || !modeIdentity) return;
    const ULONGLONG now = GetTickCount64();
    if (g_ChallengeBoundaryWorld != scope.world || g_ChallengeBoundaryWorldIdentity != worldIdentity ||
        g_ChallengeBoundaryModeIdentity != modeIdentity)
    {
        g_ChallengeBoundaryPawns = {};
        g_ChallengeBoundaryVehicles = {};
        g_ChallengeBoundaryVolumes = {};
        g_ChallengeBoundaryNextRoster = 0;
        g_ChallengeBoundaryNextSlice = 0;
        g_ChallengeBoundaryNextSample = 0;
        g_ChallengeBoundaryBuilding = false;
        g_ChallengeBoundaryRosterSignature = 0;
        g_ChallengeBoundaryReports = 0;
        g_ChallengeBoundaryRosterReports = 0;
        g_ChallengeBoundarySampleReports = 0;
        g_ChallengeBoundaryForwardReports = 0;
        g_ChallengeBoundaryRejectedReports = 0;
        g_ChallengeBoundaryVehicleReports = 0;
        g_ChallengeBoundaryVehicleGateReports = 0;
        g_ChallengeBoundaryGeometryReports = 0;
        g_ChallengeBoundaryRosterComplete = false;
        Logger::Success("CHALLENGE ESCAPE: local challenge boundary armed; other game modes unchanged");
    }
    g_ChallengeBoundaryWorld = scope.world;
    g_ChallengeBoundaryWorldIdentity = worldIdentity;
    g_ChallengeBoundaryModeIdentity = modeIdentity;
    g_ChallengeBoundaryState = reinterpret_cast<UObject*>(scope.gameState);
    g_ChallengeBoundaryStateIdentity = ChallengeObjectIdentity(scope.gameState);
    g_ChallengeBoundaryThread = GetCurrentThreadId();
    if (now >= g_ChallengeBoundaryNextSlice)
    {
        g_ChallengeBoundaryNextSlice = now + 250;
        const auto* levels = reinterpret_cast<const TArray<ULevel*>*>(scope.world + 0x110);
        if (!Memory::IsReadable(levels, sizeof(*levels)) || !levels->Data || levels->Count <= 0 || levels->Count > 128 ||
            !Memory::IsReadable(levels->Data, static_cast<size_t>(levels->Count) * sizeof(ULevel*)))
        {
            g_ChallengeBoundaryRosterComplete = false;
            g_ChallengeBoundaryBuilding = false;
            g_ChallengeBoundaryNextRoster = 0;
            return;
        }
        // Check only level headers here, never actor ancestry. Detect streaming
        // and roster changes without restarting an unchanged complete roster.
        uint64_t signature = static_cast<uint64_t>(levels->Count);
        for (int32_t l = 0; l < levels->Count; ++l)
        {
            auto* level = levels->Data[l];
            if (!level || !Memory::IsReadable(level, sizeof(ULevel)))
            {
                g_ChallengeBoundaryRosterComplete = false;
                g_ChallengeBoundaryBuilding = false;
                g_ChallengeBoundaryNextRoster = 0;
                return;
            }
            const auto& actors = level->Actors;
            if (actors.Count < 0 || actors.Count > 30000 || (actors.Count && (!actors.Data ||
                !Memory::IsReadable(actors.Data, static_cast<size_t>(actors.Count) * sizeof(AActor*)))))
            {
                g_ChallengeBoundaryRosterComplete = false;
                g_ChallengeBoundaryBuilding = false;
                g_ChallengeBoundaryNextRoster = 0;
                return;
            }
            signature = (signature * 1099511628211ULL) ^ reinterpret_cast<uintptr_t>(level);
            signature = (signature * 1099511628211ULL) ^ reinterpret_cast<uintptr_t>(actors.Data);
            signature = (signature * 1099511628211ULL) ^ static_cast<uint64_t>(actors.Count);
        }
        const bool changed = signature != g_ChallengeBoundaryRosterSignature;
        if (changed || (!g_ChallengeBoundaryBuilding && now >= g_ChallengeBoundaryNextRoster))
        {
            g_ChallengeBoundaryStagingPawns = {};
            g_ChallengeBoundaryStagingVehicles = {};
            g_ChallengeBoundaryStagingVolumes = {};
            g_ChallengeBoundaryPawnCount = g_ChallengeBoundaryVolumeCount = g_ChallengeBoundaryVehicleCount = 0;
            g_ChallengeBoundaryLevelCursor = g_ChallengeBoundaryActorCursor = 0;
            g_ChallengeBoundaryRosterSignature = signature;
            g_ChallengeBoundaryBuilding = true;
            g_ChallengeBoundaryBuildValid = true;
            // Old roster still blocks known attempts; no new safe samples may
            // rely on a volume list which streaming just invalidated.
            if (changed) g_ChallengeBoundaryRosterComplete = false;
        }
        size_t examined = 0;
        while (g_ChallengeBoundaryBuilding && g_ChallengeBoundaryLevelCursor < levels->Count && examined < 128)
        {
            const auto& actors = levels->Data[g_ChallengeBoundaryLevelCursor]->Actors;
            if (g_ChallengeBoundaryActorCursor >= actors.Count)
            {
                ++g_ChallengeBoundaryLevelCursor;
                g_ChallengeBoundaryActorCursor = 0;
                continue;
            }
            ++examined;
            auto* actor = reinterpret_cast<UObject*>(actors.Data[g_ChallengeBoundaryActorCursor++]);
            if (!ChallengeBoundaryLive(actor)) continue;
            const uint64_t identity = ChallengeObjectIdentity(actor);
            if (ClassDerivesFrom(actor->Class, "SCEscapeVolume"))
            {
                if (g_ChallengeBoundaryVolumeCount == g_ChallengeBoundaryStagingVolumes.size())
                    g_ChallengeBoundaryBuildValid = false;
                else g_ChallengeBoundaryStagingVolumes[g_ChallengeBoundaryVolumeCount++] = {actor, identity};
            }
            else if (ChallengeBoundaryKnownAi(actor))
            {
                if (g_ChallengeBoundaryPawnCount == g_ChallengeBoundaryStagingPawns.size())
                    g_ChallengeBoundaryBuildValid = false;
                else g_ChallengeBoundaryStagingPawns[g_ChallengeBoundaryPawnCount++] = {actor, identity};
            }
            else if (ClassDerivesFrom(actor->Class, "SCDriveableVehicle"))
            {
                if (g_ChallengeBoundaryVehicleCount == g_ChallengeBoundaryStagingVehicles.size())
                    g_ChallengeBoundaryBuildValid = false;
                else g_ChallengeBoundaryStagingVehicles[g_ChallengeBoundaryVehicleCount++] = {actor, identity};
            }
        }
        if (g_ChallengeBoundaryBuilding && g_ChallengeBoundaryLevelCursor >= levels->Count)
        {
            for (auto& pawn : g_ChallengeBoundaryStagingPawns)
                if (pawn.Object)
                    if (auto* previous = ChallengeBoundaryFind(g_ChallengeBoundaryPawns, pawn.Object)) pawn = *previous;
            for (auto& vehicle : g_ChallengeBoundaryStagingVehicles)
                if (vehicle.Object)
                    if (auto* previous = ChallengeBoundaryFind(g_ChallengeBoundaryVehicles, vehicle.Object)) vehicle = *previous;
            size_t geometryCount = 0;
            for (auto& volume : g_ChallengeBoundaryStagingVolumes)
                if (volume.Object)
                {
                    if (auto* previous = ChallengeBoundaryFind(g_ChallengeBoundaryVolumes, volume.Object)) volume = *previous;
                    if (volume.HasSafePoint || ChallengeBoundaryCacheEscapeSphere(volume)) ++geometryCount;
                }
            g_ChallengeBoundaryPawns = g_ChallengeBoundaryStagingPawns;
            g_ChallengeBoundaryVolumes = g_ChallengeBoundaryStagingVolumes;
            g_ChallengeBoundaryVehicles = g_ChallengeBoundaryStagingVehicles;
            g_ChallengeBoundaryRosterComplete = g_ChallengeBoundaryBuildValid && g_ChallengeBoundaryVolumeCount != 0;
            if (g_ChallengeBoundaryGeometryReports++ < 3)
                Logger::Debug("CHALLENGE ESCAPE: cached vehicle escape spheres | ready=" + std::to_string(geometryCount) +
                    " | total=" + std::to_string(g_ChallengeBoundaryVolumeCount));
            if (g_ChallengeBoundaryRosterReports++ < 3)
                Logger::Success("CHALLENGE ESCAPE: bounded roster ready | pawns=" + std::to_string(g_ChallengeBoundaryPawnCount) +
                    " | volumes=" + std::to_string(g_ChallengeBoundaryVolumeCount) +
                    " | vehicles=" + std::to_string(g_ChallengeBoundaryVehicleCount) +
                    " | valid=" + std::to_string(g_ChallengeBoundaryRosterComplete));
            g_ChallengeBoundaryBuilding = false;
            g_ChallengeBoundaryNextRoster = now + 15000;
        }
    }
    if (now < g_ChallengeBoundaryNextSample || !ChallengeBoundaryEnabled()) return;
    g_ChallengeBoundaryNextSample = now + 250;
    for (auto& pawn : g_ChallengeBoundaryPawns)
    {
        if (!ChallengeBoundaryLive(pawn.Object, pawn.Identity) || !ChallengeBoundaryAi(pawn.Object) ||
            now - pawn.RecoveredAt < 500) continue;
        if (!ChallengeBoundaryOutsideAll(pawn.Object))
        {
            // Overlap suppression prevents commitment, but does not itself stop
            // AI movement. Retry recovery on the existing bounded sample cadence
            // if entry happened between samples; never invent a safe position.
            if (Memory::IsReadable(pawn.Object, 0x15CD) &&
                !*reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(pawn.Object) + 0x14C8) &&
                *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pawn.Object) + 0x1031) == 0 &&
                *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pawn.Object) + 0x15CC) == 0 &&
                ChallengeBoundaryRecover(pawn) && g_ChallengeBoundaryReports++ < 12)
                Logger::Success("CHALLENGE ESCAPE: boundary movement recovered | actor=" + SafeName(pawn.Object));
            continue;
        }
        FVector position{};
        FRotator rotation{};
        if (ChallengeBoundaryReadTransform(pawn.Object, position, rotation))
        {
            if (!pawn.HasSafePoint && g_ChallengeBoundarySampleReports++ < 8)
                Logger::Success("CHALLENGE ESCAPE: verified safe position captured | actor=" + SafeName(pawn.Object));
            pawn.SafeLocation = position;
            pawn.SafeRotation = rotation;
            pawn.SampledAt = now;
            pawn.HasSafePoint = true;
        }
    }
    // Share the existing 250ms challenge-only sampler; no extra worker or scan.
    for (auto& vehicle : g_ChallengeBoundaryVehicles)
    {
        if (!ChallengeBoundaryLive(vehicle.Object, vehicle.Identity) || now - vehicle.RecoveredAt < 500) continue;
        if (!ChallengeBoundaryAiVehicle(vehicle.Object))
        {
            if (vehicle.HasSafePoint && g_ChallengeBoundaryVehicleGateReports++ < 6)
                Logger::Debug("CHALLENGE ESCAPE: previously sampled vehicle no longer AI-occupied | actor=" + SafeName(vehicle.Object));
            continue;
        }
        FVector position{};
        FRotator rotation{};
        if (!ChallengeBoundaryReadTransform(vehicle.Object, position, rotation)) continue;
        bool geometryComplete = false;
        const bool crossesEscape = ChallengeBoundaryVehiclePathHitsEscape(
            vehicle.HasSafePoint ? vehicle.SafeLocation : position, position, geometryComplete);
        if (crossesEscape || !ChallengeBoundaryOutsideAll(vehicle.Object))
        {
            if (ChallengeBoundaryRecoverVehicle(vehicle) && g_ChallengeBoundaryVehicleReports++ < 16)
                Logger::Success("CHALLENGE ESCAPE: vehicle boundary movement recovered | actor=" + SafeName(vehicle.Object));
            continue;
        }
        if (geometryComplete)
        {
            if (!vehicle.HasSafePoint && g_ChallengeBoundaryVehicleReports++ < 16)
                Logger::Success("CHALLENGE ESCAPE: verified vehicle safe position captured | actor=" + SafeName(vehicle.Object));
            vehicle.SafeLocation = position;
            vehicle.SafeRotation = rotation;
            vehicle.SampledAt = now;
            vehicle.HasSafePoint = true;
        }
    }
}
