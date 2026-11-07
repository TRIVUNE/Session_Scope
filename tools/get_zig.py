"""Fetch the pinned Zig toolchain (a self-contained clang for C and C++) into .toolchain/.

    python tools/get_zig.py

Zig is only the compiler driver for the native pieces (native/build.py); nothing in SessionScope
is written in Zig. MSVC or any clang works too: see native/README.md.
"""

from __future__ import annotations

import hashlib
import platform
import sys
import urllib.request
import zipfile
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
VERSION = "0.16.0"
# From https://ziglang.org/download/index.json (checked 27 Oct 2026).
BUILDS = {
    ("win32", "AMD64"): ("zig-x86_64-windows-0.16.0.zip",
                         "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"),
}


def zig_path() -> Path | None:
    for p in (REPO / ".toolchain").glob(f"zig-*-{VERSION}/zig*"):
        if p.name in ("zig", "zig.exe"):
            return p
    return None


def main():
    if zig_path():
        print(f"Zig {VERSION} already at {zig_path()}")
        return
    key = (sys.platform, platform.machine())
    if key not in BUILDS:
        raise SystemExit(f"No pinned Zig build for {key}; install a C++ compiler and see native/README.md.")
    name, sha = BUILDS[key]
    dest = REPO / ".toolchain"
    dest.mkdir(exist_ok=True)
    archive = dest / name
    if not archive.exists():
        url = f"https://ziglang.org/download/{VERSION}/{name}"
        print(f"Downloading {url}")
        urllib.request.urlretrieve(url, archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != sha:
        archive.unlink()
        raise SystemExit(f"Checksum mismatch for {name}: got {digest}, expected {sha}. Deleted it.")
    print("Checksum ok, unpacking")
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dest, filter="data")
    archive.unlink()
    print(f"Zig {VERSION} at {zig_path()}")


if __name__ == "__main__":
    main()
