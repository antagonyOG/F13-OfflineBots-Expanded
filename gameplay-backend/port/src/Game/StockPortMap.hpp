#pragma once

#include <cstddef>
#include <cstdint>

// Binary-compatibility map for the packed base-game executable.  The AI
// controller remains the frozen Resurrected implementation; only executable
// RVAs and reflected/native layout offsets belong in this file.
namespace StockPortMap
{
#if defined(F13_BASE_GAME_PORT)
    constexpr uintptr_t KillerControllerTick = 0x00321F40;
    constexpr uintptr_t BaseAIControllerTick = 0x01115100;
    constexpr uintptr_t KillerOnPossess = 0x0031D030;
    constexpr uintptr_t BaseAIOnPossess = 0x0110E290;
    constexpr uintptr_t KillerCrowdFollowerOverrideCall = 0x002F04A0;
    constexpr uintptr_t MoveRequestCtor = 0x010F90E0;
    constexpr uintptr_t JasonInteraction = 0x003F89A0;
    constexpr uintptr_t AttackRelease = 0x003A443C;
    constexpr uintptr_t AttackPress = 0x003A44C0;
    constexpr uintptr_t Grab = 0x003E5F40;
    constexpr uintptr_t CanPlaceTrap = 0x003E74F0;
    constexpr uintptr_t AttemptPlaceTrap = 0x003E5D80;
    constexpr uintptr_t CanGrabKill = 0x003E6F10;
    constexpr uintptr_t GetGrabbedCounselor = 0x003EE880;
    constexpr uintptr_t IsGrabKillAvailable = 0x003F5500;
    constexpr uintptr_t IsGrabKilling = 0x003F5630;
    constexpr uintptr_t GrabKillInput = 0x003FFA80;
    constexpr uintptr_t CanInteractWithKnife = 0x00375000;
    constexpr uintptr_t AttemptInteract = 0x002586A0;
    constexpr uintptr_t ReleaseKnifeThrow = 0x003FFB10;
    constexpr uintptr_t FinalizeKnifeDriver = 0x004047C0;
    constexpr uintptr_t PressKnifeThrow = 0x004015B0;
    constexpr uintptr_t ThrowableBuildVelocity = 0x003F2F60;
    constexpr uintptr_t ThrowableServerUseImplementation = 0x00405B80;
    constexpr uintptr_t NativeInteractionUpdate = 0x0035FD00;
    constexpr uintptr_t NativeInteractionNullCallA = 0x00360242;
    constexpr uintptr_t NativeInteractionResumeA = 0x0036024B;
    constexpr uintptr_t NativeInteractionNullCallB = 0x00360252;
    constexpr uintptr_t NativeInteractionResumeB = 0x0036025B;
    constexpr uintptr_t TrapTriggered = 0x0039C670;
    constexpr uintptr_t GiveStartingItem = 0x002E2E00;
    constexpr uintptr_t GetCurrentWeapon = 0x002E0E80;
    constexpr uintptr_t IsJasonStunned = 0x002E34F0;
    constexpr uintptr_t SpawnParamsCtor = 0x017D0E10;
    constexpr uintptr_t SpawnActor = 0x014DA140;
    constexpr uintptr_t SetSweaterAbilityAvailable = 0x00443550;
    constexpr uintptr_t PamelaSweaterCanInteract = 0x00374E00;
    constexpr uintptr_t BasePamelaPickupCanInteract = 0x00374BF0;
    constexpr uintptr_t JasonOnlyNavigationFilterClassSlot = 0x02D0AB08;
    constexpr uintptr_t JasonOnlyNavigationFilterStaticClass = 0x000AA570;

