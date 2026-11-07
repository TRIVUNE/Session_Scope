"""Build the sensor for macOS as VST3, CLAP and Audio Unit bundles into build/native/mac/.

    python tools/get_vst3sdk.py      # once: the VST3 SDK (MIT)
    python tools/get_clap.py         # once: the CLAP headers (MIT)
    python tools/get_au_sdk.py       # once: Apple's AudioUnitSDK (Apache 2.0)
    python tools/build_native_mac.py

Run on a Mac with the Xcode command line tools (xcode-select --install). tools/build_native.py
calls this when it runs on macOS. Each bundle is a universal binary (Apple silicon and Intel)
with its Info.plist, signed ad hoc so the system loads it; a format whose SDK is missing is
skipped with a note.

    SessionScope.vst3/Contents/MacOS/SessionScope        VST3 hosts
    SessionScope.clap/Contents/MacOS/SessionScope        CLAP hosts
    SessionScope.component/Contents/MacOS/SessionScope   Audio Unit hosts (Logic, GarageBand, ...)
"""

from __future__ import annotations

import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "build" / "native" / "mac"
VERSION = "0.3.0"
ARCHS = ["-arch", "arm64", "-arch", "x86_64"]
MIN_MACOS = "-mmacosx-version-min=11.0"

VST3_SDK = REPO / ".toolchain" / "vst3sdk"
CLAP_INCLUDE = REPO / ".toolchain" / "clap" / "include"
AU_SDK = REPO / ".toolchain" / "AudioUnitSDK"

# The VST3 SDK sources the sensor needs: the same as on Windows, with the macOS module entry.
VST3_SDK_SOURCES = [
    "public.sdk/source/main/macmain.cpp", "public.sdk/source/main/pluginfactory.cpp",
    "public.sdk/source/vst/vstsinglecomponenteffect.cpp", "public.sdk/source/vst/vstcomponentbase.cpp",
    "public.sdk/source/vst/vstbus.cpp", "public.sdk/source/vst/vstinitiids.cpp",
    "public.sdk/source/vst/vstparameters.cpp", "public.sdk/source/common/pluginview.cpp",
    "public.sdk/source/common/commoniids.cpp",
    "base/source/fobject.cpp", "base/source/fstring.cpp", "base/source/baseiids.cpp", "base/source/fdebug.cpp",
    "base/source/updatehandler.cpp", "base/thread/source/flock.cpp",
    "pluginterfaces/base/funknown.cpp", "pluginterfaces/base/coreiids.cpp",
    "pluginterfaces/base/conststringtable.cpp", "pluginterfaces/base/ustring.cpp",
]

# Audio Unit identity: must match nothing else installed. 0.3.0 is 0x000300 = 768.
AU_TYPE, AU_SUBTYPE, AU_MANUFACTURER = "aufx", "SScp", "Trvn"
AU_VERSION = 0x000300


def info_plist(identifier: str, extra: dict | None = None) -> dict:
    """The Info.plist every bundle needs: what to load, and what it is called."""
    plist = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": "SessionScope",
        "CFBundleIdentifier": identifier,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "SessionScope Sensor",
        "CFBundlePackageType": "BNDL",
        "CFBundleShortVersionString": VERSION,
        "CFBundleVersion": VERSION,
        "CFBundleSignature": "????",
    }
    plist.update(extra or {})
    return plist


def make_bundle(name: str, plist: dict, compile_and_link: list[str]) -> Path:
    """Create name/Contents/{Info.plist, PkgInfo, MacOS/SessionScope}, build into it, sign it."""
    bundle = OUT / name
    if bundle.exists():
        shutil.rmtree(bundle)
    macos = bundle / "Contents" / "MacOS"
    macos.mkdir(parents=True)
    with open(bundle / "Contents" / "Info.plist", "wb") as f:
        plistlib.dump(plist, f)
    (bundle / "Contents" / "PkgInfo").write_text("BNDL????", encoding="ascii")

    print(f"building {name}")
    subprocess.run([*compile_and_link, "-o", str(macos / "SessionScope")], check=True)
    # An ad-hoc signature: no developer account needed, and Apple silicon refuses unsigned code.
    subprocess.run(["codesign", "--force", "--sign", "-", str(bundle)], check=True)
    return bundle


def compiler(standard: str) -> list[str]:
    """clang++ for a universal plug-in bundle, with our usual warnings."""
    return ["clang++", f"-std={standard}", "-O2", "-Wall", "-Wextra", "-DNDEBUG", *ARCHS, MIN_MACOS,
            "-bundle", "-fvisibility=hidden"]


def build_vst3() -> None:
    if not VST3_SDK.exists():
        print("skipping SessionScope.vst3: no VST3 SDK (python tools/get_vst3sdk.py)")
        return
    make_bundle("SessionScope.vst3", info_plist("com.trivune.sessionscope.vst3"), [
        *compiler("c++17"), "-DRELEASE=1", "-isystem", str(VST3_SDK),
        str(REPO / "native" / "vst3_sensor" / "sessionscope_sensor.cpp"),
        *[str(VST3_SDK / s) for s in VST3_SDK_SOURCES],
        "-exported_symbols_list", str(VST3_SDK / "public.sdk" / "source" / "main" / "macexport.exp"),
        "-framework", "CoreFoundation", "-framework", "Foundation",
    ])


def build_clap() -> None:
    if not CLAP_INCLUDE.exists():
        print("skipping SessionScope.clap: no CLAP headers (python tools/get_clap.py)")
        return
    # Only clap_entry is exported; -fvisibility=hidden hides everything else.
    make_bundle("SessionScope.clap", info_plist("com.trivune.sessionscope.clap"), [
        *compiler("c++17"), "-isystem", str(CLAP_INCLUDE),
        str(REPO / "native" / "clap_sensor" / "sessionscope_clap.cpp"),
    ])


def build_au() -> None:
    if not AU_SDK.exists():
        print("skipping SessionScope.component: no AudioUnitSDK (python tools/get_au_sdk.py)")
        return
    component = {
        "type": AU_TYPE, "subtype": AU_SUBTYPE, "manufacturer": AU_MANUFACTURER,
        "name": "TRIVUNE: SessionScope Sensor",
        "description": "Measures every audio block for the SessionScope HUD; audio passes through.",
        "version": AU_VERSION,
        "factoryFunction": "SensorAUFactory",
        "sandboxSafe": True,
        "tags": ["Effects"],
    }
    make_bundle("SessionScope.component", info_plist("com.trivune.sessionscope.component", {"AudioComponents": [component]}), [
        # The SDK needs C++23. Its render methods are marked real-time safe ('nonblocking'); clang
        # cannot see that the sensor core is too, so that one check is off for this file.
        *compiler("c++2b"), "-Wno-unknown-warning-option", "-Wno-function-effects",
        "-isystem", str(AU_SDK / "include"),
        str(REPO / "native" / "au_sensor" / "sessionscope_au.cpp"),
        *[str(p) for p in sorted((AU_SDK / "src" / "AudioUnitSDK").glob("*.cpp"))],
        "-framework", "AudioToolbox", "-framework", "CoreFoundation", "-framework", "CoreAudio",
    ])


def main() -> int:
    if sys.platform != "darwin":
        sys.exit("This builds macOS bundles: run it on a Mac (or let CI's macOS job do it).")
    OUT.mkdir(parents=True, exist_ok=True)
    build_vst3()
    build_clap()
    build_au()
    print(f"done: {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
