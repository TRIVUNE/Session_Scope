"""Fetch the CLAP headers and clap-validator into .toolchain/, pinned and checked.

    python tools/get_clap.py

CLAP 1.2.9 (MIT licence) from https://github.com/free-audio/clap: headers only, nothing compiled.
clap-validator 0.4.1 (MIT licence) from https://github.com/free-audio/clap-validator: the
official test program for CLAP plug-ins, used by tools/run_clap_validator.py (Windows and macOS builds).

Skips what is already there. Stops with an error if the CLAP commit or the validator's SHA-256
differs from the pinned one.
"""

from __future__ import annotations

import hashlib
import io
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOLCHAIN = REPO / ".toolchain"

CLAP_DEST = TOOLCHAIN / "clap"
CLAP_URL = "https://github.com/free-audio/clap"
CLAP_TAG = "1.2.9"
CLAP_COMMIT = "cd94482ba5941ae410809b6fbaed3bc851044270"

VALIDATOR_DEST = TOOLCHAIN / "clap-validator"
VALIDATOR_EXE = VALIDATOR_DEST / ("clap-validator.exe" if sys.platform == "win32" else "clap-validator")
VALIDATOR_RELEASE = "https://github.com/free-audio/clap-validator/releases/download/0.4.1/"
# Per platform: the release file and its SHA-256, checked before anything is unpacked.
VALIDATOR_DOWNLOADS = {
    "win32": ("clap-validator-0.4.1-127-g152b982-windows.zip",
              "d935c3af0a45c3911ea2e900f4aa5d6709dac82bb485f0c4ce28648ab2cd0c10"),
    "darwin": ("clap-validator-0.4.1-127-g152b982-macos-universal.zip",
               "bbec8cd7d18274e549d5d8c12ece3cec54be966129388dd2e742b9957f2ba9f1"),
}


def get_headers() -> None:
    if not CLAP_DEST.exists():
        subprocess.run(["git", "clone", "--quiet", "--depth", "1", "--branch", CLAP_TAG, CLAP_URL, str(CLAP_DEST)],
                       check=True)
    actual = subprocess.run(["git", "-C", str(CLAP_DEST), "rev-parse", "HEAD"],
                            check=True, capture_output=True, text=True).stdout.strip()
    if actual != CLAP_COMMIT:
        sys.exit(f"CLAP: expected commit {CLAP_COMMIT}, found {actual}. Delete {CLAP_DEST} and run this again.")
    print(f"CLAP {CLAP_TAG} headers ready ({actual[:12]})")


def validator_program(archive: bytes) -> bytes:
    """The program inside the release file. The Windows zip holds the .exe directly; the macOS zip
    holds a .tar.gz that holds it."""
    with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
        if "clap-validator.exe" in zipped.namelist():
            return zipped.read("clap-validator.exe")
        tarball = zipped.read(next(n for n in zipped.namelist() if n.endswith(".tar.gz")))
    with tarfile.open(fileobj=io.BytesIO(tarball), mode="r:gz") as tar:
        member = next(m for m in tar.getmembers() if m.isfile() and m.name.endswith("clap-validator"))
        return tar.extractfile(member).read()


def get_validator() -> None:
    if VALIDATOR_EXE.exists():
        print(f"clap-validator ready ({VALIDATOR_EXE})")
        return
    if sys.platform not in VALIDATOR_DOWNLOADS:
        print(f"clap-validator: no pinned build for {sys.platform}; skipped")
        return
    name, expected = VALIDATOR_DOWNLOADS[sys.platform]
    with urllib.request.urlopen(VALIDATOR_RELEASE + name) as response:
        archive = response.read()
    digest = hashlib.sha256(archive).hexdigest()
    if digest != expected:
        sys.exit(f"clap-validator: expected SHA-256 {expected}, got {digest}. Not installed.")
    VALIDATOR_DEST.mkdir(parents=True, exist_ok=True)
    VALIDATOR_EXE.write_bytes(validator_program(archive))
    VALIDATOR_EXE.chmod(0o755)
    print(f"clap-validator ready ({VALIDATOR_EXE}, SHA-256 checked)")


def main() -> int:
    TOOLCHAIN.mkdir(exist_ok=True)
    get_headers()
    get_validator()
    return 0


if __name__ == "__main__":
    sys.exit(main())
