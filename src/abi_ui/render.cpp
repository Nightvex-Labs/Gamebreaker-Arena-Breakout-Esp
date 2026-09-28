#include "render.hpp"
#include "reader.hpp"
#include "palette.hpp"
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

// Direct positional read вЂ” the DMA-era client-side extrapolation is no longer
// needed on 1PC: bridge latency is ~50 Ојs, reader ticks at 60+ Hz, and any
// jitter is well below one render frame. Keeping the shim so all call sites
// stay stable.
struct PredPos { float x, y, z; };
static PredPos predict(const Entity& e) { return { e.x, e.y, e.z }; }

constexpr float UE_UNITS_PER_M = 100.0f;
constexpr float PI = 3.14159265358979323846f;

static inline float deg2rad(float d) { return d * PI / 180.0f; }

struct Mat3 { float m[3][3]; };

// v0.9.422 dev toggles вЂ” defined in reader.cpp (namespace abi), pushed here.

// v0.9.421: TEST panel writes here at the start of each render frame.
// world_to_screen reads it inside its static base_fov_cached branch.
static int g_test_fov_bias = 0;
static int g_test_barrel_k1 = 0;
static int g_test_use_equirect = 1;
static int g_test_fov_source = 0;
static int g_test_scope_scale = 100;   // v0.9.422: extra ADS scale (percent)
static float g_test_scope_mag = 1.0f;   // last-seen scope for equirect gate

// v0.9.438: ultrawide FOV correction globals.  Not static вЂ” world_to_screen
// declares them extern.  Written per-frame from cfg in the same push block.
int  g_fov_correction_pct = 100;
bool g_fov_auto_horplus   = true;

