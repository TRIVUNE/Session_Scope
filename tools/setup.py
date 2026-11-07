"""One-step setup for trying SessionScope:  python tools/setup.py   (Windows or macOS)

  1. builds the sensor, if it is missing or older than the source: VST3 and CLAP on Windows (plus
     the REAPER extension), VST3, CLAP and Audio Unit on macOS. Fetches the pinned compiler and
     the SDKs the first time.
  2. installs the sensor into the per-user plug-in folders (required) and the shared ones
     (optional, needs administrator rights)
  3. Windows: puts the REAPER extension into REAPER's UserPlugins folder (optional, only if REAPER
     is installed; gives reports REAPER's realtime CPU)
  4. builds the HUD window and the console program into build/engine/
     (fetching GLFW and Dear ImGui the first time)

Python is only needed for this build step; the programs it builds don't need it.
Run it again after pulling changes: it only redoes what is out of date. If your DAW is open, files
it has loaded can't be replaced; the script says so, and you run it again after closing the DAW.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import install_paths as install  # noqa: E402

IS_MAC = sys.platform == "darwin"
# The sources of what step 1 builds on this platform: a change to any of them means rebuild.
SOURCES = [REPO / "native" / "sensor_core" / "sensor_ring.h",
           REPO / "native" / "vst3_sensor" / "sessionscope_sensor.cpp",
           REPO / "native" / "clap_sensor" / "sessionscope_clap.cpp"]
if IS_MAC:
    SOURCES.append(REPO / "native" / "au_sensor" / "sessionscope_au.cpp")
else:
    SOURCES.append(REPO / "hosts" / "reaper" / "extension" / "sessionscope_ext.cpp")
HUD_EXE = REPO / "build" / "engine" / ("SessionScopeHUD" if IS_MAC else "SessionScopeHUD.exe")
BUILT = [install.VST3_BUILT, install.CLAP_BUILT, install.AU_BUILT if IS_MAC else install.EXT_BUILT]
FORMATS = "VST3, CLAP, Audio Unit" if IS_MAC else "VST3, CLAP"


def step(text):
    print(f"\n== {text}")


def ensure_compiler():
    """Windows: the pinned Zig compiler. macOS uses Apple's clang, from the Xcode command line tools."""
    if IS_MAC:
        if shutil.which("clang++") is None:
            sys.exit("No compiler: run  xcode-select --install  and then this again.")
        return
    import get_zig
    if get_zig.zig_path() is None:                               # the compiler, once (~50 MB)
        get_zig.main()


def needs_build() -> bool:
    """Missing outputs, or a source file changed after the last build."""
    if not all(o.exists() for o in BUILT):
        return True
    newest_src = max(s.stat().st_mtime for s in SOURCES)
    return min(o.stat().st_mtime for o in BUILT) < newest_src


def build_plugins():
    step(f"1/4 Build the sensor ({FORMATS})" + ("" if IS_MAC else " and the REAPER extension"))
    if not needs_build():
        print("up to date")
        return
    ensure_compiler()
    if not (REPO / ".toolchain" / "vst3sdk").exists():           # the VST3 SDK, once
        subprocess.run([sys.executable, str(REPO / "tools" / "get_vst3sdk.py")], check=True)
    import get_clap
    get_clap.get_headers()                                       # the CLAP headers, once
    if IS_MAC:
        import get_au_sdk
        get_au_sdk.main()                                        # Apple's AudioUnitSDK, once
    import build_native
    build_native.main()


def copy(src: Path, dst: Path, what: str, needs_admin: bool = False) -> bool:
    """Copy src (a file or a bundle) to dst unless it is already there. False if that failed:
    the DAW has the file open, or a shared folder needs administrator rights."""
    if install.same_file(src, dst):
        print(f"{what}: already installed ({dst})")
        return True
    try:
        install.copy_plugin(src, dst)
        print(f"{what}: installed to {dst}")
        return True
    except PermissionError:
        if needs_admin:
            print(f"{what}: not installed (needs administrator rights, or the file is in use).")
        else:
            print(f"{what}: the file is in use. Close your DAW and run `python tools/setup.py` again.")
        return False


def install_sensor() -> bool:
    """Every format into the per-user folders (required) and the shared ones (optional)."""
    step(f"2/4 Install the sensor ({FORMATS})")
    ok = True
    shared_ok = True
    for name, built, target, _ in install.plugin_copies():
        if "shared" in name:
            shared_ok &= copy(built, target, name, needs_admin=True)
        else:
            ok &= copy(built, target, name)
    if not shared_ok:
        how = "with sudo" if IS_MAC else "from an administrator terminal"
        print(f"If your DAW does not list the sensor, run `python tools/install_plugins.py` {how}.")
    return ok


def install_reaper_extension() -> bool:
    step("3/4 Install the REAPER extension (realtime CPU in reports)")
    if IS_MAC:
        print("Windows only for now: skipped.")
        return True
    if not install.REAPER_RESOURCE.exists():
        print(f"REAPER not installed (no settings folder at {install.REAPER_RESOURCE}): optional extension skipped.")
        return True
    return copy(install.EXT_BUILT, install.EXT_INSTALLED, "SessionScope extension")


def build_hud() -> bool:
    """The HUD window and the console program. build_engine only rebuilds what changed."""
    step("4/4 Build the HUD window and the console program")
    ensure_compiler()
    import get_hud_deps
    if get_hud_deps.main() != 0:                                 # GLFW and Dear ImGui, once
        print("Could not fetch GLFW and Dear ImGui (needs git and a network connection).")
        return False
    import build_engine
    if build_engine.main([]) != 0 or not HUD_EXE.exists():
        print("The HUD did not build: see the messages above.")
        return False
    return True


def main():
    if sys.platform not in ("win32", "darwin"):
        sys.exit("The native parts build on Windows and macOS for now.")
    build_plugins()
    ok = install_sensor()
    ok &= install_reaper_extension()
    ok &= build_hud()

    print("\n" + ("Done." if ok else "Done, with the notes above."))
    print("Next: restart your DAW, put 'SessionScope Sensor' on the master track, press play, and open")
    print(f"  {HUD_EXE.relative_to(REPO)}")
    print(f"If the DAW does not list the plug-in, rescan its plug-ins ({FORMATS}).")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
