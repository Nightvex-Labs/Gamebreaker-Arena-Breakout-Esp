#pragma once
#include "snapshot.hpp"
#include <imgui.h>

namespace abi {

struct RenderConfig {
    int screen_w{}, screen_h{};
    float ui_scale{1.0f};   // = actual_w / 2560 — scales HUD text, panel, radar

    // === PLAYERS (PMC) ===
    // Defaults ON per user request (2026-09-24): "оставь 1 где всё вкл".
    // Skeleton stays OFF — not implemented.
    int   box_mode{3};                // 0=off, 2=2D, 3=3D  (Players box)
    bool  show_box_pmc{true};
    bool  show_name{true};
    bool  show_team_id{true};
    bool  show_hp{true};
    bool  show_weapon{true};
    bool  show_ammo{true};             // enemy CAmmo line
    bool  show_armor{true};
    bool  show_corpse{true};
    bool  show_distance{true};
    int   pmc_corpse_min_value{0};        // 0 = show all PMC corpses regardless of value
    bool  show_mates{false};              // draw box/skel/labels for teammates too

    // ── Behavioral safety layer ────────────────────────────────────────────
    // Kills the "impossible awareness of a target behind cover" pattern that
    // drives most of the manual-review bans. When a pawn transitions from
    // hidden (game not rendering its mesh) to visible, the ESP marker is
    // suppressed for `prefire_grace_ms` so a human-plausible reaction window
    // elapses before we surface the target. 0 = feature off (legacy).
    int   prefire_grace_ms{300};          // 0 = off, 300 = ~human reaction time
    bool  prefire_grace_pmc_only{true};   // don't apply to bots (nobody bans over bot kills)
    bool  prefire_grace_fade{true};       // fade marker to 20% alpha during grace instead of hiding entirely
    // ───────────────────────────────────────────────────────────────────────
    bool  show_nearest_table{false};       // top-left "nearest" table (PMC only)
    bool  show_stats{false};              // v0.9.337: default OFF — dev-only panel
    bool  box_corners{true};              // v0.9.430: PMC corner-bracket style
    bool  box_corners_bot{true};          // v0.9.430: BOT corner-bracket style (separated)
    int   armor_display{1};               // v0.9.337: 0=Off, 1=Text (default), 2=Bar
    bool  show_armor_master{true};         // ME-tab master toggle; when false suppresses ALL armor rendering
    float armor_bar_font_scale{1.35f};    // label size multiplier over auto-computed base
    bool  armor_bar_no_distance_clamp{true}; // when true the armor bar draws regardless of box height
    bool  radar_aim_dir{true};            // small arrow on radar dot showing yaw
    bool  radar_rings{true};              // 25 m concentric rings on radar
    float pmc_range_m{400.0f};        // 1..400 (default max per user)

    // === BOTS ===
    // Defaults ON per user request. Bot HP/name/team/armor menu rows hidden
    // (unimplemented in reader), but the values still render if a tick sets
    // them. Skeleton stays OFF — not implemented.
    int   box_mode_bot{2};            // 0=off, 2=2D, 3=3D  (Bots box)
    bool  show_box_bot{true};
    bool  show_bot_name{true};
    bool  show_bot_weapon{true};
    bool  show_bot_ammo{true};
    bool  show_bot_armor{true};
    bool  show_bot_corpse{true};
    bool  show_bot_distance{true};
    int   bot_corpse_min_value{0};        // 0 = show all BOT corpses regardless of value
    float bot_range_m{50.0f};         // 1..400

    // === ME ===
    bool  show_fps_overlay{false};
    // v0.9.427: overlay render FPS cap.  Default 120 for modern GPUs,
    // 60 for weak (RX 470/570, GTX 750 Ti et al).  30 for absolute low-end.
    // Read once per Present() by overlay::run() via cfg::render_fps_cap().
    int   render_fps_cap{120};   // 30 / 60 / 120 — presets in settings tab
    // When overlay is hidden (Alt+Tab OR game not foreground), throttle to
    // this lower rate — saves GPU when user isn't looking at the ESP.
    int   render_fps_hidden{15}; // 5..30 — hidden-window rate
    bool  show_my_ammo{true};          // my mag_cur/max bottom-right
    bool  show_cam_info{false};        // cam pos/yaw/fov/scope line in status panel
    bool  show_entities_count{false};  // "entities N (user 1 pmc X bot Y)" line
    bool  show_dead_visible{false};    // "dead N visible N" line
    bool  visible_check_on{true};     // v0.9.393 color box green when enemy visible (LastRenderTime check)
    ImU32 col_visible{IM_COL32(80,255,80,255)};  // v0.9.410 configurable visible-check tint
    bool  show_connection{false};      // "net OK/DOWN" status line
    bool  show_top_loot{true};         // right-side sidebar
    int   top_loot_max{10};
    float top_loot_range_m{150.0f};
    int   min_loot_value{25000};      // 1..1000000

