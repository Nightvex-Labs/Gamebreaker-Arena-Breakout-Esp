// arenahack — background reader thread.
// One-time bypass init, then ~20Hz chain walk: cam pos + yaw + fov live-published
// to a lock-free-ish shared struct that overlay_boot reads each render frame.
//
// Fills only the "self" fields for now (cam.x/y/z, cam.yaw/pitch/roll, cam.fov)
// so the stats panel and radar have real data. Enemy walk lands next.

#include <windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#ifdef AH_DIAG
static void ah_diag(const char* fmt, ...)
{
    HANDLE h = CreateFileA("C:\\Users\\Public\\ah_reader.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[512];
    int hdr = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[%02u:%02u:%02u.%03u pid=%lu] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        (unsigned long)GetCurrentProcessId());
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf + hdr, sizeof(buf) - hdr - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\r'; buf[n++] = '\n';
    DWORD w = 0; WriteFile(h, buf, (DWORD)n, &w, NULL);
    CloseHandle(h);
}
#else
#define ah_diag(...) ((void)0)
#endif

extern "C" {
#include "../inc/dh_common.h"
#include "../inc/dh_scm.h"
#include "../inc/dh_provider.h"
#include "../inc/dh_rpm.h"
#include "../inc/ah_uspace_read.h"   // v1.0.34
#include "../inc/ah_offsets.h"

#ifndef AH_CR3_USPACE
#define AH_CR3_USPACE 1ULL
#endif
#include "../inc/ah_ace.h"
}

#include "../inc/item_names.hpp"
#include "ah_reader_thread.h"

#include <unordered_map>
#include <unordered_set>
#include <exception>       // v1.0.27 std::set_terminate + std::exception
#include <cstdlib>         // v1.0.27 _Exit
#include "abi_ui/crash_marker.hpp"   // v1.0.27 crash_marker::write

// Per-pawn cache: pawn -> limb-aset ptr. Populated by ASC sig-scan on
// first hit, reused for cheap 0x40-byte cur+max snapshots each tick.
static std::unordered_map<uint64_t, uint64_t> g_limb_aset_cache;

// Discovered bot pawns (shape-probed from Level.Actors scan).
static std::unordered_set<uint64_t>                g_known_bots;
// vtable → kind cache (0 = confirmed non-pawn, 1 = pawn-shaped).
static std::unordered_map<uint64_t, int>           g_bot_vt_kind;
// Per-bot cap (radius, half-height) — cached once, capsule static for AI.
static std::unordered_map<uint64_t, std::pair<float,float>> g_bot_cap;

// Death-pos snapshot: bot pawn → last-known {x,y,z, cap_r, cap_hh}.
// Populated on hp<=0 detection; survives eviction so corpse stays visible.
struct AhCorpse { float x,y,z, cap_r, cap_hh; DWORD stamp_ms; };
static std::unordered_map<uint64_t, AhCorpse> g_bot_corpses;

// v0.9.454: PMC/player death-pos snapshot. Same idea as g_bot_corpses but with
// full meta (name/team/armor/weapon) frozen at real-death moment so the corpse
// keeps rendering after the server strips the pawn from GameState.PlayerArray
// (usually 3-10s post-death). Without this, player corpses vanish instantly.
struct AhPmcCorpse {
    float    x, y, z, cap_r, cap_hh;
    int      team;
    int      helm, vest;
    float    helm_dur, vest_dur;
    uint32_t weapon_id;
    char     name[32];
    DWORD    stamp_ms;
};
static std::unordered_map<uint64_t, AhPmcCorpse> g_pmc_corpses;

// v0.9.457: LIVE-human snapshot cache. The PlayerArray walk has 3
// `continue` paths (PS read fail, Pawn=0, pbuf read fail) that silently
// dropped a human for that tick. Server relevance flicker (Pawn=0 during
// respawn/streaming) + transient RPM slot misses caused "1-2 players not
// loading" reports. We now cache the full AH_ENT per pawn after every
// successful emit and re-emit stale snapshots for up to HUMAN_TTL_MS
// (or HUMAN_MISS_LIMIT consecutive misses, whichever is shorter).
struct AhHumanSnap { AH_ENT ent; DWORD stamp_ms; uint8_t miss; };
static std::unordered_map<uint64_t, AhHumanSnap> g_human_snap;

// Locate the 7-limb HealthAttributeSet on a pawn. Walks
// pawn.ASC.SpawnedAttrs; matches via signature: 7 "max" floats
// at +0x4C..+0x7C (step 8), each 15..200, summing to 60..1200.
// Positive result cached forever. Negative attempts throttled to 1/sec —
// pawn's ASC often spawns aset shortly after actor creation, so instant
// negative cache misses HP for real humans that resolve a tick later.
static std::unordered_map<uint64_t, uint32_t> g_limb_aset_neg_ms;
static u64 ah_find_limb_aset(HANDLE hDev, u64 procCR3, u64 pawn) {
    auto it = g_limb_aset_cache.find(pawn);
    if (it != g_limb_aset_cache.end()) return it->second;

    // Throttle: don't re-walk pawn.ASC more than once per second on failure.
    uint32_t now = GetTickCount();
    auto nit = g_limb_aset_neg_ms.find(pawn);
    if (nit != g_limb_aset_neg_ms.end() && (now - nit->second) < 1000) return 0;

    u64 asc = 0;
    if (!RpmRead64(hDev, procCR3, pawn + AH_PAWN_ASC, &asc) || !asc) {
        g_limb_aset_neg_ms[pawn] = now; return 0;
    }
    u64 arr_data = 0; u32 arr_n = 0;
    RpmRead64(hDev, procCR3, asc + AH_ASC_SPAWNED_ATTRS, &arr_data);
    RpmReadVirtual(hDev, procCR3, asc + AH_ASC_SPAWNED_ATTRS + 8, &arr_n, 4);
    if (!arr_data || arr_n == 0 || arr_n > 32) {
        g_limb_aset_neg_ms[pawn] = now; return 0;
    }

    u64 attrs[32] = {0};
    RpmReadVirtual(hDev, procCR3, arr_data, attrs, (u32)arr_n * 8);
    for (u32 i = 0; i < arr_n; i++) {
        u64 as = attrs[i]; if (!as) continue;
        u8 buf[0x80];
        if (!RpmReadVirtual(hDev, procCR3, as, buf, sizeof(buf))) continue;
        static const int MAX_O[7] = {0x4C,0x54,0x5C,0x64,0x6C,0x74,0x7C};
        float sum = 0.0f; int ok = 0;
        for (int k = 0; k < 7; k++) {
            float m = *(const float*)(buf + MAX_O[k]);
            if (m >= 15.0f && m <= 200.0f) { sum += m; ok++; }
        }
        if (ok == 7 && sum >= 60.0f && sum <= 1200.0f) {
            g_limb_aset_cache[pawn] = as;
            g_limb_aset_neg_ms.erase(pawn);
            return as;
        }
    }
    g_limb_aset_neg_ms[pawn] = now;   // 1s cooldown before next full walk
    return 0;
}

// Read weapon ItemID + mag cur/max from a pawn via 5-hop chain:
//   pawn.WM → CurWeapon → WeaponAssembleComp → CachedMagazine → WCC → ContainList
// Returns TRUE if a valid weapon ItemID was read (fills iid_out).
// mag_cur/mag_max default to -1 on failure. Cheap when the pawn holds no
// weapon (returns after first null ptr — ~1-2 RPMs).
//
// v1.0.37: offsets re-verified against Dumper-7 4.26.1 ABInfinite SDK
// (SGFramework_classes.hpp) after 2026-09 micropatch shift:
//   ASGWeapon::WeaponAssembleComp @ 0x0BF0  (was mis-set to 0xBD0 = CurrentEngageEnemy)
//   BP_MagazineBase::SGWeaponContainer @ 0x0940 (was 0x920 = pad)
static BOOL ah_read_weapon(HANDLE hDev, u64 procCR3, u64 pawn,
                           u32* iid_out, i16* magcur_out, i16* magmax_out) {
    *iid_out = 0; *magcur_out = -1; *magmax_out = -1;
    u64 wm = 0, cw = 0;
    if (!RpmRead64(hDev, procCR3, pawn + AH_PAWN_WM, &wm) || !wm) return FALSE;
    if (!RpmRead64(hDev, procCR3, wm + AH_WM_CURWEAPON, &cw) || !cw) return FALSE;
    RpmReadVirtual(hDev, procCR3, cw + AH_INV_ITEM_ID, iid_out, 4);
    if (*iid_out == 0) return FALSE;

    u64 assemble = 0, mag = 0, wcc = 0, cl_data = 0;
    if (!RpmRead64(hDev, procCR3, cw + AH_WEAPON_ASSEMBLE,   &assemble) || !assemble) return TRUE;
    if (!RpmRead64(hDev, procCR3, assemble + AH_ASSEMBLE_CACHEDMAG, &mag) || !mag) return TRUE;
    if (!RpmRead64(hDev, procCR3, mag + AH_MAG_WCC_OFF, &wcc) || !wcc) return TRUE;
    RpmRead64(hDev, procCR3, wcc + AH_MAG_CONTAIN_LIST, &cl_data);
    if (cl_data) {
        i32 cur = 0;
        RpmReadVirtual(hDev, procCR3, cl_data + AH_CONTAIN_STACKCOUNT_ITEM, &cur, 4);
        if (cur >= 0 && cur <= 500) *magcur_out = (i16)cur;
    }
    i32 maxc = 0;
    RpmReadVirtual(hDev, procCR3, wcc + AH_MAG_MAX_STACK, &maxc, 4);
    if (maxc > 0 && maxc <= 500) *magmax_out = (i16)maxc;
    return TRUE;
}

// Range check for raid coords (ABI maps 0..~200k cm typical).
static inline BOOL ah_is_raid_loc(float x, float y, float z) {
    if (!(x > -1e6f && x < 1e6f)) return FALSE;
    if (!(y > -1e6f && y < 1e6f)) return FALSE;
    if (!(z > -3000.0f && z < 20000.0f)) return FALSE;
    if (x*x + y*y < 0.25f) return FALSE;
    return TRUE;
}

extern "C" const DH_PROVIDER* g_active_provider;
extern "C" u64 g_ah_ace_cache_rva_override;

static std::atomic<bool>     g_run{false};
// v0.9.454: alive flag set to true when reader_body enters, false right before
// return. ah_reader_reattach() uses it to wait for the previous detached thread
// to exit before spawning a new one — otherwise F10 spam could stack multiple
// reader threads hammering the same kdu handle.
static std::atomic<bool>     g_thread_alive{false};
// v0.9.455: reader progress state. Overlay reads via ah_reader_state() and
// uses it for both status UI ("Waiting for game..." etc.) and self-exit on
// GAME_GONE (raid ended + game closed — Steam-wrapper HWND is unreliable).
static std::atomic<int>      g_reader_state{AH_READER_INIT};
extern "C" int ah_reader_state(void) { return g_reader_state.load(); }
static std::mutex            g_snap_lock;
static AH_LIVE_SNAP          g_snap{};
// v1.0.37: SYNCHRONIZE handle to target for reliable process-death detection.
// gworld=0 watchdog alone is unreliable — reader re-attaches on decoy PIDs,
// resetting the streak. WaitForSingleObject on this handle flips instantly
// when the real UAGame.exe process terminates (game closed by user).
static HANDLE                g_target_hproc = NULL;
static std::atomic<uint32_t> g_reader_ticks{0};
static std::atomic<uint32_t> g_reader_last_ms{0};
static std::atomic<float>    g_reader_hz{0.0f};

// Publisher — atomically swap the whole struct.
static void publish(const AH_LIVE_SNAP& s) {
    std::lock_guard<std::mutex> lk(g_snap_lock);
    g_snap = s;
}

extern "C" void ah_reader_snapshot(AH_LIVE_SNAP* out) {
    std::lock_guard<std::mutex> lk(g_snap_lock);
    *out = g_snap;
}

// Key-driven ACE_CACHE RVA finder. Take live enemy keys captured from
// pawn root ctl fields, hash each to bucket index, and for each candidate
// RVA verify: the bucket contains an entry whose +0x1C == that same key.
// Only the real ACE_CACHE table will pass — random shape-similar pages
// won't happen to have entries matching our specific 4 keys.
//
// Sweep a wide range (±0x100000 around baseline) at 8-byte alignment.
static u64 ah_scan_ace_cache(HANDLE hDev, u64 procCR3, u64 imageBase,
                             u64 baseline_rva,
                             const u32* keys, int n_keys)
{
    if (n_keys < 2) return 0;
    // Widened to ±0x400000 (4MB each side, 8MB total sweep) so that
    // future UAGame updates whose ACE_CACHE section drifts further can
    // still be discovered without another rebuild. Bounded by common
    // .rdata/.data section sizes (usually under 32MB), fits within a
    // single reader init round.
    const u64 START = (baseline_rva >= 0x400000) ? baseline_rva - 0x400000 : 0x1000000ULL;
    const u64 END   = baseline_rva + 0x400000ULL;
    // Precompute bucket indices for each key.
    u32 buckets[8] = {0};
    for (int i = 0; i < n_keys && i < 8; i++) {
        buckets[i] = (u32)(((u64)AH_ACE_HASH_MUL * (u64)keys[i]) & 0xFFFFFFFFu) % 0x10001u;
    }
    for (u64 off = START; off < END; off += 8) {
        int match = 0;
        for (int i = 0; i < n_keys && i < 8; i++) {
            u64 entry = 0;
            if (!RpmRead64(hDev, procCR3, imageBase + off + (u64)buckets[i] * 8, &entry))
                goto next;
            if (entry == 0) goto next;
            // Walk chain up to 8 hops looking for our key.
            int found = 0;
            for (int hop = 0; hop < 8 && entry; hop++) {
                u32 k = 0;
                if (!RpmReadVirtual(hDev, procCR3, entry + 0x1C, &k, 4)) break;
                if (k == keys[i]) { found = 1; break; }
                u64 next_e = 0;
                if (!RpmRead64(hDev, procCR3, entry + 0x28, &next_e)) break;
                entry = next_e;
            }
            if (!found) goto next;
            match++;
        }
        if (match >= n_keys) {
            return off;   // all keys found — this IS ACE_CACHE
        }
        next: ;
    }
    return 0;
}

