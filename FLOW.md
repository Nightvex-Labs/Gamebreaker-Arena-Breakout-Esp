# ABIESP — Launch Flow

Full path from user clicking **Play** on `Arena Breakout: Infinite ESP` in
the KoenFlow launcher to a working overlay reading game memory.

Product ID: `arena-breakout-esp`
Ship shape: DeltaHack-identical (WinRuntimeHost stub + KFPL bundle + kdu db)
Modal flow: `DhModalProducts` (numbered errors + ready-event handshake)

---

## Stage 0 — Static assets on backend

Admin panel `koenflow.com` stores per-release:
- **Package** — `WinRuntimeHost.zip` (see stage 5 shape)
- **Launch executable** — `WinRuntimeHost.exe`
- **KFPL key** — empty (baked into stub; see stage 3.2)

## Stage 1 — Play button click (WebUI)

`home.html` line ~1181-1207:

1. Button carries `data-pid="arena-breakout-esp"`.
2. `dhModalShow('loading', {label:'Preparing…'})` — modal opens instantly.
3. `window.nvx.call('product.launch', {productId:'arena-breakout-esp'})`
   posts JSON `{id, cmd, args}` over WebView2 postMessage.
4. `nvx.call` timeout = **90 s** for `product.launch` (v2.2.1). Legacy 8 s
   was too tight and made catch() fire "Error #99" before dhstate:dh-running
   arrived, producing the "error-then-success" flicker.

## Stage 2 — Launcher C# preflight (ShellBridge.LaunchProductAsync)

`ShellBridge.cs::LaunchProductAsync(productId)`:

1. `useDhModal = DhModalProducts.Contains(productId)` → true.
2. **#1** — already-running probe via `OpenEventW(SYNCHRONIZE, DHREADY)`.
   Uses raw P/Invoke because legacy DeltaHack payloads created the event
   with a restrictive DACL (SYNCHRONIZE-only) that .NET's
   `EventWaitHandle.OpenExisting` rejects (asks for SYNC+MODIFY → gle=5).
3. **#3** — Windows build 22000..27000 (Win11 21H2..25H2).
4. **#6** — LOCALAPPDATA free space ≥ 100 MB.
5. **#9** — target game not already running (`UAGame.exe`).
   Per-product from `TargetProcMap`.
