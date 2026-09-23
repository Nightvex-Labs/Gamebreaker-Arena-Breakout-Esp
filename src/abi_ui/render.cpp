#include "render.hpp"
#include "reader.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace abi {

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Direct positional read — the DMA-era client-side extrapolation is no longer
// needed on 1PC: bridge latency is ~50 μs, reader ticks at 60+ Hz, and any
// jitter is well below one render frame. Keeping the shim so all call sites
// stay stable.
struct PredPos { float x, y, z; };
static PredPos predict(const Entity& e) { return { e.x, e.y, e.z }; }

constexpr float UE_UNITS_PER_M = 100.0f;
constexpr float PI = 3.14159265358979323846f;

static inline float deg2rad(float d) { return d * PI / 180.0f; }

struct Mat3 { float m[3][3]; };

// v0.9.422 dev toggles — defined in reader.cpp (namespace abi), pushed here.
extern std::atomic<int> g_dev_dump_all_bones;
extern std::atomic<int> g_dev_force_yaw_only;
extern std::atomic<int> g_dev_show_bone_ids;

// v0.9.421: TEST panel writes here at the start of each render frame.
// world_to_screen reads it inside its static base_fov_cached branch.
static int g_test_fov_bias = 0;
static int g_test_barrel_k1 = 0;
static int g_test_use_equirect = 1;
static int g_test_fov_source = 0;
static int g_test_scope_scale = 100;   // v0.9.422: extra ADS scale (percent)
static float g_test_scope_mag = 1.0f;   // last-seen scope for equirect gate

// v0.9.438: ultrawide FOV correction globals.  Not static — world_to_screen
// declares them extern.  Written per-frame from cfg in the same push block.
int  g_fov_correction_pct = 100;
bool g_fov_auto_horplus   = true;

// SDK Hor+ rotation matrix from cam yaw/pitch/roll (degrees).
// Same formula as Python overlay — verified against multi-build dump.
static Mat3 cam_matrix(const Cam& c) {
    // v0.9.421: live roll from POV.Rotation.Roll. Previously hard-coded 0,
    // which flat-out ignored Q/E leans → ESP tilted opposite to model.
    float y = deg2rad(c.yaw), p = deg2rad(c.pitch), r = deg2rad(c.roll);
    float cy = cosf(y), sy = sinf(y);
    float cp = cosf(p), sp = sinf(p);
    float cr = cosf(r), sr = sinf(r);
    Mat3 m;
    m.m[0][0] =  cp * cy;
    m.m[0][1] =  cp * sy;
    m.m[0][2] =  sp;
    m.m[1][0] =  sr*sp*cy - cr*sy;
    m.m[1][1] =  sr*sp*sy + cr*cy;
    m.m[1][2] = -sr * cp;
    m.m[2][0] = -(cr*sp*cy + sr*sy);
    m.m[2][1] =  cy*sr - cr*sp*sy;
    m.m[2][2] =  cr * cp;
    return m;
}

struct ScreenPt { float sx, sy, depth; bool ok; };

static ScreenPt world_to_screen(float wx, float wy, float wz,
                                const Cam& cam, const Mat3& mat,
                                int sw, int sh) {
    float dx = wx - cam.x, dy = wy - cam.y, dz = wz - cam.z;
    float fwd = dx*mat.m[0][0] + dy*mat.m[0][1] + dz*mat.m[0][2];
    if (fwd < 1.0f) return {0,0,0,false};
    float right = dx*mat.m[1][0] + dy*mat.m[1][1] + dz*mat.m[1][2];
    float up    = dx*mat.m[2][0] + dy*mat.m[2][1] + dz*mat.m[2][2];

    // v0.9.421: PCM+0x2108 POV.FOV = 110 hip, 75 ADS 7x. Ratio 110/75 = 1.47x
    // is only PART of the 7x scope zoom (game shrinks world FOV by 1.47x, then
    // the scope-glass overlay applies the remaining 4.76x as post-process
    // render-to-texture resample). Correct effective_fov for W2S at 7x is
    // 75 / 4.76 ≈ 15.75° = HIP_POV_FOV / scope_mag = 110/7. Cache the hip
    // POV.FOV as base and divide by scope_mag in ADS — matches actual scope
    // pixel scale rather than the ambient world-camera FOV that POV.FOV
    // reports mid-ADS.
    float fov  = cam.fov > 0 ? cam.fov : 90.0f;
    static float base_fov_cached = 110.0f;
    float sm = (cam.scope_mag > 0.5f && cam.scope_mag < 20.0f) ? cam.scope_mag : 1.0f;
    if (sm <= 1.05f && fov > 60.0f && fov < 130.0f) {
        base_fov_cached = fov;
    }
    float effective_fov;
    // v0.9.422: extra scope scale factor from TEST panel (default 1.0).
    float scope_extra = (float)g_test_scope_scale * 0.01f;
    if (scope_extra < 0.5f) scope_extra = 0.5f;
    if (scope_extra > 5.0f) scope_extra = 5.0f;
    float sm_eff = (sm > 1.5f) ? sm * scope_extra : sm;
    if (sm > 1.5f) {
        // v0.9.421: FOV source picker for ADS — test which formula matches
        // game's actual scope render.  cam.scope_fov now holds ADSSceneFOV.
        switch (g_test_fov_source) {
            case 1:  // ADSSceneFOV direct (÷ extra scope scale only)
                effective_fov = (cam.scope_fov > 1.0f) ? (cam.scope_fov / scope_extra)
                                                       : ((float)base_fov_cached + (float)g_test_fov_bias) / sm_eff;
                break;
            case 2:  // ADSSceneFOV / scope_mag
                effective_fov = (cam.scope_fov > 1.0f) ? (cam.scope_fov / sm_eff)
                                                       : ((float)base_fov_cached + (float)g_test_fov_bias) / sm_eff;
                break;
            case 3:  // POV.FOV / scope_mag (legacy path)
                effective_fov = fov / sm_eff;
                break;
            default: // 0 = base_fov_cached / (scope_mag * extra)
                effective_fov = ((float)base_fov_cached + (float)g_test_fov_bias) / sm_eff;
                break;
        }
    } else {
        effective_fov = fov / sm;
    }
    // v0.9.438: aspect-ratio correction for ultrawide (21:9, 32:9).  Game reports
    // POV.FOV but Hor+ titles auto-widen horizontal FOV based on aspect while
    // preserving vertical FOV — our formula needs the ACTUAL horizontal FOV.
    // Apply if enabled globally (fov_auto_horplus_g) and aspect deviates >5% from 16:9.
    {
        const float base_aspect = 16.0f / 9.0f;
        const float aspect = (float)sw / (float)sh;
        extern bool g_fov_auto_horplus;
        if (g_fov_auto_horplus && fabsf(aspect - base_aspect) > 0.08f) {
            // vfov derived assuming reported fov IS the 16:9-baseline horizontal FOV
            float vfov_rad = 2.0f * atanf(tanf(deg2rad(effective_fov) * 0.5f) / base_aspect);
            // recompute actual horizontal FOV at user's real aspect
            float new_hfov_rad = 2.0f * atanf(tanf(vfov_rad * 0.5f) * aspect);
            effective_fov = new_hfov_rad * 180.0f / 3.14159265358979323846f;
        }
    }
    // v0.9.438: apply user-side fine-tune multiplier on top (empirical dial-in).
    extern int g_fov_correction_pct;
    if (g_fov_correction_pct != 100 && g_fov_correction_pct >= 50 && g_fov_correction_pct <= 200) {
        effective_fov *= (float)g_fov_correction_pct * 0.01f;
    }

    // v0.9.421 TEST: equirect (angle-based) projection instead of perspective
    // (tan-based).  If ABI applies barrel distortion inside scope glass, our
    // linear-perspective W2S overshoots at radius > 0.  Equirect maps angle
    // linearly to pixel — closer to fisheye-corrected output.  Toggle via
    // test_fov_bias sign: negative bias enables equirect (temporary hack).
    float thf  = tanf(deg2rad(effective_fov) * 0.5f);
    float cx   = sw * 0.5f, cy = sh * 0.5f;
    float scl  = cx / thf;
    // Default: equirect for ADS (sm > 1.5), perspective for hip.  Toggle can
    // override.  User confirmed equirect fixes scope-glass barrel distortion.
    bool use_equirect = (g_test_use_equirect != 0) && (sm > 1.5f);
    float px, py;
    if (use_equirect) {
        float half_fov_rad = deg2rad(effective_fov) * 0.5f;
        float ang_x = std::atan2(right, fwd);
        float ang_y = std::atan2(up,    fwd);
        float nx = ang_x / half_fov_rad;   // normalized [-1..+1] at edge
        float ny = ang_y / half_fov_rad;
        // Optional radial barrel k1 correction: r' = r * (1 + k1*r²)
        // k1 stored as int × 0.01 (slider ±50 = k1 ±0.5)
        float k1 = (float)g_test_barrel_k1 * 0.01f;
        if (k1 != 0.0f) {
            float r2 = nx*nx + ny*ny;
            float factor = 1.0f + k1 * r2;
            nx *= factor;
            ny *= factor;
        }
        px = cx + nx * cx;
        py = cy - ny * cx;
    } else {
        px = cx + (right/fwd)*scl;
        py = cy - (up/fwd)*scl;
    }
    return { px, py, fwd, true };
}

static ImU32 col_white   = IM_COL32(255, 255, 255, 255);
static ImU32 col_purple  = IM_COL32(200, 130, 255, 255);
static ImU32 col_green   = IM_COL32( 80, 255,  80, 255);
static ImU32 col_red     = IM_COL32(255,  60,  60, 255);
static ImU32 col_orange  = IM_COL32(255, 140,   0, 255);
static ImU32 col_grey    = IM_COL32( 80,  80,  80, 255);

// Map skeleton line endpoints to limb name (for HP color).
static const char* limb_of_line(const std::string& a, const std::string& b) {
    auto eq = [&](const char* x, const char* y) {
        return (a == x && b == y) || (a == y && b == x);
    };
    if (eq("head","neck") || eq("neck","upper_chest")) return "head";
    if (eq("upper_chest","mid_spine") || eq("mid_spine","upper_abdomen")) return "chest";
    if (eq("upper_abdomen","lower_spine") || eq("lower_spine","pelvis")) return "abdomen";
    if (eq("upper_chest","shoulder_L") || eq("shoulder_L","elbow_L") || eq("elbow_L","hand_L")) return "arm_L";
    if (eq("upper_chest","shoulder_R") || eq("shoulder_R","elbow_R") || eq("elbow_R","hand_R")) return "arm_R";
    if (eq("pelvis","knee_L") || eq("knee_L","foot_L")) return "leg_L";
    if (eq("pelvis","knee_R") || eq("knee_R","foot_R")) return "leg_R";
    return nullptr;
}