    // === RADAR ===
    bool  show_radar{true};
    bool  show_radar_bots{true};
    bool  show_radar_pmc{true};
    float radar_range_m{100.0f};      // 50..400 (v0.9.470: 100m standard)
    int   radar_px_radius{200};       // v0.9.470: 200px standard
    // Radar screen position (center X/Y). 0/0 = auto top-right anchor.
    // Non-zero = manual — user-dragged position.
    float radar_screen_x{0.0f};
    float radar_screen_y{0.0f};
    // Top-loot panel screen position (top-right anchor of the panel).
    // 0/0 = auto (right side, below radar). Non-zero = user-dragged.
    float top_loot_screen_x{0.0f};
    float top_loot_screen_y{0.0f};

    // === FIGHT MODE ===
    bool  fight_mode{false};
    bool  fight_mode_auto{false};
    float fight_mode_auto_range_m{100.0f};

    // === Global / meta ===
    // v0.9.437: fuser_mode removed — was for 2PC HDMI-mixer overlay, this
    // project has been 1PC-only since v0.9.32x baseline.  All fuser branches
    // stripped from overlay.cpp/overlay.hpp/main.cpp.
    bool  show_control_panel{true};   // visible on first launch so operator can enable features; HOME toggles thereafter
    bool  show_hud{true};              // master label gate — individual show_name / show_hp / show_weapon / show_ammo / show_armor / show_distance ride on top of this. No user-visible toggle, so must default ON otherwise the per-line toggles look broken.
    // Legacy — kept for JSON forward-compat
    bool  outline_glow{false};
    float glow_range_m{100.0f};

    // === UI ===
    int   ui_language{0};             // 0=EN, 1=RU (control panel only)
    // v0.9.430 (bundle HUD): visibility highlight — colour target and VIS pill
    // in nearest cards when enemy is line-of-sight visible (draws from
    // e.visible flag).  Off: keeps swatch colour + no VIS badge.
    bool  visibility_check{true};
    // v0.9.430 (bundle HUD): radar corner placement — 0=TR (default), 1=BR,
    // 2=BL, 3=TL.  Used by hud::radar() via corner_origin().
    int   radar_position{0};
    // v0.9.430 bundle Overlay tab — status panel chip/toggles.
    bool  ovl_crosshair{false};
    bool  ovl_spectators{false};
    bool  ovl_keybinds{false};
    int   ovl_position{0};            // 0=TL, 1=BL, 2=BR
    ImU32 ovl_accent{IM_COL32(255, 179, 64, 255)};   // amber
    // v0.9.430 bundle Armor tab — per-tier colour swatch (T1..T6).
    ImU32 armor_tier[6] = {
        IM_COL32(120,120,120,255), IM_COL32(150,150,150,255),
        IM_COL32(255,200,60,255),  IM_COL32(255,140,0,255),
        IM_COL32(255,60,60,255),   IM_COL32(200,0,0,255)
    };
    // v0.9.416: user-controlled text scale. Multiplies FontGlobalScale on top
    // of the resolution-derived ui_scale. 100 = auto default, 130 = +30% text,
    // useful on 1080p where auto-scale (=0.75) leaves HUD labels tiny.
    int   text_scale_pct{100};        // v0.9.438: extended to 50..250 (was 70..150) — 4K users need >150 for readable HUD
    // v0.9.438: manual per-frame FOV correction for ultrawide monitors (21:9, 32:9).
    // Game reports POV.FOV but on non-16:9 aspect ratios the ratio between reported
    // FOV and actual rendered horizontal FOV varies per title.  This slider multiplies
    // the effective_fov used in world_to_screen — user tunes empirically until ESP
    // boxes track enemies at screen edges.  Auto Hor+ correction runs first (below);
    // this is fine-tune on top.  Range 50..200, default 100 (no correction).
    int   fov_correction_pct{100};
    // v0.9.438: auto Hor+ FOV correction toggle.  When ON (default), world_to_screen
    // computes the "real" horizontal FOV for user's aspect from a 16:9 baseline via
    // vfov-preserving Hor+ formula.  When OFF, uses cam.fov directly (v0.9.437 behavior).
    // Turn OFF if game doesn't use Hor+ scaling (rare for FPS titles).
    bool  fov_auto_horplus{true};

