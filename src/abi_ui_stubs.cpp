// arenahack — glue stubs so ABIFINAL interface code links.
// These globals + reader-diag getters live in ABIFINAL's main.cpp / reader.cpp
// which we didn't pull. Empty definitions here — safe defaults.
#include <atomic>

// FPS cap read by abi::Overlay::run() to throttle sleep.
std::atomic<int> g_render_fps_cap{144};   // unlocked-ish; real cap = min(this, monitor Hz)
std::atomic<int> g_render_fps_hidden{15}; // when overlay hidden — saves GPU
