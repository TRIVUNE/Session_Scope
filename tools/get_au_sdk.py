"""Fetch Apple's AudioUnitSDK (Apache 2.0 licence) into .toolchain/AudioUnitSDK, pinned.

    python tools/get_au_sdk.py

AudioUnitSDK 1.4.0 from https://github.com/apple/AudioUnitSDK: the C++ base classes the Audio Unit
sensor (native/au_sensor) is built on. Only needed on macOS. Stops with an error if the commit
differs from the pinned one.
"""

import subprocess
import sys
from pathlib import Path

DEST = Path(__file__).resolve().parents[1] / ".toolchain" / "AudioUnitSDK"
URL = "https://github.com/apple/AudioUnitSDK"
TAG = "AudioUnitSDK-1.4.0"
COMMIT = "bd98b31feff57a15989fcfab4cd86dc63382b1ac"


def main() -> int:
    if not DEST.exists():
        subprocess.run(["git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1",
                        "--branch", TAG, URL, str(DEST)], check=True)
    actual = subprocess.run(["git", "-C", str(DEST), "rev-parse", "HEAD"],
                            check=True, capture_output=True, text=True).stdout.strip()
    if actual != COMMIT:
        sys.exit(f"AudioUnitSDK: expected commit {COMMIT}, found {actual}. Delete {DEST} and run this again.")
    print(f"AudioUnitSDK {TAG} ready ({actual[:12]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
