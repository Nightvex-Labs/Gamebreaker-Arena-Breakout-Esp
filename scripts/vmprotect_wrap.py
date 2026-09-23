"""arenahack VMProtect wrap step.

Runs VMProtect_Con.exe on launcher/build/App.exe, virtualizing every function
that was marked with VMProtectBeginUltra("...") in the source. The wrap is
in-place (App.exe overwritten). VMProtectSDK64.dll must ship alongside the
wrapped exe.

Project file is minimal — VMProtect discovers markers from the exe itself.

Usage:
  python scripts/vmprotect_wrap.py                  # wrap launcher/build/App.exe
  python scripts/vmprotect_wrap.py --exe path.exe   # wrap arbitrary exe
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
LAUNCHER_EXE = ROOT / "launcher" / "build" / "WinRuntimeHost.exe"

VMP_CANDIDATES = [
    Path(r"C:\vmp\notVmpFull\notVmp\VMProtect_Con.exe"),
    Path(r"C:\vmp\notVmp\VMProtect_Con.exe"),
]


def find_vmp() -> Path:
    for p in VMP_CANDIDATES:
        if p.exists():
            return p
    raise RuntimeError("VMProtect_Con.exe not found; searched " + str(VMP_CANDIDATES))


def make_project(exe: Path, out: Path) -> Path:
    """Generate a minimal .vmp project pointing at input + output paths."""
    vmp = exe.with_suffix(".vmp")
    root = ET.Element("Document")
    ET.SubElement(root, "InputFileName").text  = str(exe)
    ET.SubElement(root, "OutputFileName").text = str(out)
    tree = ET.ElementTree(root)
    tree.write(vmp, encoding="utf-8", xml_declaration=True)
    return vmp


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(LAUNCHER_EXE))
    args = ap.parse_args()

    exe = Path(args.exe).resolve()
    if not exe.exists():
        print(f"[!] {exe} missing", file=sys.stderr); return 1

    vmp_bin = find_vmp()
    print(f"[wrap] VMProtect: {vmp_bin}")
    print(f"[wrap] input:     {exe}")
    size_before = exe.stat().st_size
    print(f"[wrap] size before: {size_before:,} B")

    # VMProtect_Con always writes to <input>.vmp.<ext> regardless of what the
    # .vmp project's OutputFileName says. Accept that + swap in place after.
    default_out = exe.parent / (exe.stem + ".vmp" + exe.suffix)
    if default_out.exists(): default_out.unlink()
    vmp_project = make_project(exe, default_out)
    print(f"[wrap] project:   {vmp_project}")
    print(f"[wrap] output:    {default_out}")

    cmd = [str(vmp_bin), str(exe), "-pf", str(vmp_project)]
    print(f"[run] {' '.join(cmd)}")
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout)
    if r.stderr:
        print(r.stderr, file=sys.stderr)
    if r.returncode != 0:
        print(f"[!] wrap failed rc={r.returncode}", file=sys.stderr)
        return 2
    if not default_out.exists():
        print(f"[!] VMProtect ran but {default_out.name} not produced", file=sys.stderr)
        return 3

    # Replace original with wrapped output.
    exe.unlink()
    default_out.rename(exe)
    size_after = exe.stat().st_size
    print(f"[wrap] size after:  {size_after:,} B  (delta {size_after-size_before:+,d} B)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