    constexpr size_t JasonGrabKills = 0x11A8;
    constexpr size_t JasonActiveHeldActor = 0x14D0;
    constexpr size_t JasonKnifeCount = 0x15BC;
    constexpr size_t JasonKnifePressed = 0x15C8;
    constexpr size_t JasonKnifeDriver = 0x10F0;
    constexpr size_t JasonDoorBreakTarget = 0x1650;
    constexpr size_t JasonTrapCount = 0x1718;
    constexpr size_t JasonMorphActive = 0x13E9;
    constexpr size_t JasonShiftActive = 0x14B0;
    constexpr size_t JasonExtractionComponent = 0x1528;
    constexpr size_t JasonDeathContext = 0x12A8;
    constexpr size_t JasonTrapPlacementDistance = 0x1740;
    constexpr size_t CounselorSweaterAbility = 0x18B0;
    constexpr size_t CounselorHasSweater = 0x1C52;
    constexpr size_t CounselorCurrentSeat = 0x1570;
    constexpr size_t CounselorExitingVehicle = 0x1584;
    constexpr size_t CounselorEscapedFallback = 0x14D5;
    constexpr size_t CounselorIsHunterByte = 0x1A00;
#else
    constexpr uintptr_t KillerControllerTick = 0x0032BD80;
    constexpr uintptr_t BaseAIControllerTick = 0x011360F0;
    constexpr uintptr_t KillerOnPossess = 0x00327190;
    constexpr uintptr_t BaseAIOnPossess = 0x0112F3F0;
    constexpr uintptr_t KillerCrowdFollowerOverrideCall = 0x002FE340;
    constexpr uintptr_t MoveRequestCtor = 0x0111A310;
    constexpr uintptr_t JasonInteraction = 0x00410AF0;
    constexpr uintptr_t AttackRelease = 0x003B5F00;
    constexpr uintptr_t AttackPress = 0x003B5F84;
    constexpr uintptr_t Grab = 0x003FCB50;
    constexpr uintptr_t CanPlaceTrap = 0x003FE120;
    constexpr uintptr_t AttemptPlaceTrap = 0x003FCB10;
    constexpr uintptr_t CanGrabKill = 0x003FDB40;
    constexpr uintptr_t GetGrabbedCounselor = 0x004055E0;
    constexpr uintptr_t IsGrabKillAvailable = 0x0040D070;
    constexpr uintptr_t IsGrabKilling = 0x0040D1A0;
    constexpr uintptr_t GrabKillInput = 0x0041B040;
    constexpr uintptr_t CanInteractWithKnife = 0x00382920;
    constexpr uintptr_t AttemptInteract = 0x0025AC50;
    constexpr uintptr_t ReleaseKnifeThrow = 0x0041B0D0;
    constexpr uintptr_t FinalizeKnifeDriver = 0x0041FE60;
    constexpr uintptr_t PressKnifeThrow = 0x0041CC30;
    constexpr uintptr_t ThrowableBuildVelocity = 0x0040A7B0;
    constexpr uintptr_t ThrowableServerUseImplementation = 0x00421A00;
    constexpr uintptr_t NativeInteractionUpdate = 0x0036E300;
    constexpr uintptr_t NativeInteractionNullCallA = 0x0036E842;
    constexpr uintptr_t NativeInteractionResumeA = 0x0036E84B;
    constexpr uintptr_t NativeInteractionNullCallB = 0x0036E852;
    constexpr uintptr_t NativeInteractionResumeB = 0x0036E85B;
    constexpr uintptr_t TrapTriggered = 0x003AC360;
    constexpr uintptr_t GiveStartingItem = 0x002EF980;
    constexpr uintptr_t GetCurrentWeapon = 0x002ED9F0;
    constexpr uintptr_t IsJasonStunned = 0x002F00E0;
    constexpr uintptr_t SpawnParamsCtor = 0x017F1160;
    constexpr uintptr_t SpawnActor = 0x01519D10;
    constexpr uintptr_t SetSweaterAbilityAvailable = 0x004675D0;
    constexpr uintptr_t PamelaSweaterCanInteract = 0x00382720;
    constexpr uintptr_t BasePamelaPickupCanInteract = 0x00382470;
    constexpr uintptr_t JasonOnlyNavigationFilterStaticClass = 0x004C9410;

    constexpr size_t JasonGrabKills = 0x11C0;
    constexpr size_t JasonActiveHeldActor = 0x1500;
    constexpr size_t JasonKnifeCount = 0x15EC;
    constexpr size_t JasonKnifePressed = 0x15F8;
    constexpr size_t JasonKnifeDriver = 0x10F0;
    constexpr size_t JasonDoorBreakTarget = 0x1680;
    constexpr size_t JasonTrapCount = 0x1748;
    constexpr size_t JasonMorphActive = 0x1419;
    constexpr size_t JasonShiftActive = 0x14E0;
    constexpr size_t JasonExtractionComponent = 0x1558;
    constexpr size_t JasonDeathContext = 0x12D8;
    constexpr size_t JasonTrapPlacementDistance = 0x1770;
    constexpr size_t CounselorSweaterAbility = 0x18C8;
    constexpr size_t CounselorHasSweater = 0x1C6A;
    constexpr size_t CounselorCurrentSeat = 0x1588;
    constexpr size_t CounselorExitingVehicle = 0x159C;
    constexpr size_t CounselorEscapedFallback = 0x14ED;
    constexpr size_t CounselorIsHunterByte = 0x1A18;
#endif
}
