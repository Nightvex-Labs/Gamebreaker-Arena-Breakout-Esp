"""arenahack: pack + bake helper.

Packs build/ah_overlay.exe into launcher/embed/ah_bundle.kfpl with a fresh
AES-256 key, then rewrites the KFPL_KEY[32] literal inside
launcher/src/ah_launcher.c.

Two modes:
  (default)    — ZERO-KEY mode. Baked key is left all-zeros so the launcher
                 REQUIRES env AH_KFPL_KEY_B64 / _HEX to decrypt. Admin panel
                 stores the real key and passes it via env at Play time.
                 The fresh random key is printed to stdout for the operator
                 to paste into the site's KFPL KEY form field.
  --bake       — Bake the real key into launcher source (dev/local testing).
                 Anyone with the binary can extract it — never use for wide
                 release.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PACK = HERE.parent / "scripts" / "pack_kfpl.py"
INPUT_EXE = HERE.parent / "build" / "ah_overlay.exe"
OUTPUT_KFPL = HERE / "embed" / "bundle.kfpl"
LAUNCHER_C = HERE / "src" / "ah_launcher.c"

ZERO_KEY = bytes(32)


def run_pack() -> bytes:
    r = subprocess.run(
        [sys.executable, str(PACK), str(INPUT_EXE), str(OUTPUT_KFPL)],
        check=True, capture_output=True, text=True,
    )
    print(r.stdout, end="")
    if r.stderr:
        print(r.stderr, end="", file=sys.stderr)
    m = re.search(r"KFPL key \(hex\):\s+([0-9a-fA-F]{64})", r.stdout)
    if not m:
        raise RuntimeError("could not parse key from pack output")
    return bytes.fromhex(m.group(1))


def write_key(key: bytes) -> None:
    body_lines = []
    for row in range(4):
        row_bytes = key[row * 8:(row + 1) * 8]
        body_lines.append("    " + ",".join(f"0x{b:02X}" for b in row_bytes) + ",")
    body = "\n".join(body_lines)

    src = LAUNCHER_C.read_text(encoding="utf-8")
    pattern = r"static const uint8_t KFPL_KEY\[32\] = \{[^}]+\};"
    if not re.search(pattern, src, re.S):
        raise RuntimeError("KFPL_KEY[] not found in launcher.c")
    new = re.sub(
        pattern,
        "static const uint8_t KFPL_KEY[32] = {\n" + body + "\n};",
        src, count=1, flags=re.S,
    )
    if new != src:
        LAUNCHER_C.write_text(new, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bake", action="store_true",
                    help="bake REAL key into launcher source (dev/local only)")
    args = ap.parse_args()

    if not INPUT_EXE.exists():
        print(f"[bake] {INPUT_EXE} missing — build overlay first", file=sys.stderr)
        return 1

    key = run_pack()
    if args.bake:
        write_key(key)
        print(f"[bake] REAL key written into {LAUNCHER_C.name}")
        print("[bake] WARNING — binary now carries decrypt key, anyone can extract")
    else:
        write_key(ZERO_KEY)
        print(f"[bake] ZERO key written into {LAUNCHER_C.name} (admin-panel mode)")
        print()
        print("=" * 72)
        print("  KFPL KEY (paste into site's upload form, KFPL KEY field):")
        print()
        print(f"    hex:    {key.hex()}")
        print()
        import base64
        print(f"    b64:    {base64.b64encode(key).decode()}")
        print()
        print("  Backend must inject env AH_KFPL_KEY_B64 or _HEX at Play time.")
        print("=" * 72)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
