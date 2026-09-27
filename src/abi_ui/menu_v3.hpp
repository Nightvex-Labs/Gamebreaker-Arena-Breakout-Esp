#pragma once
#include <imgui.h>

namespace abi {

struct RenderConfig;   // forward — full definition in render.hpp

// GameBreaker · меню v3.
void render_menu_v3();
void menu_v3_set_scale(float dpi);
void menu_v3_set_operator_texture(ImTextureID tex, int wpx, int hpx);

// Bridge to RenderConfig. Pull runs once (first frame after panel opens),
// push runs every frame after render_menu_v3() so toggle changes reach the
// world ESP renderer.
void menu_v3_pull(const RenderConfig& cfg);
void menu_v3_push(RenderConfig& cfg);

}
