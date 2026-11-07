"""Fetch GLFW and Dear ImGui into .toolchain/ with pinned commits.

    python tools/get_hud_deps.py

GLFW 3.4 (zlib licence) from https://github.com/glfw/glfw
Dear ImGui v1.91.9 (MIT licence) from https://github.com/ocornut/imgui
stb_image_write.h v1.16 (public domain) from https://github.com/nothings/stb, for screenshots

Skips what is already there. After cloning, verifies the commit against pinned hashes
and stops with an error if it differs. Also checks existing folders.
"""

import hashlib
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOLCHAIN = REPO / ".toolchain"

GLFW_DEST = TOOLCHAIN / "glfw"
GLFW_URL = "https://github.com/glfw/glfw"
GLFW_TAG = "3.4"
GLFW_COMMIT = "7b6aead9fb88b3623e3b3725ebb42670cbe4c579"

IMGUI_DEST = TOOLCHAIN / "imgui"
IMGUI_URL = "https://github.com/ocornut/imgui"
IMGUI_TAG = "v1.91.9"
IMGUI_COMMIT = "97428e8ac99e339ce05eee531cf55b77b29ea709"

# stb has no release tags: one file at a pinned commit, checked by SHA-256.
STB_FILE = TOOLCHAIN / "stb" / "stb_image_write.h"
STB_URL = ("https://raw.githubusercontent.com/nothings/stb/"
           "d14d46f6b44456eb88beac58cba833a4135d4c89/stb_image_write.h")
STB_SHA256 = "cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05"


def clone_if_missing(dest: Path, url: str, tag: str) -> None:
    if dest.exists():
        print(f"Already exists: {dest}")
        return
    print(f"Cloning {url} @ {tag} into {dest}")
    subprocess.run(
        ["git", "clone", "--depth", "1", "--branch", tag, url, str(dest)],
        check=True,
    )


def verify_commit(dest: Path, expected: str, name: str) -> None:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=dest,
        capture_output=True,
        text=True,
        check=True,
    )
    actual = result.stdout.strip()
    if actual != expected:
        raise SystemExit(
            f"{name} commit mismatch: got {actual}, expected {expected}. "
            f"Delete {dest} and re-run."
        )
    print(f"{name} commit verified: {actual[:12]}")


def get_stb() -> None:
    if not STB_FILE.exists():
        print(f"Downloading {STB_URL}")
        STB_FILE.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(STB_URL) as response:
            STB_FILE.write_bytes(response.read())
    digest = hashlib.sha256(STB_FILE.read_bytes()).hexdigest()
    if digest != STB_SHA256:
        raise SystemExit(f"stb_image_write.h: SHA-256 {digest}, expected {STB_SHA256}. Delete {STB_FILE} and re-run.")
    print("stb_image_write.h verified")


def main() -> int:
    TOOLCHAIN.mkdir(exist_ok=True)

    clone_if_missing(GLFW_DEST, GLFW_URL, GLFW_TAG)
    clone_if_missing(IMGUI_DEST, IMGUI_URL, IMGUI_TAG)

    verify_commit(GLFW_DEST, GLFW_COMMIT, "GLFW")
    verify_commit(IMGUI_DEST, IMGUI_COMMIT, "ImGui")
    get_stb()

    print("All dependencies ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())