// Bone-limb HP ramp:
//   100%  = white (no damage cue)
//    90%  = dim orange (barely visible warning)
//    50-60% = full bright orange
//    40%  = pinkish red
//    20%..1% = hot bright red (glow-like pop)
//     0%  = black (limb destroyed)
static ImU32 hp_color(const Entity& e, const char* limb, ImU32 base) {
    if (!limb) return base;
    if (e.hp_limbs.empty()) return base;
    auto it = e.hp_limbs.find(limb);
    if (it == e.hp_limbs.end()) return IM_COL32(0, 0, 0, 255);
    if (it->second.base <= 0.0f) return base;
    float f = it->second.cur / it->second.base;
    if (f <= 0.001f) return IM_COL32(0,   0,   0,   255);   // 0% → black
    if (f >= 0.999f) return IM_COL32(255, 255, 255, 255);   // 100% → white

    struct Stop { float t; uint8_t r, g, b; };
    static const Stop stops[] = {
        {1.00f, 255, 255, 255},   // white
        {0.90f, 180,  70,   0},   // dim orange
        {0.50f, 255, 140,   0},   // bright orange
        {0.40f, 255,  70,  90},   // pink red
        {0.20f, 255,   0,   0},   // hot red
        {0.00f, 220,   0,   0},   // deep red (just before black)
    };
    for (int i = 0; i < 5; i++) {
        if (f <= stops[i].t && f >= stops[i + 1].t) {
            float span = stops[i].t - stops[i + 1].t;
            float k = span > 0 ? (f - stops[i + 1].t) / span : 0.0f;
            int r = (int)(stops[i + 1].r * (1 - k) + stops[i].r * k);
            int g = (int)(stops[i + 1].g * (1 - k) + stops[i].g * k);
            int b = (int)(stops[i + 1].b * (1 - k) + stops[i].b * k);
            return IM_COL32(r, g, b, 255);
        }
    }
    return IM_COL32(255, 255, 255, 255);
}

// Damage fraction 0..1 (0 at full HP, 1 at destroyed). Drives the halo.
static float hp_damage(const Entity& e, const char* limb) {
    if (!limb || e.hp_limbs.empty()) return 0.0f;
    auto it = e.hp_limbs.find(limb);
    if (it == e.hp_limbs.end()) return 1.0f;    // limb missing = full damage
    if (it->second.base <= 0.0f) return 0.0f;
    float f = it->second.cur / it->second.base;
    if (f < 0) f = 0; if (f > 1) f = 1;
    return 1.0f - f;
}

// Skeleton bone-pair lines (for plain non-glow path + limb HP color lookup)
static const std::pair<const char*, const char*> SKEL_LINES[] = {
    {"head","neck"}, {"neck","upper_chest"}, {"upper_chest","mid_spine"},
    {"mid_spine","upper_abdomen"}, {"upper_abdomen","lower_spine"},
    {"lower_spine","pelvis"},
    {"upper_chest","shoulder_L"}, {"shoulder_L","elbow_L"}, {"elbow_L","hand_L"},
    {"upper_chest","shoulder_R"}, {"shoulder_R","elbow_R"}, {"elbow_R","hand_R"},
    {"pelvis","knee_L"}, {"knee_L","foot_L"},
    {"pelvis","knee_R"}, {"knee_R","foot_R"},
};

// Continuous bone chains for smooth glow polylines (avoid joint-cap artifacts)
struct LimbChain { const char* limb; const char* bones[8]; int n; };
static const LimbChain SKEL_CHAINS[] = {
    {"head",    {"head","neck","upper_chest"},                         3},
    {"chest",   {"upper_chest","mid_spine","upper_abdomen"},            3},
    {"abdomen", {"upper_abdomen","lower_spine","pelvis"},               3},
    {"arm_L",   {"upper_chest","shoulder_L","elbow_L","hand_L"},        4},
    {"arm_R",   {"upper_chest","shoulder_R","elbow_R","hand_R"},        4},
    {"leg_L",   {"pelvis","knee_L","foot_L"},                           3},
    {"leg_R",   {"pelvis","knee_R","foot_R"},                           3},
};

