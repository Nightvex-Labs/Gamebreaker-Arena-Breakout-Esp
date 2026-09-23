#pragma once
#include "render.hpp"
#include <imgui.h>

namespace abi {

// Draw ImGui-based settings window. Toggled by RenderConfig::show_control_panel.
// Mutates cfg live. Home key opens/closes; the actual key handling is in main.
void render_control_panel(RenderConfig& cfg);

// Supply the operator.png texture used by the ESP-Preview panel. Call once
// after texture creation (usually from preview_main.cpp after WIC load).
// tex = ImTextureID from a loaded D3D11 shader-resource view; wpx/hpx = size
// in pixels (kept for future aspect-ratio use).
void control_panel_set_operator_texture(ImTextureID tex, int wpx, int hpx);

// Typography tuning knobs used by the A/B preview builds.
//   text_k — global size multiplier over the TZ px values (1.0 = spec size)
//   mono_k — extra multiplier applied to the monospace face only. Consolas
//            renders visually larger than Segoe UI at the same pt size, so a
//            value below 1.0 evens the two out.
void control_panel_set_typography(float text_k, float mono_k);

// Desktop wallpaper drawn behind the panel (TZ §1.1). Pass the loaded
// assets/hero.png; it is cover-fitted to the viewport and dimmed to ~10 %
// brightness so the panel stays the focus.
void control_panel_set_background_texture(ImTextureID tex, int wpx, int hpx);

}
