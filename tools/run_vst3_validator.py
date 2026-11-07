"""Build and run the Steinberg VST3 validator on SessionScope.vst3.

    python tools/run_vst3_validator.py

Steps:
1. Locate the pinned Zig compiler (via tools/get_zig.py) and the VST3 SDK.
2. Compile the SDK's validator to build/native/validator.exe (skipping if up to date).
3. Copy build/native/SessionScope.vst3.dll to a test bundle under build/native/bundle/.
4. Run the validator on that bundle and print its output.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import get_zig  # noqa: E402

VST3_SDK = REPO / ".toolchain" / "vst3sdk"
OUT_DIR = REPO / "build" / "native"
VALIDATOR_EXE = OUT_DIR / "validator.exe"
BUNDLE_DIR = OUT_DIR / "bundle" / "SessionScope.vst3" / "Contents" / "x86_64-win"
BUNDLE_DLL = BUNDLE_DIR / "SessionScope.vst3"
BUILT_DLL = OUT_DIR / "SessionScope.vst3.dll"


def find_zig() -> Path:
    zig = get_zig.zig_path()
    if zig is None:
        raise SystemExit("Zig not found. Run: python tools/get_zig.py")
    return zig


def check_sdk() -> Path:
    if not VST3_SDK.exists():
        raise SystemExit(f"VST3 SDK not found at {VST3_SDK}. Run: python tools/get_vst3sdk.py")
    return VST3_SDK


def validator_sources(sdk: Path) -> list[Path]:
    """Return the list of .cpp files needed to build the validator (Windows)."""
    root = sdk / "public.sdk"
    base = sdk / "base"
    pluginterfaces = sdk / "pluginterfaces"
    src = sdk / "public.sdk" / "samples" / "vst-hosting" / "validator" / "source"

    files = [
        # validator sources
        src / "main.cpp",
        src / "usediids.cpp",
        src / "validator.cpp",
        # entry point: Zig links for WinMain here, while the validator only defines wmain
        REPO / "tools" / "winmain_wrapper.cpp",
        # hosting library (sdk_hosting)
        root / "source" / "vst" / "hosting" / "connectionproxy.cpp",
        root / "source" / "vst" / "hosting" / "eventlist.cpp",
        root / "source" / "vst" / "hosting" / "hostclasses.cpp",
        root / "source" / "vst" / "hosting" / "module.cpp",
        root / "source" / "vst" / "hosting" / "module_win32.cpp",
        root / "source" / "vst" / "hosting" / "parameterchanges.cpp",
        root / "source" / "vst" / "hosting" / "pluginterfacesupport.cpp",
        root / "source" / "vst" / "hosting" / "processdata.cpp",
        root / "source" / "vst" / "hosting" / "plugprovider.cpp",
        # main library (for InitModule/DeinitModule)
        root / "source" / "main" / "moduleinit.cpp",
        # moduleinfo library
        root / "source" / "vst" / "moduleinfo" / "moduleinfoparser.cpp",
        # common library (sdk_common)
        root / "source" / "common" / "commoniids.cpp",
        root / "source" / "common" / "commonstringconvert.cpp",
        root / "source" / "common" / "memorystream.cpp",
        root / "source" / "common" / "openurl.cpp",
        root / "source" / "common" / "readfile.cpp",
        root / "source" / "common" / "systemclipboard_win32.cpp",
        root / "source" / "common" / "threadchecker_win32.cpp",
        root / "source" / "vst" / "vstpresetfile.cpp",
        root / "source" / "vst" / "vstinitiids.cpp",
        # base library
        base / "source" / "baseiids.cpp",
        base / "source" / "fbuffer.cpp",
        base / "source" / "fdebug.cpp",
        base / "source" / "fdynlib.cpp",
        base / "source" / "fobject.cpp",
        base / "source" / "fstreamer.cpp",
        base / "source" / "fstring.cpp",
        base / "source" / "timer.cpp",
        base / "source" / "updatehandler.cpp",
        base / "thread" / "source" / "fcondition.cpp",
        base / "thread" / "source" / "flock.cpp",
        # pluginterfaces library
        pluginterfaces / "base" / "conststringtable.cpp",
        pluginterfaces / "base" / "coreiids.cpp",
        pluginterfaces / "base" / "funknown.cpp",
        pluginterfaces / "base" / "ustring.cpp",
        # test suite sources (all of them)
        root / "source" / "vst" / "testsuite" / "testbase.cpp",
        root / "source" / "vst" / "testsuite" / "vsttestsuite.cpp",
        root / "source" / "vst" / "utility" / "testing.cpp",
        root / "source" / "vst" / "utility" / "stringconvert.cpp",
    ]

    # bus tests
    for name in [
        "busactivation",
        "busconsistency",
        "businvalidindex",
        "checkaudiobusarrangement",
        "scanbusses",
        "sidechainarrangement",
    ]:
        files.append(root / "source" / "vst" / "testsuite" / "bus" / f"{name}.cpp")

    # general tests
    for name in [
        "editorclasses",
        "midilearn",
        "midimapping",
        "parameterfunctionname",
        "plugcompat",
        "scanparameters",
        "suspendresume",
        "terminit",
    ]:
        files.append(root / "source" / "vst" / "testsuite" / "general" / f"{name}.cpp")

    # note expression tests
    for name in ["keyswitch", "noteexpression"]:
        files.append(root / "source" / "vst" / "testsuite" / "noteexpression" / f"{name}.cpp")

    # processing tests
    for name in [
        "automation",
        "process",
        "processcontextrequirements",
        "processformat",
        "processinputoverwriting",
        "processtail",
        "processthreaded",
        "silenceflags",
        "silenceprocessing",
        "speakerarrangement",
        "variableblocksize",
    ]:
        files.append(root / "source" / "vst" / "testsuite" / "processing" / f"{name}.cpp")

    # state tests
    for name in [
        "bypasspersistence",
        "invalidstatetransition",
        "repeatidenticalstatetransition",
        "validstatetransition",
    ]:
        files.append(root / "source" / "vst" / "testsuite" / "state" / f"{name}.cpp")

    # unit tests
    for name in ["checkunitstructure", "scanprograms", "scanunits"]:
        files.append(root / "source" / "vst" / "testsuite" / "unit" / f"{name}.cpp")

    return files


def include_dirs(sdk: Path) -> list[Path]:
    """Include directories needed for the validator build."""
    root = sdk / "public.sdk"
    base = sdk / "base"
    pluginterfaces = sdk / "pluginterfaces"
    return [
        sdk,
        root,
        root / "source",
        base / "source",
        base / "thread" / "source",
        pluginterfaces / "base",
        pluginterfaces / "vst",
        pluginterfaces / "gui",
        pluginterfaces / "test",
        root / "source" / "vst",
        root / "source" / "vst" / "hosting",
        root / "source" / "vst" / "testsuite",
        root / "source" / "vst" / "utility",
        root / "source" / "common",
    ]


def needs_rebuild(target: Path, sources: list[Path], script_path: Path) -> bool:
    """Return True if target doesn't exist or is older than any source or the script."""
    if not target.exists():
        return True
    target_mtime = target.stat().st_mtime
    for src in [script_path] + sources:
        if src.exists() and src.stat().st_mtime > target_mtime:
            return True
    return False


