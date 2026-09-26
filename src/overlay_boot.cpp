// arenahack overlay bootstrap.
// abi::Overlay owns window+D3D+ImGui init/render/present. We just supply the
// per-frame callback: HOME toggle + render_control_panel(cfg).
// arenahack overlay bootstrap. HOME toggle via RegisterHotKey — the only
// reliable path when the game is foreground + running at higher integrity
// (raw-input capture + UIPI both block GetAsyncKeyState from us).
// System-level hotkey fires WM_HOTKEY regardless of who has focus.
#include <windows.h>
#include <atomic>
#include <thread>
#include <math.h>
#include <stdio.h>
#include <imgui.h>
#include "abi_ui/overlay.hpp"
#include "abi_ui/control_panel.hpp"
#include "abi_ui/render.hpp"
#include "abi_ui/snapshot.hpp"
#include "abi_ui/image_loader.hpp"
#include <d3d11.h>

namespace abi {
    void render_radar    (const Snapshot* snap, const RenderConfig& cfg);
    void render_frame    (const Snapshot* snap, const RenderConfig& cfg);
    void render_loot     (const Snapshot* snap, const RenderConfig& cfg);
    namespace hud {
        void top_loot(const Snapshot* snap, const RenderConfig& cfg, float y_anchor);
        void ammo_counter(const Snapshot* snap, const RenderConfig& cfg);
        void status_bar(const RenderConfig& cfg, const char* user, int ping_ms, float fps, ImVec2 pos);
    }
}

extern "C" {
#include "../inc/dh_common.h"
#include "ah_reader_thread.h"
#include "../inc/item_names.hpp"

// System DPI ratio published by overlay.cpp after the font atlas is baked.
float ah_get_panel_dpi(void);
}

static std::atomic<bool> g_home_press{false};
static std::atomic<bool> g_hk_run{true};

// Own message-only window in this thread so RegisterHotKey with our hWnd
// delivers WM_HOTKEY directly to a WndProc we control (avoids thread-msg
// queue pitfalls where PeekMessage can lose messages if pumped elsewhere).
static LRESULT CALLBACK hk_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_HOTKEY && wp == 1) {
        g_home_press.store(true);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void hotkey_thread(void) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = hk_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AhHotkeySink";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, L"AhHotkeySink", L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    BOOL ok = RegisterHotKey(hw, 1, 0, VK_HOME);
    DH_INFO("hotkey: RegisterHotKey(VK_HOME)=%d gle=%lu", (int)ok, GetLastError());

    MSG msg;
    while (g_hk_run.load()) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            Sleep(5);
        }
    }
    if (ok) UnregisterHotKey(hw, 1);
    DestroyWindow(hw);
}

