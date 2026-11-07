"""Fetch GLFW and Dear ImGui into .toolchain/ with pinned commits.

    python tools/get_hud_deps.py

GLFW 3.4 (zlib licence) from https://github.com/glfw/glfw
Dear ImGui v1.91.9 (MIT licence) from https://github.com/ocornut/imgui
stb_image_write.h v1.16 (public domain) from https://github.com/nothings/stb, for screenshots
Inter 4.1 (SIL Open Font Licence) from https://github.com/rsms/inter: two static weights, compiled
into the HUD by build_engine.py

Skips what is already there. After cloning, verifies the commit against pinned hashes
and stops with an error if it differs. Also checks existing folders.
"""

import hashlib
import io
import subprocess
import sys
import urllib.request
import zipfile
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


# Inter: the release zip, checked by SHA-256; only the two weights the HUD uses and the licence
# are kept, in .toolchain/inter.
INTER_DIR = TOOLCHAIN / "inter"
INTER_URL = "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
INTER_SHA256 = "9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e"
INTER_FILES = ["extras/ttf/InterDisplay-Light.ttf", "extras/ttf/Inter-Regular.ttf", "LICENSE.txt"]


def get_inter() -> None:
    if all((INTER_DIR / Path(name).name).exists() for name in INTER_FILES):
        print("Inter present")
        return
    print(f"Downloading {INTER_URL}")
    with urllib.request.urlopen(INTER_URL) as response:
        data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != INTER_SHA256:
        raise SystemExit(f"Inter zip: SHA-256 {digest}, expected {INTER_SHA256}.")
    INTER_DIR.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for name in INTER_FILES:
            (INTER_DIR / Path(name).name).write_bytes(archive.read(name))
    print("Inter verified")


def main() -> int:
    TOOLCHAIN.mkdir(exist_ok=True)

    clone_if_missing(GLFW_DEST, GLFW_URL, GLFW_TAG)
    clone_if_missing(IMGUI_DEST, IMGUI_URL, IMGUI_TAG)

    verify_commit(GLFW_DEST, GLFW_COMMIT, "GLFW")
    verify_commit(IMGUI_DEST, IMGUI_COMMIT, "ImGui")
    get_stb()
    get_inter()

    print("All dependencies ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())