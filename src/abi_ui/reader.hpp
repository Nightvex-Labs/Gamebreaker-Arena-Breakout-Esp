#pragma once
// Reader — walks UAGame world state, produces abi::Snapshot for overlay.
// Runs in its own thread, double-buffered so overlay never reads a torn
// snapshot. Started with reader::start(), stopped with reader::stop().
// Call reader::latest() from the render thread to get the newest snapshot.

#include "snapshot.hpp"
#include <memory>

namespace reader {

// Kick off the background walker thread. Idempotent.
void start();

// Signal thread to stop, join, close read primitive.
void stop();

// Latest published snapshot. Never null once at least one walk finished.
// Ownership: shared_ptr, safe to hold from render frame.
std::shared_ptr<const abi::Snapshot> latest();

// State flags for status HUD.
bool attached();    // rp attached to UAGame.exe
bool world_ok();    // GWorld resolved, entities being read

// Optional walker diagnostic (single line) — set when GWorld = 0 so overlay
// can display raw pointer value + brute-force result.
const char* diag();

// Live scope chain resolution diag — shows which link (pawn/wm/cw/zc/live)
// fails, along with pointers, so we can spot when ZoomComp offset drifts
// after a game patch or when we're aiming an off-spec weapon.
const char* scope_diag();

// PCM struct dump for post-modifier POV hunt — 4 rows of 16 floats each,
// covering PCM+0x2100..0x21FF. Screenshot in hip then in ADS, diff by eye.
const char* cam_diag(int row);

// Per-class bone dump: index 0 = last-observed PMC, 1 = last-observed BOT.
// Shows arr_num, max_idx used, and local Z of head/pelvis/knee_L/foot_L —
// diff between PMC and BOT reveals if bots use a different mesh layout.
const char* bone_diag(int cls);

// v0.9.394: reader-side filters pushed from render config each frame.
// Reader consults these on every entity iteration to skip bridge_reads
// for stuff the user has toggled off / filtered out. Big FPS win on
// entity-heavy maps (farm has 100+ containers × per-tick loot walk).
struct Filters {
    bool  loot_enabled     = true;   // show_top_loot || show_loot
    bool  pmc_corpses      = true;   // show_corpse
    bool  bot_corpses      = true;   // show_bot_corpse
    float pmc_range_m      = 400.0f; // skip PMCs beyond this
    float bot_range_m      = 400.0f; // skip bots beyond this
    float bone_range_m     = 400.0f; // skip 47-bone read beyond this (bones = biggest cost)
    bool  visible_check    = true;   // v0.9.398 skip fill_visible entirely when off
};
void set_filters(const Filters& f);

}  // namespace reader
