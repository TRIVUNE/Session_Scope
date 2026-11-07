"""Run the SessionScope REAPER validation headlessly, in an isolated resource folder.

    python hosts/reaper/tools/reaper_validate.py [--reaper PATH] [--timeout SECONDS] [--keep]

REAPER is started with -cfgfile pointing at build/reaper-validate/reaper.ini, so it uses that
folder as its resource directory: its own settings, the dummy audio device (silent, no driver),
and links to this repository's JSFX and scripts. Your normal REAPER settings are not read or
changed. If build/native/reaper_sessionscope.dll exists (python tools/build_native.py), it is
loaded too, from that folder's UserPlugins only.

hosts/reaper/tests/integration/validate.lua does the work inside REAPER; this script prepares the
folder, starts REAPER, waits, and prints what it found, followed by the analyser's report on each
trace (build/engine/sessionscope.exe, built by python tools/build_engine.py). The first run in a
new folder is slow: REAPER scans the computer's plugins once and keeps the result there. If REAPER
has not finished within the timeout it is stopped.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import reaper_install  # noqa: E402

VALIDATE = REPO / "hosts" / "reaper" / "tests" / "integration" / "validate.lua"
EXTENSION = REPO / "build" / "native" / "reaper_sessionscope.dll"
ANALYSER = REPO / "build" / "engine" / "sessionscope.exe"   # python tools/build_engine.py
DEFAULT_REAPER = {
    "win32": Path(r"C:\Program Files\REAPER (x64)\reaper.exe"),
    "darwin": Path("/Applications/REAPER.app/Contents/MacOS/REAPER"),
}.get(sys.platform, Path("/opt/REAPER/reaper"))

# REAPER's [audioconfig] mode numbers are not documented: probe for the one that is the dummy
# device. On REAPER 7.77 / Windows it is 4; the answer is cached in dummy-mode.txt.
MODE_CANDIDATES = (4, 5, 6, 7, 2)

# A one-off script REAPER runs to report which audio device it opened, then quits.
PROBE = """
local f = io.open(reaper.GetResourcePath() .. '/probe.txt', 'w')
local ok, mode = reaper.GetAudioDeviceInfo('MODE')
f:write(tostring(ok and mode or ''), '\\n', tostring(reaper.Audio_IsRunning()), '\\n', reaper.GetAppVersion(), '\\n')
f:close()
reaper.Main_OnCommand(40004, 0)
"""

# The dummy device's format: 48 kHz, 256 samples (a 5.33 ms deadline).
AUDIO = {"dummy_srate": "48000", "dummy_blocksize": "256"}


def write_ini(work: Path, mode: int):
    """Set the [audioconfig] keys, keeping everything else REAPER has written to its ini."""
    ini = work / "reaper.ini"
    want = dict(AUDIO, mode=str(mode))
    lines = ini.read_text(encoding="utf-8", errors="replace").splitlines() if ini.exists() else ["[reaper]"]
    out, section, seen = [], None, set()
    for line in lines:
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            if section == "audioconfig":                       # leaving the section: add missing keys
                out.extend(f"{k}={v}" for k, v in want.items() if k not in seen)
            section = s[1:-1].lower()
        elif section == "audioconfig" and "=" in s and s.split("=", 1)[0] in want:
            key = s.split("=", 1)[0]
            seen.add(key)
            line = f"{key}={want[key]}"                         # replace the existing value
        out.append(line)
    if section == "audioconfig":                               # the file ended inside the section
        out.extend(f"{k}={v}" for k, v in want.items() if k not in seen)
    elif "audioconfig" not in {l.strip()[1:-1].lower() for l in lines if l.strip().startswith("[")}:
        out.append("[audioconfig]")
        out.extend(f"{k}={v}" for k, v in want.items())
    ini.write_text("\n".join(out) + "\n", encoding="utf-8")


def launch(exe: Path, work: Path, script: Path, timeout: float, done):
    """Start an isolated REAPER that runs `script`; wait until it quits or the timeout passes.

    Returns (finished, seconds). On timeout, done() decides whether the work got finished anyway,
    and REAPER is stopped.
    """
    cmd = [str(exe), "-cfgfile", str(work / "reaper.ini"), "-newinst", "-nosplash", "-noactivate", "-new", str(script)]
    proc = subprocess.Popen(cmd)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if proc.poll() is not None:
            return True, time.time() - t0
        time.sleep(0.5)
    finished = done()
    proc.kill()
    proc.wait(10)
    return finished, time.time() - t0


def find_dummy_mode(exe: Path, work: Path):
    """The [audioconfig] mode number of REAPER's dummy device, from cache or by probing."""
    cache = work / "dummy-mode.txt"
    if cache.exists():
        return int(cache.read_text())
    probe = work / "probe.lua"
    probe.write_text(PROBE, encoding="utf-8")
    for mode in MODE_CANDIDATES:
        write_ini(work, mode)
        out = work / "probe.txt"
        out.unlink(missing_ok=True)
        ok, secs = launch(exe, work, probe, 240, out.exists)   # a first run scans the computer's plugins
        text = out.read_text(encoding="utf-8").split("\n") if out.exists() else []
        print(f"  probe mode={mode}: {text[0] if text else 'no answer (REAPER did not run the script)'} ({secs:.0f} s)")
        if not text:
            return None
        if "dummy" in text[0].lower():
            cache.write_text(str(mode))
            return mode
    return None


def install_extension(work: Path):
    """Copy the built extension into the isolated REAPER's UserPlugins, or remove a stale copy."""
    target = work / "UserPlugins" / EXTENSION.name
    if EXTENSION.exists():
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(EXTENSION, target)
        return True
    target.unlink(missing_ok=True)
    return False


