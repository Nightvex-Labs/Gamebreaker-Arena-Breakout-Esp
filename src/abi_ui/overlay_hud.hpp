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

}  // namespace abi::hud
