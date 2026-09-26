// overlay_hud.hpp — игровой оверлей. ОДНА реализация для игры и для превью.
//
// До этой правки версии по ТЗ жили в preview/overlay_preview_main.cpp, а игра
// рисовала своё из render.cpp — картинки разъезжались. Теперь обе стороны
// зовут отсюда, и других реализаций этих блоков в проекте нет.
//
// Порядок вызова за кадр (важен — блоки стыкуются по вертикали):
//
//     float y = 0.0f;
//     abi::hud::stats_and_nearest(snap, cfg, framerate, &y);
//     abi::hud::top_loot(snap, cfg, y);
//     abi::hud::radar(snap, cfg);
//     abi::hud::ammo_counter(snap, cfg);

#pragma once
#include "snapshot.hpp"
#include "render.hpp"

namespace abi::hud {

// Чипы статуса + карточки ближайших целей. Левый верх, отступ 18.
// out_bottom_y (если не null) получает нижнюю границу блока — по ней
// выравнивается top_loot, чтобы панели не наезжали.
void stats_and_nearest(const Snapshot* snap, const RenderConfig& cfg,
                       float framerate, float* out_bottom_y = nullptr);

// Панель добычи. Левый край, y_anchor — верх панели (обычно из stats_and_nearest).
void top_loot(const Snapshot* snap, const RenderConfig& cfg, float y_anchor);

// Радар «Растворяющийся конус». Угол берётся из cfg.radar_position.
void radar(const Snapshot* snap, const RenderConfig& cfg);

// Тот же радар в произвольный прямоугольник: org — левый верх, S — сторона
// (2×радиус). Этим рисует превью в панели, чтобы у него не было своей копии.
void radar_at(const Snapshot* snap, const RenderConfig& cfg,
              ImDrawList* dl, ImVec2 org, float S);

// Счётчик патронов «Дуга магазина». Правый низ, 92×92.
void ammo_counter(const Snapshot* snap, const RenderConfig& cfg);

// Статус-бар в стиле Gamebreaker: [лого] GameBreaker │ 👤 User │ ▂▄▆ PING │ ◠ FPS.
// Вызывается каждый кадр из overlay_boot после render_frame.
void status_bar(const RenderConfig& cfg, const char* user, int ping_ms, float fps,
                ImVec2 pos = ImVec2(18, 18));


// ── ESP-карточка цели ─────────────────────────────────────────────────────
// Одна реализация для игры (render_frame) и для превью в панели. Вызывающий
// проецирует бокс (и 8 углов 3D-каркаса) и заполняет поля; всё оформление —
// шрифты, цвета, размеры, уровни детализации — живёт в esp_style.cpp.
struct EspCard {
    ImVec2 b0{}, b1{};                 // 2D-бокс на экране
    bool   box = false, corners = true, has3d = false;
    int    mode = 2;                   // 2 = 2D, 3 = 3D
    ImVec2 c3d[8]{};                   // 0..3 низ, 4..7 верх (yaw + 45/135/225/315)
    bool   c3d_ok[8]{};
    bool   labels = true;              // cfg.show_hud
    bool   pmc = true, dead = false, knocked = false, thermal = false;
    float  dist_m = 0, alpha = 1, ui = 1;   // ui — множитель разрешения
    bool   preview = false;            // true — превью в меню (компактнее, мягкая тень)
    const char* name = nullptr;        // nullptr = скрыт
    int    team = -1;                  // -1 = скрыт
    bool   s_hp = false;  int hp = -1, hp_max = 445;
    const char* weapon = nullptr;      // nullptr = скрыт
    const char* weapon_asset = "";
    bool   s_ammo = false; int mag_cur = -1, mag_max = -1;
    bool   s_arm = false;  int armor_display = 1;   // 0 off, 1 текст, 2 полоса
    int    helm = -1, vest = -1; float helm_dur = -1, vest_dur = -1;
    bool   s_dist = false;
    ImU32  c_box = 0, c_name = 0, c_team = 0, c_wpn = 0, c_ammo = 0, c_dist = 0;
};
void esp_target(ImDrawList* dl, const EspCard& c);

}  // namespace abi::hud