// SIG_GWORLD: `48 3B 1D ?? ?? ?? ?? 75 0D`
// CMP r?, [rip+disp32]  ; JNZ +0x0D    — disp32 at byte 3, insn_len=7.
// Scan every executable section of UAGame.exe; on first match resolve the
// RIP-relative disp to get GWorld's static RVA. Returns 0 on miss.
static u64 ah_sig_scan_gworld(HANDLE hDev, u64 procCR3, u64 imageBase)
{
    IMAGE_DOS_HEADER dh = {0};
    if (!RpmReadVirtual(hDev, procCR3, imageBase, &dh, sizeof(dh))
        || dh.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nh = {0};
    if (!RpmReadVirtual(hDev, procCR3, imageBase + dh.e_lfanew, &nh, sizeof(nh))
        || nh.Signature != IMAGE_NT_SIGNATURE) return 0;

    u64 sec_hdr_va = imageBase + dh.e_lfanew
                   + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader)
                   + nh.FileHeader.SizeOfOptionalHeader;

    // Pattern: 48 3B 1D ?? ?? ?? ?? 75 0D
    static const u8 PAT[9] = { 0x48, 0x3B, 0x1D, 0, 0, 0, 0, 0x75, 0x0D };
    static const u8 MSK[9] = {   1,    1,    1,   0, 0, 0, 0,    1,    1 };

    for (u32 i = 0; i < nh.FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER sh = {0};
        if (!RpmReadVirtual(hDev, procCR3,
                            sec_hdr_va + (u64)i * sizeof(sh),
                            &sh, sizeof(sh))) continue;
        if (!(sh.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        u32 vsize = sh.Misc.VirtualSize;
        if (vsize == 0 || vsize > 300u * 1024 * 1024) continue;

        u8* buf = (u8*)VirtualAlloc(NULL, vsize, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        if (!buf) continue;
        const u32 CHUNK = 0x10000;
        u32 got = 0;
        for (u32 off = 0; off < vsize; off += CHUNK) {
            u32 rd = (CHUNK < (vsize - off)) ? CHUNK : (vsize - off);
            if (RpmReadVirtual(hDev, procCR3, imageBase + sh.VirtualAddress + off,
                               buf + off, rd)) got += rd;
            else memset(buf + off, 0, rd);
        }
        if (got == 0) { VirtualFree(buf, 0, MEM_RELEASE); continue; }

        u32 last = (vsize >= 9) ? (vsize - 9) : 0;
        for (u32 j = 0; j <= last; j++) {
            int match = 1;
            for (int k = 0; k < 9; k++) {
                if (MSK[k] && buf[j+k] != PAT[k]) { match = 0; break; }
            }
            if (match) {
                int32_t disp = *(int32_t*)(buf + j + 3);
                u64 next_rva = sh.VirtualAddress + j + 7;   // insn_len
                u64 gworld_rva = next_rva + (int64_t)disp;
                VirtualFree(buf, 0, MEM_RELEASE);
                return gworld_rva;
            }
        }
        VirtualFree(buf, 0, MEM_RELEASE);
    }
    return 0;
}

// v1.0.27: reader_body renamed to reader_body_impl. New reader_body is a
// try/catch wrapper that catches std::bad_alloc / std::exception / any other
// C++ exception that would otherwise reach RaiseFailFastException with
// marker=0 (no forensic evidence). Also installs std::set_terminate exactly
// once so uncaught C++ exceptions in ANY thread (not just reader) end up as
// a proper marker=TERMINATE_HANDLER (13) rather than marker=0.
static void reader_body_impl(void);

static void reader_body(void) {
    // Install terminate handler once. std::call_once used to be safe against
    // races if reattach spawns a fresh reader thread mid-crash of the first.
    static std::once_flag s_term_once;
    std::call_once(s_term_once, [](){
        std::set_terminate([](){
            // Fast, single-byte marker before termination. VEH v1.0.24 may or
            // may not fire depending on how the exception unwound; the marker
            // is the belt-and-suspenders channel.
            crash_marker::write(crash_marker::TERMINATE_HANDLER);
            _Exit(0x0D);
        });
    });

    try {
        reader_body_impl();
    } catch (const std::bad_alloc&) {
        // Out-of-memory in std::unordered_map (g_pmc_corpses / g_human_snap /
        // g_bot_cap etc.) — write a distinct marker so field triage doesn't
        // confuse this with a real AV.
        ah_diag("!!! reader_body: std::bad_alloc caught — marker=READER_BAD_ALLOC");
        crash_marker::write(crash_marker::READER_BAD_ALLOC);
        g_thread_alive.store(false);
        _Exit(0xE0000009);
    } catch (const std::exception& e) {
        ah_diag("!!! reader_body: std::exception caught: what()='%s' — marker=READER_UNKNOWN_EXC",
                e.what() ? e.what() : "(null)");
        crash_marker::write(crash_marker::READER_UNKNOWN_EXC);
        g_thread_alive.store(false);
        _Exit(0xE0000EEE);
    } catch (...) {
        ah_diag("!!! reader_body: unknown C++ exception caught — marker=READER_UNKNOWN_EXC");
        crash_marker::write(crash_marker::READER_UNKNOWN_EXC);
        g_thread_alive.store(false);
        _Exit(0xE0000EEE);
    }
}

static void reader_body_impl(void) {
    g_thread_alive.store(true);
    g_reader_state.store(AH_READER_INIT);
    ah_diag("=== reader_body ENTER build=%s ===", __DATE__ " " __TIME__);
    // High-resolution timer — default Windows tick is 15.6ms, so Sleep(10)
    // actually sleeps ~15ms (=~64Hz not 100Hz). timeBeginPeriod(1) drops it
    // to 1ms so Sleep(N) is honest ±0.5ms.
    timeBeginPeriod(1);

    DH_DRIVER drv = {0};
    HANDLE dev = NULL; u32 flags = 0;
    ah_diag("DhProviderSelect BEGIN");
    const DH_PROVIDER* p = DhProviderSelect(&dev, &flags);
    if (!p || !dev) {
        ah_diag("DhProviderSelect FAIL (p=%p dev=%p) -- reader EXIT", (void*)p, (void*)dev);
        DH_ERROR("reader: provider select failed"); timeEndPeriod(1);
        g_reader_state.store(AH_READER_PROVIDER_FAIL);
        g_thread_alive.store(false);
        return;
    }
    drv.hDevice = dev;
    g_active_provider = p;
    g_reader_state.store(AH_READER_PROVIDER_OK);
    ah_diag("DhProviderSelect OK kdu#%u %s dev=%p", p->kdu_id, p->name, (void*)dev);
    DH_INFO("reader: provider kdu#%u %s", p->kdu_id, p->name);

    u64 sysCR3 = 0;
    ah_diag("RpmFindSystemCR3 BEGIN");
    if (!RpmFindSystemCR3(drv.hDevice, &sysCR3)) {
        ah_diag("RpmFindSystemCR3 FAIL -- reader EXIT");
        DH_ERROR("reader: sysCR3 fail"); CloseHandle(dev);
        g_reader_state.store(AH_READER_CR3_FAIL);
        g_thread_alive.store(false);
        return;
    }
    g_reader_state.store(AH_READER_CR3_OK);
    ah_diag("RpmFindSystemCR3 OK sysCR3=0x%llX", (unsigned long long)sysCR3);

    // Signal KoenFlow launcher — DeltaHack's DHREADY event. All DhModal
    // payloads use the same name so launcher's WaitForDhReadyAsync is
    // product-agnostic. NULL DACL SD required: overlay is elevated, but
    // NightvexLauncher.exe runs unelevated — default token DACL blocks the
    // cross-integrity OpenEvent with ERROR_ACCESS_DENIED (gle=5), never
    // sees signaled state → error #16/#99 in modal. Null DACL = everyone.
    {
        SECURITY_DESCRIPTOR sd; SECURITY_ATTRIBUTES sa;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE /*present*/, NULL /*null DACL = everyone*/, FALSE);
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
        HANDLE ready_ev = CreateEventW(
            &sa, TRUE /*manual reset*/, FALSE,
            L"Global\\{DHREADY-4EC7A38D-91B2-4C6A-9F32-DE8B7C51F2A0}");
        DH_WARN("reader: DHREADY CreateEventW hEv=%p gle=%lu",
                ready_ev, (unsigned long)GetLastError());
        if (ready_ev) {
            BOOL ok = SetEvent(ready_ev);
            DH_WARN("reader: DHREADY SetEvent ok=%d gle=%lu",
                    (int)ok, (unsigned long)GetLastError());
        }
    }

    // Loop: rediscover UAGame each ~5s (raid relaunch resilience).
    u64 procCR3 = 0, eproc = 0, imageBase = 0;
    DWORD last_find = 0;

    // Bot cache — pawn pointers discovered via shape-probe during actor
    // scan. Per-tick bot walk pulls pos/HP from these addrs without another
    // Level.Actors walk (same pattern as ABIFINAL bot_update_once).
    static std::unordered_set<u64> g_known_bots;
    static std::unordered_map<u64, int> g_bot_vt_kind;   // vtable → 0/1/-1

    // Loot cache — INTERLEAVED chunk scanner.
    // Every tick we process a fixed slice (LOOT_CHUNK) of the actor array.
    // When the slice completes the full array, cached_loot atomically swaps
    // and generation bumps. This avoids the burst that starved the player
    // walk and made enemies feel like 5fps.
    // v0.9.458: FULL revert of v0.9.456 chunk bump. 256 was too big (reader
    // choked, "loot broken" reports), 128 also caused draw regressions —
    // back to the proven 64. Full-cycle time is ~1.5 s on 3000-actor maps
    // but reader stays at ~50 Hz and per-tick load matches the pre-v0.9.456
    // behavior that shipped fine for weeks.
    const int LOOT_CHUNK = 64;
    int          cached_loot_n = 0;
    unsigned int cached_loot_gen = 0;
    AH_LOOT      cached_loot[AH_MAX_LOOT];
    memset(cached_loot, 0, sizeof(cached_loot));

    // v0.9.458: reverted 8192 → 4096. Doubling buf made loot cycle time
    // 2× longer at chunk=64 (~3 s full cycle), and combined with the bigger
    // chunk regression caused visible loot outages. 4096 was the proven
    // ceiling; if a real raid pushes past it we'll bump surgically then.
    static u64 scan_actor_buf[4096];
    int  scan_total_actors = 0;   // valid entries in scan_actor_buf
    int  scan_cursor = 0;         // next index to process
    int  scan_out_n = 0;          // accumulator for this pass
    AH_LOOT scan_out[AH_MAX_LOOT];
    memset(scan_out, 0, sizeof(scan_out));
    // v0.9.459: snapshot self.x/self.y at cycle START and use them for the
    // whole cycle. Chunk=64 makes a full cycle ~1 s; without this, a moving
    // player sees earlier chunks' loot drop out of the radius gate as new
    // chunks fire against a shifted self position, so cached_loot lands
    // near-empty. Freezing the reference point fixes the "loot appears
    // then vanishes while running" pattern.
    float scan_self_x = 0.0f, scan_self_y = 0.0f;

    // v1.0.14: dynamic GWorld RVA resolution via sig scan of UAGame .text.
    // AH_RVA_GWORLD constant is the FIRST guess; if it points to a null
    // pointer for >5 sec after UAGame attach with reachable GEngine, we
    // scan for the pattern `48 3B 1D ?? ?? ?? ?? 75 0D` (CMP r?,
    // [rip+disp32]; JNZ +0x0D) and use the RIP-resolved target instead.
    u64 gworld_rva_live = AH_RVA_GWORLD;   // dynamic override
    int gworld_scan_state = 0;             // 0=not tried, 1=success, 2=failed
    // v1.0.16/17: procCR3/eproc/imageBase LATCH after first successful
    // attach. ACE tampers EPROCESS.DirectoryTableBase live so re-reading
    // g_eproc_dtb every 5s gave us a NEW CR3 each cycle pointing to
    // garbage. Keep the FIRST resolved CR3; only re-probe when either:
    //   (1) the eproc's ImageFileName check shows the process died, OR
    //   (2) we've been latched >30s and GWorld read STILL returns 0 —
    //       that means we latched a bogus CR3 (ACE decoy or timing) and
    //       need a fresh RpmFindProcess pass to find the real one.
    u64 attached_pid = 0;    // 0 = not attached; else the pid we latched on
    DWORD attach_ms  = 0;    // time we latched — for bailout timers
    BOOL  gworld_seen = FALSE;   // set TRUE the first tick gworld != 0
    BOOL  canary_seen = FALSE;   // v0.9.462: TRUE once GObjects OR FNamePool read != 0

    // v0.9.462: recent-fail EPROCESS blacklist. On LATCH BAILOUT we push
    // the eproc pointer + PID here for 60s so the next RpmFindProcess pass
    // doesn't re-latch the same decoy. Reader thread only — RpmFindProcess
    // is called from one place so a local skip-check works.
    struct FailedAttach { u64 eproc; u64 pid; DWORD stamp_ms; };
    FailedAttach failed_attachments[4] = {};   // ring buffer
    int failed_idx = 0;
    ah_diag("entering main loop -- waiting for UAGame process (GWorld initial RVA=0x%llX)",
            (unsigned long long)gworld_rva_live);
    g_reader_state.store(AH_READER_WAITING_GAME);
    DWORD last_diag_ms = 0;
    int   last_ent_n = -1;
    int   find_attempts = 0;
    while (g_run.load()) {
        DWORD now = GetTickCount();

        // v1.0.37 (top-of-tick hard death check): SYNCHRONIZE handle stays
        // open across CR3-STALE resets and re-attach cycles so the game-gone
        // signal fires regardless of the current attach state. This is the
        // fast path — WAIT_OBJECT_0 the instant UAGame terminates. Fallback
        // remains the gworld=0 streak watchdog further down.
        if (g_target_hproc &&
            WaitForSingleObject(g_target_hproc, 0) == WAIT_OBJECT_0)
        {
            ah_diag("GAME-GONE: SYNCHRONIZE wait on prior pid=%llu signaled — process terminated",
                    (unsigned long long)attached_pid);
            CloseHandle(g_target_hproc);
            g_target_hproc = NULL;
            g_reader_state.store(AH_READER_GAME_GONE);
            // Do not exit reader loop directly — overlay main thread watches
            // AH_READER_GAME_GONE and drives ordered shutdown + self-destruct.
            Sleep(50);
            continue;
        }

        // === PROCESS ATTACH / LIVENESS =========================================
        // First attach: full RpmFindProcess and LATCH pid/eproc/CR3/imageBase.
        // Subsequent probes just verify the eproc's ImageFileName still says
        // "UAGame". If it doesn't, drop the latch and re-probe. Never re-read
        // g_eproc_dtb — ACE tamper makes that read give garbage.
        if (attached_pid == 0) {
            // v1.0.29: fast-poll for first N attempts so cold-start feels
            // instant if UAGame is already running. Field triage of v1.0.28
            // showed cold-attach taking 60-177 attempts (30-90s at old 500ms
            // interval) — user waited, saw no ESP, killed via task-mgr.
            // First 30 attempts at 150ms = 4.5s of fast probe covers the
            // "game already loaded" case. Beyond that, steady 500ms to
            // avoid saturating the kdu dispatcher on real "game not started
            // yet" waits.
            DWORD find_interval = (find_attempts < 30) ? 150 : 500;
            if (now - last_find >= find_interval) {
                find_attempts++;
                if (RpmFindProcess(drv.hDevice, sysCR3, AH_PROC_NAME, &procCR3, &eproc)) {
                    // v0.9.462: skip recently-failed eproc/pid combos so the
                    // second-chance probe doesn't hand us back the same decoy.
                    // 60s cooldown per entry, ring of 4 slots.
                    BOOL blacklisted = FALSE;
                    u64 tmp_pid = 0;
                    RpmRead64(drv.hDevice, sysCR3, eproc + g_eproc_pid, &tmp_pid);
                    // v1.0.29: blacklist cooldown 60s -> 15s. First-bailout
                    // is often a wrong-CR3 decoy that clears itself once ACE
                    // stabilises; a 60s dead zone in the middle of user's
                    // "why is ESP not on" panic was aggressive. 15s is long
                    // enough that we don't re-latch the same failing eproc
                    // in the same tick, short enough that the recovery
                    // window is invisible to the user.
                    for (int fi = 0; fi < 4; fi++) {
                        FailedAttach& fa = failed_attachments[fi];
                        if (!fa.stamp_ms) continue;
                        if ((now - fa.stamp_ms) > 15000) continue;
                        if (fa.eproc == eproc || fa.pid == tmp_pid) {
                            blacklisted = TRUE; break;
                        }
                    }
                    if (blacklisted) {
                        if (now - last_diag_ms > 3000) {
                            ah_diag("RpmFindProcess #%d SKIP recently-failed eproc=0x%llX pid=%llu",
                                    find_attempts, (unsigned long long)eproc,
                                    (unsigned long long)tmp_pid);
                            last_diag_ms = now;
                        }
                        procCR3 = 0; eproc = 0;   // reset so next iter retries
                        last_find = now;
                        continue;
                    }

                    // v1.0.34: USPACE path — RpmFindProcess returned the
                    // sentinel CR3, and imageBase/pid are known from
                    // AhUspaceBase()/AhUspacePid() (Toolhelp32 already
                    // resolved them). Skip the EPROCESS.PEB walk that
                    // requires a real EPROCESS/CR3 pair.
                    u64 peb = 0;
                    if (procCR3 == AH_CR3_USPACE) {
                        imageBase = AhUspaceBase();
                        tmp_pid   = (u64)AhUspacePid();
                    } else {
                        RpmRead64(drv.hDevice, sysCR3, eproc + g_eproc_peb_off, &peb);
                        u64 sz = 0;
                        RpmGetMainImageBase(drv.hDevice, procCR3, peb, &imageBase, &sz);
                    }
                    attached_pid = tmp_pid;
                    attach_ms = now;
                    gworld_seen = FALSE;
                    canary_seen = FALSE;
                    g_reader_state.store(AH_READER_ATTACHED);
                    // v1.0.37: open SYNCHRONIZE handle to target for reliable
                    // process-death detection. SYNCHRONIZE is the lightest
                    // access mask — ACE rarely strips it (unlike VM_READ),
                    // and the wait check is O(1) per tick.
                    if (g_target_hproc) { CloseHandle(g_target_hproc); g_target_hproc = NULL; }
                    g_target_hproc = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)attached_pid);
                    ah_diag("ATTACH LATCH #%d pid=%llu procCR3=0x%llX eproc=0x%llX peb=0x%llX imageBase=0x%llX %s (SYNC handle=%p)",
                            find_attempts,
                            (unsigned long long)attached_pid,
                            (unsigned long long)procCR3, (unsigned long long)eproc,
                            (unsigned long long)peb, (unsigned long long)imageBase,
                            (procCR3 == AH_CR3_USPACE) ? "(USPACE)" : "(kdu)",
                            (void*)g_target_hproc);
                    // ACE_CACHE scan deferred until we can capture live
                    // enemy keys from PlayerArray walk — see PlayerArray
                    // section below.
                    g_ah_ace_cache_rva_override = 0;
                } else if (now - last_diag_ms > 3000) {
                    ah_diag("RpmFindProcess #%d NOT FOUND UAGame.exe (waiting)", find_attempts);
                    last_diag_ms = now;
                }
                last_find = now;
            }
        } else {
            // (SYNCHRONIZE handle death check moved to top-of-tick in v1.0.37
            // so it fires even after CR3-STALE reset detaches this branch.)
            // v0.9.462: canary check runs every tick — cheap (~4 RPM), and
            // catches wrong-CR3 attach in ≤3s instead of the old 30s wait.
            // GObjects and FNamePool are non-null the moment UAGame passes
            // its own init; if BOTH stay 0 for 3s past attach, we're on a
            // decoy/dead EPROCESS, not the real process.
            if (!canary_seen && imageBase && (now - attach_ms) > 500) {
                u64 gob = 0, fnp = 0;
                RpmRead64(drv.hDevice, procCR3, imageBase + AH_RVA_GOBJECTS, &gob);
                RpmRead64(drv.hDevice, procCR3, imageBase + AH_RVA_FNAMEPOOL, &fnp);
                if (gob || fnp) canary_seen = TRUE;
            }

            if (now - last_find >= 5000) {
                // Liveness check — read ImageFileName at latched eproc, verify
                // still starts with "UAGame". Cheap (16 bytes RPM). If gone,
                // drop latch and next iter will re-probe.
                //
                // v1.0.34: USPACE path has no EPROCESS. Liveness via handle
                // probe — AhUspaceVerify() reads MZ at module base. If the
                // process died the handle read fails; treat as liveness_lost.
                char img[16] = {0};
                BOOL rd;
                BOOL liveness_lost;
                if (procCR3 == AH_CR3_USPACE) {
                    rd = AhUspaceVerify();
                    liveness_lost = !rd;
                    if (!rd) {
                        snprintf(img, sizeof(img), "uspace-dead");
                    }
                } else {
                    rd = RpmReadVirtual(drv.hDevice, sysCR3,
                                        eproc + g_eproc_imgname, img, 15);
                    liveness_lost = (!rd || _strnicmp(img, AH_PROC_NAME, 6) != 0);
                }
                // v0.9.462: bogus-CR3 hard bailout tightened 30s → 10s.
                // GWorld may legitimately be null in main menu (game not
                // yet loaded), but 10s is more than enough for it to appear
                // once user hits Play. Users no longer suffer 30s "UI-alive,
                // ESP-dead" window on wrong-PID attach.
                // v1.0.27: bogus_gworld 10s → 15s. Slow rigs (Win10 21H2 on
                // b19045 boxes) can take 10s just to load main menu; earlier
                // bailout was misfiring on legitimate slow clients.
                // v1.0.34: USPACE path is authoritative (Toolhelp gives the
                // real UAGame — cannot be a decoy). Skip canary+gworld
                // bailouts entirely on USPACE — gworld=null just means the
                // user is sitting in the main menu, not that we latched
                // the wrong process.
                BOOL bogus_gworld = (procCR3 != AH_CR3_USPACE)
                                    && !gworld_seen
                                    && (now - attach_ms) > 15000;
                BOOL bogus_canary = (procCR3 != AH_CR3_USPACE)
                                    && !canary_seen
                                    && (now - attach_ms) > 8000;

                if (liveness_lost) {
                    ah_diag("LATCH LOST — eproc ImageFileName check failed (rd=%d name='%s'). Re-probing.",
                            (int)rd, img);
                } else if (bogus_canary) {
                    ah_diag("LATCH BAILOUT (canary) — GObjects+FNamePool both 0 for 3s (pid=%llu eproc=0x%llX). "
                            "Wrong process — full re-probe, blacklisting for 60s.",
                            (unsigned long long)attached_pid,
                            (unsigned long long)eproc);
                } else if (bogus_gworld) {
                    ah_diag("LATCH BAILOUT (gworld) — GWorld stayed 0 for 10s (pid=%llu procCR3=0x%llX). "
                            "Probably wrong CR3 latched — full re-probe, blacklisting for 60s.",
                            (unsigned long long)attached_pid,
                            (unsigned long long)procCR3);
                }
                if (liveness_lost || bogus_canary || bogus_gworld) {
                    // Push the failed attach into the blacklist ring so the
                    // next RpmFindProcess pass skips this eproc/pid for 60s.
                    // Skip liveness_lost — that just means the game exited,
                    // no reason to blacklist its PID slot.
                    if (bogus_canary || bogus_gworld) {
                        failed_attachments[failed_idx] = { eproc, attached_pid, now };
                        failed_idx = (failed_idx + 1) & 3;
                    }
                    // v1.0.34: USPACE handle held by ah_uspace_read.c —
                    // detach so the next RpmFindProcess re-tries a fresh
                    // Toolhelp lookup (game may have relaunched with a new
                    // PID between our reads).
                    if (procCR3 == AH_CR3_USPACE) {
                        AhUspaceDetach();
                    }
                    // v1.0.37: keep g_target_hproc OPEN across the detach so
                    // top-of-tick SYNCHRONIZE check still catches the death
                    // signal from prior latch. Only closed on a fresh attach
                    // (line 604) or thread exit.
                    attached_pid = 0;
                    procCR3 = 0; eproc = 0; imageBase = 0;
                    gworld_scan_state = 0;   // allow sig-scan again for fresh instance
                    gworld_rva_live = AH_RVA_GWORLD;
                    gworld_seen = FALSE;
                    canary_seen = FALSE;
                    last_find = 0;   // don't wait 5s more — probe immediately
                    g_reader_state.store(AH_READER_WAITING_GAME);
                } else {
                    last_find = now;
                }
            }
        }

        AH_LIVE_SNAP s{};
        s.find_attempts = (unsigned int)find_attempts;   // v1.0.33 HUD progress
        // Per-tick throttle flags — shared by PlayerArray walk + bot update.
        static uint32_t s_tick_ctr = 0;
        ++s_tick_ctr;
        const bool hp_tick     = (s_tick_ctr & 1) == 0;
        const bool armor_tick  = (s_tick_ctr % 5) == 0;
        const bool weapon_tick = (s_tick_ctr % 3) == 0;

        if (procCR3 && imageBase) {
            u64 gworld = 0;
            BOOL rd_ok = RpmRead64(drv.hDevice, procCR3, imageBase + gworld_rva_live, &gworld);
            if (gworld) gworld_seen = TRUE;

            // v1.0.27: CR3-stale auto-resync. When RpmRead64 itself returns
            // FALSE (not just 0-value) for a sustained window, the CR3 we
            // latched at attach can no longer translate user VAs. Regression
            // observed 2026-09-19 for hwid 574f4932 after Delta relaunch —
            // eproc DTB looked valid but every user-space translate returned
            // ERROR_INVALID_ADDRESS. bogus_gworld (15s) eventually catches
            // this, but 15s of dead reads first = "UI alive, ESP dead" for
            // the user. Faster tripwire on translate failure specifically.
            {
                static int s_rpm_fail_streak = 0;
                if (!rd_ok) {
                    int cur = ++s_rpm_fail_streak;
                    if (cur == 200 || cur == 400) {
                        ah_diag("CR3-STALE watchdog: RpmRead64(gworld) FAIL streak=%d "
                                "procCR3=0x%llX — CR3 likely stale after game relaunch",
                                cur, (unsigned long long)procCR3);
                    }
                    if (cur >= 500) {
                        ah_diag("CR3-STALE tripped: 500 consecutive translate FAIL — "
                                "force re-attach, blacklist current eproc/pid for 60s");
                        failed_attachments[failed_idx] = { eproc, attached_pid, now };
                        failed_idx = (failed_idx + 1) & 3;
                        // v1.0.37: keep g_target_hproc open across CR3-STALE
                        // reset so top-of-tick SYNCHRONIZE check keeps working.
                        attached_pid = 0;
                        procCR3 = 0; eproc = 0; imageBase = 0;
                        gworld_scan_state = 0;
                        gworld_rva_live = AH_RVA_GWORLD;
                        gworld_seen = FALSE;
                        canary_seen = FALSE;
                        last_find = 0;
                        s_rpm_fail_streak = 0;
                        continue;
                    }
                } else if (s_rpm_fail_streak > 0) {
                    if (s_rpm_fail_streak >= 200) {
                        ah_diag("CR3-STALE cleared: RpmRead64 recovered after streak=%d",
                                s_rpm_fail_streak);
                    }
                    s_rpm_fail_streak = 0;
                }
            }

            // v0.9.455 game-gone watchdog. UAGame HWND is unreliable —
            // Steam-wrapper holds the window handle for 30-60 s after the game
            // closes, so game_owns_foreground() keeps returning true and the
            // overlay hangs empty. gworld dropping to 0 for a sustained window
            // is the reliable "game gone" signal. Overlay reads g_reader_state
            // and self-exits cleanly on AH_READER_GAME_GONE.
            {
                static int s_gworld_zero_streak = 0;
                if (gworld_seen) {
                    if (gworld == 0) {
                        int cur = ++s_gworld_zero_streak;
                        // Log every 30 ticks to visualise how long we spin,
                        // and log the exact tick we transition to GAME_GONE.
                        if (cur == 30 || cur == 60 || cur == 100) {
                            ah_diag("GAME-GONE watchdog: gworld=0 streak=%d (attached_pid=%llu)",
                                    cur, (unsigned long long)attached_pid);
                        }
                        if (cur >= 150) {
                            if (g_reader_state.load() != AH_READER_GAME_GONE) {
                                ah_diag("GAME-GONE tripped: gworld=0 for 150 ticks — reader signaling overlay exit");
                                g_reader_state.store(AH_READER_GAME_GONE);
                            }
                        }
                    } else {
                        if (s_gworld_zero_streak > 0) {
                            ah_diag("GAME-GONE cleared: gworld=0x%llX after streak=%d",
                                    (unsigned long long)gworld, s_gworld_zero_streak);
                            s_gworld_zero_streak = 0;
                        }
                        if (g_reader_state.load() == AH_READER_GAME_GONE)
                            g_reader_state.store(AH_READER_ATTACHED);
                    }
                }
            }

            // Sig-scan for GWorld RVA. Runs immediately on first attach.
            // If gworld read gave NULL AND we haven't locked in a scan
            // success yet, retry every 30 seconds. Once ah_sig_scan_gworld
            // finds the pattern once we trust that RVA forever (static in
            // PE); if the returned pointer stays 0 after that, that's the
            // UE4 world not yet spawned — reader just keeps polling.
            //
            // v1.0.25: retry interval bumped 3s → 30s + failure cap. Each
            // scan burns ~150 000 IOCTLs against the kdu dispatcher
            // (300 MB of RpmReadVirtual in 64 KB chunks × 2 IOCTLs/chunk).
            // At 3 s cadence with a persistently null GWorld (main menu),
            // this saturates the KMDF work queue and can starve DPC
            // servicing to the point of a hung PC without a bugcheck. 30 s
            // + 5-miss abort gives a ~90 % IOCTL cut on the failure path.
            if (!gworld && gworld_scan_state != 1 && procCR3) {
                static DWORD s_last_scan_ms = 0;
                static int   s_scan_misses  = 0;
                DWORD now_ms = GetTickCount();
                if (s_scan_misses < 5 &&
                    (s_last_scan_ms == 0 || (now_ms - s_last_scan_ms) > 30000)) {
                    s_last_scan_ms = now_ms;
                    ah_diag("SIG-SCAN attempt #%d (state=%d gworld_rva=0x%llX gave NULL)",
                            s_scan_misses + 1,
                            gworld_scan_state,
                            (unsigned long long)gworld_rva_live);
                    u64 found = ah_sig_scan_gworld(drv.hDevice, procCR3, imageBase);
                    if (found) {
                        ah_diag("SIG-SCAN found GWORLD RVA=0x%llX (was 0x%llX, delta=%+lld)",
                                (unsigned long long)found,
                                (unsigned long long)gworld_rva_live,
                                (long long)((int64_t)found - (int64_t)gworld_rva_live));
                        gworld_rva_live = found;
                        gworld_scan_state = 1;   // locked in — trust this RVA
                        s_scan_misses = 0;
                        RpmRead64(drv.hDevice, procCR3, imageBase + gworld_rva_live, &gworld);
                    } else {
                        s_scan_misses++;
                        ah_diag("SIG-SCAN miss (%d/5) — next retry in 30s", s_scan_misses);
                    }
                }
            }
#ifdef AH_DIAG
            {
                static uint64_t s_last_gworld = 0xDEADBEEFCAFEBABEULL;
                static DWORD    s_last_gw_diag_ms = 0;
                DWORD now_ms = GetTickCount();
                if (gworld != s_last_gworld || (now_ms - s_last_gw_diag_ms) > 10000) {
                    // Also probe GObjects + FNamePool + old GWorld RVA to
                    // narrow down whether the whole RVA-set is stale (UAGame
                    // update) or just GWorld specifically.
                    u64 gobjects = 0, fnamepool = 0, gworld_old = 0;
                    BOOL ok_go = RpmRead64(drv.hDevice, procCR3, imageBase + AH_RVA_GOBJECTS, &gobjects);
                    BOOL ok_np = RpmRead64(drv.hDevice, procCR3, imageBase + AH_RVA_FNAMEPOOL, &fnamepool);
                    BOOL ok_old = RpmRead64(drv.hDevice, procCR3, imageBase + 0xB29A608ULL, &gworld_old);
                    // Sample raw bytes right around GWorld slot to spot a shift.
                    uint8_t rawbuf[64] = {0};
                    RpmReadVirtual(drv.hDevice, procCR3, imageBase + gworld_rva_live - 0x20, rawbuf, 64);
                    ah_diag("PROBE gworld=0x%llX ok=%d | gobjects=0x%llX ok=%d | fnamepool=0x%llX ok=%d | old_gworld_rva=0x%llX ok=%d",
                            (unsigned long long)gworld, (int)rd_ok,
                            (unsigned long long)gobjects, (int)ok_go,
                            (unsigned long long)fnamepool, (int)ok_np,
                            (unsigned long long)gworld_old, (int)ok_old);
                    ah_diag("RAW around GWorld[-0x20..+0x20]: "
                            "%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X "
                            "|%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X| "
                            "%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X",
                            rawbuf[0],rawbuf[1],rawbuf[2],rawbuf[3],rawbuf[4],rawbuf[5],rawbuf[6],rawbuf[7],
                            rawbuf[8],rawbuf[9],rawbuf[10],rawbuf[11],rawbuf[12],rawbuf[13],rawbuf[14],rawbuf[15],
                            rawbuf[16],rawbuf[17],rawbuf[18],rawbuf[19],rawbuf[20],rawbuf[21],rawbuf[22],rawbuf[23],
                            rawbuf[24],rawbuf[25],rawbuf[26],rawbuf[27],rawbuf[28],rawbuf[29],rawbuf[30],rawbuf[31],
                            rawbuf[32],rawbuf[33],rawbuf[34],rawbuf[35],rawbuf[36],rawbuf[37],rawbuf[38],rawbuf[39],
                            rawbuf[40],rawbuf[41],rawbuf[42],rawbuf[43],rawbuf[44],rawbuf[45],rawbuf[46],rawbuf[47]);
                    s_last_gworld = gworld;
                    s_last_gw_diag_ms = now_ms;
                }
            }
#else
            (void)rd_ok;
#endif
            u64 gi = 0, lp_arr = 0, lp0 = 0, pc = 0, pawn = 0, root = 0, pcm = 0;
            u64 gs = 0;
            if (gworld) {
                RpmRead64(drv.hDevice, procCR3, gworld + AH_UW_GAMEINSTANCE, &gi);
                RpmRead64(drv.hDevice, procCR3, gworld + AH_UW_GAMESTATE, &gs);
            }
            // v0.9.455: LIVE transition — snapshot is only meaningful once both
            // gworld AND gs are non-null. Between ATTACHED (process latched)
            // and LIVE (world+state ready) we could publish garbage roomid or
            // NULL pawn — overlay would see "in_raid" briefly and render
            // empties. Now overlay checks state and gates its own render.
            {
                int st = g_reader_state.load();
                if (gworld && gs) {
                    if (st == AH_READER_ATTACHED) {
                        ah_diag("READER-LIVE: gworld=0x%llX gs=0x%llX — publishing real snapshot",
                                (unsigned long long)gworld, (unsigned long long)gs);
                        g_reader_state.store(AH_READER_LIVE);
                    }
                } else if (st == AH_READER_LIVE) {
                    ah_diag("READER-LIVE lost: gworld=0x%llX gs=0x%llX — back to ATTACHED",
                            (unsigned long long)gworld, (unsigned long long)gs);
                    g_reader_state.store(AH_READER_ATTACHED);
                }
            }
            // v0.9.454: authoritative in-raid flag from ASGGameState+0x430.
            // Server sets on real raid start, clears on match end / return to
            // menu. ACE-decrypt-success was false-positive in the lobby (root
            // pawn = menu preview character, algo=0 = plaintext, decrypt "OK").
            //
            // v1.0.32: OR with EGameSceneType @ 0x579 (uint8 enum) — server
            // sometimes lags assigning roomid but GameSceneType flips to
            // InBattle (2) immediately on raid enter. In field triage of
            // 574f4932 v1.0.28 the overlay stayed on "ATTACHED — LOADING
            // WORLD" HUD even while the user was inside a raid because
            // roomid stayed 0 across the entire session. Second signal
            // covers that gap. ShootingRoom (4) is the tir/training range
            // — also treat as in-raid so ESP works there for testing.
            u64 raid_room = 0;
            if (gs) RpmReadVirtual(drv.hDevice, procCR3, gs + AH_GS_ROOMID, &raid_room, 8);
            s.roomid = raid_room;

            uint8_t scene_type = 0;
            if (gs) RpmReadVirtual(drv.hDevice, procCR3, gs + AH_GS_SCENETYPE, &scene_type, 1);
            s.scene_type = scene_type;

            // v0.9.454: raid → menu transition. `roomid != 0` while in a raid,
            // 0 in main menu / matchmaking. On the falling edge we wipe every
            // per-raid cache so the next raid starts clean. 24-tick debounce
            // (≈1.2 s at 20 Hz) covers momentary read glitches at the actual
            // exit moment when server tears down the replicated state.
            {
                static bool s_was_in_raid    = false;
                static int  s_out_streak     = 0;
                static bool s_cleared        = false;
                bool in_raid_now = (raid_room != 0)
                                   || (scene_type == AH_SCENE_INBATTLE)
                                   || (scene_type == AH_SCENE_SHOOTINGROOM);
                if (in_raid_now) {
                    s_was_in_raid = true;
                    s_out_streak  = 0;
                    s_cleared     = false;
                } else if (s_was_in_raid) {
                    s_out_streak++;
                    if (s_out_streak >= 24 && !s_cleared) {
                        g_pmc_corpses.clear();
                        g_bot_corpses.clear();
                        g_known_bots.clear();
                        g_bot_cap.clear();
                        g_limb_aset_cache.clear();
                        g_human_snap.clear();   // v0.9.457: human live-cache
                        s_cleared = true;
                        ah_diag("RAID-END (roomid=0): cleared PMC+bot corpse caches + bot pool + human snap");
                    }
                }
            }
            if (gi)     RpmRead64(drv.hDevice, procCR3, gi + AH_GI_LOCALPLAYERS, &lp_arr);
            if (lp_arr) RpmRead64(drv.hDevice, procCR3, lp_arr, &lp0);
            if (lp0)    RpmRead64(drv.hDevice, procCR3, lp0 + AH_LP_PC, &pc);
            if (pc) {
                RpmRead64(drv.hDevice, procCR3, pc + AH_PC_PAWN, &pawn);
                if (!pawn) RpmRead64(drv.hDevice, procCR3, pc + AH_PC_PAWN_ACK, &pawn);
                RpmRead64(drv.hDevice, procCR3, pc + AH_PC_CAMMGR, &pcm);
            }
            if (pawn) RpmRead64(drv.hDevice, procCR3, pawn + AH_PAWN_ROOT, &root);

            if (root) {
                s.attached = (AhAceDecrypt(drv.hDevice, procCR3, imageBase, root,
                                           &s.x, &s.y, &s.z) != 0);
#ifdef AH_DIAG
                {
                    static DWORD s_last_self_diag = 0;
                    static u32   s_last_ctl = 0xFFFFFFFF;
                    u32 ctl_now = 0;
                    RpmReadVirtual(drv.hDevice, procCR3, root + AH_ROOT_CTL, &ctl_now, 4);
                    DWORD now_ms = GetTickCount();
                    if (ctl_now != s_last_ctl || (now_ms - s_last_self_diag) > 10000) {
                        ah_diag("SELF-DECRYPT gworld=0x%llX gs=0x%llX pc=0x%llX pawn=0x%llX root=0x%llX ctl=0x%08X algo=%u attached=%d fail=%d roomid=0x%llX cam=(%.0f,%.0f,%.0f)",
                                (unsigned long long)gworld, (unsigned long long)gs,
                                (unsigned long long)pc, (unsigned long long)pawn,
                                (unsigned long long)root, ctl_now, ctl_now >> 29,
                                (int)s.attached, (int)AhAceLastFail(),
                                (unsigned long long)s.roomid, s.x, s.y, s.z);
                        s_last_ctl = ctl_now;
                        s_last_self_diag = now_ms;
                    }
                }
#endif
            } else {
#ifdef AH_DIAG
                {
                    static DWORD s_last_norot = 0;
                    DWORD now_ms = GetTickCount();
                    if ((now_ms - s_last_norot) > 5000) {
                        ah_diag("SELF-CHAIN missing root: gworld=0x%llX gi=0x%llX gs=0x%llX lp0=0x%llX pc=0x%llX pawn=0x%llX",
                                (unsigned long long)gworld, (unsigned long long)gi,
                                (unsigned long long)gs, (unsigned long long)lp0,
                                (unsigned long long)pc, (unsigned long long)pawn);
                        s_last_norot = now_ms;
                    }
                }
#endif
            }
            if (pc) {
                float ctl[3] = {0};
                RpmReadVirtual(drv.hDevice, procCR3, pc + AH_PC_CONTROLROT, ctl, 12);
                s.pitch = ctl[0];
                s.yaw   = ctl[1];
                s.roll  = ctl[2];
            }
            // Self weapon + ammo (for bottom-right circular HUD).
            s.my_mag_cur = -1; s.my_mag_max = -1;
            if (pawn) {
                u32 wid = 0; i16 mc = -1, mm = -1;
                ah_read_weapon(drv.hDevice, procCR3, pawn, &wid, &mc, &mm);
                s.my_mag_cur = mc;
                s.my_mag_max = mm;
            }

            if (pcm) {
                float fov = 90.0f;
                RpmReadVirtual(drv.hDevice, procCR3, pcm + AH_PCM_CACHE_FOV_PRIV, &fov, 4);
                if (fov > 30.0f && fov < 170.0f) s.fov = fov; else s.fov = 90.0f;

                // Real cam eye position — POV.Location (private cache first,
                // public fallback). Root ACE gives capsule pivot which lags
                // actual eye by pose-varying offset → boxes drift low when
                // shooter crouches/prones. POV.Location tracks eye exactly.
                float povloc[3] = {0};
                bool got = false;
                if (RpmReadVirtual(drv.hDevice, procCR3,
                                   pcm + AH_PCM_CACHE_LOC_PRIV, povloc, 12)) {
                    if (povloc[0] > -1e6f && povloc[0] < 1e6f &&
                        povloc[2] > -3000.0f && povloc[2] < 20000.0f &&
                        (povloc[0]*povloc[0] + povloc[1]*povloc[1]) > 0.25f) got = true;
                }
                if (!got && RpmReadVirtual(drv.hDevice, procCR3,
                                           pcm + AH_PCM_CACHE_LOC, povloc, 12)) {
                    if (povloc[0] > -1e6f && povloc[0] < 1e6f &&
                        povloc[2] > -3000.0f && povloc[2] < 20000.0f &&
                        (povloc[0]*povloc[0] + povloc[1]*povloc[1]) > 0.25f) got = true;
                }
                if (got) { s.x = povloc[0]; s.y = povloc[1]; s.z = povloc[2]; }

                // Cam ROTATION — POV.Rotation over PC.ControlRotation. ControlRot
                // is what the player INTENDS to aim (drives replication) but the
                // actual render cam uses POV.Rotation which includes scope sway,
                // peek offset, camera shake, ADS blending. Using ControlRot for
                // W2S made boxes drift when moving mouse in ADS (rotation source
                // ≠ what game actually projects from).
                float povrot[3] = {0};
                bool rgot = false;
                if (RpmReadVirtual(drv.hDevice, procCR3,
                                   pcm + AH_PCM_CACHE_ROT_PRIV, povrot, 12)) {
                    if (povrot[0] > -180.0f && povrot[0] < 180.0f &&
                        povrot[1] > -360.0f && povrot[1] < 360.0f) rgot = true;
                }
                if (!rgot && RpmReadVirtual(drv.hDevice, procCR3,
                                            pcm + AH_PCM_CACHE_ROT, povrot, 12)) {
                    if (povrot[0] > -180.0f && povrot[0] < 180.0f &&
                        povrot[1] > -360.0f && povrot[1] < 360.0f) rgot = true;
                }
                if (rgot) {
                    s.pitch = povrot[0];
                    s.yaw   = povrot[1];
                    s.roll  = povrot[2];
                }
            } else {
                s.fov = 90.0f;
            }
            s.scope_mag = 1.0f;
            s.scope_fov = 0.0f;

            // ── Scope magnification + sight FOV for W2S in ADS ─────────────
            // wm = pawn+0x1970, curWeapon = wm+0x1F8, ZoomComp = weapon+0xC08
            // (was +0xBE8 pre-micropatch). LIVE scope mag @ ZC+0x578 (was
            // +0x418 stale). Anim proxy CurrentSightFov path:
            // mesh+0x778(anim) → +0x740(localProxy) → +0xB34 (float).
            if (pawn) {
                u64 wm = 0, weapon = 0, zc = 0;
                RpmRead64(drv.hDevice, procCR3, pawn + AH_PAWN_WM, &wm);
                if (wm) RpmRead64(drv.hDevice, procCR3, wm + AH_WM_CURWEAPON, &weapon);
                if (weapon) RpmRead64(drv.hDevice, procCR3, weapon + AH_WEAPON_ZOOMCOMP, &zc);
                float _dbg_scope_mag_raw = 0.0f, _dbg_aim_scale = 0.0f;
                uint8_t _dbg_zt = 0;
                float _dbg_target_scope_raw = 0.0f;
                if (zc) {
                    // v0.9.454c: ABIFINAL formula — read the LIVE mag from
                    // ZC+0x418 (found via memory-diff scanner, auto-1.0 in hip
                    // and N in ADS). ZC+0x578 (SDK ScopeMagnification) is the
                    // static target and is NOT usable as a hip/ADS toggle.
                    RpmReadVirtual(drv.hDevice, procCR3, zc + AH_ZC_ZOOMING_TYPE,      &_dbg_zt, 1);
                    RpmReadVirtual(drv.hDevice, procCR3, zc + AH_ZC_AIM_SCALE,         &_dbg_aim_scale, 4);
                    RpmReadVirtual(drv.hDevice, procCR3, zc + AH_ZC_LIVE_SCOPE_MAG,    &_dbg_scope_mag_raw, 4);
                    RpmReadVirtual(drv.hDevice, procCR3, zc + AH_ZC_TARGET_SCOPE_MAG,  &_dbg_target_scope_raw, 4);
                    if (_dbg_scope_mag_raw >= 0.9f && _dbg_scope_mag_raw < 30.0f) {
                        s.scope_mag = _dbg_scope_mag_raw;
                    } else {
                        s.scope_mag = 1.0f;
                    }
                }
#ifdef AH_DIAG
                {
                    static DWORD s_last_scope = 0;
                    static uint8_t s_last_zt = 0xFF;
                    static float s_last_target = -1.0f;
                    DWORD now = GetTickCount();
                    if (_dbg_zt != s_last_zt || _dbg_scope_mag_raw != s_last_target
                        || (now - s_last_scope) > 3000) {
                        ah_diag("SCOPE-MAG pawn=0x%llX zc=0x%llX zt=%u aim=%.3f live418=%.3f target578=%.3f eff=%.3f",
                                (unsigned long long)pawn, (unsigned long long)zc,
                                (unsigned)_dbg_zt, _dbg_aim_scale,
                                _dbg_scope_mag_raw, _dbg_target_scope_raw, s.scope_mag);
                        s_last_scope = now;
                        s_last_zt = _dbg_zt;
                        s_last_target = _dbg_scope_mag_raw;
                    }
                }
#endif
                // WeaponCameraComp — ADSSceneFOV (real scope render FOV) +
                // post-process magnifier (usually > lens mag; scope glass
                // renders world into smaller viewport → higher effective mag).
                // Without this the W2S uses lens 4x but game renders at 6x
                // effective → boxes drift when moving mouse in scope.
                //
                // v0.9.454: variable-zoom scope live step from CurrentMagnification
                // (int32, Net, RepNotify). Cycles as user scrolls 2/4/7x inside
                // ADS on a variable scope (Vortex Razor, Elcan, etc.). If the
                // value is > 1 we treat it as the live magnification directly.
                float _dbg_sight_mag = 0.0f;
                int32_t _dbg_sight_cur = 0;
                float _dbg_wpn_cc_mag = 0.0f;
                int32_t _dbg_wpn_cc_cur = 0;
                u64 _dbg_sight = 0, _dbg_sight_cc = 0;
                // Path A — mounted sight (variable-mag lives here):
                //   weapon.ZoomComp.LastSight -> Sight.InventoryCameraComp
                //     -> CurrentMagnification (Net, RepNotify int32)
                //     -> Magnification (float base)
                if (zc) {
                    RpmRead64(drv.hDevice, procCR3, zc + AH_ZC_LASTSIGHT, &_dbg_sight);
                }
                if (_dbg_sight) {
                    RpmRead64(drv.hDevice, procCR3, _dbg_sight + AH_WEAPON_CAMCOMP, &_dbg_sight_cc);
                }
                // v0.9.454 confirmed: ScopeMagnification @ ZC+0x578 IS the
                // authoritative live variable-zoom step (2.0 → 7.0 flips when
                // the user cycles zoom mid-ADS, verified in-raid). CamComp
                // fields (sight.Magnification, weapon.CamComp.Magnification)
                // hold the STATIC base (7.0 for a 2-7x scope regardless of
                // current step) — they must NOT overwrite the live value.
                // Kept only for scope_fov (ADSSceneFOV) which is still useful
                // for the equirect W2S path.
                if (_dbg_sight_cc) {
                    float adsfov = 0, mag = 0;
                    int32_t cur_mag = 0;
                    RpmReadVirtual(drv.hDevice, procCR3, _dbg_sight_cc + AH_CAMCOMP_ADS_SCENE_FOV, &adsfov, 4);
                    RpmReadVirtual(drv.hDevice, procCR3, _dbg_sight_cc + AH_CAMCOMP_MAGNIFICATION, &mag, 4);
                    RpmReadVirtual(drv.hDevice, procCR3, _dbg_sight_cc + AH_CAMCOMP_CURRENT_MAG,   &cur_mag, 4);
                    _dbg_sight_mag = mag;
                    _dbg_sight_cur = cur_mag;
                    if (adsfov > 5.0f && adsfov < 180.0f) s.scope_fov = adsfov;
                }
                if (weapon) {
                    u64 cc = 0;
                    RpmRead64(drv.hDevice, procCR3, weapon + AH_WEAPON_CAMCOMP, &cc);
                    if (cc) {
                        float adsfov = 0, mag = 0;
                        int32_t cur_mag = 0;
                        RpmReadVirtual(drv.hDevice, procCR3, cc + AH_CAMCOMP_ADS_SCENE_FOV, &adsfov, 4);
                        RpmReadVirtual(drv.hDevice, procCR3, cc + AH_CAMCOMP_MAGNIFICATION, &mag, 4);
                        RpmReadVirtual(drv.hDevice, procCR3, cc + AH_CAMCOMP_CURRENT_MAG,   &cur_mag, 4);
                        _dbg_wpn_cc_mag = mag;
                        _dbg_wpn_cc_cur = cur_mag;
                        if (adsfov > 5.0f && adsfov < 180.0f && s.scope_fov == 0.0f) s.scope_fov = adsfov;
                    }
                }
#ifdef AH_DIAG
                {
                    static DWORD s_last_camdiag = 0;
                    static int32_t s_last_sm = -1;
                    static int32_t s_last_wm = -1;
                    DWORD now = GetTickCount();
                    if (_dbg_sight_cur != s_last_sm || _dbg_wpn_cc_cur != s_last_wm
                        || (now - s_last_camdiag) > 3000) {
                        ah_diag("SCOPE-CAM sight=0x%llX cc=0x%llX sight[mag=%.3f cur=%d] wpn_cc[mag=%.3f cur=%d] eff=%.3f",
                                (unsigned long long)_dbg_sight,
                                (unsigned long long)_dbg_sight_cc,
                                _dbg_sight_mag, (int)_dbg_sight_cur,
                                _dbg_wpn_cc_mag, (int)_dbg_wpn_cc_cur,
                                s.scope_mag);
                        s_last_camdiag = now;
                        s_last_sm = _dbg_sight_cur;
                        s_last_wm = _dbg_wpn_cc_cur;
                    }
                }
#endif
                u64 mesh = 0, anim = 0;
                RpmRead64(drv.hDevice, procCR3, pawn + AH_PAWN_MESH, &mesh);
                if (mesh) RpmRead64(drv.hDevice, procCR3, mesh + AH_MESH_ANIM_INSTANCE, &anim);
                if (anim) {
                    float sf = 0;
                    RpmReadVirtual(drv.hDevice, procCR3,
                                   anim + 0x740 + 0xB34, &sf, 4);
                    if (sf > 5.0f && sf < 150.0f) s.scope_fov = sf;

                    // CurrentZoomingCameraOffset — cam eye shift the game
                    // applies during ADS. Local frame: X=fwd, Y=right, Z=up.
                    // Without it W2S runs from hipfire eye while game renders
                    // from scope eye → box drift when mouse moves in zoom.
                    float zo[3] = {0};
                    if (RpmReadVirtual(drv.hDevice, procCR3,
                                       anim + 0x740 + 0x123C, zo, 12)) {
                        if (zo[0] > -200.0f && zo[0] < 200.0f &&
                            zo[1] > -200.0f && zo[1] < 200.0f &&
                            zo[2] > -200.0f && zo[2] < 200.0f) {
                            s.zoom_off_x = zo[0];
                            s.zoom_off_y = zo[1];
                            s.zoom_off_z = zo[2];
                        }
                    }
                }
            }

            s.uagame_base = imageBase;

            // ── PlayerArray walk (humans in raid — teammates + PMCs) ──
            // OPTIMIZATION 1: batch-read pawn struct as 8KB buffer, extract
            //   all field ptrs from local buffer (was 5-6 RPMs per player).
            // OPTIMIZATION 2: throttle HP (every 2nd tick), armor (every 5th),
            //   weapon+ammo (every 3rd). All throttle flags declared above at
            //   outer scope so bot walker uses same s_tick_ctr.
            struct PawnCache {
                int   hp       = 100;   // sane default until first hp_tick
                int   helm     = -1;
                int   vest     = -1;
                float helm_dur = -1.0f;
                float vest_dur = -1.0f;
                u32   weapon_id = 0;
                i16   mag_cur   = -1;
                i16   mag_max   = -1;
            };
            static std::unordered_map<u64, PawnCache> g_pawn_cache;

            if (gs) {
                struct { u64 data; i32 num; i32 max; } arr = {0};
                if (RpmReadVirtual(drv.hDevice, procCR3, gs + AH_GS_PLAYERARRAY,
                                   &arr, sizeof(arr)) &&
                    arr.num > 0 && arr.data)
                {
                    // v0.9.453: `arr.num <= AH_MAX_ENT` upper gate removed — used
                    // to fail the whole `if` on 65+ player raids and blank all
                    // humans. Inner loop already clamps via `s.ent_n >= AH_MAX_ENT`
                    // break checks, so oversized PlayerArray now yields the first
                    // AH_MAX_ENT humans instead of zero.
                    // Key-driven ACE_CACHE scan: capture first 4 non-self
                    // enemy keys (algo != 0) and hunt the table via them.
                    if (g_ah_ace_cache_rva_override == 0) {
                        u32 keys[4] = {0};
                        int n_keys = 0;
                        for (i32 i = 0; i < arr.num && n_keys < 4; i++) {
                            u64 ps = 0;
                            if (!RpmRead64(drv.hDevice, procCR3, arr.data + (u64)i * 8, &ps) || !ps) continue;
                            u64 pw = 0;
                            RpmRead64(drv.hDevice, procCR3, ps + AH_PS_PAWN, &pw);
                            if (!pw || pw == pawn) continue;   // skip self
                            u64 rt = 0;
                            RpmRead64(drv.hDevice, procCR3, pw + AH_PAWN_ROOT, &rt);
                            if (!rt) continue;
                            u32 ctl_k = 0;
                            RpmReadVirtual(drv.hDevice, procCR3, rt + AH_ROOT_CTL, &ctl_k, 4);
                            u32 algo_k = ctl_k >> 29;
                            u32 key_k  = ctl_k & 0x1FFFFFFu;
                            if (algo_k == 0 || key_k == 0) continue;
                            keys[n_keys++] = key_k;
                        }
                        if (n_keys >= 2) {
                            ah_diag("ACE_CACHE SCAN begin with %d keys: 0x%X 0x%X 0x%X 0x%X",
                                    n_keys, keys[0], keys[1], keys[2], keys[3]);
                            u64 found = ah_scan_ace_cache(drv.hDevice, procCR3, imageBase,
                                                          AH_RVA_ACE_CACHE, keys, n_keys);
                            if (found) {
                                g_ah_ace_cache_rva_override = found;
                                ah_diag("ACE_CACHE SCAN found RVA=0x%llX (baseline 0x%llX, delta=%+lld)",
                                        (unsigned long long)found,
                                        (unsigned long long)AH_RVA_ACE_CACHE,
                                        (long long)((int64_t)found - (int64_t)AH_RVA_ACE_CACHE));
                            } else {
                                ah_diag("ACE_CACHE SCAN — no matching RVA in ±0x100000 range");
                            }
                        }
                    }
                    for (i32 i = 0; i < arr.num; i++) {
                        u64 ps = 0;
                        if (!RpmRead64(drv.hDevice, procCR3, arr.data + (u64)i * 8, &ps) || !ps) continue;
                        u64 e_pawn = 0;
                        RpmRead64(drv.hDevice, procCR3, ps + AH_PS_PAWN, &e_pawn);
                        if (!e_pawn) continue;

                        // BATCH: read pawn top 0x2000 in one RPM.
                        u8 pbuf[0x2000];
                        if (!RpmReadVirtual(drv.hDevice, procCR3, e_pawn, pbuf, sizeof(pbuf))) continue;
                        u64 e_root = *(const u64*)(pbuf + AH_PAWN_ROOT);
                        u64 e_caps = *(const u64*)(pbuf + AH_PAWN_CAPSULE);
                        u64 e_dc   = *(const u64*)(pbuf + AH_PAWN_DEATH_COMP);
                        u64 e_amgr = *(const u64*)(pbuf + AH_PAWN_ARMOR_MGR);

                        AH_ENT* e = &s.ents[s.ent_n];
                        e->pawn = e_pawn;
                        e->is_me = (e_pawn == pawn) ? 1 : 0;
                        if (e_root) {
                            float dx = 0, dy = 0, dz = 0;
                            BOOL dec_ok = AhAceDecrypt(drv.hDevice, procCR3, imageBase,
                                                       e_root, &dx, &dy, &dz);
#ifdef AH_DIAG
                            {
                                // Log first non-self enemy's decrypt every 3s
                                // to see if encrypted-path fail is bucket miss.
                                static DWORD s_last_enemy_diag = 0;
                                static u64   s_last_pawn = 0;
                                if (!e->is_me && (e_pawn != s_last_pawn ||
                                    (GetTickCount() - s_last_enemy_diag) > 3000)) {
                                    u32 ctl_e = 0;
                                    RpmReadVirtual(drv.hDevice, procCR3, e_root + AH_ROOT_CTL, &ctl_e, 4);
                                    ah_diag("ENEMY-DECRYPT pawn=0x%llX root=0x%llX ctl=0x%08X algo=%u key=0x%X ok=%d fail=%d out=(%.0f,%.0f,%.0f)",
                                            (unsigned long long)e_pawn,
                                            (unsigned long long)e_root,
                                            ctl_e, ctl_e >> 29, ctl_e & 0x1FFFFFF,
                                            (int)dec_ok, (int)AhAceLastFail(),
                                            dx, dy, dz);
                                    s_last_pawn = e_pawn;
                                    s_last_enemy_diag = GetTickCount();
                                }
                            }
#endif
                            struct PosCache { float x, y, z; DWORD stamp_ms; };
                            static std::unordered_map<u64, PosCache> g_pos_cache;
                            auto it = g_pos_cache.find(e_pawn);
                            if (dec_ok && ah_is_raid_loc(dx, dy, dz)) {
                                e->x = dx; e->y = dy; e->z = dz;
                                e->valid = 1;
                                g_pos_cache[e_pawn] = { dx, dy, dz, GetTickCount() };
                            } else if (it != g_pos_cache.end() &&
                                       (GetTickCount() - it->second.stamp_ms) < 3000) {
                                // Fallback: use last-known good pos for up to 3s
                                // after a decrypt fail. Prevents running players
                                // vanishing while ACE key rotates or algo changes.
                                e->x = it->second.x;
                                e->y = it->second.y;
                                e->z = it->second.z;
                                e->valid = 1;
                            } else {
                                e->valid = 0;
                            }
                            RpmReadVirtual(drv.hDevice, procCR3,
                                           e_root + AH_ROOT_ACTOR_YAW, &e->yaw, 4);
                        }
                        RpmReadVirtual(drv.hDevice, procCR3,
                                       ps + AH_PS_TEAMINDEX, &e->team, 4);
                        // FString name @ PS+0x3F8 = {ptr, count, max}
                        struct { u64 data; i32 num; i32 max; } ns = {0};
                        RpmReadVirtual(drv.hDevice, procCR3,
                                       ps + AH_PS_PLAYERNAME, &ns, sizeof(ns));
                        if (ns.data && ns.num > 0 && ns.num < 64) {
                            wchar_t wbuf[64] = {0};
                            u32 rd = (u32)ns.num;
                            if (rd > 31) rd = 31;
                            if (RpmReadVirtual(drv.hDevice, procCR3, ns.data, wbuf, rd * 2)) {
                                for (u32 k = 0; k < rd; k++) {
                                    wchar_t c = wbuf[k];
                                    e->name[k] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
                                }
                                e->name[rd] = 0;
                            }
                        }
                        // HP throttle — every 2nd tick actually reads; between
                        // ticks entity keeps cached value so bar doesn't blink.
                        auto& pc = g_pawn_cache[e_pawn];
                        float sum_cur = -1.0f;
                        if (hp_tick) {
                            u64 laset = ah_find_limb_aset(drv.hDevice, procCR3, e_pawn);
                            if (laset) {
                                u8 region[0x40];
                                if (RpmReadVirtual(drv.hDevice, procCR3,
                                                   laset + 0x48, region, sizeof(region))) {
                                    static const int CUR_O[7] = {0x00,0x08,0x10,0x18,0x20,0x28,0x30};
                                    static const int MAX_O[7] = {0x04,0x0C,0x14,0x1C,0x24,0x2C,0x34};
                                    float sc = 0, sm = 0;
                                    for (int k = 0; k < 7; k++) {
                                        float c = *(const float*)(region + CUR_O[k]);
                                        float m = *(const float*)(region + MAX_O[k]);
                                        if (c < 0 || c > 1000) c = 0;
                                        if (m < 0 || m > 1000) m = 0;
                                        if (c > m) c = m;
                                        sc += c; sm += m;
                                    }
                                    if (sm > 0.0f) { pc.hp = (int)(sc + 0.5f); sum_cur = sc; }
                                }
                            } else {
                                float hpf = *(const float*)(pbuf + AH_PAWN_HP_DIRECT);
                                if (hpf >= 0.0f && hpf < 1000.0f) { pc.hp = (int)hpf; sum_cur = hpf; }
                            }
                        }
                        e->hp = pc.hp;
                        // Dead state: DeathComponent.bIsDead read every tick
                        // (cheap: e_dc from batch buf; 1 RPM for byte).
                        if (e_dc) {
                            unsigned char is_dead = 0;
                            RpmReadVirtual(drv.hDevice, procCR3,
                                           e_dc + AH_DEATH_IS_DEAD, &is_dead, 1);
                            e->dead = is_dead ? 1 : 0;
                        }
                        // v0.9.454: freeze PMC corpse the first time we see it
                        // dead. Server strips this pawn from PlayerArray a few
                        // seconds later; without a persistent copy the corpse
                        // pops out of the snapshot mid-loot. Refresh stamp_ms
                        // each tick while pawn is still in PlayerArray so TTL
                        // starts counting from the LAST time we saw it (not the
                        // death moment) — a long-visible corpse gets a fresh 5
                        // min after server drop.
                        if (e->dead && e->valid && !e->is_me) {
                            AhPmcCorpse pc_frz{};
                            pc_frz.x = e->x; pc_frz.y = e->y; pc_frz.z = e->z;
                            pc_frz.cap_r  = e->cap_r;
                            pc_frz.cap_hh = e->cap_hh;
                            pc_frz.team   = e->team;
                            pc_frz.helm   = pc.helm;
                            pc_frz.vest   = pc.vest;
                            pc_frz.helm_dur = pc.helm_dur;
                            pc_frz.vest_dur = pc.vest_dur;
                            pc_frz.weapon_id = pc.weapon_id;
                            memcpy(pc_frz.name, e->name, sizeof(pc_frz.name));
                            pc_frz.stamp_ms = GetTickCount();
                            g_pmc_corpses[e_pawn] = pc_frz;
                        }
                        // Live capsule (every tick — pose changes fast).
                        e->cap_r  = 57.8f;
                        e->cap_hh = 88.0f;
                        if (e_caps) {
                            float r = 0, hh = 0;
                            RpmReadVirtual(drv.hDevice, procCR3,
                                           e_caps + AH_CAPSULE_RADIUS, &r, 4);
                            RpmReadVirtual(drv.hDevice, procCR3,
                                           e_caps + AH_CAPSULE_HALFHEIGHT, &hh, 4);
                            if (r  > 10.0f && r  < 200.0f) e->cap_r  = r;
                            if (hh > 20.0f && hh < 250.0f) e->cap_hh = hh;
                        }
                        // Armor throttle — every 5th tick. Armor doesn't change
                        // mid-fight (gear locked at raid start).
                        if (armor_tick && e_amgr) {
                            pc.helm = pc.vest = -1;
                            pc.helm_dur = pc.vest_dur = -1.0f;
                            struct { u64 data; i32 num; i32 max; } al = {0};
                            RpmReadVirtual(drv.hDevice, procCR3,
                                           e_amgr + 0x278, &al, sizeof(al));
                            if (al.data && al.num > 0 && al.num <= 16) {
                                for (i32 k = 0; k < al.num; k++) {
                                    u64 item = 0;
                                    RpmRead64(drv.hDevice, procCR3,
                                              al.data + (u64)k * 8, &item);
                                    if (!item) continue;
                                    u32 iid = 0;
                                    i32 tier = 0;
                                    RpmReadVirtual(drv.hDevice, procCR3,
                                                   item + AH_INV_ITEM_ID, &iid, 4);
                                    RpmReadVirtual(drv.hDevice, procCR3,
                                                   item + AH_INV_ARMOR_LVL, &tier, 4);
                                    if (tier < 1 || tier > 6) continue;
                                    int is_helm;
                                    u32 pref = iid / 10000u;
                                    if      (pref == 301060) is_helm = 1;
                                    else if (pref == 301040) is_helm = 0;
                                    else if (k == 0)         is_helm = 1;
                                    else if (k == 1)         is_helm = 0;
                                    else continue;
                                    float dur = -1.0f;
                                    u64 cdc = 0;
                                    RpmRead64(drv.hDevice, procCR3,
                                              item + AH_INV_COMMON_DATA, &cdc);
                                    if (cdc) {
                                        float d = 0.0f;
                                        RpmReadVirtual(drv.hDevice, procCR3,
                                                       cdc + 0x118, &d, 4);
                                        if (d >= 0.0f && d <= 5000.0f) dur = d / 10.0f;
                                    }
                                    if (is_helm && pc.helm < 0) { pc.helm = tier; pc.helm_dur = dur; }
                                    else if (!is_helm && pc.vest < 0) { pc.vest = tier; pc.vest_dur = dur; }
                                }
                            }
                        }
                        e->helm     = pc.helm;
                        e->vest     = pc.vest;
                        e->helm_dur = pc.helm_dur;
                        e->vest_dur = pc.vest_dur;
                        // Weapon + ammo throttle — every 3rd tick.
                        if (weapon_tick) {
                            ah_read_weapon(drv.hDevice, procCR3, e_pawn,
                                           &pc.weapon_id, &pc.mag_cur, &pc.mag_max);
                        }
                        e->weapon_id = pc.weapon_id;
                        e->mag_cur   = pc.mag_cur;
                        e->mag_max   = pc.mag_max;

                        // v0.9.457: freeze full snapshot for miss-tolerance
                        // re-emit next tick if PlayerArray flickers. Skip self
                        // (we always find self via GameInstance chain, no cache
                        // needed) and dead (that path goes through g_pmc_corpses).
                        if (!e->is_me && !e->dead && e->valid) {
                            AhHumanSnap& hs = g_human_snap[e_pawn];
                            hs.ent      = *e;
                            hs.stamp_ms = GetTickCount();
                            hs.miss     = 0;
                        }

                        s.ent_n++;
                        if (s.ent_n >= AH_MAX_ENT) break;
                    }
                }
            }
        }

        // ── LIVE-human snapshot re-emit (v0.9.457) ─────────────────────────
        // For every pawn in g_human_snap not represented in this tick's
        // snapshot AND within TTL AND under miss threshold: emit cached ent.
        // Bridges the 1-2 tick relevance-flicker windows that used to blank
        // 1-2 humans per tick on busy raids.
        if (s.ent_n < AH_MAX_ENT && !g_human_snap.empty()) {
            const DWORD   HUMAN_TTL_MS      = 3000;
            const uint8_t HUMAN_MISS_LIMIT  = 20;   // ~0.7 s at 30 Hz reader
            const DWORD   hs_now_ms         = GetTickCount();

            std::unordered_set<u64> hs_seen;
            hs_seen.reserve(s.ent_n);
            for (int i = 0; i < s.ent_n; i++) hs_seen.insert(s.ents[i].pawn);

            std::vector<u64> hs_evict;
            for (auto& kv : g_human_snap) {
                if (s.ent_n >= AH_MAX_ENT) break;
                u64 pp = kv.first;
                AhHumanSnap& hs = kv.second;
                if (hs_seen.count(pp)) { hs.miss = 0; continue; }
                // Corpses are handled by g_pmc_corpses — don't shadow.
                if (g_pmc_corpses.count(pp)) continue;
                hs.miss++;
                if (hs.miss >= HUMAN_MISS_LIMIT ||
                    (hs_now_ms - hs.stamp_ms) > HUMAN_TTL_MS) {
                    hs_evict.push_back(pp);
                    continue;
                }
                AH_ENT* pe = &s.ents[s.ent_n];
                *pe = hs.ent;
                s.ent_n++;
            }
            for (u64 pp : hs_evict) g_human_snap.erase(pp);
        }

        // ── PMC corpse persistence (v0.9.454) ──────────────────────────────
        // For every pawn in g_pmc_corpses that is NOT currently in the live
        // snapshot (i.e. server has stripped it from PlayerArray), emit the
        // frozen entry so the corpse keeps rendering. No TTL — corpses persist
        // until the reader is restarted (F10 reattach) or the raid ends. Between
        // raids the state gets reset via ah_reader_reset_state(), so PMC corpses
        // from the previous raid do not leak into the next.
        if (s.ent_n < AH_MAX_ENT && !g_pmc_corpses.empty()) {
            std::unordered_set<u64> pmc_present;
            pmc_present.reserve(s.ent_n);
            for (int i = 0; i < s.ent_n; i++) pmc_present.insert(s.ents[i].pawn);

            for (auto& kv : g_pmc_corpses) {
                if (s.ent_n >= AH_MAX_ENT) break;
                u64 pp = kv.first;
                const AhPmcCorpse& pcv = kv.second;
                if (pmc_present.count(pp)) continue;   // still in PlayerArray

                AH_ENT* pe = &s.ents[s.ent_n];
                memset(pe, 0, sizeof(*pe));
                pe->pawn   = pp;
                pe->x      = pcv.x;
                pe->y      = pcv.y;
                pe->z      = pcv.z;
                pe->yaw    = 0.0f;
                pe->team   = pcv.team;
                pe->hp     = 0;
                pe->valid  = 1;
                pe->is_bot = 0;
                pe->is_me  = 0;
                pe->dead   = 1;
                pe->helm   = pcv.helm;
                pe->vest   = pcv.vest;
                pe->helm_dur = pcv.helm_dur;
                pe->vest_dur = pcv.vest_dur;
                pe->cap_r  = pcv.cap_r  > 0 ? pcv.cap_r  : 57.8f;
                pe->cap_hh = pcv.cap_hh > 0 ? pcv.cap_hh : 88.0f;
                pe->weapon_id = pcv.weapon_id;
                pe->mag_cur = -1;
                pe->mag_max = -1;
                memcpy(pe->name, pcv.name, sizeof(pe->name));
                s.ent_n++;
            }
        }

        // ── Bot update — MINIMUM: pos + yaw + cap (cached). No HP/dead/armor.
        // Per-bot per-tick: root ptr + ACE (~4) + yaw + cached-cap = ~6 RPM.
        if (procCR3 && imageBase && s.ent_n < AH_MAX_ENT) {
            std::unordered_set<u64> present;
            for (int i = 0; i < s.ent_n; i++) present.insert(s.ents[i].pawn);

            // v0.9.456: DeltaHack-style miss-tolerance. Prior code evicted a
            // bot from g_known_bots on the FIRST transient RPM fail (bad
            // slot, brief pawn-respawn, sublevel unload). Rediscovery then
            // waited for the next full Level.Actors chunk cycle (~0.5–1.5 s),
            // causing visible flicker of "some bots not loading". Now we:
            //   • cache last-known (x,y,z,yaw) per bot pawn on every success
            //   • on read fail, increment miss counter; if counter < LIMIT
            //     AND cached pos is younger than TTL → emit cached, no evict
            //   • evict only after LIMIT consecutive misses (≈1 s @30 Hz)
            static std::unordered_map<u64, uint8_t> g_bot_miss;
            struct BotPosSnap { float x, y, z, yaw; DWORD stamp_ms; };
            static std::unordered_map<u64, BotPosSnap> g_bot_pos_cache;
            // v0.9.458: tolerance dialed back. 30 misses + 5s TTL kept the
            // bot set full of stragglers, ballooning per-tick RPM budget
            // for the bot walker. 8 misses (~160 ms @50 Hz reader) + 2 s
            // TTL still bridges brief transient RPM slot conflicts but
            // evicts dead pawns fast enough to keep g_known_bots lean.
            const uint8_t BOT_MISS_LIMIT = 8;
            const DWORD   BOT_POS_TTL_MS = 2000;
            const DWORD   bot_now_ms = GetTickCount();

            std::vector<u64> to_evict;
            for (u64 bp : g_known_bots) {
                if (s.ent_n >= AH_MAX_ENT) break;
                if (present.count(bp)) continue;   // dedupe: humans go via PlayerArray

                // If already dead (in corpse map) — publish from corpse and
                // SKIP live read. Frozen pos per user spec.
                auto corp_it = g_bot_corpses.find(bp);
                if (corp_it != g_bot_corpses.end()) {
                    AH_ENT* be = &s.ents[s.ent_n];
                    be->pawn = bp;
                    be->x = corp_it->second.x;
                    be->y = corp_it->second.y;
                    be->z = corp_it->second.z;
                    be->cap_r  = corp_it->second.cap_r;
                    be->cap_hh = corp_it->second.cap_hh;
                    be->valid  = 1;
                    be->is_bot = 1;
                    be->dead   = 1;
                    be->hp     = 0;
                    be->team   = -1;
                    be->yaw    = 0;
                    be->name[0] = 0;
                    present.insert(bp);
                    s.ent_n++;
                    continue;
                }

                // v0.9.456: read live pos; on any transient fail fall back to
                // cached last-known pos (still counts as miss, evicts after
                // BOT_MISS_LIMIT consecutive fails).
                u64 broot = 0;
                RpmRead64(drv.hDevice, procCR3, bp + AH_PAWN_ROOT, &broot);
                float bxyz[3] = {0};
                bool  live_pos_ok = false;
                if (broot &&
                    RpmReadVirtual(drv.hDevice, procCR3,
                                   broot + AH_ROOT_LOC, bxyz, 12) &&
                    ah_is_raid_loc(bxyz[0], bxyz[1], bxyz[2])) {
                    live_pos_ok = true;
                }
                float bx, by, bz, byaw = 0.0f;
                if (live_pos_ok) {
                    bx = bxyz[0]; by = bxyz[1]; bz = bxyz[2];
                    RpmReadVirtual(drv.hDevice, procCR3,
                                   broot + AH_ROOT_ACTOR_YAW, &byaw, 4);
                    g_bot_miss[bp] = 0;
                    g_bot_pos_cache[bp] = { bx, by, bz, byaw, bot_now_ms };
                } else {
                    // Cache fallback path — keeps bot visible through
                    // transient RPM misses instead of flicker-vanishing.
                    uint8_t m = ++g_bot_miss[bp];
                    auto cit = g_bot_pos_cache.find(bp);
                    bool cache_fresh = (cit != g_bot_pos_cache.end()) &&
                                       (bot_now_ms - cit->second.stamp_ms) < BOT_POS_TTL_MS;
                    if (m >= BOT_MISS_LIMIT || !cache_fresh) {
                        to_evict.push_back(bp);
                        continue;
                    }
                    bx = cit->second.x; by = cit->second.y;
                    bz = cit->second.z; byaw = cit->second.yaw;
                }

                AH_ENT* be = &s.ents[s.ent_n];
                be->pawn   = bp;
                be->x = bx; be->y = by; be->z = bz;
                be->valid  = 1;
                be->is_bot = 1;
                be->team   = -1;
                be->hp     = 0;
                be->dead   = 0;
                be->name[0] = 0;
                be->yaw    = byaw;

                // Weapon + ammo (reuse human weapon_tick throttle).
                // FAKE-BOT FILTER: real bots hold a weapon; fakes/decoys have
                // no WM chain (iid=0). Skip publishing after cache confirmed.
                static std::unordered_map<u64, std::tuple<u32,i16,i16,u8>> g_bot_weap;
                // tuple: wid, mag_cur, mag_max, confirmed_zero_ticks
                if (weapon_tick) {
                    u32 wid = 0; i16 mc = -1, mm = -1;
                    ah_read_weapon(drv.hDevice, procCR3, bp, &wid, &mc, &mm);
                    auto& t = g_bot_weap[bp];
                    if (wid == 0) std::get<3>(t) = std::min<u8>(255, (u8)(std::get<3>(t) + 1));
                    else          std::get<3>(t) = 0;
                    std::get<0>(t) = wid;
                    std::get<1>(t) = mc;
                    std::get<2>(t) = mm;
                }
                auto wit = g_bot_weap.find(bp);
                if (wit != g_bot_weap.end()) {
                    // 3+ consecutive zero-weapon confirmations (~150ms) = fake
                    // bot. Skip publishing — slot stays reserved for next bot,
                    // s.ent_n NOT incremented, so it overwrites naturally.
                    if (std::get<3>(wit->second) >= 3) continue;
                    be->weapon_id = std::get<0>(wit->second);
                    be->mag_cur   = std::get<1>(wit->second);
                    be->mag_max   = std::get<2>(wit->second);
                } else {
                    be->weapon_id = 0; be->mag_cur = -1; be->mag_max = -1;
                }

                // Cap: cached during shape probe (see g_bot_cap map below).
                auto cit = g_bot_cap.find(bp);
                if (cit != g_bot_cap.end()) {
                    be->cap_r  = cit->second.first;
                    be->cap_hh = cit->second.second;
                } else {
                    be->cap_r = 57.8f; be->cap_hh = 88.0f;
                }

                // HP — try 7-limb ASC first (works for PMC-shaped AI). If not
                // present (simple mob bots), default to 100 so entity renders
                // alive; DeathComponent decides real death below.
                be->hp = 100;
                {
                    u64 laset = ah_find_limb_aset(drv.hDevice, procCR3, bp);
                    if (laset) {
                        u8 region[0x40];
                        if (RpmReadVirtual(drv.hDevice, procCR3,
                                           laset + 0x48, region, sizeof(region))) {
                            static const int CUR_O[7] = {0x00,0x08,0x10,0x18,0x20,0x28,0x30};
                            float sc = 0;
                            for (int k = 0; k < 7; k++) {
                                float c = *(const float*)(region + CUR_O[k]);
                                if (c < 0 || c > 1000) c = 0;
                                sc += c;
                            }
                            if (sc > 0) be->hp = (int)(sc + 0.5f);
                        }
                    }
                }
                {
                    u64 dc = 0;
                    RpmRead64(drv.hDevice, procCR3, bp + AH_PAWN_DEATH_COMP, &dc);
                    if (dc) {
                        unsigned char is_dead = 0;
                        RpmReadVirtual(drv.hDevice, procCR3,
                                       dc + AH_DEATH_IS_DEAD, &is_dead, 1);
                        if (is_dead) {
                            be->dead = 1;
                            // Freeze corpse pos ONCE at real death moment.
                            AhCorpse c{bx, by, bz, be->cap_r, be->cap_hh, GetTickCount()};
                            g_bot_corpses[bp] = c;
                        }
                    }
                }

                present.insert(bp);
                s.ent_n++;
            }
            for (u64 bp : to_evict) {
                g_known_bots.erase(bp);
                g_bot_miss.erase(bp);
                g_bot_pos_cache.erase(bp);
                // Keep g_bot_cap so a still-cached corpse renders with correct size.
                // Only wipe cap when corpse is also gone.
                if (!g_bot_corpses.count(bp)) g_bot_cap.erase(bp);
            }

            // Publish orphan corpses (bot pawn destroyed but we saved deathpos).
            // Skip any bp still in s.ents (live-dead entry above already covered).
            const DWORD now_ms = GetTickCount();
            const DWORD CORPSE_TTL_MS = 300000;   // 5 min
            std::vector<u64> corpse_expired;
            for (auto& kv : g_bot_corpses) {
                if (s.ent_n >= AH_MAX_ENT) break;
                u64 bp = kv.first;
                if (now_ms - kv.second.stamp_ms > CORPSE_TTL_MS) { corpse_expired.push_back(bp); continue; }
                if (present.count(bp)) continue;   // already emitted this tick

                AH_ENT* be = &s.ents[s.ent_n];
                be->pawn = bp;
                be->x = kv.second.x; be->y = kv.second.y; be->z = kv.second.z;
                be->cap_r  = kv.second.cap_r;
                be->cap_hh = kv.second.cap_hh;
                be->valid  = 1;
                be->is_bot = 1;
                be->dead   = 1;
                be->hp     = 0;
                be->team   = -1;
                be->yaw    = 0;
                be->name[0] = 0;
                s.ent_n++;
            }
            for (u64 bp : corpse_expired) g_bot_corpses.erase(bp);
        }

        // ── Loot scan — INTERLEAVED chunks of LOOT_CHUNK actors per tick.
        // When cursor == total, we re-collect actor pointers and start over.
        //
        // v0.9.460: throttle to every 2nd tick. Chunk work (vt reads, iid
        // rejects, per-hit decrypt) is the second-biggest RPM consumer after
        // PlayerArray. Skipping every other tick halves that with no visible
        // impact — full cycle grows from ~1 s to ~2 s, which is still well
        // inside the human noticeability floor for static ground loot.
        if (procCR3 && imageBase && (s_tick_ctr & 1) == 0) {
            // (Re)collect actor pointers when the scan wraps.
            if (scan_cursor >= scan_total_actors) {
                scan_total_actors = 0;
                scan_out_n = 0;
                u64 gworld_l = 0;
                RpmRead64(drv.hDevice, procCR3, imageBase + gworld_rva_live, &gworld_l);
                auto grab_level = [&](u64 level) {
                    if (!level || scan_total_actors >= 4096) return;   // v0.9.458: reverted to 4096
                    u64 la_ptr = 0; u32 la_n = 0;
                    RpmRead64(drv.hDevice, procCR3, level + AH_ULEVEL_ACTORS, &la_ptr);
                    RpmReadVirtual(drv.hDevice, procCR3, level + AH_ULEVEL_ACTORS + 8, &la_n, 4);
                    if (!la_ptr || la_n == 0 || la_n > 8000) return;
                    u32 room = 4096 - scan_total_actors;
                    u32 take = (la_n < room) ? la_n : room;
                    for (u32 c = 0; c < take; c += 512) {
                        u32 cc = take - c; if (cc > 512) cc = 512;
                        RpmReadVirtual(drv.hDevice, procCR3,
                                       la_ptr + (u64)c * 8,
                                       scan_actor_buf + scan_total_actors + c, cc * 8);
                    }
                    scan_total_actors += take;
                };
                if (gworld_l) {
                    u64 plevel = 0;
                    RpmRead64(drv.hDevice, procCR3, gworld_l + AH_UW_PERSISTENTLVL, &plevel);
                    grab_level(plevel);
                    u64 sl_ptr = 0; u32 sl_n = 0;
                    RpmRead64(drv.hDevice, procCR3, gworld_l + 0x1A8, &sl_ptr);
                    RpmReadVirtual(drv.hDevice, procCR3, gworld_l + 0x1B0, &sl_n, 4);
                    if (sl_ptr && sl_n > 0 && sl_n <= 128) {
                        u64 streams[128] = {0};
                        RpmReadVirtual(drv.hDevice, procCR3, sl_ptr, streams, sl_n * 8);
                        for (u32 s_i = 0; s_i < sl_n; s_i++) {
                            u64 sls = streams[s_i]; if (!sls) continue;
                            u64 loaded_level = 0;
                            RpmRead64(drv.hDevice, procCR3, sls + 0xA8, &loaded_level);
                            grab_level(loaded_level);
                        }
                    }
                }
                scan_cursor = 0;
                // v0.9.459: freeze self position for the whole cycle.
                scan_self_x = s.x;
                scan_self_y = s.y;
            }

            // Process this tick's slice.
            {
                int end = scan_cursor + LOOT_CHUNK;
                if (end > scan_total_actors) end = scan_total_actors;
                // v0.9.459: 60m radius kept — server-side replication only
                // reaches ~60m for valuable loot, so a bigger client radius
                // adds nothing but rejected chunk work.
                const float RADIUS_CM  = 6000.0f;
                const float RADIUS_CM2 = RADIUS_CM * RADIUS_CM;
                float self_x = scan_self_x, self_y = scan_self_y;

                // Bot discovery inline with the loot chunk — cheap: 1-2 RPMs
                // per candidate actor (vtable + shape). Persists into g_known_bots.
                for (int i = scan_cursor; i < end; i++) {
                    u64 a = scan_actor_buf[i];
                    if (!a) continue;
                    // Fast reject: known-bad vtable.
                    u64 vt = 0;
                    RpmRead64(drv.hDevice, procCR3, a, &vt);
                    if (!vt) continue;
                    auto vit = g_bot_vt_kind.find(vt);
                    int kind = (vit != g_bot_vt_kind.end()) ? vit->second : -1;
                    if (kind == 0) continue;   // cached non-pawn class
                    if (kind == -1) {
                        // Shape probe: mesh+cap+root heap, capsule human-scale.
                        u64 mesh = 0, cap = 0, root = 0;
                        RpmRead64(drv.hDevice, procCR3, a + AH_PAWN_MESH, &mesh);
                        RpmRead64(drv.hDevice, procCR3, a + AH_PAWN_CAPSULE, &cap);
                        RpmRead64(drv.hDevice, procCR3, a + AH_PAWN_ROOT, &root);
                        if (!mesh || !cap || !root) {
                            g_bot_vt_kind[vt] = 0;   // structurally not a pawn
                            continue;
                        }
                        float hh = 0, rr = 0;
                        RpmReadVirtual(drv.hDevice, procCR3, cap + AH_CAPSULE_HALFHEIGHT, &hh, 4);
                        RpmReadVirtual(drv.hDevice, procCR3, cap + AH_CAPSULE_RADIUS, &rr, 4);
                        if (hh < 20.0f || hh > 200.0f || rr < 5.0f || rr > 200.0f) continue;
                        g_bot_vt_kind[vt] = 1;   // cache as pawn-shaped
                    }
                    // Freeze cap on first insert (cheap 2 RPM, static thereafter).
                    if (!g_known_bots.count(a)) {
                        u64 cap = 0;
                        RpmRead64(drv.hDevice, procCR3, a + AH_PAWN_CAPSULE, &cap);
                        if (cap) {
                            float r=0, hh=0;
                            RpmReadVirtual(drv.hDevice, procCR3, cap + AH_CAPSULE_RADIUS,     &r,  4);
                            RpmReadVirtual(drv.hDevice, procCR3, cap + AH_CAPSULE_HALFHEIGHT, &hh, 4);
                            if (r > 10 && r < 200 && hh > 20 && hh < 250) g_bot_cap[a] = {r, hh};
                        }
                    }
                    g_known_bots.insert(a);
                }

                {
                    // v0.9.460: pos cache for loot actors, TTL 500 ms. Loot
                    // is stationary in ABI raids (dropped bags don't move),
                    // so re-decrypting a known actor every full pass is pure
                    // waste. On a 60-item scene with ~5 hits per chunk this
                    // trims 25 RPM/tick average from the loot walker without
                    // any change to visible freshness.
                    static std::unordered_map<u64, std::tuple<float,float,float,DWORD>> g_loot_pos_cache;
                    const DWORD LOOT_POS_TTL_MS = 500;
                    const DWORD loot_now_ms = GetTickCount();

                    for (int i = scan_cursor; i < end && scan_out_n < AH_MAX_LOOT; i++) {
                        u64 a = scan_actor_buf[i];
                        if (!a) continue;
                        u32 iid = 0;
                        RpmReadVirtual(drv.hDevice, procCR3, a + AH_INV_ITEM_ID, &iid, 4);
                        if (iid < 100000u || iid > 900000000u) continue;
                        u64 cd = 0;
                        RpmRead64(drv.hDevice, procCR3, a + AH_INV_COMMON_DATA, &cd);
                        if (!cd) continue;
                        // Skip if owner is a character (item in someone's inventory).
                        u64 owner = 0;
                        RpmRead64(drv.hDevice, procCR3, a + 0x118, &owner);
                        if (owner) {
                            u64 dc = 0, wm = 0;
                            RpmRead64(drv.hDevice, procCR3, owner + AH_PAWN_DEATH_COMP, &dc);
                            RpmRead64(drv.hDevice, procCR3, owner + AH_PAWN_WM, &wm);
                            if (dc || wm) continue;
                        }
                        u32 price = 0;
                        RpmReadVirtual(drv.hDevice, procCR3, cd + AH_CD_PRICE, &price, 4);
                        if (price == 0u || price >= 100000000u) continue;
                        i32 rar = -1;
                        RpmReadVirtual(drv.hDevice, procCR3, cd + AH_CD_RARITY, &rar, 4);

                        // v0.9.460: pos cache hit — skip the decrypt entirely.
                        float lx = 0, ly = 0, lz = 0;
                        auto lit = g_loot_pos_cache.find(a);
                        bool cache_hit = (lit != g_loot_pos_cache.end()) &&
                                         (loot_now_ms - std::get<3>(lit->second)) < LOOT_POS_TTL_MS;
                        if (cache_hit) {
                            lx = std::get<0>(lit->second);
                            ly = std::get<1>(lit->second);
                            lz = std::get<2>(lit->second);
                        } else {
                            u64 root = 0;
                            RpmRead64(drv.hDevice, procCR3, a + AH_PAWN_ROOT, &root);
                            if (!root) continue;
                            BOOL pos_ok = AhAceDecrypt(drv.hDevice, procCR3, imageBase, root,
                                                       &lx, &ly, &lz);
                            if (!pos_ok || !ah_is_raid_loc(lx, ly, lz)) {
                                float raw[3] = {0};
                                if (RpmReadVirtual(drv.hDevice, procCR3, root + 0x150, raw, 12)
                                    && ah_is_raid_loc(raw[0], raw[1], raw[2])) {
                                    lx = raw[0]; ly = raw[1]; lz = raw[2];
                                } else continue;
                            }
                            g_loot_pos_cache[a] = { lx, ly, lz, loot_now_ms };
                        }
                        // Radius gate — drop anything beyond 60m from self.
                        float dxr = lx - self_x, dyr = ly - self_y;
                        if (dxr*dxr + dyr*dyr > RADIUS_CM2) continue;

                        AH_LOOT* L = &scan_out[scan_out_n++];
                        L->a = a; L->x = lx; L->y = ly; L->z = lz;
                        L->id = iid; L->price = price;
                        L->rarity = (rar >= 0 && rar <= 6) ? rar : -1;
                        const char* nm = abi::items::lookup(iid);
                        if (nm) {
                            size_t sl = strlen(nm);
                            if (sl > sizeof(L->name) - 1) sl = sizeof(L->name) - 1;
                            memcpy(L->name, nm, sl);
                            L->name[sl] = 0;
                        } else {
                            L->name[0] = 0;
                        }
                    }
                }
                scan_cursor = end;
                // Full-pass complete → swap into cached (visible) buffer.
                if (scan_cursor >= scan_total_actors && scan_total_actors > 0) {
                    cached_loot_n = scan_out_n;
                    memcpy(cached_loot, scan_out, sizeof(AH_LOOT) * scan_out_n);
                    cached_loot_gen++;
                }
            }
        }

        // Merge cached loot into snap (1Hz refresh; interim ticks reuse).
        s.loot_n = cached_loot_n;
        s.loot_gen = cached_loot_gen;
        if (cached_loot_n > 0) {
            memcpy(s.loot, cached_loot, sizeof(AH_LOOT) * cached_loot_n);
        }

        publish(s);
        {
            DWORD now2 = GetTickCount();
            if (s.ent_n != last_ent_n || (now2 - last_diag_ms) > 5000) {
                ah_diag("tick ent_n=%d loot_n=%d cam=(%.0f,%.0f,%.0f) procCR3=%s img=0x%llX",
                        s.ent_n, s.loot_n, s.x, s.y, s.z,
                        procCR3 ? "OK" : "NULL",
                        (unsigned long long)imageBase);
                last_diag_ms = now2;
                last_ent_n = s.ent_n;
            }
            // v1.0.24: HEALTH beacon — every 3 seconds, unconditional. Lets
            // us diagnose "overlay UI alive, reader silently dead / stalled"
            // reports. The values here are enough to spot:
            //   * reader loop stuck (Hz=0 or crashed → line stops appearing)
            //   * provider handle went stale (procCR3 valid but RPMs all
            //     return 0 → we see it in ent_n=0 with attached_pid non-zero)
            //   * GAME_GONE (imageBase == 0 for extended period)
            //   * roomid=0 (out of raid — expected when in menu)
            static DWORD s_health_last_ms = 0;
            if ((now2 - s_health_last_ms) >= 3000) {
                s_health_last_ms = now2;
                ah_diag("HEALTH state=%d hz=%.1f attached_pid=%llu procCR3=0x%llX "
                        "img=0x%llX gworld_seen=%d canary_seen=%d "
                        "ent=%d loot=%d roomid=0x%llX scene=%u in_raid=%d",
                        g_reader_state.load(),
                        g_reader_hz.load(),
                        (unsigned long long)attached_pid,
                        (unsigned long long)procCR3,
                        (unsigned long long)imageBase,
                        (int)gworld_seen, (int)canary_seen,
                        s.ent_n, s.loot_n,
                        (unsigned long long)s.roomid,
                        (unsigned)s.scene_type,
                        (s.roomid != 0)
                            || (s.scene_type == AH_SCENE_INBATTLE)
                            || (s.scene_type == AH_SCENE_SHOOTINGROOM));
            }
        }
        // Update reader-Hz gauge (rolling 500ms average).
        {
            uint32_t now = GetTickCount();
            uint32_t last = g_reader_last_ms.load();
            uint32_t ticks = g_reader_ticks.fetch_add(1) + 1;
            if (last == 0) { g_reader_last_ms.store(now); g_reader_ticks.store(0); }
            else if (now - last >= 500) {
                g_reader_hz.store(ticks * 1000.0f / (float)(now - last));
                g_reader_last_ms.store(now);
                g_reader_ticks.store(0);
            }
        }
        Sleep(5);    // ~200Hz publisher (timeBeginPeriod(1) makes this honest)
    }

    CloseHandle(dev);
    if (g_target_hproc) { CloseHandle(g_target_hproc); g_target_hproc = NULL; }
    timeEndPeriod(1);
    g_thread_alive.store(false);
}

