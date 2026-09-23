// arenahack — ACE position decrypt (from-scratch C port of
// C:\ABIFINAL\src\ace.cpp). Constants from AH_ACE_* in ah_offsets.h.
//
// Verified against UAGame.exe disasm 0x140BC6B60-C4A: 4 identical murmur
// passes composed byte-by-byte. This is the ONLY correct implementation —
// older Python impls had a wrong byte-3 derivation.

#include "../inc/dh_common.h"
#include "../inc/dh_rpm.h"
#include "../inc/ah_offsets.h"
#include "../inc/ah_ace.h"

#include <string.h>

static AH_ACE_STATUS g_ah_ace_last = AH_ACE_OK;
AH_ACE_STATUS AhAceLastFail(void) { return g_ah_ace_last; }

static inline u32 u32c(u64 x) { return (u32)x; }

static inline u32 ah_murmur(u32 x) {
    return u32c(u32c(x ^ (x >> 16)) * AH_ACE_MURMUR_MULT);
}

static inline u32 ah_mask(u32 v12, u32 seed, u32 v16, u32 v17) {
    u32 m1 = ah_murmur(seed ^ v12 ^ u32c(v17 + AH_ACE_MASK_MAGIC1));
    u32 m2 = ah_murmur(seed ^ v12 ^ u32c(v17 - AH_ACE_V16_INIT));
    u32 m3 = ah_murmur(seed ^ v12 ^ v16);
    u32 m4 = ah_murmur(seed ^ v12 ^ v17);
    u32 b0 = ((m1 ^ (m1 >> 16)) & 0xFFu) <<  0;
    u32 b1 = ((m2 ^ (m2 >> 16)) & 0xFFu) <<  8;
    u32 b2 = ((m3 ^ (m3 >> 16)) & 0xFFu) << 16;
    u32 b3 = ((m4 ^ (m4 >> 16)) & 0xFFu) << 24;
    return b0 | b1 | b2 | b3;
}

// Walk hash-bucket chain in ACE cache table. base = UAGame image base.
// Returns TRUE if entry with matching key found; fills data_ptr + seed.
static BOOL walk_bucket(HANDLE hDev, u64 procCR3, u64 imageBase, u32 key,
                        u64* dataPtrOut, u32* seedOut) {
    u32 bucket = u32c((u64)AH_ACE_HASH_MUL * key) % 0x10001u;
    u64 bucketVA = imageBase + AH_RVA_ACE_CACHE + (u64)bucket * 8;
    u64 entry = 0;
    if (!RpmRead64(hDev, procCR3, bucketVA, &entry) || entry == 0) return FALSE;

    for (int i = 0; i < 32 && entry; i++) {
        u8 buf[0x30];
        if (!RpmReadVirtual(hDev, procCR3, entry, buf, sizeof(buf))) return FALSE;
        u32 k = *(u32*)(buf + 0x1C);
        if (k == key) {
            *dataPtrOut = *(u64*)(buf + 0x10);
            *seedOut    = *(u32*)(buf + 0x20);
            return TRUE;
        }
        entry = *(u64*)(buf + 0x28);
    }
    return FALSE;
}

static BOOL decrypt_bytes(u32 key, u32 seed,
                          u32 d0, u32 d1, u32 d2,
                          float* xOut, float* yOut, float* zOut) {
    u32 v12 = u32c((u64)AH_ACE_HASH_MUL * key);
    u32 v16 = AH_ACE_V16_INIT;
    u32 v17 = AH_ACE_V17_INIT;
    d0 = u32c(d0 ^ ah_mask(v12, seed, v16, v17));
    v16 = u32c(v16 + AH_ACE_STEP); v17 = u32c(v17 + AH_ACE_STEP);
    d1 = u32c(d1 ^ ah_mask(v12, seed, v16, v17));
    v16 = u32c(v16 + AH_ACE_STEP); v17 = u32c(v17 + AH_ACE_STEP);
    d2 = u32c(d2 ^ ah_mask(v12, seed, v16, v17));

    memcpy(xOut, &d0, 4);
    memcpy(yOut, &d1, 4);
    memcpy(zOut, &d2, 4);
    return TRUE;
}

BOOL AhAceDecrypt(HANDLE hDev, u64 procCR3, u64 imageBase,
                  u64 rootComponent, float* xOut, float* yOut, float* zOut)
{
    if (!rootComponent) { g_ah_ace_last = AH_ACE_FAIL_READ_CTL; return FALSE; }

    u32 ctl = 0;
    if (!RpmReadVirtual(hDev, procCR3, rootComponent + AH_ROOT_CTL, &ctl, 4)) {
        g_ah_ace_last = AH_ACE_FAIL_READ_CTL;
        return FALSE;
    }

    u32 algo = ctl >> 29;
    u32 key  = ctl & 0x1FFFFFFu;

    if (algo == 0) {
        // Plaintext — read FVector directly.
        u32 first = 0;
        if (!RpmReadVirtual(hDev, procCR3, rootComponent + AH_ROOT_LOC, &first, 4)) {
            g_ah_ace_last = AH_ACE_FAIL_ALGO0_READ;
            return FALSE;
        }
        if (first == AH_ACE_DEAD_SENTINEL) {
            g_ah_ace_last = AH_ACE_FAIL_ALGO0_DEAD;
            return FALSE;
        }
        float v[3];
        if (!RpmReadVirtual(hDev, procCR3, rootComponent + AH_ROOT_LOC, v, 12)) {
            g_ah_ace_last = AH_ACE_FAIL_ALGO0_READ;
            return FALSE;
        }
        *xOut = v[0]; *yOut = v[1]; *zOut = v[2];
        g_ah_ace_last = AH_ACE_OK;
        return TRUE;
    }

    if (key == 0) { g_ah_ace_last = AH_ACE_FAIL_KEY_ZERO; return FALSE; }

    u64 dataPtr = 0;
    u32 seed = 0;
    if (!walk_bucket(hDev, procCR3, imageBase, key, &dataPtr, &seed)) {
        g_ah_ace_last = AH_ACE_FAIL_BUCKET_MISS;
        return FALSE;
    }

    u8 dbuf[12];
    if (!RpmReadVirtual(hDev, procCR3, dataPtr, dbuf, sizeof(dbuf))) {
        g_ah_ace_last = AH_ACE_FAIL_DATA_READ;
        return FALSE;
    }
    u32 d0 = *(u32*)(dbuf + 0);
    u32 d1 = *(u32*)(dbuf + 4);
    u32 d2 = *(u32*)(dbuf + 8);

    decrypt_bytes(key, seed, d0, d1, d2, xOut, yOut, zOut);
    g_ah_ace_last = AH_ACE_OK;
    return TRUE;
}