void render_frame(const Snapshot* snap, const RenderConfig& cfg) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (!snap) {
        dl->AddText(ImVec2(20, 20), col_red, "memserver disconnected");
        return;
    }
    // v0.9.337: menu / lobby gate. Reader sets in_raid=false when cam or
    // "me" pawn coords are outside the raid envelope. Skip drawing entirely
    // — the preview mannequin + fake loadout entities that show up in the
    // main menu would otherwise get boxes / skeletons.
    if (!snap->in_raid) return;

    // v0.9.421 TEST: expose bias to W2S static base FOV path.
    g_test_fov_bias    = cfg.test_fov_bias;
    g_test_barrel_k1   = cfg.test_barrel_k1;
    g_test_use_equirect = cfg.test_use_equirect;
    g_test_fov_source   = cfg.test_fov_source;
    g_test_scope_scale  = cfg.test_scope_scale;
    // v0.9.438: ultrawide + user fine-tune push
    g_fov_correction_pct = cfg.fov_correction_pct;
    g_fov_auto_horplus   = cfg.fov_auto_horplus;
    // v0.9.422 dev: push reader-side toggles (definitions live in reader.cpp).
    g_dev_dump_all_bones.store(cfg.dev_dump_all_bones, std::memory_order_relaxed);
    g_dev_force_yaw_only.store(cfg.dev_force_yaw_only, std::memory_order_relaxed);
    g_dev_show_bone_ids.store(cfg.dev_show_bone_ids, std::memory_order_relaxed);

    Mat3 mat = cam_matrix(snap->cam);
    const auto& cam = snap->cam;

    // Find user's own team ID for teammate filtering
    int my_team = -1;
    for (const auto& e : snap->entities) {
        if (e.me) { my_team = e.team; break; }
    }

    // Prefire-grace per-entity state. Suppresses the marker for `prefire_grace_ms`
    // right after a pawn walks into view so the ESP can't feed the player a
    // sub-human reaction window. Legacy behaviour when the config's
    // prefire_grace_ms is 0 — first_seen just tracks visibility for the HUD.
    struct PfState { bool prev_visible{false}; std::chrono::steady_clock::time_point became_visible_t{}; };
    static std::unordered_map<uint64_t, PfState> pf_state;
    static auto pf_last_gc = std::chrono::steady_clock::now();
    auto pf_now = std::chrono::steady_clock::now();
    // GC entries not seen for a while so the map doesn't grow raid-over-raid.
    if (pf_now - pf_last_gc > std::chrono::seconds(60)) {
        std::unordered_set<uint64_t> live;
        for (const auto& e : snap->entities) live.insert(e.a);
        for (auto it = pf_state.begin(); it != pf_state.end();)
            it = live.count(it->first) ? std::next(it) : pf_state.erase(it);
        pf_last_gc = pf_now;
    }

    for (const auto& e : snap->entities) {
        if (e.me) continue;
        // (phantom filter dropped — was a DMA jitter-grace shim)
        // Teammates in same party — hide entirely (no box/skel/label/glow)
        if (!cfg.show_mates && my_team >= 0 && e.team == my_team) continue;
        bool is_pmc = e.cls.starts_with("PMC") || e.cls.starts_with("Player") || e.cls.starts_with("USER");
        bool is_bot = e.cls.starts_with("BOT");
        if (!(is_pmc || is_bot)) continue;
        // Corpse toggle (per-class)
        if (e.dead) {
            if (is_pmc && !cfg.show_corpse) continue;
            if (is_bot && !cfg.show_bot_corpse) continue;
        }
        // Show dead bots too — user wants full visibility for triangulation
        // if (is_bot && e.dead) continue;

        // v0.9.421: cam-snap auto-apply disabled (baseline).  TEST cam-offset
        // now applied in CAM-LOCAL space (forward / right / up basis) instead
        // of world XYZ.  At pitch=0 cam.up == world.up so behavior is
        // identical to the old world-Z shift; at any tilt the offset stays
        // perpendicular to the look direction — matches how the game's
        // weapon-eye lifts along the scope axis, not along absolute Z.
        Cam  cam_local = snap->cam;
        Mat3 mat_local = mat;
        {
            // Auto-apply CurrentZoomingCameraOffset in ADS. Fixes box drift
            // that appears when mouse moves in scope: game renders from scope
            // eye, we projected from hipfire eye. Only fold in when actually
            // ADS'd — offset floats near zero in hip but not always exactly 0.
            float sm = (snap->cam.scope_mag > 0.5f && snap->cam.scope_mag < 20.0f)
                       ? snap->cam.scope_mag : 1.0f;
            float fo = (float)cfg.test_cam_off_x;
            float ro = (float)cfg.test_cam_off_y;
            float uo = (float)cfg.test_cam_off_z;
            if (sm > 1.05f) {
                fo += snap->cam.zoom_offset_x;
                ro += snap->cam.zoom_offset_y;
                uo += snap->cam.zoom_offset_z;
            }
            if (fo || ro || uo) {
                cam_local.x += fo * mat.m[0][0] + ro * mat.m[1][0] + uo * mat.m[2][0];
                cam_local.y += fo * mat.m[0][1] + ro * mat.m[1][1] + uo * mat.m[2][1];
                cam_local.z += fo * mat.m[0][2] + ro * mat.m[1][2] + uo * mat.m[2][2];
            }
        }
        const auto& cam = cam_local;
        const auto& mat = mat_local;

        // Client-side prediction — extrapolate to current render time
        PredPos pp = predict(e);

        // Distance cull
        float dx = pp.x - cam.x, dy = pp.y - cam.y;
        float range = is_pmc ? cfg.pmc_range_m : cfg.bot_range_m;
        float dist_units = sqrtf(dx*dx + dy*dy);
        if (dist_units > range * UE_UNITS_PER_M) continue;

        // ── Prefire grace — skip / dim the marker when the target just
        // walked into view so we don't feed the player a superhuman-reaction
        // window. Only for live targets (not corpses) and, when configured,
        // only for PMCs (nobody's getting reported for bot kills).
        float pf_alpha = 1.0f;
        if (cfg.prefire_grace_ms > 0 && !e.dead
            && (!cfg.prefire_grace_pmc_only || is_pmc))
        {
            auto& st = pf_state[e.a];
            if (e.visible && !st.prev_visible) st.became_visible_t = pf_now;
            st.prev_visible = e.visible;
            if (e.visible) {
                auto ms_since = std::chrono::duration_cast<std::chrono::milliseconds>(
                    pf_now - st.became_visible_t).count();
                if (ms_since < cfg.prefire_grace_ms) {
                    if (!cfg.prefire_grace_fade) continue;  // hide entirely
                    pf_alpha = 0.20f;                        // 20 % ghost outline
                }
            }
        }

        // v0.9.337: per-element colours from RenderConfig swatches. Dead
        // grey overrides for all elements. Visible-check green ONLY
        // overrides the BOX per user request v0.9.463 — labels (name /
        // distance / skeleton) keep their configured colors regardless
        // of visibility state.
        auto class_col_dead = [&](ImU32 pmc, ImU32 bot) -> ImU32 {
            if (e.dead) return is_pmc ? cfg.col_corpses_pmc : cfg.col_corpses_bot;
            return is_pmc ? pmc : bot;
        };
        // Box gets the visible-check override in addition to dead handling.
        ImU32 base = e.dead
            ? (is_pmc ? cfg.col_corpses_pmc : cfg.col_corpses_bot)
            : ((cfg.visible_check_on && e.visible)
                 ? cfg.col_visible
                 : (is_pmc ? cfg.col_box_pmc : cfg.col_box_bot));
        // Labels — no visible-check tint.
        ImU32 col_name_e = class_col_dead(cfg.col_name_pmc,     cfg.col_name_bot);
        ImU32 col_dist_e = class_col_dead(cfg.col_distance_pmc, cfg.col_distance_bot);
        ImU32 col_skel_e = class_col_dead(cfg.col_skel_pmc,     cfg.col_skel_bot);
        // Fold the prefire-grace alpha into the base tint so every downstream
        // rect / text / line the loop draws dims uniformly.
        auto fold_alpha = [&](ImU32& c) {
            if (pf_alpha < 0.999f) {
                uint32_t a = (uint32_t)(((c >> 24) & 0xFF) * pf_alpha);
                c = (c & 0x00FFFFFFu) | (a << 24);
            }
        };
        fold_alpha(base);
        fold_alpha(col_name_e);
        fold_alpha(col_dist_e);
        fold_alpha(col_skel_e);

        // Box anchor — chest-ish height (predicted pos).
        // v2026-09-23 arenahack:
        //   * BOT pp.z (ACE algo=0, plaintext) — sits near capsule TOP;
        //     shift -0.85*cap_hh lands anchor on chest. Empirically correct.
        //   * HUMAN pp.z (ACE algo>0, encrypted bucket) — encoded reference
        //     point sits HIGHER than bot (head-ish level). Same shift left
        //     boxes ~42px too high; extra -0.30*cap_hh drops them onto chest.
        float ez_chest = pp.z;
        auto p = world_to_screen(pp.x, pp.y, ez_chest, cam, mat, cfg.screen_w, cfg.screen_h);
        if (!p.ok) continue;

        // Projected pixel dims — used for box AND label scaling.
        // v0.9.420: mirror world_to_screen formula — in ADS use fixed base
        // FOV 90 divided by scope_mag (POV.FOV is post-modifier and would
        // double-apply the zoom). In hip use POV.FOV as before.
        float scope = (cam.scope_mag > 0.5f && cam.scope_mag < 20.0f) ? cam.scope_mag : 1.0f;
        // v0.9.421: mirror w2s effective_fov — use cached HIP POV.FOV / scope
        // instead of 90/scope. See detailed math in world_to_screen above.
        static float box_base_fov = 110.0f;
        if (scope <= 1.05f && cam.fov > 60.0f && cam.fov < 130.0f) box_base_fov = cam.fov;
        float scope_extra_box = (float)cfg.test_scope_scale * 0.01f;
        if (scope_extra_box < 0.5f) scope_extra_box = 0.5f;
        if (scope_extra_box > 5.0f) scope_extra_box = 5.0f;
        float box_eff_fov = (scope > 1.5f)
            ? ((box_base_fov + (float)cfg.test_fov_bias) / (scope * scope_extra_box))
            : (cam.fov > 0 ? cam.fov : 90.0f);
        float thf = tanf(deg2rad(box_eff_fov) * 0.5f);
        float scale_factor = cfg.screen_w * 0.5f / thf;
        // v0.9.421: test pads (world cm) — top shrink + bottom shrink; box
        // center shifts by (bot_pad - top_pad)/2 to keep the ends aligned to
        // capsule minus pads.  test_box_shift_y adds an extra absolute nudge.
        float top_pad = (float)cfg.test_box_top_pad;
        float bot_pad = (float)cfg.test_box_bot_pad;
        float box_h = scale_factor * (e.cap_hh * 2.0f - top_pad + bot_pad) / p.depth;
        float box_w = scale_factor * (e.cap_r  * 2.0f) / p.depth;
        float box_center_y = p.sy + ((top_pad + bot_pad) * 0.5f + (float)cfg.test_box_shift_y)
                             * scale_factor / p.depth;

        // v0.9.422: BONE-ANCHORED BOX is DISABLED — the current fallback
        // BONE_TABLE contains approximate ids that project head/foot bones
        // to wrong screen coords, which would stretch/warp the box.  Revert
        // to pure capsule-based box (stable).  Re-enable this override only
        // once auto-classify (skeleton_classify.cpp) reliably returns valid
        // head/foot indices per pawn.
        //
        // The block still runs so find_pt lambda stays defined for the
        // downstream head-circle draw — but bot_y stays nullopt so box_h /
        // box_center_y keep their capsule-computed values.
        {
            const float bo_x = pp.x - e.bone_anchor_x;
            const float bo_y = pp.y - e.bone_anchor_y;
            auto find_pt = [&](const char* name) -> std::optional<ScreenPt> {
                auto it = e.bones.find(name);
                if (it == e.bones.end()) return std::nullopt;
                ScreenPt sp = world_to_screen(it->second.x + bo_x,
                                              it->second.y + bo_y,
                                              it->second.z,
                                              cam, mat, cfg.screen_w, cfg.screen_h);
                return sp.ok ? std::optional<ScreenPt>(sp) : std::nullopt;
            };
            auto head = find_pt("head");
            auto footL = find_pt("foot_L");
            auto footR = find_pt("foot_R");
            std::optional<float> bot_y;   // stays nullopt — capsule box wins
            if (head && bot_y) {
                float top_y = head->sy - 4.0f;   // 4px hair/helmet padding
                float bot_y_val = *bot_y + 2.0f;
                box_h = bot_y_val - top_y;
                box_center_y = (top_y + bot_y_val) * 0.5f;
                // recompute box_w scale slightly since bones give tighter fit
                box_w = scale_factor * (e.cap_r * 1.8f) / p.depth;
            }
        }

        // 2D / 3D Box — per-class toggle, plain crisp lines (no glow).
        int box_mode_this = is_pmc ? cfg.box_mode : cfg.box_mode_bot;
        bool box_class_on = (is_pmc && cfg.show_box_pmc) || (is_bot && cfg.show_box_bot);
        if (box_class_on && box_mode_this > 0) {
            if (box_h >= 6.0f) {
                if (box_mode_this == 2) {
                    float x0 = p.sx - box_w*0.5f, y0 = box_center_y - box_h*0.5f;
                    float x1 = p.sx + box_w*0.5f, y1 = box_center_y + box_h*0.5f;
                    if ((is_bot ? cfg.box_corners_bot : cfg.box_corners)) {
                        // Corner-bracket style: L-shapes at each corner.
                        // Bracket length ~22% of the box, min 4 px.
                        float bw = (x1 - x0) * 0.22f;
                        float bh = (y1 - y0) * 0.22f;
                        if (bw < 4.0f) bw = 4.0f;
                        if (bh < 4.0f) bh = 4.0f;
                        float thk = 1.6f;
                        // Shadow underlay for legibility on bright bg.
                        ImU32 shd = IM_COL32(0, 0, 0, 180);
                        auto seg = [&](float ax, float ay, float bx, float by) {
                            dl->AddLine(ImVec2(ax+0.5f, ay+0.5f), ImVec2(bx+0.5f, by+0.5f), shd, thk + 0.6f);
                            dl->AddLine(ImVec2(ax,      ay),      ImVec2(bx,      by),      base, thk);
                        };
                        // TL
                        seg(x0, y0, x0 + bw, y0);
                        seg(x0, y0, x0, y0 + bh);
                        // TR
                        seg(x1, y0, x1 - bw, y0);
                        seg(x1, y0, x1, y0 + bh);
                        // BL
                        seg(x0, y1, x0 + bw, y1);
                        seg(x0, y1, x0, y1 - bh);
                        // BR
                        seg(x1, y1, x1 - bw, y1);
                        seg(x1, y1, x1, y1 - bh);
                    } else {
                        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                                    base, 0.0f, 0, 1.5f);
                    }
                } else {  // 3D
                    float yaw = e.yaw.value_or(0.0f);
                    // Anchor the 3D cage on the SAME chest-height reference as
                    // the 2D box (ez_chest = pp.z - cap_hh*shift_factor). Both
                    // box modes need the same per-source correction, else 3D
                    // stays high for humans while 2D is fixed.
                    float anchor_z_3d = ez_chest;
                    auto corner = [&](float ang_off, float zoff) -> ScreenPt {
                        float ang = deg2rad(yaw + ang_off);
                        float cx = pp.x + cosf(ang) * e.cap_r;
                        float cyw= pp.y + sinf(ang) * e.cap_r;
                        return world_to_screen(cx, cyw, anchor_z_3d + zoff, cam, mat, cfg.screen_w, cfg.screen_h);
                    };
                    ScreenPt c[8];
                    const float offs[4] = { 45.0f, 135.0f, 225.0f, 315.0f };
                    float z_bot = -e.cap_hh, z_top = e.cap_hh;
                    for (int i=0;i<4;i++) c[i]   = corner(offs[i], z_bot);
                    for (int i=0;i<4;i++) c[i+4] = corner(offs[i], z_top);
                    int edges[12][2] = {
                        {0,1},{1,2},{2,3},{3,0},
                        {4,5},{5,6},{6,7},{7,4},
                        {0,4},{1,5},{2,6},{3,7}
                    };
                    if ((is_bot ? cfg.box_corners_bot : cfg.box_corners)) {
                        // Corner-bracket mode: for each edge draw two short
                        // stubs (~25% of edge length) growing from the two
                        // endpoints. 8 corners × 3 arms = 24 short segments,
                        // reads as a hologram-target frame.
                        ImU32 shd = IM_COL32(0, 0, 0, 180);
                        auto stub = [&](const ScreenPt& a, const ScreenPt& b, float t) {
                            if (!a.ok || !b.ok) return;
                            float ex = a.sx + (b.sx - a.sx) * t;
                            float ey = a.sy + (b.sy - a.sy) * t;
                            dl->AddLine(ImVec2(a.sx+0.5f, a.sy+0.5f),
                                        ImVec2(ex+0.5f,   ey+0.5f),   shd,  2.1f);
                            dl->AddLine(ImVec2(a.sx, a.sy),
                                        ImVec2(ex, ey), base, 1.5f);
                        };
                        for (auto& e2 : edges) {
                            stub(c[e2[0]], c[e2[1]], 0.28f);
                            stub(c[e2[1]], c[e2[0]], 0.28f);
                        }
                    } else {
                        for (auto& e2 : edges) {
                            if (c[e2[0]].ok && c[e2[1]].ok) {
                                dl->AddLine(
                                    ImVec2(c[e2[0]].sx, c[e2[0]].sy),
                                    ImVec2(c[e2[1]].sx, c[e2[1]].sy),
                                    base, 1.5f);
                            }
                        }
                    }
                }
            }
        }

        // Armor bar — vertical tier stripe just to the right of the 2D box
        // silhouette. Two stacked segments (top = helm, bottom = vest) with
        // a color per tier so the top labels can stay lean.
        // v0.9.337 semantic: 0=Off, 1=Text, 2=Bar. Off = neither text nor bar.
        bool armor_bar_on  = cfg.show_armor_master
                          && (cfg.armor_display == 2);
        // Distance clamp: drop only if the box is unreadably tiny (~1 px);
        // the old 18 px gate hid armor bar on distant enemies which the user
        // wants visible at all ranges the box itself renders.
        // v0.9.422: no armor range limit — no_distance_clamp path allows any
        // box size (was 4.0f, now 0 = draw at any distance).
        float armor_min_h = cfg.armor_bar_no_distance_clamp ? 0.0f : 18.0f;
        if (armor_bar_on && box_h >= armor_min_h) {
            auto tier_col = [](int t) -> ImU32 {
                switch (t) {
                    case 1: return IM_COL32(200, 200, 200, 255);   // grey
                    case 2: return IM_COL32(120, 220, 120, 255);   // green
                    case 3: return IM_COL32(120, 160, 255, 255);   // blue
                    case 4: return IM_COL32(200, 120, 255, 255);   // purple
                    case 5: return IM_COL32(255, 160,  80, 255);   // orange
                    case 6: return IM_COL32(255,  80,  80, 255);   // red
                    default: return IM_COL32(150, 150, 150, 255);
                }
            };
            float bx = p.sx + box_w * 0.5f + 4.0f;
            float by = box_center_y - box_h * 0.5f;
            float bw = std::max(4.0f, std::min(9.0f, box_h * 0.045f));
            float half = box_h * 0.5f - 2.0f;
            ImU32 shadow = IM_COL32(0, 0, 0, 200);
            ImFont* afont = ImGui::GetFont();
            // Larger legible font: min 16 px so H4/V4 read at any distance,
            // capped at 26 px so close-range boxes don't get engulfed.
            // User-facing multiplier lets fine-tuning without recompile.
            float afs_base = std::max(16.0f, std::min(26.0f, box_h * 0.11f));
            float afs = afs_base * cfg.armor_bar_font_scale;
            char tbuf[8];
            if (e.helm >= 0) {
                dl->AddRectFilled(ImVec2(bx - 0.5f, by - 0.5f),
                                  ImVec2(bx + bw + 0.5f, by + half + 0.5f), shadow);
                dl->AddRectFilled(ImVec2(bx, by),
                                  ImVec2(bx + bw, by + half), tier_col(e.helm));
                std::snprintf(tbuf, sizeof(tbuf), "H%d", e.helm);
                dl->AddText(afont, afs, ImVec2(bx + bw + 3, by), tier_col(e.helm), tbuf);
            }
            if (e.vest >= 0) {
                float vy = by + half + 2.0f;
                dl->AddRectFilled(ImVec2(bx - 0.5f, vy - 0.5f),
                                  ImVec2(bx + bw + 0.5f, vy + half + 0.5f), shadow);
                dl->AddRectFilled(ImVec2(bx, vy),
                                  ImVec2(bx + bw, vy + half), tier_col(e.vest));
                std::snprintf(tbuf, sizeof(tbuf), "V%d", e.vest);
                dl->AddText(afont, afs, ImVec2(bx + bw + 3, vy), tier_col(e.vest), tbuf);
            }
        }

        // Skeleton + HP coloring — gated by per-class skeleton range
        float dist_m_skel = dist_units / UE_UNITS_PER_M;
        float skel_range_class = is_pmc ? cfg.skeleton_range_m : cfg.bot_skeleton_range_m;
        bool skel_on = ((is_pmc && cfg.show_skeleton_pmc) || (is_bot && cfg.show_skeleton_bot))
                       && dist_m_skel <= skel_range_class;
        // v0.9.337: two guards against close-range skeleton mangling.
        //
        //   (a) DEAD entities carry a frozen pose from the moment the reader
        //       last captured bones — usually mid-walk/mid-fall, and the
        //       bone_anchor drift compensation then teleports that mid-motion
        //       pose to the corpse's death location. Result: contorted
        //       skeletons on 7m corpses (screenshot from operator). Just
        //       hide the skeleton on dead entities — box+name+dist are still
        //       drawn so you can spot the corpse for looting.
        //
        //   (b) STALE live entities: if the reader hasn't refreshed this
        //       entity's position within the last ~350ms, the pose we have
        //       is old, the predicted pp probably diverges from real,
        //       and (predicted - bone_anchor) offset warps knees, arms,
        //       spine independently — the "twisted skeleton at close range"
        //       glitch. Drop the skeleton until fresh data arrives.
        if (skel_on && e.dead) skel_on = false;
        if (skel_on && e.last_pos_t > 0.0) {
            using namespace std::chrono;
            double now_s = duration<double>(steady_clock::now().time_since_epoch()).count();
            double age = now_s - e.last_pos_t;
            // v0.9.422: relaxed 0.35→1.5s.  With ComponentToWorld matrix now
            // properly applied to bones, the "twisted skeleton at close range"
            // glitch this guard used to catch is fixed at source.  Old tight
            // window caused skeletons to flicker/freeze when reader missed a
            // tick or two under bridge contention at close range.
            if (age > 1.5) skel_on = false;
        }
        // v0.9.337: MOVEMENT gate. Bones are captured in world coords AT
        // bone_anchor_pos with the pawn's yaw at that moment baked in.
        // We translate them by (pp - bone_anchor) to move the skeleton
        // with the predicted position, but we DON'T re-rotate.
        //
        // v0.9.410 CLEANUP:
        //   * dropped the drift2 > 10000 kill-switch (users report skeleton
        //     disappears at close range when character is moving — this was
        //     the culprit; naive translation is fine visually up to 5-6m)
        //   * dropped the "collapsed pose < 80cm" guard (fired on legit
        //     crouches, revive stances, low-cover peeks)
        //   * dropped the foot-alignment override (it was pinning feet to
        //     pp.z - cap_hh, which in this game's crouch dropped the whole
        //     skeleton ~50px below the box because cap_hh doesn't shrink
        //     the way the v0.9.337 comment assumed; bones are stored in
        //     absolute world Z so no vertical re-shift is needed)
        if (skel_on && !e.bones.empty()) {
            // Horizontal shift only — bones stored in absolute world XY at
            // capture time, so slide them to the predicted position.
            const float bo_x = pp.x - e.bone_anchor_x;
            const float bo_y = pp.y - e.bone_anchor_y;
            // v2026-09-20: restore historical delta-Z shift (was in
            // .ui_backup_prev/render.cpp). Bones captured at bone_anchor_z;
            // shift by (pp.z - anchor_z) so skeleton tracks vertical
            // pelvis movement (jumps, crouches, terrain slope).
            const float bo_z = pp.z - e.bone_anchor_z;
            auto find_bone = [&](const char* name) -> std::optional<ImVec2> {
                auto it = e.bones.find(name);
                if (it == e.bones.end()) return std::nullopt;
                auto sp = world_to_screen(it->second.x + bo_x,
                                          it->second.y + bo_y,
                                          it->second.z + bo_z,
                                          cam, mat, cfg.screen_w, cfg.screen_h);
                if (!sp.ok) return std::nullopt;
                return ImVec2(sp.sx, sp.sy);
            };
            // Plain crisp lines only — glow path removed. Thickness scales
            // inversely with distance. v0.9.337: coefficient dropped from
            // 1.6 → 1.35 (~-16%) and floor 0.6 → 0.5 per operator request,
            // close-range bones were reading too chunky and covering armor
            // ID text underneath.
            // v0.9.410: another -12% pass — 1.35 → 1.18, floor 0.5 → 0.44
            // v0.9.422 polish: thinner lines matching Crooked-Arms reference.
            // Base 0.75 (was 1.18) with floor 0.35 (was 0.44).  Distance
            // falloff still applies but caps at ~1px at close range.
            float thk = 0.75f * std::min(1.0f, 25.0f / std::max(dist_m_skel, 1.0f));
            if (thk < 0.35f) thk = 0.35f;
            for (auto& [a, b] : SKEL_LINES) {
                auto pa = find_bone(a);
                auto pb = find_bone(b);
                if (!pa || !pb) continue;
                const char* limb = limb_of_line(a, b);
                ImU32 c = hp_color(e, limb, col_skel_e);

                // Halo underlay — wider, semi-transparent copy of the core
                // color. Alpha grows with damage so low-HP limbs bloom.
                float dmg = hp_damage(e, limb);
                if (dmg > 0.05f) {
                    ImU32 halo = (c & 0x00FFFFFFu) |
                                 ((uint32_t)(dmg * 140.0f) << 24);
                    dl->AddLine(*pa, *pb, halo, thk * (1.6f + dmg * 2.0f));
                }
                dl->AddLine(*pa, *pb, c, thk);
            }

            // Joint dots at key articulation points — cleaner readability.
            if (cfg.skeleton_joints) {
                // Only limb extremities — 8 dots total. Spine/shoulder dots
                // stacked visually and made the skeleton look busy at range.
                static const struct { const char* bone; const char* limb; } JOINTS[] = {
                    { "elbow_L", "arm_L" }, { "hand_L", "arm_L" },
                    { "elbow_R", "arm_R" }, { "hand_R", "arm_R" },
                    { "knee_L",  "leg_L" }, { "foot_L", "leg_L" },
                    { "knee_R",  "leg_R" }, { "foot_R", "leg_R" },
                };
                // v0.9.422: joint dots — decouple radius from line thickness
                // (thin lines look better, but dots need to stay visible).
                // r = 1.65px close, floors at 0.8px far.
                float r_dot_base = 1.18f * std::min(1.0f, 25.0f / std::max(dist_m_skel, 1.0f));
                if (r_dot_base < 0.5f) r_dot_base = 0.5f;
                float r = r_dot_base * 1.4f + 0.4f;
                for (auto& j : JOINTS) {
                    auto jp = find_bone(j.bone);
                    if (!jp) continue;
                    ImU32 jc = hp_color(e, j.limb, col_skel_e);
                    dl->AddCircleFilled(*jp, r, jc, 10);
                    dl->AddCircle(*jp, r + 0.6f, IM_COL32(0, 0, 0, 220), 10, 0.8f);
                }
            }

            // v0.9.422 dev: raw bone-id picker.  When dump_all_bones=1, reader
            // emits every bone as raw_N; we draw a small dot per raw and — if
            // show_bone_ids=1 — the id label next to it so you can identify
            // which numeric id corresponds to shoulder/elbow/spine/etc.
            if (cfg.dev_dump_all_bones || cfg.dev_show_bone_ids) {
                bool draw_labels = cfg.dev_show_bone_ids != 0;
                ImU32 raw_dot = IM_COL32(255, 220,  60, 220);   // yellow
                ImU32 raw_txt = IM_COL32(255, 255, 255, 255);
                ImU32 raw_bg  = IM_COL32(  0,   0,   0, 200);
                for (const auto& [name, b] : e.bones) {
                    // Only process raw_* entries (present when dump_all_bones=1).
                    if (name.rfind("raw_", 0) != 0) continue;
                    ScreenPt sp = world_to_screen(b.x, b.y, b.z, cam, mat,
                                                  cfg.screen_w, cfg.screen_h);
                    if (!sp.ok) continue;
                    ImVec2 pt(sp.sx, sp.sy);
                    dl->AddCircleFilled(pt, 2.5f, raw_dot, 8);
                    if (draw_labels) {
                        const char* idstr = name.c_str() + 4;   // skip "raw_"
                        ImVec2 tsz = ImGui::CalcTextSize(idstr);
                        ImVec2 tp(sp.sx + 3.0f, sp.sy - tsz.y - 1.0f);
                        dl->AddRectFilled(ImVec2(tp.x - 1, tp.y),
                                          ImVec2(tp.x + tsz.x + 1, tp.y + tsz.y),
                                          raw_bg);
                        dl->AddText(tp, raw_txt, idstr);
                    }
                }
            }
            // If show_bone_ids is on but dump is off, also label the named
            // BONE_TABLE joints so you can see what id our current table
            // resolves to on this pawn.
            if (cfg.dev_show_bone_ids && !cfg.dev_dump_all_bones) {
                ImU32 ntxt = IM_COL32(180, 255, 180, 255);
                ImU32 nbg  = IM_COL32(  0,   0,   0, 200);
                for (const auto& [name, b] : e.bones) {
                    if (name.rfind("raw_", 0) == 0) continue;
                    ScreenPt sp = world_to_screen(b.x, b.y, b.z, cam, mat,
                                                  cfg.screen_w, cfg.screen_h);
                    if (!sp.ok) continue;
                    ImVec2 tsz = ImGui::CalcTextSize(name.c_str());
                    ImVec2 tp(sp.sx + 4.0f, sp.sy - tsz.y * 0.5f);
                    dl->AddRectFilled(ImVec2(tp.x - 1, tp.y),
                                      ImVec2(tp.x + tsz.x + 1, tp.y + tsz.y),
                                      nbg);
                    dl->AddText(tp, ntxt, name.c_str());
                }
            }

            // Head ring — sized from neck-to-head distance so it hugs the
            // real head, not a fixed pixel radius.
            if (cfg.head_circle) {
                auto hp = find_bone("head");
                auto np = find_bone("neck");
                if (hp && np) {
                    float dx = hp->x - np->x, dy = hp->y - np->y;
                    float r  = sqrtf(dx*dx + dy*dy) * 0.75f;
                    if (r < 3.0f) r = 3.0f;
                    ImU32 hc = hp_color(e, "head", cfg.col_head);
                    dl->AddCircle(*hp, r, hc, 20, thk + 0.4f);
                    dl->AddCircle(*hp, r + 0.8f, IM_COL32(0, 0, 0, 220), 20, 0.8f);
                }
            }
        }

        // Multi-line label stack — name, weapon, armor, dist+flags. HP hidden.
        if (cfg.show_hud) {
            float dist_m = dist_units / UE_UNITS_PER_M;
            char l_name[64], l_team[16], l_hp[24], l_wpn[32], l_mag[32], l_arm[32], l_dist[64];
            char l_thermal[16]; l_thermal[0] = 0;
            // v0.9.463: THERMAL badge above name when enemy has T7 Thermal Imager
            // (helmet attachment, item id prefix 30112xx). Detection in
            // reader.cpp fill_armor sets e.has_thermal.
            if (e.has_thermal) snprintf(l_thermal, sizeof(l_thermal), "THERMAL");

            // Line 1: name + inline team badge — saves a line vs the old
            // separate TEAM:X row.
            l_name[0] = 0;
            l_team[0] = 0;
            bool show_name_class = is_pmc ? cfg.show_name : cfg.show_bot_name;
            if (show_name_class) {
                const char* who = e.name.empty() ? e.cls.c_str() : e.name.c_str();
                if (is_pmc && cfg.show_team_id && e.team >= 0)
                    snprintf(l_name, sizeof(l_name), "%s  [T%d]", who, e.team);
                else
                    snprintf(l_name, sizeof(l_name), "%s", who);
            } else if (is_pmc && cfg.show_team_id && e.team >= 0) {
                snprintf(l_name, sizeof(l_name), "[T%d]", e.team);
            }

            // Line: HP — PMC only, always visible with '-' when unknown.
            l_hp[0] = 0;
            if (is_pmc && cfg.show_hp) {
                int denom = (e.hp_max > 0) ? e.hp_max : 445;
                if (e.hp >= 0) snprintf(l_hp, sizeof(l_hp), "%d/%d", e.hp, denom);
                else           snprintf(l_hp, sizeof(l_hp), "-/%d",  denom);
            }

            // Line: weapon (per-class) — '-' placeholder when unread.
            l_wpn[0] = 0;
            bool show_wpn_class = is_pmc ? cfg.show_weapon : cfg.show_bot_weapon;
            if (show_wpn_class) {
                snprintf(l_wpn, sizeof(l_wpn), "%s",
                         e.weapon.empty() ? "-" : e.weapon.c_str());
            }

            // Ammo suffix appended to the weapon line — one row instead of two.
            l_mag[0] = 0;
            bool show_ammo_class = is_pmc ? cfg.show_ammo : cfg.show_bot_ammo;
            if (show_ammo_class) {
                if (e.mag_cur >= 0 && e.mag_max > 0)
                    snprintf(l_mag, sizeof(l_mag), "%d/%d", e.mag_cur, e.mag_max);
                else if (e.mag_cur >= 0)
                    snprintf(l_mag, sizeof(l_mag), "%d",    e.mag_cur);
                else if (e.mag_max > 0)
                    snprintf(l_mag, sizeof(l_mag), "-/%d",  e.mag_max);
                else
                    snprintf(l_mag, sizeof(l_mag), "-");
            }
            // Merge weapon and ammo into one string so they render as a
            // single label line ("AR-15 · 30/30").
            if (l_wpn[0] && l_mag[0]) {
                char merged[80];
                snprintf(merged, sizeof(merged), "%s  ·  %s", l_wpn, l_mag);
                strncpy(l_wpn, merged, sizeof(l_wpn) - 1);
                l_wpn[sizeof(l_wpn) - 1] = 0;
                l_mag[0] = 0;
            }

            // Line: armor — always visible with '-' placeholders.
            l_arm[0] = 0;
            int helm_show = e.helm, vest_show = e.vest;
            if (helm_show < 0 && vest_show < 0 && !e.armor.empty()) {
                helm_show = e.armor[0];
                if (e.armor.size() > 1) vest_show = e.armor[1];
            }
            bool show_arm_class = cfg.show_armor_master
                                && (is_pmc ? cfg.show_armor : cfg.show_bot_armor);
            // v0.9.337 semantic: 0=Off, 1=Text, 2=Bar.
            bool armor_text_on  = (cfg.armor_display == 1);
            if (show_arm_class && armor_text_on) {
                char h_str[16], a_str[16];
                // v0.9.463: append current durability in parens after tier.
                // Format matches in-game: "H:6 (25.5) A:5 (48.0)". Raw memory
                // value is in tenths; reader.cpp already divided by 10.
                // Skip parens when durability unknown (-1) so unpatched entities
                // render as before.
                if (helm_show >= 0) {
                    if (e.helm_dur >= 0.0f)
                        snprintf(h_str, sizeof(h_str), "%d (%.1f)", helm_show, e.helm_dur);
                    else
                        snprintf(h_str, sizeof(h_str), "%d", helm_show);
                } else                snprintf(h_str, sizeof(h_str), "-");
                if (vest_show >= 0) {
                    if (e.vest_dur >= 0.0f)
                        snprintf(a_str, sizeof(a_str), "%d (%.1f)", vest_show, e.vest_dur);
                    else
                        snprintf(a_str, sizeof(a_str), "%d", vest_show);
                } else                snprintf(a_str, sizeof(a_str), "-");
                snprintf(l_arm, sizeof(l_arm), "H:%s A:%s", h_str, a_str);
            }

            // Line: distance + visibility + dead flags (per-class toggle)
            l_dist[0] = 0;
            bool show_dist_class = is_pmc ? cfg.show_distance : cfg.show_bot_distance;
            if (show_dist_class) {
                char vf[8] = "";
                if (cfg.visible_check_on && e.visible) snprintf(vf, sizeof(vf), " V");
                char df[8] = "";
                if (e.dead)    snprintf(df, sizeof(df), " DEAD");
                snprintf(l_dist, sizeof(l_dist), "%dm%s%s", (int)dist_m, vf, df);
            }

            // Tier-based armor color: T0-T2 grey, T3-T4 yellow, T5-T6 orange/red
            auto armor_color = [](int tier) -> ImU32 {
                if (tier >= 5) return IM_COL32(255, 110,  50, 255);
                if (tier >= 3) return IM_COL32(255, 220, 100, 255);
                return                IM_COL32(180, 180, 180, 255);
            };
            // v0.9.337: armor label uses operator's swatch — swatch wins so
            // the row's colour actually changes what you see. Tier grey/
            // yellow/orange logic dropped (was carrying limited info).
            (void)armor_color;
            ImU32 c_arm = is_pmc ? cfg.col_armor_pmc : cfg.col_armor_bot;

            // Font scale — ≤100m holds at 100m size, >100m gradually shrinks.
            float font_base = ImGui::GetFontSize();      // typically 13-14
            float lscale = (dist_m > 0.5f) ? (100.0f / dist_m) : 1.0f;
            if (lscale > 1.0f)  lscale = 1.0f;   // cap close enemies at 100m size
            if (lscale < 0.85f) lscale = 0.85f;  // floor for far enemies
            float font_sz = font_base * lscale;
            float line_h  = font_sz * 1.05f;

            ImFont* font = ImGui::GetFont();
            auto text_w = [&](const char* s) {
                return font->CalcTextSizeA(font_sz, FLT_MAX, 0.0f, s).x;
            };

            // Stack above box: [THERMAL] → name (+team) → HP → armor → wpn+ammo.
            // Distance rides BELOW the box to keep the top area lean.
            int n_lines = (l_thermal[0] ? 1 : 0)
                        + (l_name[0] ? 1 : 0)
                        + (l_hp[0]   ? 1 : 0)
                        + (l_wpn[0]  ? 1 : 0)
                        + (l_mag[0]  ? 1 : 0)
                        + (l_arm[0]  ? 1 : 0);
            if (n_lines == 0) n_lines = 1;   // reserve one row so top_y math is safe
            float top_y = box_center_y - box_h * 0.5f - n_lines * line_h - 4.0f;
            int line_idx = 0;
            auto draw_line = [&](const char* s, ImU32 col) {
                if (!s || !s[0]) return;
                float w = text_w(s);
                ImVec2 pos(p.sx - w * 0.5f, top_y + line_idx * line_h);
                dl->AddText(font, font_sz, pos, col, s);
                line_idx++;
            };

            // Mag color: operator swatch as base; red-empty overrides for
            // combat urgency (empty mag = safe engage). Yellow-low dropped
            // — one urgency signal is enough.
            ImU32 c_mag = is_pmc ? cfg.col_ammo_pmc : cfg.col_ammo_bot;
            if (e.mag_cur == 0) {
                c_mag = IM_COL32(255, 80, 80, 255);
            }

            // HP color: green > 66%, yellow > 33%, red otherwise
            ImU32 c_hp = IM_COL32(120, 255, 120, 255);
            if (e.hp_max > 0) {
                float f = (float)e.hp / (float)e.hp_max;
                if (f <= 0.33f) c_hp = IM_COL32(255, 80, 80, 255);
                else if (f <= 0.66f) c_hp = IM_COL32(255, 220, 100, 255);
            }

            // v0.9.463: bright cyan-white for THERMAL — high contrast, easy to spot.
            const ImU32 c_thermal = IM_COL32(120, 240, 255, 255);
            if (e.knocked) {
                draw_line(l_thermal, c_thermal);
                draw_line(l_name, col_name_e);
                draw_line(l_team, cfg.col_team);
                draw_line("Knocked!", IM_COL32(255, 140, 40, 255));
            } else {
                draw_line(l_thermal, c_thermal);
                draw_line(l_name, col_name_e);
                draw_line(l_team, cfg.col_team);
                draw_line(l_hp,   c_hp);
                draw_line(l_arm,  c_arm);
                // Weapon line — colored category chip in front of the name.
                if (l_wpn[0]) {
                    auto chip_col = [](const std::string& asset) -> ImU32 {
                        if (asset.empty()) return IM_COL32(150,150,150,255);
                        std::string s = asset;
                        for (auto& c : s) c = (char)std::toupper((unsigned char)c);
                        auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
                        if (has("SVD") || has("AWP") || has("SR ") || has("SVU") ||
                            has("BOLT") || has("SNIPER")) return IM_COL32(220, 90, 90, 255);
                        if (has("DMR") || has("M14") || has("MK14") || has("SKS") ||
                            has("MDR"))                    return IM_COL32(255, 90, 200, 255);
                        if (has("SG ") || has("SHOT") || has("BENELLI") ||
                            has("M870")|| has("KSG"))     return IM_COL32(180, 120, 255, 255);
                        if (has("SMG") || has("MP5") || has("MP7") || has("MP9") ||
                            has("UMP") || has("PP19") || has("PP2000") ||
                            has("VECTOR"))               return IM_COL32(255, 220, 80, 255);
                        if (has("PST") || has("PISTOL") || has("GLOCK") ||
                            has("USP") || has("DEAGLE"))  return IM_COL32(160, 200, 220, 255);
                        // Default AR bucket — most common category.
                        return IM_COL32(255, 150, 60, 255);
                    };
                    ImU32 chip = chip_col(e.weapon_asset);
                    float tw = text_w(l_wpn);
                    float cw = font_sz * 0.6f;
                    float gap = 4.0f;
                    ImVec2 pos(p.sx - (tw + cw + gap) * 0.5f,
                               top_y + line_idx * line_h);
                    dl->AddRectFilled(ImVec2(pos.x, pos.y + font_sz * 0.20f),
                                      ImVec2(pos.x + cw, pos.y + font_sz * 0.85f),
                                      chip, 1.5f);
                    // v0.9.337: weapon text uses operator swatch. Chip stays
                    // class-based (SR=red / SMG=yellow / etc) — that carries
                    // real weapon-category info, not just aesthetic hue.
                    ImU32 c_wpn = is_pmc ? cfg.col_weapon_pmc : cfg.col_weapon_bot;
                    dl->AddText(font, font_sz,
                                ImVec2(pos.x + cw + gap, pos.y),
                                c_wpn, l_wpn);
                    line_idx++;
                }
                draw_line(l_mag,  c_mag);
            }
            // Distance sits below the box — pull it out of the top stack.
            if (l_dist[0]) {
                float w = text_w(l_dist);
                float dy = box_center_y + box_h * 0.5f + 4.0f;
                dl->AddText(font, font_sz,
                            ImVec2(p.sx - w * 0.5f, dy),
                            col_dist_e, l_dist);
            }
        }
    }
}