    // v0.9.421: TEST panel — runtime tuning of drift-related constants so we
    // don't have to rebuild for each experiment.  All measured in world cm
    // (or degrees for fov_bias) and applied additively in render.cpp.
    int   test_fov_bias      {-22}; // user-tuned ~-20..-25 range fixes scope pan drift on this build
    int   test_box_shift_y   {0};   // ±30 cm world — shifts box CENTER vertically
    int   test_box_top_pad   {0};   // ±30 cm — shrinks/grows box TOP only
    int   test_box_bot_pad   {0};   // ±30 cm — shrinks/grows box BOTTOM only
    int   test_cam_off_x     {0};   // ±30 cm — cam.x offset applied at W2S
    int   test_cam_off_y     {0};   // ±30 cm — cam.y offset applied at W2S
    int   test_cam_off_z     {0};   // ±30 cm — cam.z offset applied at W2S
    int   test_show_hud      {0};   // 0/1 — draw test HUD with current values
    int   test_barrel_k1     {0};   // ±50 — radial barrel k1 coefficient × 0.01
    int   test_use_equirect  {1};   // 0=perspective, 1=equirect (default for ADS)
    // v0.9.421: FOV source selection for ADS.  0=base_cached/mag (current),
    // 1=ADSSceneFOV direct, 2=ADSSceneFOV/scope_mag, 3=POV.FOV/scope_mag (legacy).
    int   test_fov_source    {0};
    // v0.9.422: SCOPE additional scale factor when box+skel look too small in
    // scope glass.  1.00 = no extra scale, 2.5 = compensate 2.5× glass viewport
    // shrink.  Multiplies scope_mag inside W2S/box calc (denominator of
    // effective_fov).  Slider stored as ×100 int for imgui.
    int   test_scope_scale   {100};   // 50..500 (0.5x..5x), default 100 (1x)

    // === PER-ROW COLOURS (v0.9.337) ─ each swatch on Visuals binds here.
    // Applied as the "base" tint for that element on that class of entity.
    // "Visible" green tint still overrides for enemies you can shoot; dead
    // grey still overrides for dropped entities. Everything else uses these.
    // Format: IM_COL32(R,G,B,A) — 0xAABBGGRR little-endian on disk.
    ImU32 col_box_pmc      {IM_COL32(255, 255, 255, 255)}; // white
    ImU32 col_box_bot      {IM_COL32(255,  90,  90, 255)}; // red
    ImU32 col_name_pmc     {IM_COL32(150, 170, 255, 255)}; // pale blue
    ImU32 col_name_bot     {IM_COL32(150, 170, 255, 255)};
    ImU32 col_team         {IM_COL32(212,  82, 122, 255)}; // rose
    ImU32 col_weapon_pmc   {IM_COL32(200, 140, 255, 255)}; // purple
    ImU32 col_weapon_bot   {IM_COL32(200, 140, 255, 255)};
    ImU32 col_ammo_pmc     {IM_COL32( 96, 204, 168, 255)}; // teal
    ImU32 col_ammo_bot     {IM_COL32( 96, 204, 168, 255)};
    ImU32 col_armor_pmc    {IM_COL32(230, 120, 150, 255)}; // pink
    ImU32 col_armor_bot    {IM_COL32(230, 120, 150, 255)};
    ImU32 col_distance_pmc {IM_COL32( 96, 204, 168, 255)}; // teal
    ImU32 col_distance_bot {IM_COL32( 96, 204, 168, 255)};
    ImU32 col_corpses_pmc  {IM_COL32(150, 150, 150, 255)}; // grey
    ImU32 col_corpses_bot  {IM_COL32(150, 150, 150, 255)};
    ImU32 col_allies       {IM_COL32( 80, 220, 130, 255)}; // green
    ImU32 col_nearest      {IM_COL32(212,  82, 122, 255)}; // rose
    ImU32 col_head         {IM_COL32(230,  90,  90, 255)}; // red
};

void render_frame(const Snapshot* snap, const RenderConfig& cfg);
void render_loot(const Snapshot* snap, const RenderConfig& cfg);
void render_top_loot(const Snapshot* snap, const RenderConfig& cfg);
void render_radar(const Snapshot* snap, const RenderConfig& cfg);
void render_perf_hud(const RenderConfig& cfg);       // FPS corner text
void render_my_ammo(const Snapshot* snap, const RenderConfig& cfg);

// Fight-mode helper: returns true if a PMC is alive within auto-radius of user.
bool fight_mode_trigger(const Snapshot* snap, const RenderConfig& cfg);

}