extern "C" void ah_reader_start(void) {
    if (g_run.exchange(true)) return;
    std::thread(reader_body).detach();
}

extern "C" float ah_reader_hz(void) { return g_reader_hz.load(); }

extern "C" void ah_reader_stop(void) {
    g_run.store(false);
}

// v0.9.454: F10 reattach — synchronous restart of the reader thread. Signals
// stop, waits for the detached thread to actually exit, then re-launches.
// v0.9.455: reject re-entry while previous thread still alive. Previous
// implementation spawned anyway after 500 ms, so F10-spam produced 3+ parallel
// reader threads racing on the same kdu device handle → DhProviderSelect FAIL.
// Now waits 3 s and REFUSES to spawn a duplicate if the old thread hasn't
// unwound. User can press F10 again after the previous attempt finishes.
extern "C" void ah_reader_reattach(void) {
    static std::atomic<int> s_in_progress{0};
    int expected = 0;
    if (!s_in_progress.compare_exchange_strong(expected, 1)) {
        ah_diag("=== reader_reattach REJECTED — previous attach still in progress ===");
        return;
    }
    ah_diag("=== reader_reattach requested ===");
    g_run.store(false);
    for (int i = 0; i < 600 && g_thread_alive.load(); i++) Sleep(5);   // up to 3s
    if (g_thread_alive.load()) {
        ah_diag("reader_reattach: thread STILL alive after 3s — refusing to spawn duplicate");
        s_in_progress.store(0);
        return;
    }
    g_reader_hz.store(0.0f);
    g_reader_ticks.store(0);
    g_reader_last_ms.store(0);
    g_reader_state.store(AH_READER_INIT);
    if (!g_run.exchange(true)) {
        std::thread(reader_body).detach();
    }
    s_in_progress.store(0);
}