// Loot ESP — draw text label at each loot box world position
void render_loot(const Snapshot* snap, const RenderConfig& cfg) {
    if (!snap || !snap->in_raid) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    Mat3 mat = cam_matrix(snap->cam);
    const Cam& cam = snap->cam;
    // Rarity → color. v0.9.337 calibration: shifted ramp down by 1 so red
    // is reserved for the truly high-tier drops. Empirically ABI's rarity
    // field goes 1-6, with 5 already being "epic" not "legendary" in game
    // UI. Previous thresholds painted 80k items red which read like top-
    // shelf loot when they're actually mid-tier. New scale:
    //   6 = mythic (red), 5 = legendary (gold), 4 = epic (purple),
    //   3 = rare (blue),  2 = uncommon (green), ≤1 = common (grey).
    auto rarity_col = [](int r) -> ImU32 {
        if (r >= 6) return IM_COL32(255,  90,  90, 255);   // mythic red
        if (r >= 5) return IM_COL32(255, 175,  60, 255);   // legendary gold
        if (r >= 4) return IM_COL32(200, 100, 255, 255);   // epic purple
        if (r >= 3) return IM_COL32(100, 130, 255, 255);   // rare blue
        if (r >= 2) return IM_COL32(100, 220, 100, 255);   // uncommon green
        return              IM_COL32(220, 220, 220, 255);  // common grey
    };
    ImFont* font = ImGui::GetFont();
    float fs_base = ImGui::GetFontSize() * 1.15f;   // bigger for readability
    for (const auto& lb : snap->loot) {
        auto p = world_to_screen(lb.x, lb.y, lb.z, cam, mat, cfg.screen_w, cfg.screen_h);
        if (!p.ok) continue;
        float dist_m = p.depth / UE_UNITS_PER_M;
        if (dist_m > 200.0f) continue;   // v0.9.392 fps: 500m→200m (farm has hundreds of containers past 200m)
        // Font scale by distance: close → full, far → shrinks to 0.7
        float fs = fs_base * std::max(0.7f, std::min(1.0f, 20.0f / std::max(dist_m, 1.0f)));

        // Corpse: always draw marker if class toggle on. Value display and
        // value gate are separate per-class settings.
        if (lb.is_corpse) {
            bool show_class = lb.is_bot_corpse ? cfg.show_bot_corpse : cfg.show_corpse;
            if (!show_class) continue;
            int thr = lb.is_bot_corpse ? cfg.bot_corpse_min_value
                                        : cfg.pmc_corpse_min_value;
            if ((int)lb.corpse_val < thr) continue;

            // Simple filled black box marker at corpse position — distance-scaled
            float box_r = std::max(4.0f, 10.0f * (20.0f / std::max(dist_m, 1.0f)));
            if (box_r > 10.0f) box_r = 10.0f;
            dl->AddRectFilled(
                ImVec2(p.sx - box_r, p.sy - box_r),
                ImVec2(p.sx + box_r, p.sy + box_r),
                IM_COL32(0, 0, 0, 230), 1.5f);
            // Thin outline: bot=grey, pmc=red
            ImU32 outline = lb.is_bot_corpse ? IM_COL32(180, 180, 180, 255)
                                              : IM_COL32(255, 80, 80, 255);
            dl->AddRect(
                ImVec2(p.sx - box_r, p.sy - box_r),
                ImVec2(p.sx + box_r, p.sy + box_r),
                outline, 1.5f, 0, 1.5f);
            continue;
        }

        // v0.9.337: `min_loot_value` restored as visibility gate — the
        // operator's earlier "remove cost highlighting" ask was about the
        // colour, not the filter. Colour now uses game rarity palette
        // (grey/green/blue/purple/red — matches the tint the game paints
        // on the item card). The slider still controls WHICH containers
        // are drawn at all so a low-value junk pile can be silenced.
        int best_r = -1;
        bool any_pass = false;
        for (const auto& it : lb.items) {
            if ((int)it.price < cfg.min_loot_value) continue;
            any_pass = true;
            if (it.rarity > best_r) best_r = it.rarity;
        }
        if (!any_pass) continue;
        ImU32 dot_col = rarity_col(best_r);
        dl->AddCircleFilled(ImVec2(p.sx, p.sy), 5.0f, dot_col);
        dl->AddCircle(ImVec2(p.sx, p.sy), 5.0f, IM_COL32(0,0,0,255), 12, 1.2f);

        // Distance line
        char dbuf[32];
        snprintf(dbuf, sizeof(dbuf), "%dm", (int)dist_m);
        ImVec2 dsz = font->CalcTextSizeA(fs*0.85f, FLT_MAX, 0.0f, dbuf);
        ImVec2 dpos(p.sx - dsz.x*0.5f, p.sy - dsz.y - 8.0f);
        dl->AddText(font, fs*0.85f, ImVec2(dpos.x+1, dpos.y+1), IM_COL32(0,0,0,220), dbuf);
        dl->AddText(font, fs*0.85f, dpos, IM_COL32(200,200,200,255), dbuf);

        int line = 0;
        for (const auto& it : lb.items) {
            // v0.9.337: value filter restored (see container-gate comment).
            if ((int)it.price < cfg.min_loot_value) continue;
            // v2026-09-23 arenahack: unknown-name items still get a text row
            // ("id=… $price") so testers see what's on the ground even when
            // the item_names table lags behind fresh builds.
            char buf[128];
            const char* nm = it.name.empty() ? "?" : it.name.c_str();
            if (it.price > 0)
                snprintf(buf, sizeof(buf), "%s  $%u", nm, it.price);
            else
                snprintf(buf, sizeof(buf), "%s", nm);
            ImU32 col = rarity_col(it.rarity);
            ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
            ImVec2 pos(p.sx - tsz.x * 0.5f, p.sy + 10.0f + line * (fs + 3.0f));
            // BG rect for readability
            dl->AddRectFilled(
                ImVec2(pos.x - 3, pos.y - 1),
                ImVec2(pos.x + tsz.x + 3, pos.y + tsz.y + 1),
                IM_COL32(0, 0, 0, 170), 2.0f);
            dl->AddText(font, fs, pos, col, buf);
            line++;
            if (line >= 4) break;
        }
    }
}