extern "C" int AhOverlayRun(void) {
    abi::Overlay ov;
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    if (!ov.init(sw, sh)) { DH_ERROR("abi::Overlay::init failed"); return 1; }

    // Load operator.png. Primary path: embedded byte array — the overlay is
    // self-contained, no sidecar file needed. Sidecar fallbacks kept for dev
    // iteration where the embedded copy might be stale relative to assets/.
    {
        #include "../inc/operator_png_data.h"
        ID3D11ShaderResourceView* op_srv = nullptr;
        int op_w = 0, op_h = 0;

        if (abi::image_loader::load_png(ov.device(),
                                         OPERATOR_PNG_DATA, OPERATOR_PNG_SIZE,
                                         &op_srv, &op_w, &op_h)) {
            abi::control_panel_set_operator_texture(
                reinterpret_cast<ImTextureID>(op_srv), op_w, op_h);
            DH_INFO("operator.png: loaded from embed (%d bytes, %dx%d)",
                    (int)OPERATOR_PNG_SIZE, op_w, op_h);
        } else {
            // Fallback: sidecar assets/operator.png next to the exe (dev only).
            wchar_t exe_path[MAX_PATH]{};
            DWORD glen = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            if (glen > 0 && glen < MAX_PATH) {
                wchar_t* last_sep = wcsrchr(exe_path, L'\\');
                if (last_sep) *(last_sep + 1) = 0;
                const wchar_t* rels[] = {
                    L"assets\\operator.png",
                    L"..\\assets\\operator.png",
                };
                wchar_t buf[MAX_PATH * 2]{};
                for (const wchar_t* rel : rels) {
                    _snwprintf_s(buf, _TRUNCATE, L"%s%s", exe_path, rel);
                    if (abi::image_loader::load_png(ov.device(), buf, &op_srv, &op_w, &op_h)) {
                        abi::control_panel_set_operator_texture(
                            reinterpret_cast<ImTextureID>(op_srv), op_w, op_h);
                        DH_INFO("operator.png: fallback file %ls (%dx%d)", buf, op_w, op_h);
                        break;
                    }
                }
                if (!op_srv) DH_INFO("operator.png: embed AND sidecar failed — placeholder text will show");
            }
        }
    }

    // Feed the DPI ratio (baked into fonts by overlay::init) into the
    // panel typography knob. control_panel_set_typography(dpi, dpi) tells
    // the panel to divide baked glyph size by DPI and scale its own
    // geometry by DPI, keeping the layout 1:1 with the mock.
    {
        float dpi = ah_get_panel_dpi();
        if (dpi < 1.0f) dpi = 1.0f;
        abi::control_panel_set_typography(dpi, dpi);
    }

    abi::RenderConfig cfg{};
    cfg.screen_w = sw;
    cfg.screen_h = sh;
    cfg.show_control_panel = true;
    // MVP: don't try to gate on visibility (mesh LastRenderTime not read yet)
    // and suppress prefire grace so live entities show without extra delay.
    cfg.visible_check_on   = false;
    cfg.prefire_grace_ms   = 0;
    cfg.min_loot_value     = 75000; // мировые маркеры лута от 75к
    cfg.show_top_loot      = false; // top-loot список удалён из UI
    ov.set_input_capture(cfg.show_control_panel);

    std::thread hk(hotkey_thread);

    // Reader thread — populates AH_LIVE_SNAP (self pos + yaw + fov @20Hz).
    ah_reader_start();

    // Snapshot the overlay hands to render_radar. cam.* filled from live
    // reader each frame; entities empty until enemy walker lands.
    abi::Snapshot stub_snap;
    // v0.9.454: in_raid tracks reader.attached — the flag is 1 when ACE
    // self-decrypt succeeds, which only happens once the player is inside a
    // raid. In the lobby/menu ACE returns null coords → attached=0 → in_raid
    // false, HUD/render bail out, and reader clears per-raid corpse caches on
    // its own transition edge (see ah_reader_thread.cpp RAID-END).
    stub_snap.in_raid = false;

    // Radar drag state — grab anywhere inside the disc, drop = new center.
    bool  radar_dragging = false;
    float rd_off_x = 0.0f, rd_off_y = 0.0f;
    // Top-loot drag state — grab in panel bbox (top-right anchor).
    bool  tl_dragging = false;
    float tl_off_x = 0.0f, tl_off_y = 0.0f;

    ov.run([&]() {
        // RegisterHotKey delivers WM_HOTKEY globally (elevated overlay bypasses
        // UIPI). ALWAYS drain GetAsyncKeyState latch even when hotkey already
        // consumed the toggle — otherwise its stale transition bit fires again
        // next frame → open/close flicker on single press.
        bool toggle = g_home_press.exchange(false);
        bool async_bit = (GetAsyncKeyState(VK_HOME) & 1) != 0;
        if (!toggle && async_bit) toggle = true;
        if (toggle) {
            cfg.show_control_panel = !cfg.show_control_panel;
            ov.set_input_capture(cfg.show_control_panel);
        }

        // Soft reattach = Numpad * (VK_MULTIPLY). Rebuilds the kdu handle,
        // re-scans for UAGame, re-sig-scans GWorld. Use when the overlay is
        // up but reader has silently stalled (provider dropped, procCR3
        // died, EDR rearmed). No process restart — panel state + configs
        // preserved.
        {
            static bool reattach_edge = false;
            bool reattach_down = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
            if (reattach_down && !reattach_edge) {
                DH_INFO("Numpad * pressed → ah_reader_reattach()");
                ah_reader_reattach();
            }
            reattach_edge = reattach_down;
        }

        // Pull live self snapshot from reader thread.
        AH_LIVE_SNAP live;
        ah_reader_snapshot(&live);
        // v0.9.454: in_raid from ASGGameState::roomid (0 in menu, non-zero in
        // raid). Was using live.attached (ACE-decrypt-success) — that false-
        // positived in the lobby because the menu preview character has a valid
        // root pawn with algo=0, so decrypt "worked" and attached stayed 1.
        stub_snap.in_raid = (live.roomid != 0);
        stub_snap.cam.x   = live.x;
        stub_snap.cam.y   = live.y;
        stub_snap.cam.z   = live.z;
        stub_snap.cam.yaw = live.yaw;
        stub_snap.cam.pitch = live.pitch;
        stub_snap.cam.roll  = live.roll;
        stub_snap.cam.fov = live.fov;
        stub_snap.cam.scope_mag = (live.scope_mag > 0.5f) ? live.scope_mag : 1.0f;
        stub_snap.cam.scope_fov = live.scope_fov;
        stub_snap.cam.zoom_offset_x = live.zoom_off_x;
        stub_snap.cam.zoom_offset_y = live.zoom_off_y;
        stub_snap.cam.zoom_offset_z = live.zoom_off_z;
        stub_snap.cam.mag_cur_a     = live.my_mag_cur;
        stub_snap.cam.mag_max       = live.my_mag_max;
        // Rebuild snapshot.loot ONLY when reader publishes a fresh scan.
        // Prevents per-frame std::vector allocation storm at 60fps.
        static unsigned int last_loot_gen = 0;
        if (live.loot_gen != last_loot_gen) {
            last_loot_gen = live.loot_gen;
            stub_snap.loot.clear();
            stub_snap.loot.reserve(live.loot_n);
            for (int i = 0; i < live.loot_n; i++) {
                const AH_LOOT& ll = live.loot[i];
                abi::LootBox lb{};
                lb.a = ll.a; lb.x = ll.x; lb.y = ll.y; lb.z = ll.z;
                lb.count = 1;
                abi::LootItem it{};
                it.id = ll.id; it.price = ll.price; it.rarity = ll.rarity;
                if (ll.name[0]) it.name = ll.name;
                lb.items.push_back(std::move(it));
                stub_snap.loot.push_back(std::move(lb));
            }
        }

        // Rebuild snapshot.entities from live snap. Cheap: <=64 shallow copies.
        stub_snap.entities.clear();
        int alive_n = 0;
        for (int i = 0; i < live.ent_n; i++) {
            const AH_ENT& le = live.ents[i];
            if (!le.valid) continue;
            abi::Entity e{};
            e.a    = le.pawn;
            e.x    = le.x; e.y = le.y; e.z = le.z;
            e.yaw  = le.yaw;
            e.hp     = le.hp;
            e.hp_max = 445;   // baseline PMC — full 7-limb sum can override once wired
            e.me   = le.is_me != 0;
            if (le.cap_r  > 0) e.cap_r  = le.cap_r;
            if (le.cap_hh > 0) e.cap_hh = le.cap_hh;
            e.dead    = le.dead != 0;
            e.team = le.team;
            e.name = le.name;
            e.cls  = le.is_bot ? "BOT_PRIMARY" : "PMC";
            e.helm = le.helm;
            e.vest = le.vest;
            e.helm_dur = le.helm_dur;
            e.vest_dur = le.vest_dur;
            e.mag_cur = le.mag_cur;
            e.mag_max = le.mag_max;
            if (le.weapon_id) {
                if (const char* wn = abi::items::lookup(le.weapon_id)) {
                    e.weapon_asset = wn;
                    e.weapon = std::string(wn).substr(0, 5);
                } else {
                    // ID not in item_names.hpp — surface it so we can extend
                    // the catalog. Short form "ID:xxxxx" (last 5 digits) for
                    // label; full 9-digit id in asset for hover/log side.
                    char buf[16];
                    _snprintf_s(buf, _TRUNCATE, "%u", (unsigned)(le.weapon_id % 100000u));
                    e.weapon = std::string("?") + buf;
                    char asset[32];
                    _snprintf_s(asset, _TRUNCATE, "unknown:%u", (unsigned)le.weapon_id);
                    e.weapon_asset = asset;
                }
            }
            stub_snap.entities.push_back(std::move(e));
            if (!le.is_me && !le.dead) alive_n++;
        }

        // Dev stats panel removed — was leaking cam pos / attached state /
        // reader Hz which is operator-visible ONLY when debugging box math.
        // Restore only under a build flag if needed for field diag.
        (void)alive_n;

        // World ESP: boxes, names, HP for each entity. Draws on background
        // list so menu (foreground) sits above.
        abi::render_frame(&stub_snap, cfg);
        // Floor-loot markers (colored dots + price text via rarity).
        abi::render_loot(&stub_snap, cfg);
        // Top-loot panel — NEW Fey style (abi::hud::top_loot, right-anchored,
        // vertically centered; override via cfg.top_loot_screen_x/y).
        abi::hud::top_loot(&stub_snap, cfg, 0.0f);
        abi::hud::ammo_counter(&stub_snap, cfg);

        // Gamebreaker статус-бар в левом верхнем углу: [лого] GameBreaker │ 👤 User │ ▂▄▆ PING │ ◠ FPS
        {
            float fps = ImGui::GetIO().Framerate;
            int   ping_ms = 0;   // TODO: подцепить реальный ping когда reader начнёт его читать
            abi::hud::status_bar(cfg, "Operator", ping_ms, fps, ImVec2(18.0f, 18.0f));
        }

        // Top-loot panel drag (menu-only). Panel dims match hud::top_loot
        // (LOOT_W 380 + 2*LOOT_PAD_X 14 = 408; height varies with row count).
        if (cfg.show_top_loot && cfg.show_control_panel) {
            const float panel_w = 408.0f;
            // Height estimate: header + up to LOOT_ROWS_MAX(6) rows @ 32 + gaps.
            const float panel_h = 24.0f + 18.0f + 11.0f + 32.0f*6 + 10.0f*5;   // ~295
            // Recreate hud::top_loot anchor logic (top-right anchor).
            bool manual = (cfg.top_loot_screen_x > 0.5f || cfg.top_loot_screen_y > 0.5f);
            float OX = manual ? (cfg.top_loot_screen_x - panel_w)
                              : ((float)cfg.screen_w - panel_w - 18.0f);
            float OY = manual ? cfg.top_loot_screen_y
                              : (((float)cfg.screen_h - panel_h) * 0.5f);
            ImVec2 mp = ImGui::GetIO().MousePos;
            bool in_tl = (mp.x >= OX && mp.x < OX + panel_w &&
                          mp.y >= OY && mp.y < OY + panel_h);
            if (in_tl && ImGui::IsMouseClicked(0) && !tl_dragging) {
                tl_dragging = true;
                // Store offset of mouse relative to top-right anchor.
                tl_off_x = (OX + panel_w) - mp.x;
                tl_off_y = OY - mp.y;
            }
            if (tl_dragging) {
                if (ImGui::IsMouseDown(0)) {
                    cfg.top_loot_screen_x = mp.x + tl_off_x;
                    cfg.top_loot_screen_y = mp.y + tl_off_y;
                    // Clamp fully on-screen.
                    if (cfg.top_loot_screen_x < panel_w) cfg.top_loot_screen_x = panel_w;
                    if (cfg.top_loot_screen_y < 0)       cfg.top_loot_screen_y = 0;
                    if (cfg.top_loot_screen_x > cfg.screen_w)
                        cfg.top_loot_screen_x = (float)cfg.screen_w;
                    if (cfg.top_loot_screen_y > cfg.screen_h - panel_h)
                        cfg.top_loot_screen_y = (float)cfg.screen_h - panel_h;
                } else {
                    tl_dragging = false;
                }
            }
        }

        // Radar draw + drag. Active only while menu is open (input capture
        // routes clicks to us; game still gets clicks otherwise).
        if (cfg.show_radar) {
            abi::render_radar(&stub_snap, cfg);

            if (cfg.show_control_panel) {
                // Compute current center same way render_radar does — учитывая
                // cfg.radar_position, если пользователь ещё не перетаскивал.
                const float rr = (float)cfg.radar_px_radius;
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
                ImVec2 mp = ImGui::GetIO().MousePos;
                float dx = mp.x - cx, dy = mp.y - cy;
                bool inside = (dx*dx + dy*dy) <= (rr * rr);

                if (inside && ImGui::IsMouseClicked(0) && !radar_dragging) {
                    radar_dragging = true;
                    rd_off_x = cx - mp.x;
                    rd_off_y = cy - mp.y;
                }
                if (radar_dragging) {
                    if (ImGui::IsMouseDown(0)) {
                        cfg.radar_screen_x = mp.x + rd_off_x;
                        cfg.radar_screen_y = mp.y + rd_off_y;
                        // Clamp fully on-screen.
                        if (cfg.radar_screen_x < rr) cfg.radar_screen_x = rr;
                        if (cfg.radar_screen_y < rr) cfg.radar_screen_y = rr;
                        if (cfg.radar_screen_x > cfg.screen_w - rr)
                            cfg.radar_screen_x = (float)cfg.screen_w - rr;
                        if (cfg.radar_screen_y > cfg.screen_h - rr)
                            cfg.radar_screen_y = (float)cfg.screen_h - rr;
                    } else {
                        radar_dragging = false;
                    }
                }
            }
        }

        if (cfg.show_control_panel) {
            abi::render_control_panel(cfg);
        }
    });

    g_hk_run.store(false);
    if (hk.joinable()) hk.join();
    ah_reader_stop();
    ov.shutdown();
    return 0;
}
