// arenahack — glue stubs so ABIFINAL interface code links.
// These globals + reader-diag getters live in ABIFINAL's main.cpp / reader.cpp
// which we didn't pull. Empty definitions here — safe defaults.
#include <atomic>

// FPS cap read by abi::Overlay::run() to throttle sleep.
std::atomic<int> g_render_fps_cap{144};   // unlocked-ish; real cap = min(this, monitor Hz)
std::atomic<int> g_render_fps_hidden{15}; // when overlay hidden — saves GPU

namespace abi {
    std::atomic<int> g_dev_dump_all_bones{0};
    std::atomic<int> g_dev_force_yaw_only{0};
    std::atomic<int> g_dev_show_bone_ids{0};
}

// Reader diag hooks used only by render_zoom_debug (dev debug HUD).
namespace reader {
    const char* diag(void)         { return ""; }
    const char* scope_diag(void)   { return ""; }
    const char* cam_diag(int)      { return ""; }
    const char* bone_diag(int)     { return ""; }
}
