"""Build the native components into build/native/ with the pinned Zig toolchain.

    python tools/get_zig.py        # once: fetches the compiler into .toolchain/
    python tools/get_vst3sdk.py    # once: the VST3 SDK (MIT) into .toolchain/
    python tools/get_clap.py       # once: the CLAP headers (MIT) into .toolchain/
    python tools/build_native.py   # builds build/native/: the REAPER extension, the VST3 and the CLAP sensor

Zig is used only as a clang++ driver; MSVC with native/CMakeLists.txt builds the same source.
The result needs nothing beyond Windows itself: the C++ runtime is linked in, and the C runtime
is the Universal CRT that ships with Windows 10 and 11.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import get_zig  # noqa: E402

OUT = REPO / "build" / "native"

# The VST3 SDK (MIT, github.com/steinbergmedia/vst3sdk) is cloned into .toolchain/ by get_vst3sdk.py.
VST3 = ".toolchain/vst3sdk"
VST3_SOURCES = [
    "native/vst3_sensor/sessionscope_sensor.cpp",
    *[f"{VST3}/{f}" for f in (
        "public.sdk/source/main/dllmain.cpp", "public.sdk/source/main/pluginfactory.cpp",
        "public.sdk/source/vst/vstsinglecomponenteffect.cpp", "public.sdk/source/vst/vstcomponentbase.cpp",
        "public.sdk/source/vst/vstbus.cpp", "public.sdk/source/vst/vstinitiids.cpp",
        "public.sdk/source/vst/vstparameters.cpp", "public.sdk/source/common/pluginview.cpp",
        "public.sdk/source/common/commoniids.cpp",
        "base/source/fobject.cpp", "base/source/fstring.cpp", "base/source/baseiids.cpp", "base/source/fdebug.cpp",
        "base/source/updatehandler.cpp", "base/thread/source/flock.cpp",
        "pluginterfaces/base/funknown.cpp", "pluginterfaces/base/coreiids.cpp",
        "pluginterfaces/base/conststringtable.cpp", "pluginterfaces/base/ustring.cpp",
    )],
]

# The CLAP headers (MIT, github.com/free-audio/clap) are cloned into .toolchain/ by get_clap.py.
CLAP = ".toolchain/clap/include"

# (output file, sources, include folders, extra compiler flags). One entry per native component.
TARGETS = [
    ("reaper_sessionscope.dll", ["hosts/reaper/extension/sessionscope_ext.cpp"], ["hosts/reaper/third_party/reaper-sdk"], []),
    ("SessionScope.vst3.dll", VST3_SOURCES, [VST3], ["-DRELEASE=1", "-DNDEBUG", "-D_CRT_SECURE_NO_WARNINGS", "-lole32"]),
    # A CLAP plug-in on Windows is a DLL named .clap. Only the CLAP headers are needed.
    ("SessionScope.clap", ["native/clap_sensor/sessionscope_clap.cpp"], [CLAP], ["-DNDEBUG"]),
]

# How to fetch each third-party folder when it is missing.
FETCH = {VST3: "python tools/get_vst3sdk.py", CLAP: "python tools/get_clap.py"}


def main():
    if sys.platform == "darwin":   # macOS: plug-in bundles built with Apple's clang
        import build_native_mac
        return build_native_mac.main()
    zig = get_zig.zig_path()
    if zig is None:
        raise SystemExit("No compiler: run python tools/get_zig.py first (or build with CMake and MSVC).")
    OUT.mkdir(parents=True, exist_ok=True)
    for out, sources, includes, flags in TARGETS:
        if includes and not (REPO / includes[0]).exists():
            print(f"skipping {out}: {includes[0]} is missing ({FETCH.get(includes[0], 'see README')})")
            continue
        cmd = [str(zig), "c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-shared", "-target", "x86_64-windows-gnu"]
        for inc in includes:
            cmd += ["-isystem", str(REPO / inc)]     # third-party headers: their warnings are not ours
        cmd += flags + [str(REPO / s) for s in sources] + ["-o", str(OUT / out)]
        print(f"building {out}")
        subprocess.run(cmd, check=True)
    print(f"done: {OUT}")


if __name__ == "__main__":
    main()
