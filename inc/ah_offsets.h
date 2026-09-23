// arenahack — Arena Breakout Infinite (UAGame.exe) offsets.
// Copied from C:\ABIFINAL\src\offsets.hpp on 2026-09-23.
// Global RVAs verified Sep-18/20 patch. Struct offsets Sep-17 SDK re-audit.

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
#define AH_RVA_GWORLD           0xB29A608ULL   // PLAIN pointer
#define AH_RVA_GWORLD_MIRROR    0xB29C608ULL   // encrypted tamper mirror (swap+XOR 0x36)
#define AH_RVA_GOBJECTS         0xB52C798ULL   // Sep-17
#define AH_RVA_FNAMEPOOL        0xBB2C780ULL   // Sep-16
#define AH_RVA_FNAME_MASK_KEY   0xBADE76CULL   // byte source for FName derived mask
#define AH_RVA_ACE_CACHE        0xB5C0400ULL   // Sep-20 corrected -0x20

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
#define AH_MAG_WCC_OFF          0x920
#define AH_MAG_CONTAIN_LIST     0x208
#define AH_MAG_MAX_STACK        0x120
#define AH_CONTAIN_STACKCOUNT_ITEM 0x08
#define AH_WEAPON_AMMO_COMP     0xC50
#define AH_WEAPON_ASSEMBLE      0xBD0
#define AH_WEAPON_ZOOMCOMP      0xBE8
#define AH_WEAPON_CAMCOMP       0xC40
#define AH_CAMCOMP_ADS_SCENE_FOV 0x148   // real scope render FOV
#define AH_CAMCOMP_MAGNIFICATION 0x14C   // post-process/glass mag (higher than lens)
#define AH_CAMCOMP_ZOOM_FOV      0x250
#define AH_CAMCOMP_HOLDBREATH_FOV 0x254
#define AH_ZC_LIVE_SCOPE_MAG    0x418

// ============================================================================
// Mesh + bones
// ============================================================================
#define AH_MESH_ANIM_INSTANCE   0x778
#define AH_MESH_LAST_RENDER     0x32C
#define AH_MESH_BONE_ARRAY      0x818
#define AH_MESH_COMPONENT_TO_WORLD  0x220
#define AH_BONE_FTRANSFORM_SZ   48

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
