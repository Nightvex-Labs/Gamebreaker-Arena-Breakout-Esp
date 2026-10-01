// arenahack — Arena Breakout Infinite (UAGame.exe) offsets.
// Copied from C:\ABIFINAL\src\offsets.hpp on 2026-09-23.
// Global RVAs updated 2026-09-24 for micro-patch (Dumper-7 fresh dump,
// GameVersion 4.26.1-0+++UE4+Release-4.26). ACE_CACHE + FNAME_MASK_KEY
// kept as-is until live-verified; sig-rescan pending if reader reads
// garbage after this update.

#pragma once
#include <stdint.h>

// Target process image name (EPROCESS.ImageFileName is 15-char truncated).
// UAGame.exe fits fully. Match is prefix-insensitive on the first 14 chars.
#define AH_PROC_NAME  "UAGame.exe"

// Base assumption — Windows loads x64 exe at 0x140000000 unless ASLR relocates.
// Real base always read from PEB.ImageBaseAddress; this is the SDK anchor only.
#define AH_UAGAME_BASE_STATIC  0x140000000ULL

// ============================================================================
// GLOBAL RVAs — Sep-18/20 live-verified
// ============================================================================
#define AH_RVA_GWORLD           0xB2BEE48ULL   // 2026-09-24 dump (PLAIN pointer)
#define AH_RVA_GWORLD_MIRROR    0xB2C0E48ULL   // GWorld + 0x2000 (tamper mirror)
#define AH_RVA_GOBJECTS         0xB5510D8ULL   // 2026-09-24 dump
#define AH_RVA_FNAMEPOOL        0xBB58F80ULL   // 2026-09-24 dump (Dumpspace OFFSET_GNAMES)
// 2026-09-24: delta-shifted from GWorld micro-patch move (+0x24840).
// GWorld moved 0xB29A608 → 0xB2BEE48; assume ACE/FName tables followed
// the same section shift. If ACE decrypt returns garbage after this,
// port sig_scanner ACE_CACHE hunt from ABIFINAL.
#define AH_RVA_FNAME_MASK_KEY   0xBB02FACULL
// v1.0.38.14 2026-10-01: match github.com/Nightvex-Labs/
// Arena-Breakout-Infinite-Radar prod value (2026-09-25 runtime
// sig-discovery, key-verified: key=0x12FF, LEA @ 0xCEB08E in ACE
// bucket-hash fn). Prior 0xB5E4D40 (local 09-24 "+0x24940 delta"
// guess) was off by 0x30 (6 buckets). Our murmur/hash code bytes-identical
// to radar's → same algo, same baseline → encrypted enemies now decrypt.
// Field evidence that forced this: voiddrift / warriormaster algo=7 fail=5
// BUCKET_MISS every attempt (0xB5E4D40), web radar same build worked
// (0xB5E4D70).
#define AH_RVA_ACE_CACHE        0xB5E4D70ULL   // 2026-09-25 radar-verified

#define AH_FNAME_POOL_OFF       0x7D000        // pool base -> entry 0 offset (Tencent-fork)
#define AH_FNAME_MASK_LITERAL   0x4A           // literal in mask_eff derive

// ============================================================================
// UWorld
// ============================================================================
#define AH_UW_PERSISTENTLVL     0x30
#define AH_UW_GAMESTATE         0x120
#define AH_UW_GAMEINSTANCE      0x180

// ============================================================================
// AGameState / AGameStateBase
// ============================================================================
#define AH_GS_PLAYERARRAY       0x330
#define AH_GS_MATCHSTATE        0x368    // FName — "InProgress" during raid
// EGameSceneType uint8 — MFBaseModule enum. Verified from Dumper-7 4.26.1 ABInfinite SDK.
// 0=GST_None, 1=GST_Lobby, 2=GST_InBattle (raid), 3=GST_CG, 4=GST_ShootingRoom, 5=GST_MAX
#define AH_GS_SCENETYPE         0x579
#define AH_SCENE_INBATTLE       2
#define AH_SCENE_SHOOTINGROOM   4
// v0.9.454: verified in Dumper-7 dump ABInfinite 4.26.1
// (C:\Dumper-7\CppSDK\...\SGFramework_classes.hpp:44627). uint64, replicated,
// 0 in main menu / matchmaking, non-zero once server assigns a room. Cheapest
// in-raid indicator: one RPM read, no follow-up, no key rotation risk.
#define AH_GS_ROOMID            0x430