def build_validator(zig: Path, sdk: Path) -> None:
    """Compile the validator executable."""
    sources = validator_sources(sdk)
    missing = [s for s in sources if not s.exists()]
    if missing:
        raise SystemExit("Missing validator sources:\n" + "\n".join(str(m) for m in missing))

    if not needs_rebuild(VALIDATOR_EXE, sources, Path(__file__)):
        print(f"validator.exe is up to date at {VALIDATOR_EXE}")
        return

    print(f"Building validator.exe ({len(sources)} source files)...")
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    inc_flags = []
    for inc in include_dirs(sdk):
        inc_flags += ["-isystem", str(inc)]

    # Link libraries needed on Windows
    link_flags = ["-lole32", "-luuid", "-lshell32", "-lshlwapi", "-ladvapi32", "-Wl,--subsystem=console"]

    cmd = [
        str(zig),
        "c++",
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Wno-date-time",
        "-Wno-unused-but-set-variable",
        "-Wno-unknown-pragmas",
        "-Wno-deprecated-declarations",
        "-Wno-unnecessary-virtual-specifier",
        "-target",
        "x86_64-windows-gnu",
        "-fuse-ld=lld",
        "-DSMTG_OS_WINDOWS=1",
        "-DCOM_COMPATIBLE=1",
        "-D_CRT_SECURE_NO_WARNINGS",
        "-DRELEASE=1",
        "-DNDEBUG",
    ] + inc_flags + [str(s) for s in sources] + link_flags + ["-o", str(VALIDATOR_EXE)]

    print(f"  zig c++ ... {len(sources)} files")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        raise SystemExit(f"Validator compilation failed with exit code {result.returncode}")

    print(f"  Built {VALIDATOR_EXE}")


def prepare_bundle() -> Path:
    """Copy the built VST3 DLL into a test bundle structure."""
    if not BUILT_DLL.exists():
        raise SystemExit(f"SessionScope.vst3.dll not found at {BUILT_DLL}. Run: python tools/build_native.py")

    BUNDLE_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(BUILT_DLL, BUNDLE_DLL)
    print(f"Test bundle ready at {BUNDLE_DLL}")
    return BUNDLE_DLL


def run_validator(bundle_dll: Path) -> int:
    """Run the validator on the bundle, print its output and summary, and return its exit code."""
    print(f"Running validator on {bundle_dll}...")
    result = subprocess.run([str(VALIDATOR_EXE), str(bundle_dll)], capture_output=True, text=True)
    print(result.stdout)
    print(result.stderr, file=sys.stderr)

    # The validator prints a result line such as "Result: 47 tests passed, 0 tests failed".
    lines = result.stdout.splitlines() + result.stderr.splitlines()
    summary = [line for line in lines if line.startswith("Result:")]
    print(f"=== {summary[-1] if summary else 'No result line found'} ===")
    return result.returncode


def main() -> int:
    zig = find_zig()
    sdk = check_sdk()
    build_validator(zig, sdk)
    return run_validator(prepare_bundle())


if __name__ == "__main__":
    sys.exit(main())