// Top loot sidebar — sorted list of highest-value loot within range.
void render_top_loot(const Snapshot* snap, const RenderConfig& cfg) {
    if (!cfg.show_top_loot || !snap || !snap->in_raid) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const auto& cam = snap->cam;
    const float RANGE_CM = cfg.top_loot_range_m * UE_UNITS_PER_M;

    struct Row {
        std::string name;
        uint32_t price;
        int rarity;
        float dist_m;
    };
    std::vector<Row> rows;
    rows.reserve(64);

    for (const auto& lb : snap->loot) {
        float dx = lb.x - cam.x, dy = lb.y - cam.y;
        float d = sqrtf(dx*dx + dy*dy);
        if (d > RANGE_CM) continue;
        float dm = d / UE_UNITS_PER_M;
        // Skip corpse aggregates — they already have a big marker on ESP
        if (lb.corpse_val > 0) continue;
        for (const auto& it : lb.items) {
            if ((int)it.price < cfg.min_loot_value) continue;
            // v2026-09-23: allow unknown-name items into the list ("?" name).
            std::string nm = it.name.empty() ? "?" : it.name;
            rows.push_back({ nm, it.price, it.rarity, dm });
        }
    }

    if (rows.empty()) return;
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b){
        return a.price > b.price;
    });
    if ((int)rows.size() > cfg.top_loot_max) rows.resize(cfg.top_loot_max);

    // Anchor: user-dragged position when set, else right side below radar.
    float x_right = (cfg.top_loot_screen_x > 0.5f)
                        ? cfg.top_loot_screen_x
                        : (float)cfg.screen_w - 20.0f;
    float y = (cfg.top_loot_screen_y > 0.5f)
                        ? cfg.top_loot_screen_y
                        : 60.0f + (float)cfg.radar_px_radius * 2.0f + 40.0f;
    float line_h = fs * 1.15f;

    // Header + bg
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "TOP LOOT [%dm]", (int)cfg.top_loot_range_m);
    ImVec2 hdr_sz = font->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.0f, hdr);

    float panel_w = 220.0f;
    float panel_h = line_h * (rows.size() + 1) + 12.0f;
    ImVec2 tl(x_right - panel_w, y);
    ImVec2 br(x_right, y + panel_h);
    dl->AddRectFilled(tl, br, IM_COL32(0, 0, 0, 180), 4.0f);
    dl->AddRect(tl, br, IM_COL32(120, 120, 140, 180), 4.0f);
    dl->AddText(font, fs * 0.9f,
                ImVec2(x_right - hdr_sz.x - 8, y + 4),
                IM_COL32(200, 200, 220, 255), hdr);

    y += line_h + 6;
    // v0.9.337: game-consistent styling. Each row gets a rarity-coloured
    // pill on the left (matches how ABI paints the item card border), the
    // name+distance in bright white for scanability, and the price in the
    // same rarity colour as the pill — so a purple row scans "purple" all
    // the way through instead of split colour-vs-gold.
    for (const auto& r : rows) {
        // v0.9.337: matches shifted rarity_col ramp in render_loot.
        ImU32 col_rar = r.rarity >= 6 ? IM_COL32(255,  90,  90, 255)   // mythic red
                     : r.rarity >= 5 ? IM_COL32(255, 175,  60, 255)   // legendary gold
                     : r.rarity >= 4 ? IM_COL32(200, 100, 255, 255)   // epic purple
                     : r.rarity >= 3 ? IM_COL32(100, 130, 255, 255)   // rare blue
                     : r.rarity >= 2 ? IM_COL32(100, 220, 100, 255)   // uncommon green
                     :                 IM_COL32(200, 200, 210, 255);  // common grey

        // Left-edge rarity pill (2 px stripe) — like the border on ABI item card
        float pill_x = x_right - panel_w + 4.0f;
        dl->AddRectFilled(ImVec2(pill_x, y + 2.0f),
                          ImVec2(pill_x + 2.5f, y + line_h - 3.0f),
                          col_rar, 1.2f);

        char left[80], right_s[32];
        snprintf(left, sizeof(left), "%s  %dm", r.name.c_str(), (int)r.dist_m);
        if (r.price >= 1000000)  snprintf(right_s, sizeof(right_s), "%.1fM", r.price / 1e6f);
        else if (r.price >= 1000) snprintf(right_s, sizeof(right_s), "%dk",  r.price / 1000);
        else                      snprintf(right_s, sizeof(right_s), "%d",   r.price);
        ImVec2 rsz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, right_s);

        // Name + distance — bright white for scanability
        dl->AddText(font, fs,
                    ImVec2(x_right - panel_w + 12, y),
                    IM_COL32(240, 240, 245, 255), left);
        // Price — rarity colour (so the whole row reads as one tier)
        dl->AddText(font, fs,
                    ImVec2(x_right - rsz.x - 8, y),
                    col_rar, right_s);
        y += line_h;
    }
}

