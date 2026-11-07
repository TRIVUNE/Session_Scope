"""Check the CLAP sensor: the official clap-validator, then a small host that reads its records.

    python tools/run_clap_validator.py

Needs build/native/SessionScope.clap (python tools/build_native.py) and the CLAP headers and
validator (python tools/get_clap.py). The second check uses the real shared ring, so close any
DAW that has the sensor loaded first. Exits non-zero if either check fails.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import get_clap  # noqa: E402
import get_zig  # noqa: E402

IS_MAC = sys.platform == "darwin"
# Windows: a single file. macOS: a bundle, built by tools/build_native_mac.py.
PLUGIN = REPO / "build" / "native" / ("mac/SessionScope.clap" if IS_MAC else "SessionScope.clap")
HOST_SOURCE = REPO / "native" / "clap_sensor" / "test_clap_sensor.cpp"
HOST_EXE = REPO / "build" / "native" / ("test_clap_sensor" if IS_MAC else "test_clap_sensor.exe")


def run_validator() -> bool:
    """clap-validator: every test must pass or be skipped (skips are features the sensor lacks)."""
    result = subprocess.run([str(get_clap.VALIDATOR_EXE), "validate", "--json", str(PLUGIN)],
                            capture_output=True, text=True)
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError:
        print(result.stdout + result.stderr)
        print("clap-validator: no readable result")
        return False

    counts: dict[str, int] = {}
    failed = []
    for result_entry in report.get("results", []):
        # {"test": {<kind>: {"test": name, ...}}, "status": {"code": ..., "details": ...}}
        name = next(iter(result_entry["test"].values()))["test"]
        code = result_entry["status"]["code"]
        counts[code] = counts.get(code, 0) + 1
        if code not in ("success", "skipped"):
            failed.append(f"{name}: {result_entry['status'].get('details') or code}")
    print("clap-validator: " + ", ".join(f"{n} {code}" for code, n in sorted(counts.items())))
    for line in failed:
        print(f"  {line}")
    return not failed and counts.get("success", 0) > 0


def run_host_test() -> bool:
    """Build the test host if needed, then play blocks through the sensor and read them back."""
    if IS_MAC:
        cxx = ["clang++"]
    else:
        zig = get_zig.zig_path()
        if zig is None:
            sys.exit("No compiler: run python tools/get_zig.py first.")
        cxx = [str(zig), "c++"]
    if not HOST_EXE.exists() or HOST_SOURCE.stat().st_mtime > HOST_EXE.stat().st_mtime:
        HOST_EXE.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([*cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-isystem", str(get_clap.CLAP_DEST / "include"),
                        str(HOST_SOURCE), "-o", str(HOST_EXE)], check=True)
    result = subprocess.run([str(HOST_EXE), str(PLUGIN)], capture_output=True, text=True)
    print("test host: " + (result.stdout + result.stderr).strip())
    return result.returncode == 0


def main() -> int:
    if not PLUGIN.exists():
        sys.exit(f"{PLUGIN.relative_to(REPO)} is missing: python tools/build_native.py")
    if not get_clap.VALIDATOR_EXE.exists():
        get_clap.get_validator()
    if not get_clap.VALIDATOR_EXE.exists():
        sys.exit("clap-validator is missing: python tools/get_clap.py")
    validator_ok = run_validator()
    host_ok = run_host_test()
    return 0 if validator_ok and host_ok else 1


if __name__ == "__main__":
    sys.exit(main())