// ============================================================================
// UGameInstance / ULocalPlayer / APlayerController
// ============================================================================
#define AH_GI_LOCALPLAYERS      0x38
#define AH_LP_PC                0x30
#define AH_PC_PAWN              0x348    // AController::Pawn (base, server-auth)
#define AH_PC_PAWN_ACK          0x398    // APlayerController::AcknowledgedPawn (fallback)
#define AH_PC_CONTROLROT        0x380
#define AH_PC_CAMMGR            0x3B0
#define AH_PC_LASTSYNC_LOC      0x410    // plain FVector (self-locator)
#define AH_PC_SPAWNLOC          0x658    // plain FVector, net-repl

// ============================================================================
// APlayerCameraManager (POV — public + private cache)
// ============================================================================
#define AH_PCM_CACHE_LOC        0x03A0   // FMinimalViewInfo public
#define AH_PCM_CACHE_ROT        0x03AC
#define AH_PCM_CACHE_FOV        0x03B8
#define AH_PCM_CACHE_LOC_PRIV   0x2130   // FMinimalViewInfo private (pre-blend)
#define AH_PCM_CACHE_ROT_PRIV   0x213C
#define AH_PCM_CACHE_FOV_PRIV   0x2148

// ============================================================================
// PlayerState / Pawn base
// ============================================================================
#define AH_PS_PAWN              0x378
#define AH_PS_PLAYERNAME        0x3F8
#define AH_PS_TEAMINDEX         0x598
#define AH_PAWN_PS              0x348

// ============================================================================
// AActor / RootComponent (ACE encrypted)
// ============================================================================
#define AH_PAWN_ROOT            0x170
#define AH_ROOT_LOC             0x170    // RelativeLocation (ACE encrypted)
#define AH_ROOT_CTL             0x17C    // ACE control DWORD (algo|key)
#define AH_ROOT_CAPSULE_R       0x15C
#define AH_ROOT_HALFHEIGHT      0x164
#define AH_ROOT_ACTOR_YAW       0x184

// ============================================================================
// ACE anti-cheat encryption constants (murmur XOR stream)
// ============================================================================
#define AH_ACE_STEP             0x78DDE6E4u
#define AH_ACE_V16_INIT         0x3C6EF372u
#define AH_ACE_V17_INIT         0xDAA66D2Bu
#define AH_ACE_DEAD_SENTINEL    0xFF8B6D2Bu
#define AH_ACE_HASH_MUL         0x9E3779B1u
#define AH_ACE_MURMUR_MULT      0x045D9F3Bu
#define AH_ACE_MASK_MAGIC1      0x255992D5u

// ============================================================================
// ACharacter (Tencent SGFramework fork) — Sep-17 verified
// ============================================================================
#define AH_PAWN_MESH            0x388
#define AH_PAWN_CAPSULE         0x398
#define AH_CAPSULE_HALFHEIGHT   0x508
#define AH_CAPSULE_RADIUS       0x50C
#define AH_PAWN_ASC             0x17F8   // AbilitySystemComponent
#define AH_PAWN_WM              0x1970   // WeaponManager
#define AH_PAWN_DEATH_COMP      0x1978
#define AH_PAWN_INV_MGR         0x1AE0
#define AH_PAWN_ARMOR_MGR       0x1B08
#define AH_PAWN_HP_DIRECT       0x1C5C   // Health float (single raw)
#define AH_PAWN_BOT_NAME        0x2138