// Military sweep radar — cyan glow style.
void render_radar(const Snapshot* snap, const RenderConfig& cfg) {
    if (!cfg.show_radar) return;
    if (!snap || !snap->in_raid) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const float rr  = (float)cfg.radar_px_radius;
    // Manual position from cfg.radar_screen_x/y when set; else auto top-right.
    const float cx  = (cfg.radar_screen_x > 0.5f)
                        ? cfg.radar_screen_x
                        : (float)cfg.screen_w - rr - 30.0f;
    const float cy  = (cfg.radar_screen_y > 0.5f)
                        ? cfg.radar_screen_y
                        : rr + 30.0f;
    const ImVec2 C(cx, cy);

    // Circular dark disc — no rectangular panel.
    const ImU32 col_disc      = IM_COL32(  0,   0,   0, 128);
    const ImU32 col_ring_edge = IM_COL32(220, 230, 235, 115);
    const ImU32 col_ring_mid  = IM_COL32(220, 230, 235,  65);
    const ImU32 col_fov_wedge = IM_COL32( 90,  95, 105, 165);
    const ImU32 col_range_lab = IM_COL32(200, 210, 220, 230);
    dl->AddCircleFilled(C, rr, col_disc, 96);

    // Helper: dashed circle — even segments drawn, odd skipped.
    auto dashed_circle = [&](float r, ImU32 c, float thk, int segments) {
        float step = 2.0f * PI / (float)segments;
        for (int i = 0; i < segments; i += 2) {
            float a0 = i * step, a1 = (i + 1) * step;
            ImVec2 p0(cx + cosf(a0) * r, cy + sinf(a0) * r);
            ImVec2 p1(cx + cosf(a1) * r, cy + sinf(a1) * r);
            dl->AddLine(p0, p1, c, thk);
        }
    };

    // Dashed concentric rings — 50 m step, plus dashed outer ring.
    if (cfg.radar_rings && cfg.radar_range_m > 5.0f) {
        float step   = 50.0f;
        int   n_rings = (int)std::floor(cfg.radar_range_m / step + 0.001f);
        if (n_rings > 12) n_rings = 12;
        for (int i = 1; i <= n_rings; i++) {
            float r = rr * ((float)i * step) / cfg.radar_range_m;
            if (r > rr - 0.5f) break;
            dashed_circle(r, col_ring_mid, 1.0f, 56);
        }
    }
    dashed_circle(rr, col_ring_edge, 1.4f, 64);

    // Range label ("250") floating just above the outer ring.
    {
        char rlab[8]; std::snprintf(rlab, sizeof(rlab), "%d", (int)cfg.radar_range_m);
        ImFont* rfont = ImGui::GetFont();
        float   rfs   = ImGui::GetFontSize() * 0.9f;
        ImVec2  sz    = rfont->CalcTextSizeA(rfs, FLT_MAX, 0.0f, rlab);
        dl->AddText(rfont, rfs,
                    ImVec2(cx - sz.x * 0.5f, cy - rr - rfs - 4.0f),
                    col_range_lab, rlab);
    }

    if (!snap) {
        dl->AddText(ImVec2(cx - 30, cy - 6), IM_COL32(255, 80, 80, 255), "no data");
        return;
    }
    const auto& cam = snap->cam;

    // (User's own FOV wedge removed by request.)
    (void)col_fov_wedge;

    // Center dot only — heading-up convention handled by world rotation, no arrow needed
    dl->AddCircleFilled(C, 2.5f, IM_COL32(255, 255, 255, 255), 16);

    // Enemy dots — heading-up transform (UE4 CW yaw → negate right sign)
    const float yr   = cam.yaw * PI / 180.0f;
    const float cy_r = cosf(yr);
    const float sy_r = sinf(yr);
    const float scale = rr / (cfg.radar_range_m * UE_UNITS_PER_M);
    // Find user's team for teammate filtering
    int my_team_r = -1;
    for (const auto& e : snap->entities) {
        if (e.me) { my_team_r = e.team; break; }
    }
    int n_drawn = 0;
    for (const auto& e : snap->entities) {
        if (e.me) continue;
        if (!cfg.show_mates && my_team_r >= 0 && e.team == my_team_r) continue;  // hide teammates
        bool is_bot_r = e.cls.starts_with("BOT");
        bool is_pmc_r = e.cls.starts_with("PMC") || e.cls.starts_with("Player") || e.cls.starts_with("USER");
        if (is_bot_r && !cfg.show_radar_bots) continue;
        if (is_pmc_r && !cfg.show_radar_pmc) continue;
        if (is_bot_r && e.dead) continue;   // hide bot corpses on radar too
        float dx = e.x - cam.x;
        float dy = e.y - cam.y;
        float dist_m = sqrtf(dx*dx + dy*dy) / UE_UNITS_PER_M;
        if (dist_m > cfg.radar_range_m) continue;

        float fwd   =  dx * cy_r + dy * sy_r;
        float right = -dx * sy_r + dy * cy_r;
        float sx = cx + right * scale;
        float sy = cy - fwd   * scale;

        bool is_pmc = e.cls.starts_with("PMC") || e.cls.starts_with("Player") || e.cls.starts_with("USER");
        bool is_bot = e.cls.starts_with("BOT");
        ImU32 c_dot, c_glow;
        float dot_r;
        if (e.dead) {
            c_dot  = IM_COL32(110, 110, 110, 220);
            c_glow = IM_COL32(110, 110, 110,  60);
            dot_r = 4.5f;
        } else if (is_pmc) {
            // v0.9.337: radar dot uses the operator's Box swatch — same
            // colour reads on-screen and on the radar for a given class.
            c_dot  = cfg.col_box_pmc;
            // Halo: same hue at 100/255 alpha
            c_glow = (cfg.col_box_pmc & 0x00FFFFFFu) | (100u << 24);
            dot_r = 6.5f;
        } else if (is_bot) {
            c_dot  = cfg.col_box_bot;
            c_glow = (cfg.col_box_bot & 0x00FFFFFFu) | (90u << 24);
            dot_r = 6.0f;
        } else {
            c_dot  = IM_COL32(120, 230, 255, 255);
            c_glow = IM_COL32( 80, 220, 255, 100);
            dot_r = 6.0f;
        }
        // View wedge FIRST so the dot draws on top of the wedge apex.
        // v0.9.337: proper FOV cone — apex AT the enemy dot, base flared
        // outward along the aim direction (widest end far from the dot).
        // Reads like a projector beam / vision cone in RTS games.
        if (cfg.radar_aim_dir && !e.dead && e.yaw.has_value()) {
            float ea  = *e.yaw * PI / 180.0f;
            float eax = cosf(ea), eay = sinf(ea);
            float fwd_x   =  eax * cy_r + eay * sy_r;
            float right_x = -eax * sy_r + eay * cy_r;
            float len   = dot_r * 6.5f;    // cone length from dot to base line
            float half  = 30.0f * PI / 180.0f;  // v0.9.337: 60° total aperture (was 120°)
            float ch = cosf(half), sh = sinf(half);
            float rx = right_x, ry = -fwd_x;    // cone axis in screen space
            // Two wing points at `len` distance, rotated ±half from axis
            float lx  =  ch * rx - sh * ry;
            float ly  =  sh * rx + ch * ry;
            float rx2 =  ch * rx + sh * ry;
            float ry2 = -sh * rx + ch * ry;
            ImVec2 apex  (sx, sy);
            ImVec2 leftp (sx + lx  * len, sy + ly  * len);
            ImVec2 rightp(sx + rx2 * len, sy + ry2 * len);
            // Vertex-alpha gradient: opaque at apex (dot), transparent at
            // the wings (far end) so the cone fades into the radar disc.
            ImU32 c_apex = (c_dot & 0x00FFFFFFu) | ((uint32_t)210 << 24);
            ImU32 c_wing = (c_dot & 0x00FFFFFFu);
            ImVec2 uv = ImGui::GetIO().Fonts->TexUvWhitePixel;
            dl->PrimReserve(3, 3);
            ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
            dl->PrimWriteVtx(apex,   uv, c_apex);
            dl->PrimWriteVtx(leftp,  uv, c_wing);
            dl->PrimWriteVtx(rightp, uv, c_wing);
            dl->PrimWriteIdx(i0);
            dl->PrimWriteIdx((ImDrawIdx)(i0 + 1));
            dl->PrimWriteIdx((ImDrawIdx)(i0 + 2));
        }

        // Solid coloured dot for every kind (PMC red / BOT yellow / etc).
        ImU32 c_halo = (c_dot & 0x00FFFFFFu) | ((uint32_t)55 << 24);
        dl->AddCircleFilled(ImVec2(sx, sy), dot_r + 3.5f, c_halo, 20);
        dl->AddCircleFilled(ImVec2(sx, sy), dot_r,        c_dot, 16);
        dl->AddCircle      (ImVec2(sx, sy), dot_r + 0.4f,
                            IM_COL32(0, 0, 0, 150), 12, 0.5f);
        n_drawn++;
    }
}

