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
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

extern "C" {
#include "../inc/dh_common.h"
#include "../inc/dh_scm.h"
#include "../inc/dh_provider.h"
#include "../inc/dh_rpm.h"
#include "../inc/ah_offsets.h"
#include "../inc/ah_ace.h"
}

#include "../inc/item_names.hpp"
#include "ah_reader_thread.h"

#include <unordered_map>
#include <unordered_set>

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
//   pawn.WM → curWeapon → AssembleComp → CachedMag → WCC → ContainList
// Returns TRUE if a valid weapon ItemID was read (fills iid_out).
// mag_cur/mag_max default to -1 on failure. Cheap when the pawn holds no
// weapon (returns after first null ptr — ~1-2 RPMs).
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

static std::atomic<bool>     g_run{false};
static std::mutex            g_snap_lock;
static AH_LIVE_SNAP          g_snap{};
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

static void reader_body(void) {
    // High-resolution timer — default Windows tick is 15.6ms, so Sleep(10)
    // actually sleeps ~15ms (=~64Hz not 100Hz). timeBeginPeriod(1) drops it
    // to 1ms so Sleep(N) is honest ±0.5ms.
    timeBeginPeriod(1);

    DH_DRIVER drv = {0};
    HANDLE dev = NULL; u32 flags = 0;
    const DH_PROVIDER* p = DhProviderSelect(&dev, &flags);
    if (!p || !dev) { DH_ERROR("reader: provider select failed"); timeEndPeriod(1); return; }
    drv.hDevice = dev;
    g_active_provider = p;
    DH_INFO("reader: provider kdu#%u %s", p->kdu_id, p->name);

    u64 sysCR3 = 0;
    if (!RpmFindSystemCR3(drv.hDevice, &sysCR3)) {
        DH_ERROR("reader: sysCR3 fail"); CloseHandle(dev); return;
    }

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
    const int LOOT_CHUNK = 64;   // actors per tick — tune for driver load
    int          cached_loot_n = 0;
    unsigned int cached_loot_gen = 0;
    AH_LOOT      cached_loot[AH_MAX_LOOT];
    memset(cached_loot, 0, sizeof(cached_loot));

    // Scan state carried across ticks.
    static u64 scan_actor_buf[4096];
    int  scan_total_actors = 0;   // valid entries in scan_actor_buf
    int  scan_cursor = 0;         // next index to process
    int  scan_out_n = 0;          // accumulator for this pass
    AH_LOOT scan_out[AH_MAX_LOOT];
    memset(scan_out, 0, sizeof(scan_out));

    while (g_run.load()) {
        DWORD now = GetTickCount();
        if (!procCR3 || (now - last_find) > 5000) {
            if (RpmFindProcess(drv.hDevice, sysCR3, AH_PROC_NAME, &procCR3, &eproc)) {
                u64 peb = 0;
                RpmRead64(drv.hDevice, sysCR3, eproc + g_eproc_peb_off, &peb);
                u64 sz = 0;
                RpmGetMainImageBase(drv.hDevice, procCR3, peb, &imageBase, &sz);
            }
            last_find = now;
        }

        AH_LIVE_SNAP s{};
        // Per-tick throttle flags — shared by PlayerArray walk + bot update.
        static uint32_t s_tick_ctr = 0;
        ++s_tick_ctr;
        const bool hp_tick     = (s_tick_ctr & 1) == 0;
        const bool armor_tick  = (s_tick_ctr % 5) == 0;
        const bool weapon_tick = (s_tick_ctr % 3) == 0;

        if (procCR3 && imageBase) {
            u64 gworld = 0;
            RpmRead64(drv.hDevice, procCR3, imageBase + AH_RVA_GWORLD, &gworld);
            u64 gi = 0, lp_arr = 0, lp0 = 0, pc = 0, pawn = 0, root = 0, pcm = 0;
            u64 gs = 0;
            if (gworld) {
                RpmRead64(drv.hDevice, procCR3, gworld + AH_UW_GAMEINSTANCE, &gi);
                RpmRead64(drv.hDevice, procCR3, gworld + AH_UW_GAMESTATE, &gs);
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
            // wm = pawn+0x1970, curWeapon = wm+0x1F8, ZoomComp = weapon+0xBE8
            // LIVE scope mag @ ZC+0x418. Anim proxy CurrentSightFov path:
            // mesh+0x778(anim) → +0x740(localProxy) → +0xB34 (float).
            if (pawn) {
                u64 wm = 0, weapon = 0, zc = 0;
                RpmRead64(drv.hDevice, procCR3, pawn + AH_PAWN_WM, &wm);
                if (wm) RpmRead64(drv.hDevice, procCR3, wm + AH_WM_CURWEAPON, &weapon);
                if (weapon) RpmRead64(drv.hDevice, procCR3, weapon + AH_WEAPON_ZOOMCOMP, &zc);
                if (zc) {
                    float sm = 0;
                    RpmReadVirtual(drv.hDevice, procCR3, zc + AH_ZC_LIVE_SCOPE_MAG, &sm, 4);
                    if (sm > 0.5f && sm < 20.0f) s.scope_mag = sm;
                }
                // WeaponCameraComp — ADSSceneFOV (real scope render FOV) +
                // post-process magnifier (usually > lens mag; scope glass
                // renders world into smaller viewport → higher effective mag).
                // Without this the W2S uses lens 4x but game renders at 6x
                // effective → boxes drift when moving mouse in scope.
                if (weapon) {
                    u64 cc = 0;
                    RpmRead64(drv.hDevice, procCR3, weapon + AH_WEAPON_CAMCOMP, &cc);
                    if (cc) {
                        float adsfov = 0, mag = 0;
                        RpmReadVirtual(drv.hDevice, procCR3, cc + AH_CAMCOMP_ADS_SCENE_FOV, &adsfov, 4);
                        RpmReadVirtual(drv.hDevice, procCR3, cc + AH_CAMCOMP_MAGNIFICATION, &mag, 4);
                        if (adsfov > 5.0f && adsfov < 180.0f) s.scope_fov = adsfov;
                        if (mag > 0.9f && mag < 30.0f && mag >= s.scope_mag) s.scope_mag = mag;
                    }
                }
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
                    arr.num > 0 && arr.num <= AH_MAX_ENT && arr.data)
                {
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
                            e->valid = (AhAceDecrypt(drv.hDevice, procCR3, imageBase,
                                                     e_root, &e->x, &e->y, &e->z) != 0);
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
                        s.ent_n++;
                        if (s.ent_n >= AH_MAX_ENT) break;
                    }
                }
            }
        }
        // ── Bot update — MINIMUM: pos + yaw + cap (cached). No HP/dead/armor.
        // Per-bot per-tick: root ptr + ACE (~4) + yaw + cached-cap = ~6 RPM.
        if (procCR3 && imageBase && s.ent_n < AH_MAX_ENT) {
            std::unordered_set<u64> present;
            for (int i = 0; i < s.ent_n; i++) present.insert(s.ents[i].pawn);

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

                u64 broot = 0;
                RpmRead64(drv.hDevice, procCR3, bp + AH_PAWN_ROOT, &broot);
                if (!broot) { to_evict.push_back(bp); continue; }

                // Plaintext direct-read — bot Root ACE algo = 0 always, skip
                // ctl+bucket walk. Saves 1 RPM/bot/tick.
                float bxyz[3] = {0};
                if (!RpmReadVirtual(drv.hDevice, procCR3,
                                    broot + AH_ROOT_LOC, bxyz, 12) ||
                    !ah_is_raid_loc(bxyz[0], bxyz[1], bxyz[2])) {
                    to_evict.push_back(bp); continue;
                }
                float bx = bxyz[0], by = bxyz[1], bz = bxyz[2];

                AH_ENT* be = &s.ents[s.ent_n];
                be->pawn   = bp;
                be->x = bx; be->y = by; be->z = bz;
                be->valid  = 1;
                be->is_bot = 1;
                be->team   = -1;
                be->hp     = 0;
                be->dead   = 0;
                be->name[0] = 0;

                RpmReadVirtual(drv.hDevice, procCR3, broot + AH_ROOT_ACTOR_YAW, &be->yaw, 4);

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
        if (procCR3 && imageBase) {
            // (Re)collect actor pointers when the scan wraps.
            if (scan_cursor >= scan_total_actors) {
                scan_total_actors = 0;
                scan_out_n = 0;
                u64 gworld_l = 0;
                RpmRead64(drv.hDevice, procCR3, imageBase + AH_RVA_GWORLD, &gworld_l);
                auto grab_level = [&](u64 level) {
                    if (!level || scan_total_actors >= 4096) return;
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
            }

            // Process this tick's slice.
            {
                int end = scan_cursor + LOOT_CHUNK;
                if (end > scan_total_actors) end = scan_total_actors;
                const float RADIUS_CM  = 6000.0f;
                const float RADIUS_CM2 = RADIUS_CM * RADIUS_CM;
                float self_x = s.x, self_y = s.y;

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
                        u64 root = 0;
                        RpmRead64(drv.hDevice, procCR3, a + AH_PAWN_ROOT, &root);
                        if (!root) continue;
                        float lx = 0, ly = 0, lz = 0;
                        BOOL pos_ok = AhAceDecrypt(drv.hDevice, procCR3, imageBase, root,
                                                   &lx, &ly, &lz);
                        if (!pos_ok || !ah_is_raid_loc(lx, ly, lz)) {
                            float raw[3] = {0};
                            if (RpmReadVirtual(drv.hDevice, procCR3, root + 0x150, raw, 12)
                                && ah_is_raid_loc(raw[0], raw[1], raw[2])) {
                                lx = raw[0]; ly = raw[1]; lz = raw[2];
                            } else continue;
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
    timeEndPeriod(1);
}

extern "C" void ah_reader_start(void) {
    if (g_run.exchange(true)) return;
    std::thread(reader_body).detach();
}

extern "C" float ah_reader_hz(void) { return g_reader_hz.load(); }

extern "C" void ah_reader_stop(void) {
    g_run.store(false);
}