def summarize(result: dict):
    """Print the validation's findings in a few lines per step."""
    dev = result.get("device", {})
    print(f"\nREAPER {result['host']['version']} · audio: {dev.get('mode')} {dev.get('srate')} Hz / {dev.get('bsize')} samples"
          f" · running: {dev.get('audio_running')} · extension: {result.get('host_extension') or 'not loaded'}")
    steps = {s["step"]: s for s in result.get("steps", [])}
    build = steps.get("build fixtures", {})
    print(f"Fixtures built: {build.get('built', 0)}")
    for p in build.get("problems", []):
        print(f"  ! {p}")
    inv = steps.get("F4 inventory")
    if inv:
        print("Spike C, declared delay of the Fixed Latency JSFX (slider vs what REAPER reports):")
        for c in inv["fixed_latency"]:
            state = "bypassed" if not c["enabled"] else ("offline" if c["offline"] else "active")
            verdict = "match" if c["pdc"] == c["slider"] else "differs"
            print(f"  {c['where']:<36} {state:<8} slider {c['slider']:>6.0f}  reported {c['pdc']!s:>6}  {verdict}")
        print(f"  container children seen: {inv.get('container_children')}")
        print(f"  capabilities: {', '.join(k for k, v in sorted(inv['capabilities'].items()) if v)}")
    byp = steps.get("F4 bypass")
    if byp:
        p = byp["pdc"]
        print(f"  bypass test (slider 512): enabled and running {p.get('enabled')}, right after bypass {p.get('just_bypassed')},"
              f" 1.5 s later {p.get('bypassed_after_1_5_s')}")
    ident = steps.get("identity")
    if ident:                                           # spike A: GUIDs across edits and save + reopen
        print(f"Spike A, identity stability: {'passed' if ident['passed'] else 'FAILED'}")
        for name, ok in sorted(ident["checks"].items()):   # one line per check, failures marked
            print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    for sid, s in steps.items():
        if sid.endswith("skipped"):
            print(f"{sid}: {s.get('reason')}")
    for tid, t in result.get("traces", {}).items():
        sm = t["summary"]
        host_csv = Path(t["folder"]) / "host.csv"
        host_rows = sum(1 for _ in open(host_csv, encoding="utf-8")) - 1 if host_csv.exists() else 0
        print(f"Trace {tid}: {sm['sensor_records']} sensor records, {sm['dropped_records']} dropped,"
              f" {sm['audio_xruns']} xruns, sensor {'seen' if sm['sensor_connected'] else 'NOT seen'},"
              f" second sensor {'reported' if sm.get('second_sensor') else 'not reported'},"
              f" {host_rows} REAPER blocks timed by the extension")
    for e in result.get("errors", []):
        print(f"ERROR {e}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--reaper", type=Path, default=DEFAULT_REAPER)
    ap.add_argument("--timeout", type=float, default=600)   # above the script's own 9-minute watchdog
    ap.add_argument("--work", type=Path, default=REPO / "build" / "reaper-validate")
    ap.add_argument("--keep", action="store_true", help="keep the previous run's files")
    args = ap.parse_args(argv)
    if not args.reaper.exists():
        raise SystemExit(f"REAPER not found at {args.reaper}; pass --reaper.")

    # The folder persists between runs so REAPER's first-run plugin scan happens only once;
    # SessionScope's own output from the previous run is cleared.
    work = args.work
    work.mkdir(parents=True, exist_ok=True)
    if (work / "SessionScope").exists() and not args.keep:
        shutil.rmtree(work / "SessionScope")
    reaper_install.install(work, quiet=True)
    has_ext = install_extension(work)

    print(f"Isolated REAPER resource folder: {work}")
    print(f"Extension: {'loading ' + EXTENSION.name if has_ext else 'not built (python tools/build_native.py)'}")
    mode = find_dummy_mode(args.reaper, work)
    if mode is None:
        print("Could not select REAPER's dummy audio device; continuing with mode 4. Audible fixtures will be skipped.")
        mode = 4
    write_ini(work, mode)

    result_path = work / "SessionScope" / "validation" / "result.json"

    def finished():
        try:
            return "finished_utc" in json.loads(result_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return False

    print(f"Running {VALIDATE.relative_to(REPO)} (about five minutes)...")
    ok, secs = launch(args.reaper, work, VALIDATE, args.timeout, finished)
    if not result_path.exists():
        raise SystemExit(f"REAPER ran for {secs:.0f} s but wrote no result: the script never started (a dialog may be waiting).")
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if "dummy" in str(result.get("device", {}).get("mode", "")).lower():
        (work / "dummy-mode.txt").write_text(str(mode))   # the probe can be skipped next time
    summarize(result)
    if not finished():
        print(f"\nREAPER was stopped after {secs:.0f} s before the validation finished.")

    # The analyser's full report on each recording (T10 is only about the second-sensor warning).
    traces = result.get("traces", {})
    if not ANALYSER.exists():
        print(f"\nNo reports: {ANALYSER.relative_to(REPO)} is missing (python tools/build_engine.py).")
        traces = {}
    for tid, t in traces.items():
        if tid == "T10":
            continue
        print("\n" + "=" * 100, flush=True)
        subprocess.run([str(ANALYSER), "report", t["folder"], "--no-color"])
        if tid.startswith("F7"):   # the known load next to it, for reading S2 by eye
            print(flush=True)
            subprocess.run([str(ANALYSER), "report", t["folder"], "--metric", "workload", "--no-color"])
    return 0 if finished() and not result.get("errors") else 1


if __name__ == "__main__":
    sys.exit(main())