// ---- ME HUD helpers ----

// True if any alive enemy PMC within cfg.fight_mode_auto_range_m of user.
bool fight_mode_trigger(const Snapshot* snap, const RenderConfig& cfg) {
    if (!snap) return false;
    int my_team = -1;
    for (const auto& e : snap->entities) if (e.me) { my_team = e.team; break; }
    const auto& cam = snap->cam;
    const float R = cfg.fight_mode_auto_range_m * UE_UNITS_PER_M;
    for (const auto& e : snap->entities) {
        if (e.me || e.dead) continue;
        bool is_pmc = e.cls.starts_with("PMC") || e.cls.starts_with("Player") || e.cls.starts_with("USER");
        if (!is_pmc) continue;
        if (!cfg.show_mates && my_team >= 0 && e.team == my_team) continue;
        float dx = e.x - cam.x, dy = e.y - cam.y;
        if ((dx*dx + dy*dy) <= R*R) return true;
    }
    return false;
}

// FPS in top-left corner.
void render_perf_hud(const RenderConfig& cfg) {
    if (!cfg.show_fps_overlay) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize();
    float x = 12.0f, y = 12.0f;
    char buf[128];
    float fps = ImGui::GetIO().Framerate;
    snprintf(buf, sizeof(buf), "FPS %.0f", fps);
    dl->AddText(font, fs, ImVec2(x+1,y+1), IM_COL32(0,0,0,220), buf);
    dl->AddText(font, fs, ImVec2(x,y),     IM_COL32(180,255,180,255), buf);
}