// SDK Hor+ rotation matrix from cam yaw/pitch/roll (degrees).
// Same formula as Python overlay вЂ” verified against multi-build dump.
static Mat3 cam_matrix(const Cam& c) {
    // v0.9.421: live roll from POV.Rotation.Roll. Previously hard-coded 0,
    // which flat-out ignored Q/E leans в†’ ESP tilted opposite to model.
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
    // 75 / 4.76 в‰€ 15.75В° = HIP_POV_FOV / scope_mag = 110/7. Cache the hip
    // POV.FOV as base and divide by scope_mag in ADS вЂ” matches actual scope
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
        // v0.9.421: FOV source picker for ADS вЂ” test which formula matches
        // game's actual scope render.  cam.scope_fov now holds ADSSceneFOV.
        switch (g_test_fov_source) {
            case 1:  // ADSSceneFOV direct (Г· extra scope scale only)
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
    // preserving vertical FOV вЂ” our formula needs the ACTUAL horizontal FOV.
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
    // linearly to pixel вЂ” closer to fisheye-corrected output.  Toggle via
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
        // Optional radial barrel k1 correction: r' = r * (1 + k1*rВІ)
        // k1 stored as int Г— 0.01 (slider В±50 = k1 В±0.5)
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
void render_frame(const Snapshot* snap, const RenderConfig& cfg) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (!snap) {
        dl->AddText(ImVec2(20, 20), col_red, "memserver disconnected");
        return;
    }
    // v0.9.337: menu / lobby gate. Reader sets in_raid=false when cam or
    // "me" pawn coords are outside the raid envelope. Skip drawing entirely
    // вЂ” the preview mannequin + fake loadout entities that show up in the
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
    // prefire_grace_ms is 0 вЂ” first_seen just tracks visibility for the HUD.
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
        // Show dead bots too вЂ” user wants full visibility for triangulation
        // if (is_bot && e.dead) continue;

        // v0.9.421: cam-snap auto-apply disabled (baseline).  TEST cam-offset
        // now applied in CAM-LOCAL space (forward / right / up basis) instead
        // of world XYZ.  At pitch=0 cam.up == world.up so behavior is
        // identical to the old world-Z shift; at any tilt the offset stays
        // perpendicular to the look direction вЂ” matches how the game's
        // weapon-eye lifts along the scope axis, not along absolute Z.
        Cam  cam_local = snap->cam;
        Mat3 mat_local = mat;
        {
            // Auto-apply CurrentZoomingCameraOffset in ADS. Fixes box drift
            // that appears when mouse moves in scope: game renders from scope
            // eye, we projected from hipfire eye. Only fold in when actually
            // ADS'd вЂ” offset floats near zero in hip but not always exactly 0.
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

        // Client-side prediction вЂ” extrapolate to current render time
        PredPos pp = predict(e);

        // Distance cull
        float dx = pp.x - cam.x, dy = pp.y - cam.y;
        float range = is_pmc ? cfg.pmc_range_m : cfg.bot_range_m;
        float dist_units = sqrtf(dx*dx + dy*dy);
        if (dist_units > range * UE_UNITS_PER_M) continue;

        // в”Ђв”Ђ Prefire grace вЂ” skip / dim the marker when the target just
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
        // overrides the BOX per user request v0.9.463 вЂ” labels (name /
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
        // Labels вЂ” no visible-check tint.
        ImU32 col_name_e = class_col_dead(cfg.col_name_pmc,     cfg.col_name_bot);
        ImU32 col_dist_e = class_col_dead(cfg.col_distance_pmc, cfg.col_distance_bot);
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

        // Box anchor вЂ” chest-ish height (predicted pos).
        // v2026-09-23 arenahack:
        //   * BOT pp.z (ACE algo=0, plaintext) вЂ” sits near capsule TOP;
        //     shift -0.85*cap_hh lands anchor on chest. Empirically correct.
        //   * HUMAN pp.z (ACE algo>0, encrypted bucket) вЂ” encoded reference
        //     point sits HIGHER than bot (head-ish level). Same shift left
        //     boxes ~42px too high; extra -0.30*cap_hh drops them onto chest.
        float ez_chest = pp.z;
        auto p = world_to_screen(pp.x, pp.y, ez_chest, cam, mat, cfg.screen_w, cfg.screen_h);
        if (!p.ok) continue;

        // Projected pixel dims вЂ” used for box AND label scaling.
        // v0.9.420: mirror world_to_screen formula вЂ” in ADS use fixed base
        // FOV 90 divided by scope_mag (POV.FOV is post-modifier and would
        // double-apply the zoom). In hip use POV.FOV as before.
        float scope = (cam.scope_mag > 0.5f && cam.scope_mag < 20.0f) ? cam.scope_mag : 1.0f;
        // v0.9.421: mirror w2s effective_fov вЂ” use cached HIP POV.FOV / scope
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
        // v0.9.421: test pads (world cm) вЂ” top shrink + bottom shrink; box
        // center shifts by (bot_pad - top_pad)/2 to keep the ends aligned to
        // capsule minus pads.  test_box_shift_y adds an extra absolute nudge.
        float top_pad = (float)cfg.test_box_top_pad;
        float bot_pad = (float)cfg.test_box_bot_pad;
        float box_h = scale_factor * (e.cap_hh * 2.0f - top_pad + bot_pad) / p.depth;
        float box_w = scale_factor * (e.cap_r  * 2.0f) / p.depth;
        float box_center_y = p.sy + ((top_pad + bot_pad) * 0.5f + (float)cfg.test_box_shift_y)
                             * scale_factor / p.depth;

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
                        // endpoints. 8 corners Г— 3 arms = 24 short segments,
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

        // Armor bar вЂ” vertical tier stripe just to the right of the 2D box
        // silhouette. Two stacked segments (top = helm, bottom = vest) with
        // a color per tier so the top labels can stay lean.
        // v0.9.337 semantic: 0=Off, 1=Text, 2=Bar. Off = neither text nor bar.
        // Armor bar/text — PMC only. Bots не имеют настройки брони в панели,
        // и рендерить её у них не нужно (нет данных, нет UI, только шум).
        bool armor_bar_on  = is_pmc
                          && cfg.show_armor_master
                          && (cfg.armor_display == 2);
        // Distance clamp: drop only if the box is unreadably tiny (~1 px);
        // the old 18 px gate hid armor bar on distant enemies which the user
        // wants visible at all ranges the box itself renders.
        // v0.9.422: no armor range limit вЂ” no_distance_clamp path allows any
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
            float bw = std::max(3.0f, std::min(8.0f, box_h * 0.055f));
            float half = box_h * 0.5f - 2.0f;
            ImU32 shadow = IM_COL32(0, 0, 0, 200);
            ImFont* afont = ImGui::GetFont();
            // Font pairs to the bar itself instead of to the whole box вЂ”
            // otherwise the H4/V4 labels stay 16 px on tiny bars at long
            // range and swallow the box. half*0.55 keeps the label about
            // the height of one bar-half; the [8..20] clamp keeps it
            // readable close AND proportionate far.
            float afs_base = std::max(8.0f, std::min(20.0f, half * 0.55f));
            float afs = afs_base * cfg.armor_bar_font_scale;
            char tbuf[8];
            // Same fallback as text-mode: РµСЃР»Рё e.helm/e.vest РЅРµ Р·Р°РїРѕР»РЅРµРЅС‹
            // (РЅР°РїСЂ. РЅР° Р±РѕС‚Р°С… reader РєР»Р°РґС‘С‚ С‚РёСЂС‹ РІ e.armor РІРјРµСЃС‚Рѕ .helm/.vest),
            // Р±РµСЂС‘Рј РёР· e.armor[]. РќСѓР»РµРІРѕР№ С‚РёСЂ = В«РЅРµ РЅР°РґРµС‚РѕВ» вЂ” СЃРєСЂС‹РІР°РµРј.
            int hb = e.helm, vb = e.vest;
            if (hb > 0) {
                dl->AddRectFilled(ImVec2(bx - 0.5f, by - 0.5f),
                                  ImVec2(bx + bw + 0.5f, by + half + 0.5f), shadow);
                dl->AddRectFilled(ImVec2(bx, by),
                                  ImVec2(bx + bw, by + half), tier_col(hb));
                std::snprintf(tbuf, sizeof(tbuf), "H%d", hb);
                dl->AddText(afont, afs, ImVec2(bx + bw + 3, by), tier_col(hb), tbuf);
            }
            if (vb > 0) {
                float vy = by + half + 2.0f;
                dl->AddRectFilled(ImVec2(bx - 0.5f, vy - 0.5f),
                                  ImVec2(bx + bw + 0.5f, vy + half + 0.5f), shadow);
                dl->AddRectFilled(ImVec2(bx, vy),
                                  ImVec2(bx + bw, vy + half), tier_col(vb));
                std::snprintf(tbuf, sizeof(tbuf), "V%d", vb);
                dl->AddText(afont, afs, ImVec2(bx + bw + 3, vy), tier_col(vb), tbuf);
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

            // Line 1: nick + outlined team pill (matches control-panel preview).
            // Nick keeps its class colour; the pill uses cfg.col_team, drawn
            // as an outlined rounded rect with the team label inside.
            l_name[0] = 0;
            l_team[0] = 0;
            bool show_name_class = is_pmc ? cfg.show_name : cfg.show_bot_name;
            if (show_name_class) {
                // Bots don't have real usernames — show generic "Scav" instead
                // of the raw class string (e.g. "BOTBoss123").
                const char* who = !e.name.empty()
                                    ? e.name.c_str()
                                    : (is_bot ? "Scav" : e.cls.c_str());
                snprintf(l_name, sizeof(l_name), "%s", who);
            }
            if (is_pmc && cfg.show_team_id && e.team >= 0) {
                snprintf(l_team, sizeof(l_team), "Team %d", e.team);
            }

            // Line: HP вЂ” PMC only, always visible with '-' when unknown.
            l_hp[0] = 0;
            if (is_pmc && cfg.show_hp) {
                int denom = (e.hp_max > 0) ? e.hp_max : 445;
                if (e.hp >= 0) snprintf(l_hp, sizeof(l_hp), "%d/%d", e.hp, denom);
                else           snprintf(l_hp, sizeof(l_hp), "-/%d",  denom);
            }

            // Line: weapon (per-class) вЂ” '-' placeholder when unread.
            l_wpn[0] = 0;
            bool show_wpn_class = is_pmc ? cfg.show_weapon : cfg.show_bot_weapon;
            if (show_wpn_class) {
                snprintf(l_wpn, sizeof(l_wpn), "%s",
                         e.weapon.empty() ? "-" : e.weapon.c_str());
            }

            // Ammo counter вЂ” СЂРµРЅРґРµСЂРёС‚СЃСЏ РІ РєРѕРјРїРѕР·РёС‚РЅРѕР№ СЃС‚СЂРѕРєРµ РїРѕРґ Р±РѕРєСЃРѕРј
            // (weapon В· ammo В· dist). Fallback "-/-" РєРѕРіРґР° reader РЅРµ С‡РёС‚Р°РµС‚
            // РјР°РіР°Р·РёРЅ, С‡С‚РѕР±С‹ Сѓ РѕРїРµСЂР°С‚РѕСЂР° РІСЃРµРіРґР° Р±С‹Р» РІРёР·СѓР°Р»СЊРЅС‹Р№ СЃР»РѕС‚.
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
                    snprintf(l_mag, sizeof(l_mag), "-/-");
            }
            // РќРµ РјРµСЂР¶РёРј вЂ” РѕСЂСѓР¶РёРµ/РїР°С‚СЂРѕРЅС‹/РґРёСЃС‚Р°РЅС†РёСЏ СЂРёСЃСѓСЋС‚СЃСЏ РѕРґРЅРѕР№ СЃС‚СЂРѕРєРѕР№
            // РџРћР” Р±РѕРєСЃРѕРј РЅРёР¶Рµ (weapon В· ammo В· dist).

            // Line: armor — always visible with '-' placeholders.
            l_arm[0] = 0;
            int helm_show = e.helm, vest_show = e.vest;
            bool show_arm_class = is_pmc
                                && cfg.show_armor_master
                                && cfg.show_armor;
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
            // v0.9.337: armor label uses operator's swatch вЂ” swatch wins so
            // the row's colour actually changes what you see. Tier grey/
            // yellow/orange logic dropped (was carrying limited info).
            (void)armor_color;
            ImU32 c_arm = is_pmc ? cfg.col_armor_pmc : cfg.col_armor_bot;

            // Smart distance-based scaling — close = 1.0 (baseline), then
            // logarithmic fall-off for distant targets:
            //   <= 40  m — 1.00x (reference)
            //   60  m — 0.92x
            //   80  m — 0.86x
            //   120 m — 0.78x
            //   200 m — 0.68x
            //   300+ m — 0.60x (floor)
            float font_base = ImGui::GetFontSize();      // typically 13-14
            float d = std::max(dist_m, 40.0f);
            float lscale = 1.0f - 0.20f * std::log(d / 40.0f);
            if (lscale > 1.0f)  lscale = 1.0f;
            if (lscale < 0.60f) lscale = 0.60f;
            float font_sz = font_base * lscale;
            float line_h  = font_sz * 1.05f;

            ImFont* font = ImGui::GetFont();
            auto text_w = [&](const char* s) {
                return font->CalcTextSizeA(font_sz, FLT_MAX, 0.0f, s).x;
            };

            // Stack above box: [THERMAL] в†’ name (+team) в†’ HP в†’ armor.
            // Weapon В· ammo В· distance вЂ” РѕРґРЅР° СЃС‚СЂРѕРєР° РџРћР” Р±РѕРєСЃРѕРј.
            // nick and pill share one row.
            int n_lines = (l_thermal[0] ? 1 : 0)
                        + ((l_name[0] || l_team[0]) ? 1 : 0)
                        + (l_hp[0]   ? 1 : 0)
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

            // nick + plain team tag on one centered row вЂ” mirrors the
            // preview panel layout, no outline on the team tag.
            auto draw_name_team = [&](const char* nick, const char* team,
                                      ImU32 c_nick, ImU32 c_team) {
                if ((!nick || !nick[0]) && (!team || !team[0])) return;
                bool has_n = nick && nick[0];
                bool has_t = team && team[0];
                float nw = has_n ? text_w(nick) : 0.f;
                float tw = has_t ? text_w(team) : 0.f;
                float gp = (has_n && has_t) ? font_sz * 0.6f : 0.f;
                float total = nw + gp + tw;
                float x = p.sx - total * 0.5f;
                float y = top_y + line_idx * line_h;
                if (has_n) {
                    dl->AddText(font, font_sz, ImVec2(x, y), c_nick, nick);
                    x += nw + gp;
                }
                if (has_t) {
                    dl->AddText(font, font_sz, ImVec2(x, y), c_team, team);
                }
                line_idx++;
            };

            // Mag color: operator swatch as base; red-empty overrides for
            // combat urgency (empty mag = safe engage). Yellow-low dropped
            // вЂ” one urgency signal is enough.
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

            // v0.9.463: bright cyan-white for THERMAL вЂ” high contrast, easy to spot.
            const ImU32 c_thermal = IM_COL32(120, 240, 255, 255);
            // Armor drawn as two tier-colored segments (H + A), not a single
            // c_arm line вЂ” matches the bar mode's tier palette.
            auto arm_tier_col = [](int t) -> ImU32 {
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
            auto draw_armor = [&]() {
                if (!(show_arm_class && armor_text_on)) return;
                char h[16] = "", a[16] = "";
                if (helm_show >= 0) {
                    if (e.helm_dur >= 0.f) snprintf(h, sizeof(h), "H:%d (%.1f)", helm_show, e.helm_dur);
                    else                   snprintf(h, sizeof(h), "H:%d", helm_show);
                } else snprintf(h, sizeof(h), "H:-");
                if (vest_show >= 0) {
                    if (e.vest_dur >= 0.f) snprintf(a, sizeof(a), "A:%d (%.1f)", vest_show, e.vest_dur);
                    else                   snprintf(a, sizeof(a), "A:%d", vest_show);
                } else snprintf(a, sizeof(a), "A:-");
                ImU32 ch = helm_show > 0 ? arm_tier_col(helm_show) : IM_COL32(150,150,150,255);
                ImU32 ca = vest_show > 0 ? arm_tier_col(vest_show) : IM_COL32(150,150,150,255);
                float wh = text_w(h), wa = text_w(a), gap = font_sz * 0.5f;
                float total = wh + gap + wa;
                float x = p.sx - total * 0.5f;
                float y = top_y + line_idx * line_h;
                dl->AddText(font, font_sz, ImVec2(x, y), ch, h);
                dl->AddText(font, font_sz, ImVec2(x + wh + gap, y), ca, a);
                line_idx++;
            };

            if (e.knocked) {
                draw_line(l_thermal, c_thermal);
                draw_name_team(l_name, l_team, col_name_e, cfg.col_team);
                draw_line("Knocked!", IM_COL32(255, 140, 40, 255));
            } else {
                draw_line(l_thermal, c_thermal);
                draw_name_team(l_name, l_team, col_name_e, cfg.col_team);
                draw_line(l_hp,   c_hp);
                draw_armor();
            }
            // Weapon В· ammo В· distance вЂ” РµРґРёРЅР°СЏ СЃС‚СЂРѕРєР° РџРћР” Р±РѕРєСЃРѕРј,
            // СЃРµРіРјРµРЅС‚С‹ СЃРІРѕРёС… С†РІРµС‚РѕРІ, С†РµРЅС‚СЂРёСЂСѓРµС‚СЃСЏ РїРѕ Р±РѕРєСЃСѓ.
            {
                ImU32 c_wpn = is_pmc ? cfg.col_weapon_pmc : cfg.col_weapon_bot;
                struct Seg { const char* s; ImU32 c; } seg[3];
                int ns = 0;
                if (l_wpn[0])  seg[ns++] = { l_wpn,  c_wpn };
                if (l_mag[0])  seg[ns++] = { l_mag,  c_mag };
                if (l_dist[0]) seg[ns++] = { l_dist, col_dist_e };
                if (ns) {
                    const float gap = font_sz * 0.75f;
                    float total = 0;
                    for (int i = 0; i < ns; i++)
                        total += text_w(seg[i].s) + (i ? gap : 0);
                    float x = p.sx - total * 0.5f;
                    float y = box_center_y + box_h * 0.5f + 4.0f;
                    for (int i = 0; i < ns; i++) {
                        if (i) x += gap;
                        dl->AddText(font, font_sz, ImVec2(x, y), seg[i].c, seg[i].s);
                        x += text_w(seg[i].s);
                    }
                }
            }
        }
    }
}

// Loot ESP вЂ” draw text label at each loot box world position
void render_loot(const Snapshot* snap, const RenderConfig& cfg) {
    if (!snap || !snap->in_raid) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    Mat3 mat = cam_matrix(snap->cam);
    const Cam& cam = snap->cam;
    // Rarity в†’ color. v0.9.337 calibration: shifted ramp down by 1 so red
    // is reserved for the truly high-tier drops. Empirically ABI's rarity
    // field goes 1-6, with 5 already being "epic" not "legendary" in game
    // UI. Previous thresholds painted 80k items red which read like top-
    // shelf loot when they're actually mid-tier. New scale:
    //   6 = mythic (red), 5 = legendary (gold), 4 = epic (purple),
    //   3 = rare (blue),  2 = uncommon (green), в‰¤1 = common (grey).
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
        if (dist_m > 200.0f) continue;   // v0.9.392 fps: 500mв†’200m (farm has hundreds of containers past 200m)
        // Font scale by distance: close в†’ full, far в†’ shrinks to 0.7
        float fs = fs_base * std::max(0.7f, std::min(1.0f, 20.0f / std::max(dist_m, 1.0f)));

        // Corpse: always draw marker if class toggle on. Value display and
        // value gate are separate per-class settings.
        if (lb.is_corpse) {
            bool show_class = lb.is_bot_corpse ? cfg.show_bot_corpse : cfg.show_corpse;
            if (!show_class) continue;
            int thr = lb.is_bot_corpse ? cfg.bot_corpse_min_value
                                        : cfg.pmc_corpse_min_value;
            if ((int)lb.corpse_val < thr) continue;

            // Simple filled black box marker at corpse position вЂ” distance-scaled
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

        // v0.9.337: `min_loot_value` restored as visibility gate вЂ” the
        // operator's earlier "remove cost highlighting" ask was about the
        // colour, not the filter. Colour now uses game rarity palette
        // (grey/green/blue/purple/red вЂ” matches the tint the game paints
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
            // v0.9.454: unknown-name items (item_names catalog miss) are NOT
            // real loot in practice вЂ” they're spawned junk / world clutter
            // that leaked past the price gate.  Skip rather than paint "?".
            if (it.name.empty()) continue;
            char buf[128];
            const char* nm = it.name.c_str();
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

// Top loot sidebar вЂ” sorted list of highest-value loot within range.
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
        // Skip corpse aggregates вЂ” they already have a big marker on ESP
        if (lb.corpse_val > 0) continue;
        for (const auto& it : lb.items) {
            if ((int)it.price < cfg.min_loot_value) continue;
            // v0.9.454: skip unknown-name items вЂ” treat as garbage, not loot.
            if (it.name.empty()) continue;
            rows.push_back({ it.name, it.price, it.rarity, dm });
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
    // same rarity colour as the pill вЂ” so a purple row scans "purple" all
    // the way through instead of split colour-vs-gold.
    for (const auto& r : rows) {
        // v0.9.337: matches shifted rarity_col ramp in render_loot.
        ImU32 col_rar = r.rarity >= 6 ? IM_COL32(255,  90,  90, 255)   // mythic red
                     : r.rarity >= 5 ? IM_COL32(255, 175,  60, 255)   // legendary gold
                     : r.rarity >= 4 ? IM_COL32(200, 100, 255, 255)   // epic purple
                     : r.rarity >= 3 ? IM_COL32(100, 130, 255, 255)   // rare blue
                     : r.rarity >= 2 ? IM_COL32(100, 220, 100, 255)   // uncommon green
                     :                 IM_COL32(200, 200, 210, 255);  // common grey

        // Left-edge rarity pill (2 px stripe) вЂ” like the border on ABI item card
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

        // Name + distance вЂ” bright white for scanability
        dl->AddText(font, fs,
                    ImVec2(x_right - panel_w + 12, y),
                    IM_COL32(240, 240, 245, 255), left);
        // Price вЂ” rarity colour (so the whole row reads as one tier)
        dl->AddText(font, fs,
                    ImVec2(x_right - rsz.x - 8, y),
                    col_rar, right_s);
        y += line_h;
    }
}

// Military sweep radar вЂ” cyan glow style.
void render_radar(const Snapshot* snap, const RenderConfig& cfg) {
    if (!cfg.show_radar) return;
    if (!snap || !snap->in_raid) return;
    namespace P = abi::pal;
    namespace G = abi::pal::gb;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const float rr  = (float)cfg.radar_px_radius;
    const float pad = 30.0f;
    float cx, cy;
    if (cfg.radar_screen_x > 0.5f) {
        cx = cfg.radar_screen_x;
        cy = cfg.radar_screen_y;
    } else {
        switch (cfg.radar_position) {
            case 1:  cx = (float)cfg.screen_w - pad - rr; cy = (float)cfg.screen_h - pad - rr; break; // BR
            case 2:  cx =                      pad + rr; cy = (float)cfg.screen_h - pad - rr; break; // BL
            case 3:  cx =                      pad + rr; cy =                      pad + rr;  break; // TL
            default: cx = (float)cfg.screen_w - pad - rr; cy =                      pad + rr;  break; // TR
        }
    }
    const ImVec2 C(cx, cy);
    const float  k = std::clamp(rr / 200.0f, 0.7f, 1.3f);
    const ImVec2 uv = ImGui::GetIO().Fonts->TexUvWhitePixel;
    auto with_a = [](ImU32 c, float a) {
        return (c & ~IM_COL32_A_MASK) | ((ImU32)std::clamp((int)(a * 255.0f + 0.5f), 0, 255) << IM_COL32_A_SHIFT);
    };
    // РІРµРµСЂ С‚СЂРµСѓРіРѕР»СЊРЅРёРєРѕРІ СЃ Р°Р»СЊС„РѕР№ РїРѕ РІРµСЂС€РёРЅР°Рј: С†РµРЅС‚СЂ в†’ РґСѓРіР°
    auto fan = [&](ImVec2 o, float r, float a0, float a1, ImU32 c_in, ImU32 c_out, int seg) {
        dl->PrimReserve(seg * 3, seg + 2);
        ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(o, uv, c_in);
        for (int i = 0; i <= seg; i++) {
            float a = a0 + (a1 - a0) * (float)i / (float)seg;
            dl->PrimWriteVtx(ImVec2(o.x + cosf(a) * r, o.y + sinf(a) * r), uv, c_out);
        }
        for (int i = 0; i < seg; i++) {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
            dl->PrimWriteIdx((ImDrawIdx)(base + 2 + i));
        }
    };

    // в”Ђв”Ђ РґРёСЃРє, СЃРІРѕР№ РѕР±Р·РѕСЂ, РєРѕР»СЊС†Р°, РєСЂРµСЃС‚, РєСЂР°Р№
    dl->AddCircleFilled(C, rr, with_a(G::WINDOW, 0.78f), 128);
    {   // СЃРµРєС‚РѕСЂ 90В° РІРІРµСЂС… (СЌРєСЂР°РЅРЅС‹Р№ СѓРіРѕР» в€’90В° = РІРІРµСЂС…)
        const float h = 45.0f * PI / 180.0f, up = -PI * 0.5f;
        fan(C, rr, up - h, up + h, with_a(G::ACCENT, 0.16f), with_a(G::ACCENT, 0.0f), 48);
    }
    if (cfg.radar_rings) {
        dl->AddCircle(C, rr / 3.0f,        P::wht(0.06f), 96, 1.0f);
        dl->AddCircle(C, rr * 2.0f / 3.0f, P::wht(0.06f), 96, 1.0f);
    }
    dl->AddLine(ImVec2(cx, cy - rr), ImVec2(cx, cy + rr), P::wht(0.04f), 1.0f);
    dl->AddLine(ImVec2(cx - rr, cy), ImVec2(cx + rr, cy), P::wht(0.04f), 1.0f);
    dl->AddCircle(C, rr, P::wht(0.12f), 128, 1.0f);

    // в”Ђв”Ђ РїРѕРґРїРёСЃСЊ РґР°Р»СЊРЅРѕСЃС‚Рё РІРЅРёР·Сѓ РґРёСЃРєР°: В«100 mВ» РЅР° С‚С‘РјРЅРѕР№ РїР»Р°С€РєРµ
    {
        static ImFont* mono = nullptr;
        if (!mono) {
            for (ImFont* f : ImGui::GetIO().Fonts->Fonts)
                if (f && std::strcmp(f->GetDebugName(), "gb:jb500:12") == 0) { mono = f; break; }
            if (!mono) mono = ImGui::GetFont();
        }
        char lab[16]; std::snprintf(lab, sizeof(lab), "%d m", (int)cfg.radar_range_m);
        const float fs = 10.5f * k;
        ImVec2 ts = mono->CalcTextSizeA(fs, FLT_MAX, 0.0f, lab);
        float pw = ts.x + 14.0f * k, ph = 18.0f * k, py = cy + rr - 12.0f * k - ph;
        dl->AddRectFilled(ImVec2(cx - pw * 0.5f, py), ImVec2(cx + pw * 0.5f, py + ph), P::wht(0.05f), 6.0f * k);
        dl->AddRect(ImVec2(cx - pw * 0.5f, py), ImVec2(cx + pw * 0.5f, py + ph), P::wht(0.06f), 6.0f * k, 0, 1.0f);
        dl->AddText(mono, fs, ImVec2(std::floor(cx - ts.x * 0.5f + 0.5f), std::floor(py + (ph - fs) * 0.5f + 0.5f)), G::TEXT_MUTED, lab);
    }

    // в”Ђв”Ђ С†РµР»Рё (heading-up, РєР°Рє Р±С‹Р»Рѕ)
    const auto& cam = snap->cam;
    const float yr = cam.yaw * PI / 180.0f, cy_r = cosf(yr), sy_r = sinf(yr);
    const float scale = rr / (cfg.radar_range_m * UE_UNITS_PER_M);
    int my_team = -1;
    for (const auto& e : snap->entities) if (e.me) { my_team = e.team; break; }

    for (const auto& e : snap->entities) {
        if (e.me) continue;
        if (!cfg.show_mates && my_team >= 0 && e.team == my_team) continue;
        const bool is_bot = e.cls.starts_with("BOT");
        const bool is_pmc = e.cls.starts_with("PMC") || e.cls.starts_with("Player") || e.cls.starts_with("USER");
        if (is_bot && !cfg.show_radar_bots) continue;
        if (is_pmc && !cfg.show_radar_pmc) continue;
        if (is_bot && e.dead) continue;
        const float dx = e.x - cam.x, dy = e.y - cam.y;
        if (sqrtf(dx * dx + dy * dy) / UE_UNITS_PER_M > cfg.radar_range_m) continue;

        const float fwd = dx * cy_r + dy * sy_r, right = -dx * sy_r + dy * cy_r;
        const ImVec2 p(cx + right * scale, cy - fwd * scale);

        if (e.dead) {   // С‚СЂСѓРї вЂ” РїСѓСЃС‚РѕРµ РєРѕР»СЊС†Рѕ
            dl->AddCircle(p, 4.0f * k, is_pmc ? cfg.col_corpses_pmc : cfg.col_corpses_bot, 16, 1.5f);
            continue;
        }
        const ImU32 col = is_pmc ? cfg.col_box_pmc : is_bot ? cfg.col_box_bot : G::ACCENT;

        // РєРѕРЅСѓСЃ РІР·РіР»СЏРґР° 50В°: РіСЂР°РґРёРµРЅС‚ РѕС‚ С‚РѕС‡РєРё РЅР°СЂСѓР¶Сѓ
        if (cfg.radar_aim_dir && e.yaw.has_value()) {
            const float ea = *e.yaw * PI / 180.0f;
            const float ax = cosf(ea), ay = sinf(ea);
            const float f2 = ax * cy_r + ay * sy_r, r2 = -ax * sy_r + ay * cy_r;
            const float ang = atan2f(-f2, r2), half = 25.0f * PI / 180.0f;
            fan(p, 30.0f * k, ang - half, ang + half, with_a(col, 0.55f), with_a(col, 0.0f), 12);
        }
        // РјРµС‚РєР°: РёРіСЂРѕРє вЂ” РєСЂСѓРі, Р±РѕС‚ вЂ” СЂРѕРјР±; С‚С‘РјРЅР°СЏ РѕР±РІРѕРґРєР° РѕС‚РґРµР»СЏРµС‚ РѕС‚ РєРѕРЅСѓСЃР° Рё РґРёСЃРєР°
        if (is_bot) {
            const float r = 5.0f * k, o = r + 1.5f;
            dl->AddQuadFilled(ImVec2(p.x, p.y - o), ImVec2(p.x + o, p.y), ImVec2(p.x, p.y + o), ImVec2(p.x - o, p.y), G::WINDOW);
            dl->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), col);
        } else {
            dl->AddCircleFilled(p, 5.5f * k + 1.5f, G::WINDOW, 20);
            dl->AddCircleFilled(p, 5.5f * k, col, 20);
        }
    }

    // в”Ђв”Ђ СЃРІРѕСЏ РјРµС‚РєР° РїРѕРІРµСЂС… РІСЃРµРіРѕ: Р°РєС†РµРЅС‚-СЃС‚СЂРµР»РєР° РІРІРµСЂС…
    {
        const float m = 7.0f * k;
        ImVec2 q[4] = { ImVec2(cx, cy - m), ImVec2(cx + m * 0.8f, cy + m * 0.75f),
                        ImVec2(cx, cy + m * 0.35f), ImVec2(cx - m * 0.8f, cy + m * 0.75f) };
        dl->AddPolyline(q, 4, G::WINDOW, ImDrawFlags_Closed, 3.0f);
        dl->PathLineTo(q[0]); dl->PathLineTo(q[1]); dl->PathLineTo(q[2]);
        dl->PathFillConvex(G::ACCENT);
        dl->PathLineTo(q[0]); dl->PathLineTo(q[2]); dl->PathLineTo(q[3]);
        dl->PathFillConvex(G::ACCENT);
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
