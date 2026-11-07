"""Where SessionScope's native parts are built and where they are installed, on Windows and macOS.

Used by tools/setup.py and tools/install_plugins.py.

Every plug-in format has two standard folders: a per-user one, which needs no administrator
rights but not every host scans, and a shared one, the format's standard location, which needs
administrator rights. The first folder in each list is the per-user one.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD = REPO / "build" / "native"
IS_MAC = sys.platform == "darwin"

if IS_MAC:
    # macOS: each plug-in is a bundle (a folder), built by tools/build_native_mac.py.
    MAC_BUILD = BUILD / "mac"
    _USER = Path.home() / "Library" / "Audio" / "Plug-Ins"
    _SHARED = Path("/Library/Audio/Plug-Ins")
    VST3_BUILT = MAC_BUILD / "SessionScope.vst3"
    VST3_FOLDERS = [_USER / "VST3", _SHARED / "VST3"]
    CLAP_BUILT = MAC_BUILD / "SessionScope.clap"
    CLAP_FOLDERS = [_USER / "CLAP", _SHARED / "CLAP"]
    AU_BUILT = MAC_BUILD / "SessionScope.component"
    AU_FOLDERS = [_USER / "Components", _SHARED / "Components"]
else:
    # Windows: a VST3 is a folder holding the DLL; a CLAP is a single DLL named .clap.
    _LOCAL = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "Programs" / "Common"
    _COMMON = Path(os.environ.get("CommonProgramFiles", r"C:\Program Files\Common Files"))
    VST3_BUILT = BUILD / "SessionScope.vst3.dll"
    VST3_FOLDERS = [_LOCAL / "VST3", _COMMON / "VST3"]
    CLAP_BUILT = BUILD / "SessionScope.clap"
    CLAP_FOLDERS = [_LOCAL / "CLAP", _COMMON / "CLAP"]
    AU_BUILT = None          # Audio Units exist only on macOS
    AU_FOLDERS = []


def vst3_bundle_dir(folder: Path) -> Path:
    """The SessionScope.vst3 bundle folder inside a VST3 folder."""
    return folder / "SessionScope.vst3"


def vst3_target(folder: Path) -> Path:
    """What to copy the build to: the whole bundle on macOS, the DLL inside the bundle on Windows."""
    if IS_MAC:
        return vst3_bundle_dir(folder)
    return vst3_bundle_dir(folder) / "Contents" / "x86_64-win" / "SessionScope.vst3"


def clap_file(folder: Path) -> Path:
    """The plug-in (a file on Windows, a bundle on macOS) inside a CLAP folder."""
    return folder / "SessionScope.clap"


def au_component(folder: Path) -> Path:
    """The Audio Unit bundle inside a Components folder (macOS only)."""
    return folder / "SessionScope.component"


def plugin_copies() -> list[tuple[str, Path, Path, Path]]:
    """Every copy to make: (name, what was built, where it goes, what an uninstall removes),
    per-user folders before shared ones."""
    copies = []
    for i, scope in enumerate(["per-user", "shared"]):
        copies.append((f"VST3 sensor ({scope} folder)", VST3_BUILT, vst3_target(VST3_FOLDERS[i]),
                       vst3_bundle_dir(VST3_FOLDERS[i])))
        copies.append((f"CLAP sensor ({scope} folder)", CLAP_BUILT, clap_file(CLAP_FOLDERS[i]),
                       clap_file(CLAP_FOLDERS[i])))
        if AU_BUILT:
            copies.append((f"Audio Unit sensor ({scope} folder)", AU_BUILT, au_component(AU_FOLDERS[i]),
                           au_component(AU_FOLDERS[i])))
    return copies


# The optional REAPER extension (realtime CPU, Windows only for now): REAPER loads reaper_*.dll
# from <resource path>/UserPlugins when it starts.
REAPER_RESOURCE = Path(os.environ.get("APPDATA", Path.home())) / "REAPER"
EXT_BUILT = BUILD / "reaper_sessionscope.dll"
EXT_INSTALLED = REAPER_RESOURCE / "UserPlugins" / "reaper_sessionscope.dll"


def same_file(a: Path, b: Path) -> bool:
    """True if b exists and is a copy of a (same size and contents; cheap size check first).
    For bundles (folders), every file inside must match."""
    if not (a.exists() and b.exists()):
        return False
    if a.is_dir() or b.is_dir():
        if not (a.is_dir() and b.is_dir()):
            return False
        files_a = sorted(p.relative_to(a) for p in a.rglob("*") if p.is_file())
        files_b = sorted(p.relative_to(b) for p in b.rglob("*") if p.is_file())
        return files_a == files_b and all(same_file(a / f, b / f) for f in files_a)
    if a.stat().st_size != b.stat().st_size:
        return False
    return a.read_bytes() == b.read_bytes()             # a few MB at most


def copy_plugin(built: Path, target: Path) -> None:
    """Copy a plug-in file, or a whole bundle folder, replacing what was there."""
    target.parent.mkdir(parents=True, exist_ok=True)
    if built.is_dir():
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(built, target)
    else:
        shutil.copy2(built, target)
