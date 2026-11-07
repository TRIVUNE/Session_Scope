"""Check the macOS sensor bundles with each format's official validator.

    python tools/build_native_mac.py      # first: builds build/native/mac/
    python tools/check_mac_plugins.py

1. VST3: Steinberg's validator, built from the VST3 SDK with CMake (once, into build/vst3sdk-mac/).
2. CLAP: clap-validator plus the test host that reads the sensor's records (run_clap_validator.py).
3. Audio Unit: Apple's auval, after copying the component into ~/Library/Audio/Plug-Ins/Components,
   where macOS looks for it.

Runs on a Mac (CI's macOS job runs it). Exits non-zero if any check fails; prints which.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import build_native_mac as mac  # noqa: E402

VST3_BUILD = REPO / "build" / "vst3sdk-mac"
COMPONENTS = Path.home() / "Library" / "Audio" / "Plug-Ins" / "Components"


def check_vst3() -> bool:
    bundle = mac.OUT / "SessionScope.vst3"
    if not bundle.exists():
        print("VST3: no bundle to check")
        return False
    validator = next(VST3_BUILD.glob("bin/**/validator"), None) if VST3_BUILD.exists() else None
    if validator is None:
        # Only the validator: no GUI library, no example plug-ins.
        subprocess.run(["cmake", "-S", str(mac.VST3_SDK), "-B", str(VST3_BUILD), "-DCMAKE_BUILD_TYPE=Release",
                        "-DSMTG_ENABLE_VSTGUI_SUPPORT=OFF", "-DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF"], check=True)
        subprocess.run(["cmake", "--build", str(VST3_BUILD), "--target", "validator", "--config", "Release",
                        "--parallel"], check=True)
        validator = next(VST3_BUILD.glob("bin/**/validator"))
    result = subprocess.run([str(validator), str(bundle)], capture_output=True, text=True)
    output = result.stdout + result.stderr
    summary = [line for line in output.splitlines() if "tests passed" in line or "tests failed" in line]
    print("VST3 validator: " + ("; ".join(summary) if summary else "no summary"))
    if result.returncode != 0:
        print(output[-3000:])
    return result.returncode == 0


def check_clap() -> bool:
    return subprocess.run([sys.executable, str(REPO / "tools" / "run_clap_validator.py")]).returncode == 0


def check_au() -> bool:
    bundle = mac.OUT / "SessionScope.component"
    if not bundle.exists():
        print("Audio Unit: no component to check")
        return False
    installed = COMPONENTS / bundle.name
    COMPONENTS.mkdir(parents=True, exist_ok=True)
    if installed.exists():
        shutil.rmtree(installed)
    shutil.copytree(bundle, installed)
    # macOS caches the list of components; restarting its registrar makes it look again.
    subprocess.run(["killall", "-9", "AudioComponentRegistrar"], capture_output=True)
    result = subprocess.run(["auval", "-v", mac.AU_TYPE, mac.AU_SUBTYPE, mac.AU_MANUFACTURER],
                            capture_output=True, text=True)
    output = result.stdout + result.stderr
    passed = result.returncode == 0 and "AU VALIDATION SUCCEEDED" in output
    print("auval: " + ("AU VALIDATION SUCCEEDED" if passed else "failed"))
    if not passed:
        print(output[-4000:])
    return passed


def main() -> int:
    if sys.platform != "darwin":
        sys.exit("This checks macOS bundles: run it on a Mac (or let CI's macOS job do it).")
    results = {"VST3": check_vst3(), "CLAP": check_clap(), "Audio Unit": check_au()}
    failed = [name for name, ok in results.items() if not ok]
    print("all formats passed" if not failed else "failed: " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