6. **#2** — Windows Defender realtime enabled → block.
7. **#4** — third-party AV detected → block.
8. `IProductLaunchService.CanLaunchAsync` — license valid (#5).

Any block → `PostDhError(productId, N)` → modal shows Error #N → return.

## Stage 3 — Update + spawn (BackendProductLaunchService)

`ShellBridge` continues:

1. `CheckForUpdateAsync` — GET `/api/public/products/arena-breakout-esp/manifest`.
   If server has newer version → download ZIP → extract to
   `%LOCALAPPDATA%\KoenFlowLauncher\products\arena-breakout-esp\current\`.
   Progress reported via `PostProgress`.
2. `IProductLaunchService.LaunchAsync(new ProductLaunchContext{ProductId})`
   → `BackendProductLaunchService`:
   - GET `/api/public/launch-tokens/preview` — fetches `kfplKey` per release
     (arenahack uses baked key so this is empty; DeltaHack uses per-release
     key that gets injected).
   - `KfplContainer.IsKfplFile(executablePath)` on `WinRuntimeHost.exe` →
     **FALSE** (our stub has MZ header, magic "AHKF" is inside `bundle.kfpl`
     next to it, not in the launch executable). KoenFlow does NOT decrypt.
   - Writes `runtime\launch-contexts\<guid>.json` with product context.
   - `CreateProcessAsUser(WinRuntimeHost.exe, "--koenflow-launch-context <ctx.json>")`
     **elevated** (AlwaysRunProductsElevated=true).
3. Post-launch: launcher watches child PID + calls license/validate every N sec.

## Stage 4 — WinRuntimeHost launcher stub (VMProtected)

`ah_launcher.c` compiled to `WinRuntimeHost.exe`, VMProtect Ultra wraps
`resolve_key` + `aes_gcm_decrypt`. Baked KFPL_KEY[32] hidden inside VM.

`wmain(argc, argv)`:

1. `SetEnvironmentVariableW(DH_INSTALL_DIR, self_dir)` — so overlay's
   `dh_prov_impl.c::provider_probe` finds `<self_dir>\db\<name>.bin`.
2. `bundle_path(...)` = `<self_dir>\bundle.kfpl`.
3. `read_file_all(bundle path)` — 1.5 MB AHKF blob into RAM.
4. Parse KFPL header — magic "AHKF", version=1, nonce 12B, ct_len u64,
   ciphertext, GCM tag 16B.
5. `resolve_key(key32)` — VMProtected. Tries env vars first:
   `AH_KFPL_KEY_B64`, `KFPL_KEY_B64`, `DH_KFPL_KEY_B64`, `KOENFLOW_KFPL_KEY_B64`
   (base64); then HEX equivalents. Falls back to baked `KFPL_KEY[32]`
   (arenahack always uses baked, so env fallback is defensive).
6. `aes_gcm_decrypt` via CNG (BCryptDecrypt) with AAD="AHKF" — 4 KB code
   inside VMProtect Ultra VM bytecode. On success, plaintext = raw
   `ah_overlay.exe` bytes (1.55 MB).
7. `make_random_name(rand_name)` — 12 hex chars via `BCryptGenRandom`.
8. `CreateFileW(%TEMP%\<12hex>.exe)`, `WriteFile(plaintext)`, close.
9. `CreateProcessW(tmp_path, cmdline, DETACHED_PROCESS|CREATE_NO_WINDOW,
   cwd=self_dir)`.
   - `SUBSYSTEM:CONSOLE` overlay + `DETACHED_PROCESS` is safe because
     `overlay_main.c::wmain` has no `printf` calls (would `__fastfail`
     0xC0000409 with no stdio; removed by design).
   - Env inherited from launcher (DH_INSTALL_DIR set at step 4.1).
10. `WaitForSingleObject(pi.hProcess, INFINITE)` — stub blocks until
    overlay exits, returns overlay's exit code.
11. `DeleteFileW(tmp_path)` on exit.

## Stage 5 — Overlay init (`ah_overlay.exe`)

`overlay_main.c::wmain`:

1. **`DhInitHardening()`** (src/hardening/dh_amsi_etw.c):
   - Anti-debug: PEB.BeingDebugged @gs:[0x60]+0x02, then
     `NtQueryInformationProcess(ProcessDebugPort=7)` +
     `NtQueryInformationProcess(ProcessDebugFlags=0x1F)`.
     Any hit → silent `ExitProcess(0)`.
   - AMSI patch — `LoadLibraryA("amsi.dll")`, then patch prologues of
     `AmsiScanBuffer` + `AmsiScanString` to `31 C0 05 57 00 07 80 C3`
     (XOR EAX,EAX ; ADD EAX,0x80070057 ; RET). Non-standard byte pattern
     avoids public YARA hits on `B8 57 00 07 80 C3`.
   - ETW blind — patch `ntdll!EtwEventWrite*` + `NtTraceEvent` to
     `48 33 C0 C3` (XOR RAX,RAX ; RET).
2. `AhOverlayRun()` → `overlay_boot.cpp::AhOverlayRun`:
   - `abi::Overlay::init(sw, sh)` — creates DirectComposition + D3D11 +
     ImGui context. Window class name randomized (`Chrome_WidgetWin:hex`
     pool). `protect_from_capture()` → `SetWindowDisplayAffinity(
     WDA_EXCLUDEFROMCAPTURE)` — blocks OBS/ShadowPlay screen capture.
   - Load `assets\operator.png` for control panel character preview.
   - `ah_reader_start()` — spawns background reader thread.
   - `hotkey_thread` — RegisterHotKey(VK_HOME) message-only window.
   - `ov.run([&](){...})` — 144 Hz render loop.

## Stage 6 — Reader thread (bypass + memory)

`ah_reader_thread.cpp::reader_body` runs on detached thread:

1. `timeBeginPeriod(1)` — Sleep resolution 1 ms.
2. `DhProviderSelect` — kdu shellcode BYOVD. Iterates
   `dh_prov_registry.c::g_providers[]` in priority order:
   - `#26 inpoutx64` (REDFOX protocol) — first-line
   - `#6 EneIo64`, `#N MsIo64`, `#N rtkio64` — fallbacks
   For each: install driver via SCM (or reuse if resident), open device,
   probe an IOCTL. First success wins.
3. `RpmFindSystemCR3` — low-stub scan for kernel DirectoryTableBase.
4. **Signal DHREADY** — `CreateEventW` with **NULL DACL SD**
   (`InitializeSecurityDescriptor` + `SetSecurityDescriptorDacl(TRUE,NULL,FALSE)`)
   → everyone can open. Manual reset, name
   `Global\{DHREADY-4EC7A38D-91B2-4C6A-9F32-DE8B7C51F2A0}` — same as
   DeltaHack so launcher's `WaitForDhReadyAsync` is product-agnostic.
   `SetEvent()` fires.
5. Loop @ 5 ms Sleep (~200 Hz cap):
   - Every ~5 s: rediscover UAGame process → EPROCESS layout via
     `dh_rpm.c::eprocess_layout_table` (Cobalt 22000/22621/22631 vs
     Germanium 26100/26200) → procCR3 + PEB + ImageBase.
   - Self chain: `GWORLD → LocalPlayers[0] → PC → Pawn → Root → ACE decrypt`
     for cam pos (uses `POV.Location` from PCM CameraCache, not root).
   - Scope: `pawn.WM.curWeapon.ZoomComp @+0x418` for lens mag +
     `WeaponCameraComp @+0x148,+0x14C` for effective scope mag +
     ADSSceneFOV + `anim+0x740+0x123C` `CurrentZoomingCameraOffset` for ADS
     drift fix.
   - PlayerArray walk (humans): batch 8 KB pawn read → extract ROOT / MESH
     / CAPSULE / DEATH_COMP / ARMOR_MGR ptrs from local buffer → ACE decrypt
     pos → 7-limb ASC HP → armor tier/durability → cap live (crouch/prone).
   - Bot discovery inline w/ actor scan (vtable+shape probe → g_known_bots).
   - Bot update per tick: pos (plaintext ACE algo=0 direct read) + yaw + cap
     + HP + DeathComp. Corpses freeze at deathpos.
   - Loot chunk scan 64 actors/tick, ItemID lookup via
     `abi::items::lookup()` (14690-entry table).
   - Throttling: HP every 2nd tick, armor every 5th, weapon every 3rd
     (per-pawn cache preserves values on skipped ticks).

## Stage 7 — Launcher waits + modal transitions

While overlay reader initializes (typically 3-10 s from stub spawn):

1. `ShellBridge` returns from `LaunchAsync` → success=true, PID.
2. `WaitForDhReadyAsync(TimeSpan.FromSeconds(30))` — polls every 200 ms via
   `OpenEventW(SYNCHRONIZE)`. Event appears when overlay reader signals.
   On found → `WaitForSingleObject(raw, remainingMs)` → returns
   `WAIT_OBJECT_0` immediately (manual-reset event stays signaled).
3. `PostDhRunning(productId)` → WebSocket `product.dhstate` phase=dh-running.
4. WebUI receives → `dhModalShow('running')` → modal flips to "Close the
   launcher and start the game."
5. `nvx.call` promise resolves → `.then()` handler: `r.success=true`,
   `r.errorCode=null` → skip error branches → modal already correct.

## Stage 8 — Overlay running

Overlay window:
- WDA_EXCLUDEFROMCAPTURE blocks screencap (OBS/ShadowPlay/Discord).
- Random class name (`Chrome_WidgetWin:2a7f9d` etc).
- HOME hotkey via RegisterHotKey (message-only sink window) — toggle menu.
- Reader thread @ ~100-200 Hz publishing to `AH_LIVE_SNAP`.
- Render thread @ 144 Hz reads snap + draws ESP.

## Failure modes → error codes

| # | Cause | User action |
|---|-------|-------------|
| 1 | AHREADY/DHREADY event already exists | Close previous instance |
| 2 | Defender realtime on | Disable Defender realtime |
| 3 | Windows build out of 22000-27000 range | Update Windows |
| 4 | Third-party AV (Kaspersky, Avast, ESET, ...) | Disable AV |
| 5 | License inactive/expired | Renew license |
| 6 | < 100 MB free on LOCALAPPDATA drive | Free space |
| 9 | UAGame.exe already running | Close game, launch loader first |
| 10 | Manifest fetch failed | Check network |
| 11 | Download failed | Check network |
| 12 | Install failed | Retry |
| 13 | Update exception | Retry |
| 14 | payload_corrupt (AV gutted exe) | Whitelist + re-download |
| 15 | LaunchAsync returned !Success | Contact support |
| 16 | DHREADY event never fired within 30 s | Overlay reader init failed (kdu blocked, HVCI reject, etc.) |
| 99 | catch-all / bridge timeout | (v2.2.0 bug — fixed in 2.2.1) |

## Cross-product architecture

Launcher is product-agnostic:
- `DhModalProducts` — one line per product
- `TargetProcMap` — one line per product (game process name)
- `DhReadyEventName` — SHARED constant (all products signal same DHREADY)

Adding new product = 2 lines in ShellBridge.cs + backend catalog entry +
new WinRuntimeHost.zip in admin panel. No new event constants, no new
preflight code paths.

## Anti-detect layers (arenahack v1.0.4)

| Layer | Where |
|-------|-------|
| VMProtect Ultra on launcher `resolve_key` + `aes_gcm_decrypt` | launcher/src/ah_launcher.c |
| KFPL AES-256-GCM (AHKF magic) at rest | scripts/pack_kfpl.py |
| Baked key hidden inside VM bytecode | launcher/bake.py --bake |
| PDB strip + /PDBALTPATH:%%_PDB%% | build.bat |
| DH_RELEASE gate — DH_TRACE/DH_INFO compile to no-op | inc/dh_common.h |
| Per-build unique SHA256 (4-16 KB random padding past PE end) | build.bat post-step |
| AMSI patch (custom XOR+ADD prologue) | src/hardening/dh_amsi_etw.c |
| ETW blind (XOR RAX/RET on Etw* + NtTraceEvent) | src/hardening/dh_amsi_etw.c |
| Anti-debug (PEB.BeingDebugged + NtQIP DebugPort + DebugFlags) | src/hardening/dh_amsi_etw.c |
| Defaults OFF (31 show_* toggles → false at init) | src/abi_ui/render.hpp |
| WDA_EXCLUDEFROMCAPTURE overlay | src/abi_ui/overlay.cpp |
| Random overlay window class per launch | src/abi_ui/overlay.cpp |
| Random temp exe name per spawn | launcher/src/ah_launcher.c |
| DACL null on ready event (cross-elevation open) | src/ah_reader_thread.cpp |

## Ship files

Zip contents (14.94 MB):
```
WinRuntimeHost.exe    — VMProtected launcher stub, baked key
bundle.kfpl           — AES-256-GCM encrypted overlay, AHKF magic
VMProtectSDK64.dll    — VM runtime
assets/operator.png   — character sprite for control panel
db/EneIo64.bin        — kdu vulnerable driver #6
db/inpoutx64.bin      — kdu #26 (REDFOX)
db/MsIo64.bin         — kdu fallback
db/rtkio64.bin        — kdu fallback
```

Upload form (KoenFlow admin panel):
```
VERSION            1.0.x
CHANNEL            stable
PACKAGE            WinRuntimeHost.zip
LAUNCH EXECUTABLE  WinRuntimeHost.exe
LAUNCH ARGUMENTS   (empty)
CONTENT KEY        (empty)
LOADER KEY         (empty)
KFPL KEY           (empty — baked)
Activate now       ON
```