// ============================================================================
// Weapon / Zoom / Mag / Cam-comp
// ============================================================================
#define AH_WM_WEAPON_LIST       0x350
#define AH_WM_CURWEAPON         0x1F8
#define AH_ASSEMBLE_CACHEDMAG   0x280
// v1.0.37: fixed +0x20 direction error. SDK 4.26.1 ABInfinite:
//   ASGWeapon::WeaponAssembleComp @ 0x0BF0  (was 0xBD0 = CurrentEngageEnemy — off-target!)
//   BP_MagazineBase::SGWeaponContainer @ 0x0940 (was 0x920 = pad)
//   ASGWeapon::WeaponAmmoComp @ 0x0C70 (was 0xC50 = GunSoundComp)
#define AH_MAG_WCC_OFF          0x940
#define AH_MAG_CONTAIN_LIST     0x208
#define AH_MAG_MAX_STACK        0x120
#define AH_CONTAIN_STACKCOUNT_ITEM 0x08
#define AH_WEAPON_AMMO_COMP     0xC70
#define AH_WEAPON_ASSEMBLE      0xBF0
// v0.9.454: verified in Dumper-7 ABInfinite 4.26.1 SDK (SGFramework_classes.hpp:60936).
// Old 0xBE8 was pre-micropatch; struct shifted +0x20 since 2026-07-25.
#define AH_WEAPON_ZOOMCOMP      0xC08
// v0.9.454: WeaponCameraComp on the weapon itself lives on the ASGInventory
// base class at 0x0850 (SDK: SGFramework_classes.hpp:26054, InventoryCameraComp).
// Old 0xC40 was pre-micropatch. The variable-scope live magnification does NOT
// live on the weapon's own CamComp — it lives on the mounted sight-attachment
// (a separate ASGInventory hanging off ZoomComp::LastSight @0x490). Read from
// there for CurrentMagnification.
#define AH_WEAPON_CAMCOMP       0x850   // was 0xC40, base class InventoryCameraComp
#define AH_ZC_LASTSIGHT         0x490   // USGWeaponZoomComponent::LastSight (ASGInventory*)
#define AH_CAMCOMP_ADS_SCENE_FOV 0x148   // real scope render FOV
#define AH_CAMCOMP_MAGNIFICATION 0x14C   // post-process/glass mag (higher than lens)
#define AH_CAMCOMP_ZOOM_FOV      0x250
#define AH_CAMCOMP_HOLDBREATH_FOV 0x254
// v0.9.454: variable-zoom scope live magnification step. int32, Net+RepNotify
// in USGInventoryCameraComponent (SGFramework_classes.hpp:36318). Updates when
// user cycles 2/4/7x mid-ADS on a variable scope. USGWeaponCameraComponent
// inherits the field so it's readable from the same weapon+0xC40 CamComp we
// already dereference. Base mag for that step lives in SubMagnificationInfoList
// @ +0x110 (TArray<FSubCameraInfo>).
#define AH_CAMCOMP_CURRENT_MAG   0x1AC
#define AH_CAMCOMP_SUB_MAG_LIST  0x110
// v0.9.454c: TWO distinct scope-mag fields in USGWeaponZoomComponent.
//   +0x578 = ScopeMagnification (SDK-named, Net/RepNotify) — TARGET / max scope
//            mag for the mounted sight. Constant per weapon build (7.0 for a
//            2-7x variable scope regardless of live state) → USELESS as live
//            indicator, "hip shows 7x" symptom traces to reading this.
//   +0x418 = LIVE current mag (found via memory-diff scanner in ABIFINAL v0.9.387,
//            documented in C:\ABIFINAL\src\offsets.hpp). Auto 1.0 in hip fire,
//            2/4/7 in ADS, updates instantly on mid-ADS variable-zoom cycle.
// ZoomingType/AimScale are Net fields but unreliable for state (AimScale=1.0
// always, ZoomingType flips to 0 during 7→2 transition glitch).
#define AH_ZC_ZOOMING_TYPE      0x3F1   // uint8 ESGZoomType (diag only)
#define AH_ZC_AIM_SCALE         0x3F4   // float (diag only, not the blend I hoped)
#define AH_ZC_LIVE_SCOPE_MAG    0x418   // ★ live mag (memory-diff, 1.0 hip / N ADS)
#define AH_ZC_TARGET_SCOPE_MAG  0x578   // SDK ScopeMagnification (static target)

// ============================================================================
// Mesh
// ============================================================================
#define AH_MESH_ANIM_INSTANCE   0x778
#define AH_MESH_LAST_RENDER     0x32C
#define AH_MESH_COMPONENT_TO_WORLD  0x220

// ============================================================================
// ULevel
// ============================================================================
#define AH_ULEVEL_ACTORS        0x98

// ============================================================================
// Inventory
// ============================================================================
#define AH_INV_ITEM_ID          0x6E0
#define AH_INV_ARMOR_LVL        0x6E8
#define AH_INV_COMMON_DATA      0x780
#define AH_CD_PRICE             0x10C
#define AH_CD_RARITY            0x110
#define AH_CD_SIMPLE_NAME       0x140

// ============================================================================
// DeathComponent
// ============================================================================
#define AH_DEATH_IS_DEAD        0x240

// ============================================================================
// Health 7-limb (ASC path)
// ============================================================================
#define AH_ASC_SPAWNED_ATTRS    0x188
#define AH_HP_INST_VALUE_CUR    0xE0
#define AH_HP_INST_VALUE_MAX    0xE4

// ============================================================================
// Vtables (Jul-01/03) — need re-verify per patch
// ============================================================================
#define AH_PMC_VTABLE           0x148CEA620ULL
#define AH_BOT_VTABLE           0x14805CA08ULL
#define AH_BOT_BOSS_VTABLE      0x1477EF7C0ULL
#define AH_BOT_BODY_VTABLE      0x147FF9000ULL
