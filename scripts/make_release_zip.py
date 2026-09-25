"""arenahack release packer — DeltaHack shape.

Layout (matches C:\\DeltaHack\\release\\WinRuntimeHost.zip):
  WinRuntimeHost.zip
    ├── WinRuntimeHost.exe    — VMProtected launcher stub (baked KFPL key)
    ├── bundle.kfpl           — AES-256-GCM(overlay), magic "AHKF"
    ├── VMProtectSDK64.dll    — VM runtime for the launcher
    ├── assets/operator.png
    └── db/*.bin              — kdu vulnerable-driver payloads

Flow at Play:
  1. KoenFlow launcher runs WinRuntimeHost.exe (MZ header, not KFPL magic →
     KoenFlow does not touch it).
  2. Launcher stub reads bundle.kfpl beside itself, decrypts AHKF blob using
     BAKED KFPL_KEY[32] (hidden behind VMProtect Ultra on resolve_key +
     aes_gcm_decrypt).
  3. Launcher writes decrypted overlay to %TEMP%\\<hex>.exe, spawns it
     detached, waits, deletes on exit.

Admin panel KFPL KEY field can stay empty — key is baked (rotates per build).

Usage:
  python scripts/make_release_zip.py --version 1.0.0
  python scripts/make_release_zip.py --version 1.0.0 --no-overlay-build
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


ROOT          = Path(__file__).resolve().parent.parent
OVERLAY_EXE   = ROOT / "build" / "ah_overlay.exe"
OVERLAY_BUILD = ROOT / "build.bat"
LAUNCHER_DIR  = ROOT / "launcher"
LAUNCHER_EXE  = LAUNCHER_DIR / "build" / "WinRuntimeHost.exe"
BUNDLE_KFPL   = LAUNCHER_DIR / "embed" / "bundle.kfpl"
ASSETS_DIR    = ROOT / "assets"
DB_DIR        = ROOT / "src" / "db"
RELEASES      = ROOT / "releases"


def run(cmd, cwd=None):
    print(f"[run] {' '.join(str(x) for x in cmd)}")
    r = subprocess.run(cmd, cwd=cwd, shell=False)
    if r.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(str(x) for x in cmd)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--channel", default="stable")
    ap.add_argument("--no-overlay-build", action="store_true",
                    help="Skip overlay build; use existing build/ah_overlay.exe.")
    ap.add_argument("--zero-key", action="store_true",
                    help="Ship-mode: baked KFPL_KEY[32] = all zeros. Launcher "
                         "CANNOT decrypt bundle without env AH_KFPL_KEY_B64. "
                         "Standalone double-click of WinRuntimeHost.exe fails. "
                         "The generated key is printed for the admin panel form.")
    args = ap.parse_args()

    # 1. Overlay build.
    if not args.no_overlay_build or not OVERLAY_EXE.exists():
        print("[step] building overlay")
        run(["cmd.exe", "/c", str(OVERLAY_BUILD)], cwd=ROOT)
    if not OVERLAY_EXE.exists():
        print(f"[!] missing {OVERLAY_EXE}", file=sys.stderr); return 1

    # 2. Pack + key material.
    if args.zero_key:
        print("[step] KFPL wrap — ZERO-key mode (baked key = all zeros; env-injected required)")
        run([sys.executable, str(LAUNCHER_DIR / "bake.py")], cwd=LAUNCHER_DIR)
    else:
        print("[step] KFPL wrap + BAKE real key into launcher source (dev / local test)")
        run([sys.executable, str(LAUNCHER_DIR / "bake.py"), "--bake"], cwd=LAUNCHER_DIR)

    # 3. Launcher build.
    print("[step] building launcher stub (WinRuntimeHost.exe)")
    run(["cmd.exe", "/c", str(LAUNCHER_DIR / "build.bat")], cwd=LAUNCHER_DIR)
    if not LAUNCHER_EXE.exists():
        print(f"[!] missing {LAUNCHER_EXE}", file=sys.stderr); return 2

    # 4. VMProtect wrap ONLY launcher stub — small binary, marked functions
    # (resolve_key, aes_gcm_decrypt) get virtualized. Key + decrypt hidden.
    print("[step] VMProtect wrap WinRuntimeHost.exe")
    run([sys.executable, str(ROOT / "scripts" / "vmprotect_wrap.py")], cwd=ROOT)

    # 5. Assemble ZIP (flat, DeltaHack-shape).
    # assets/ is intentionally NOT included: operator.png is now embedded
    # into the overlay binary via inc/operator_png_data.h. Removing the
    # sidecar reduces the shipped surface (one less thing an AV can flag or
    # a user can delete/replace).
    stage = ROOT / "release_staging"
    if stage.exists(): shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)
    (stage / "db").mkdir(exist_ok=True)

    # Single-file distribution: append bundle.kfpl to the tail of the VMProtected
    # WinRuntimeHost.exe under an "AHBE" trailer marker. The launcher reads
    # itself at runtime (GetModuleFileName + tail seek), extracts the bundle
    # blob past its own PE end, and decrypts as before — no sidecar file on
    # disk. Windows loader ignores bytes past the last section so the file
    # still runs as a normal MZ .exe.
    #
    # Trailer format (12 bytes at end of file):
    #   [ bundle bytes (N)       ]
    #   [ u64 LE  bundle_size    ]  8 bytes
    #   [ 4 byte magic "AHBE"    ]  4 bytes
    stage_exe = stage / "WinRuntimeHost.exe"
    with open(LAUNCHER_EXE, "rb") as f: stub_bytes = f.read()
    with open(BUNDLE_KFPL,  "rb") as f: bundle_bytes = f.read()
    import struct
    trailer = struct.pack("<Q", len(bundle_bytes)) + b"AHBE"
    with open(stage_exe, "wb") as f:
        f.write(stub_bytes)
        f.write(bundle_bytes)
        f.write(trailer)
    print(f"[pack] embedded bundle into WinRuntimeHost.exe "
          f"(stub={len(stub_bytes):,} + bundle={len(bundle_bytes):,} + trailer=12 = "
          f"{stage_exe.stat().st_size:,} B)")

    # VMProtectSDK64.dll — NOT shipped. After VMProtect_Con wraps the launcher,
    # marker calls are replaced by virtualized bytecode; dumpbin /imports on
    # the wrapped exe shows no VMProtectSDK64.dll entry. Empirical test on
    # 25H2: launcher runs identically without the DLL present.

    n_bins = 0
    for p in DB_DIR.glob("*.bin"):
        shutil.copyfile(p, stage / "db" / p.name)
        n_bins += 1
    print(f"[pack] included {n_bins} driver .bin payloads under db/")

    payload = OVERLAY_EXE.read_bytes()
    print(f"[pack] overlay:  {len(payload):>10,} B  sha256 {hashlib.sha256(payload).hexdigest()[:16]}...")
    print(f"[pack] launcher: {LAUNCHER_EXE.stat().st_size:>10,} B  WinRuntimeHost.exe (VMPed)")
    print(f"[pack] bundle:   {BUNDLE_KFPL.stat().st_size:>10,} B  bundle.kfpl")

    RELEASES.mkdir(exist_ok=True)
    zip_path = RELEASES / "WinRuntimeHost.zip"
    if zip_path.exists(): zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in stage.rglob("*"):
            if p.is_file():
                z.write(p, p.relative_to(stage))
    shutil.rmtree(stage)
    zip_size = zip_path.stat().st_size

    print()
    print("=" * 72)
    print(f"  READY:  {zip_path}")
    print(f"  Size:   {zip_size:,} bytes ({zip_size/1024/1024:.2f} MB)")
    print()
    if args.zero_key:
        print("  Upload form — ZERO-KEY ship mode (paste b64 into KFPL KEY):")
    else:
        print("  Upload form (KFPL KEY can stay empty — key baked into launcher):")
    print(f"    VERSION            {args.version}")
    print(f"    CHANNEL            {args.channel}")
    print(f"    PACKAGE            {zip_path.name}")
    print(f"    LAUNCH EXECUTABLE  WinRuntimeHost.exe")
    print(f"    LAUNCH ARGUMENTS   (empty)")
    print(f"    CONTENT KEY        (empty)")
    print(f"    LOADER KEY         (empty)")
    if args.zero_key:
        print(f"    KFPL KEY           (paste b64 printed by bake.py above)")
    else:
        print(f"    KFPL KEY           (empty — baked)")
    print(f"    Activate now       ON")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
