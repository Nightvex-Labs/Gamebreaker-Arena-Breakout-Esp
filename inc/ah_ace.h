// arenahack — ACE position decrypt.
// Ported from C:\ABIFINAL\src\ace.cpp (murmur XOR stream, verified against
// UAGame.exe disasm 0x140BC6B60-C4A).
//
// Usage:
//   float x, y, z;
//   if (AhAceDecrypt(hDev, procCR3, imageBase, rootComp, &x, &y, &z)) {
//       // valid world coords
//   }
#pragma once
#include "dh_common.h"

typedef enum {
    AH_ACE_OK = 0,
    AH_ACE_FAIL_READ_CTL,
    AH_ACE_FAIL_ALGO0_DEAD,
    AH_ACE_FAIL_ALGO0_READ,
    AH_ACE_FAIL_KEY_ZERO,
    AH_ACE_FAIL_BUCKET_MISS,
    AH_ACE_FAIL_DATA_READ,
} AH_ACE_STATUS;

// Read + decrypt Root.RelativeLocation. rootComponent = pawn+PAWN_ROOT deref.
// Returns TRUE on success and fills x/y/z with world coords.
BOOL AhAceDecrypt(HANDLE hDev, u64 procCR3, u64 imageBase,
                  u64 rootComponent, float* xOut, float* yOut, float* zOut);

// Last-failure code from most recent AhAceDecrypt call.
AH_ACE_STATUS AhAceLastFail(void);
