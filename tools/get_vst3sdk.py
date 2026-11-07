"""Fetch Steinberg's VST3 SDK (MIT licence) into .toolchain/vst3sdk with git.

    python tools/get_vst3sdk.py

Only the SDK's plain C++ sources are compiled (native/vst3_sensor); no installer or GUI parts.
"""

import subprocess
import sys
from pathlib import Path

DEST = Path(__file__).resolve().parents[1] / ".toolchain" / "vst3sdk"

if DEST.exists():
    print(f"VST3 SDK already at {DEST}")
    sys.exit(0)
subprocess.run(["git", "clone", "--depth", "1", "--recurse-submodules", "--shallow-submodules",
                "https://github.com/steinbergmedia/vst3sdk.git", str(DEST)], check=True)
