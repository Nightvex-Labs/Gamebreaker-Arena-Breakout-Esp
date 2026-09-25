"""arenahack KFPL packer.

Wraps ah_overlay.exe in an AES-256-GCM container. Format identical to
DeltaHack's DHKF blob but MAGIC = "AHKF" so the two families of packed
blobs can never accidentally cross-load between products.

    Offset  0   Magic     "AHKF"                    4 bytes
    Offset  4   Version   u32 LE = 1                4 bytes
    Offset  8   Nonce                              12 bytes (fresh random)
    Offset 20   Ciphertext length (u64 LE)          8 bytes
    Offset 28   Ciphertext (AES-256-GCM)            N bytes
    Offset 28+N GCM tag                            16 bytes

Usage:
  python scripts/pack_kfpl.py build/ah_overlay.exe launcher/embed/ah_bundle.kfpl
  python scripts/pack_kfpl.py <in> <out> --key-hex <64hex>
"""
from __future__ import annotations

import base64
import os
import secrets
import struct
import sys

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    print("[!] pip install cryptography", file=sys.stderr)
    sys.exit(2)

MAGIC = b"AHKF"  # arenahack-specific — bundle.kfpl NOT touched by KoenFlow
                 # launcher (executablePath = WinRuntimeHost.exe has MZ, not
                 # KFPL magic). Our launcher stub does the decrypt.
VERSION = 1
NONCE_LEN = 12
TAG_LEN = 16


def pack(payload: bytes, key: bytes) -> bytes:
    nonce = os.urandom(NONCE_LEN)
    gcm = AESGCM(key)
    ct_and_tag = gcm.encrypt(nonce, payload, associated_data=MAGIC)
    ct = ct_and_tag[:-TAG_LEN]
    tag = ct_and_tag[-TAG_LEN:]
    header = MAGIC + struct.pack("<I", VERSION) + nonce + struct.pack("<Q", len(ct))
    return header + ct + tag


def main() -> int:
    args = sys.argv[1:]
    if len(args) < 2:
        print("usage: pack_kfpl.py <input.exe> <output.kfpl> "
              "[--key-hex HEX | --key-b64 B64]", file=sys.stderr)
        return 2

    src, dst = args[0], args[1]
    rest = args[2:]

    if len(rest) == 2 and rest[0] == "--key-hex":
        key = bytes.fromhex(rest[1].strip())
        src_desc = "reused (--key-hex)"
    elif len(rest) == 2 and rest[0] == "--key-b64":
        key = base64.b64decode(rest[1].strip())
        src_desc = "reused (--key-b64)"
    elif rest:
        print(f"[!] unknown args: {rest}", file=sys.stderr)
        return 2
    else:
        # Generate a "safe" base64 — no '+' or '/' so the HTML-escaper in
        # C# WriteLaunchContextFile (default JsonSerializer options) doesn't
        # turn them into + / /, which our C-side plain-string
        # parser in ah_launcher.c does not resolve. 25% probability per try,
        # ~0.01% failure at 30 tries.
        for _ in range(200):
            key = secrets.token_bytes(32)
            b64 = base64.b64encode(key).decode()
            if '+' not in b64 and '/' not in b64:
                break
        src_desc = "fresh random (safe-b64, no +/)"

    if len(key) != 32:
        print(f"[!] KFPL key must be 32 bytes, got {len(key)}", file=sys.stderr)
        return 2

    with open(src, "rb") as f:
        payload = f.read()
    blob = pack(payload, key)

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(blob)

    print(f"[+] packed {src} ({len(payload):,} B) -> {dst} ({len(blob):,} B)")
    print(f"    key source: {src_desc}")
    print(f"    KFPL key (hex):    {key.hex()}")
    print(f"    KFPL key (base64): {base64.b64encode(key).decode()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