// v0.9.419 exp: temporary diag HUD — dump raw AnimProxy zoom_offset + cam
// angles so we can derive the correct application formula from real values
// (hip, ADS level, ADS aim-up, ADS aim-down). Remove before shipping.
void render_zoom_debug(const Snapshot* snap) {
    if (!snap) return;
    const auto& cam = snap->cam;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize();
    float x = 12.0f, y = 40.0f;
    char buf[192];
    // v0.9.421: show effective_fov actually used, matching source selector.
    float sm_dbg = (cam.scope_mag > 0.5f && cam.scope_mag < 20.0f) ? cam.scope_mag : 1.0f;
    static float base_fov_dbg = 110.0f;
    if (sm_dbg <= 1.05f && cam.fov > 60.0f && cam.fov < 130.0f) base_fov_dbg = cam.fov;
    float eff_fov_dbg;
    const char* src_lbl = "base/sm";
    if (sm_dbg > 1.5f) {
        int src = g_test_fov_source;
        if      (src == 1 && cam.scope_fov > 1.0f) { eff_fov_dbg = cam.scope_fov;      src_lbl = "ADSScene"; }
        else if (src == 2 && cam.scope_fov > 1.0f) { eff_fov_dbg = cam.scope_fov/sm_dbg; src_lbl = "ADSScene/sm"; }
        else if (src == 3)                          { eff_fov_dbg = cam.fov/sm_dbg;    src_lbl = "POV/sm"; }
        else                                        { eff_fov_dbg = (base_fov_dbg + g_test_fov_bias)/sm_dbg; src_lbl = "base/sm"; }
    } else {
        eff_fov_dbg = cam.fov;
        src_lbl = "hip";
    }
    snprintf(buf, sizeof(buf),
             "FOV[%s]=%.2f  POV=%.1f base=%.1f ADSScn=%.1f mag=%.1fx  yaw=%+.1f pitch=%+.1f roll=%+.1f",
             src_lbl, eff_fov_dbg, cam.fov, base_fov_dbg, cam.scope_fov, sm_dbg,
             cam.yaw, cam.pitch, cam.roll);
    dl->AddText(font, fs, ImVec2(x+1, y+1), IM_COL32(0,0,0,220), buf);
    dl->AddText(font, fs, ImVec2(x,   y),   IM_COL32(255,200,120,255), buf);
    // v0.9.419: always show CHAIN string from reader::diag() (fallback)
    const char* d = reader::diag();
    if (d && d[0]) {
        float y2 = y + fs + 4.0f;
        dl->AddText(font, fs, ImVec2(x+1, y2+1), IM_COL32(0,0,0,220), d);
        dl->AddText(font, fs, ImVec2(x,   y2),   IM_COL32(120,255,180,255), d);
    }
    // v0.9.420: scope chain — shows which link (pawn/wm/cw/zc/live) is
    // failing so we can pinpoint the offset drift after game patches.
    const char* sd = reader::scope_diag();
    if (sd && sd[0]) {
        float y3 = y + (fs + 4.0f) * 2.0f;
        dl->AddText(font, fs, ImVec2(x+1, y3+1), IM_COL32(0,0,0,220), sd);
        dl->AddText(font, fs, ImVec2(x,   y3),   IM_COL32(255,140,200,255), sd);
    }
    // v0.9.420: PCM struct dump — 4 rows of 16 floats each, PCM+0x2100..0x21FF.
    // Screenshot in hip vs 7x ADS, diff to find post-modifier POV location.
    for (int i = 0; i < 4; ++i) {
        const char* cd = reader::cam_diag(i);
        if (cd && cd[0]) {
            float y4 = y + (fs + 4.0f) * (3 + i);
            dl->AddText(font, fs, ImVec2(x+1, y4+1), IM_COL32(0,0,0,220), cd);
            dl->AddText(font, fs, ImVec2(x,   y4),   IM_COL32(180,220,255,255), cd);
        }
    }
    // v0.9.421: per-class bone diag — compare PMC vs BOT arr_num and lz values.
    for (int i = 0; i < 2; ++i) {
        const char* bd = reader::bone_diag(i);
        if (bd && bd[0]) {
            float y5 = y + (fs + 4.0f) * (7 + i);
            dl->AddText(font, fs, ImVec2(x+1, y5+1), IM_COL32(0,0,0,220), bd);
            dl->AddText(font, fs, ImVec2(x,   y5),   IM_COL32(255,220,120,255), bd);
        }
    }
}

// User's own ammo — bottom-right, big font.
void render_my_ammo(const Snapshot* snap, const RenderConfig& cfg) {
    if (!snap || !cfg.show_my_ammo) return;
    const auto& cam = snap->cam;
    int cur = cam.mag_cur_a >= 0 ? cam.mag_cur_a : -1;
    int mx  = cam.mag_max   >  0 ? cam.mag_max   : -1;
    if (cur < 0 && mx <= 0) return;
    char buf[32];
    if (mx > 0 && cur >= 0) snprintf(buf, sizeof(buf), "%d / %d", cur, mx);
    else if (cur >= 0)      snprintf(buf, sizeof(buf), "%d", cur);
    else return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 2.0f;
    ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
    float x = (float)cfg.screen_w - sz.x - 30.0f;
    float y = (float)cfg.screen_h - sz.y - 30.0f;
    ImU32 col = cur == 0                     ? IM_COL32(255, 80, 80, 255)
              : (mx > 0 && cur * 3 <= mx)    ? IM_COL32(255, 220, 100, 255)
              :                                IM_COL32(220, 240, 255, 255);
    dl->AddText(font, fs, ImVec2(x+2, y+2), IM_COL32(0,0,0,220), buf);
    dl->AddText(font, fs, ImVec2(x,   y),   col, buf);
}

}  // namespace abi
