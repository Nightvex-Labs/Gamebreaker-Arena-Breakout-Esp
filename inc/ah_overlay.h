// arenahack — overlay base.
// External DXGI transparent always-on-top window + ImGui context.
// Blocking loop; call from main after driver_up if you want overlay running.
//
// Interface base only — no ESP draw, no menu content yet. User fills in
// per-frame ImGui / DrawList calls as features come online.
//
// Interaction model (mirrors ABIFINAL):
//   - Click-through by default (game gets all input)
//   - INSERT key toggles input-capture (ImGui menu takes over cursor/keys)
//   - Menu draws in center; boxes/lines outside menu regardless of capture
#pragma once
#include "dh_common.h"

// Blocking. Returns on WM_QUIT (user closed window) or Ctrl+C.
int AhOverlayRun(void);
