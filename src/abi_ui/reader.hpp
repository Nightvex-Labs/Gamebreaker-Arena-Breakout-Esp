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

}  // namespace reader
