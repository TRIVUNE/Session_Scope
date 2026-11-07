"""Build the engine, the console program and the HUD into build/engine/.

    python tools/build_engine.py          # compile engine/src into build/engine/obj
    python tools/build_engine.py --test   # also build and run every engine/tests/test_*.cpp

Windows: the pinned Zig toolchain (get_zig.zig_path(), as in build_native.py).
macOS: Apple's clang from the Xcode command line tools.
Our own code is always compiled with -std=c++17 -O2 -Wall -Wextra -Werror.

Compiles each engine/src/*.cpp to its own object file, skipping when the object is newer
than the source and newer than every header in engine/src.
Builds each test into build/engine/tests/.
Adds engine/src to the include path.
Exits non-zero if anything fails to compile or any test fails.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import get_zig  # noqa: E402


OUT = REPO / "build" / "engine"
OBJ_DIR = OUT / "obj"
TEST_OUT = OUT / "tests"
TOOLS_OUT = OUT
SRC_DIR = REPO / "engine" / "src"
TEST_DIR = REPO / "engine" / "tests"
TOOLS_DIR = REPO / "engine" / "tools"
HUD_DIR = REPO / "engine" / "hud"
IS_MAC = sys.platform == "darwin"
EXE = "" if IS_MAC else ".exe"                 # program file names: sessionscope.exe or sessionscope
HUD_EXE = OUT / f"SessionScopeHUD{EXE}"
# std::from_chars for doubles needs macOS 13.3 or newer.
MAC_FLAGS = ["-mmacosx-version-min=13.3"] if IS_MAC else []

GLFW_DIR = REPO / ".toolchain" / "glfw"
IMGUI_DIR = REPO / ".toolchain" / "imgui"
STB_DIR = REPO / ".toolchain" / "stb"
GLFW_OBJ_DIR = OBJ_DIR / "glfw"
IMGUI_OBJ_DIR = OBJ_DIR / "imgui"


def compiler(language: str) -> list[str]:
    """The compiler command for "c++", "c" or "objective-c": Apple's clang on macOS, Zig elsewhere."""
    if IS_MAC:
        return {"c++": ["clang++"], "c": ["clang"], "objective-c": ["clang", "-x", "objective-c"]}[language] + MAC_FLAGS
    zig = get_zig.zig_path()
    if zig is None:
        raise SystemExit("No compiler: run python tools/get_zig.py first.")
    return [str(zig), "c++" if language == "c++" else "cc"]


def our_cxx() -> list[str]:
    """The C++ command for our own code, with the standard flags."""
    return [
        *compiler("c++"),
        "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(SRC_DIR),
        "-I", str(REPO / "native" / "sensor_core"),
    ]


def third_party_c(language: str = "c") -> list[str]:
    """The C (or Objective-C) command for GLFW's sources: third-party, so no warnings."""
    return [
        *compiler(language),
        "-O2", "-w",  # -w suppresses warnings from third-party code
        *(["-std=c11"] if language == "c" else []),
        "-isystem", str(GLFW_DIR / "include"),
        "-isystem", str(GLFW_DIR / "src"),
        "-D_GLFW_COCOA" if IS_MAC else "-D_GLFW_WIN32",
    ]


def third_party_cxx() -> list[str]:
    """The C++ command for third-party code (ImGui), with no warnings."""
    return [
        *compiler("c++"),
        "-std=c++17", "-O2", "-w",  # -w suppresses warnings from third-party code
        "-isystem", str(IMGUI_DIR),
        "-isystem", str(IMGUI_DIR / "backends"),
        "-isystem", str(GLFW_DIR / "include"),
    ]


def ensure_dirs() -> None:
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    TEST_OUT.mkdir(parents=True, exist_ok=True)
    GLFW_OBJ_DIR.mkdir(parents=True, exist_ok=True)
    IMGUI_OBJ_DIR.mkdir(parents=True, exist_ok=True)


def source_files() -> list[Path]:
    return sorted(SRC_DIR.glob("*.cpp"))


def header_files() -> list[Path]:
    return sorted(SRC_DIR.glob("*.h"))


def object_path(src: Path) -> Path:
    return OBJ_DIR / (src.stem + ".o")


def needs_rebuild(src: Path, obj: Path, headers: list[Path]) -> bool:
    if not obj.exists():
        return True
    src_mtime = src.stat().st_mtime
    obj_mtime = obj.stat().st_mtime
    if src_mtime > obj_mtime:
        return True
    for h in headers:
        if h.stat().st_mtime > obj_mtime:
            return True
    return False


def compile_source(src: Path, obj: Path) -> None:
    cmd = our_cxx() + ["-c", str(src), "-o", str(obj)]
    print(f"  compiling {src.name}")
    subprocess.run(cmd, check=True)


def build_objects() -> list[Path]:
    headers = header_files()
    objects = []
    for src in source_files():
        obj = object_path(src)
        if needs_rebuild(src, obj, headers):
            compile_source(src, obj)
        else:
            print(f"  up to date {src.name}")
        objects.append(obj)
    return objects


def build_tests(objects: list[Path]) -> list[Path]:
    test_exes = []
    for test_src in sorted(TEST_DIR.glob("test_*.cpp")):
        exe = TEST_OUT / f"{test_src.stem}{EXE}"
        inputs = [test_src, *objects, *header_files(), *TEST_DIR.glob("*.h")]
        if not exe.exists() or any(f.stat().st_mtime > exe.stat().st_mtime for f in inputs):
            print(f"  linking {exe.name}")
            subprocess.run(our_cxx() + [str(test_src), *map(str, objects), "-o", str(exe)], check=True)
        else:
            print(f"  up to date {exe.name}")
        test_exes.append(exe)
    return test_exes


def build_tools(objects: list[Path]) -> list[Path]:
    tool_exes = []
    for tool_src in sorted(TOOLS_DIR.glob("*.cpp")):
        exe = TOOLS_OUT / f"{tool_src.stem}{EXE}"
        inputs = [tool_src, *objects, *header_files()]
        if not exe.exists() or any(f.stat().st_mtime > exe.stat().st_mtime for f in inputs):
            print(f"  linking {exe.name}")
            subprocess.run(our_cxx() + [str(tool_src), *map(str, objects), "-o", str(exe)], check=True)
        else:
            print(f"  up to date {exe.name}")
        tool_exes.append(exe)
    return tool_exes


# -----------------------------------------------------------------------------
# GLFW (Win32 or Cocoa backend) — compiled once as C (and, on macOS, Objective-C) sources

GLFW_COMMON = [
    "context.c", "init.c", "input.c", "monitor.c", "platform.c", "vulkan.c", "window.c",
    "egl_context.c", "osmesa_context.c",
    "null_init.c", "null_monitor.c", "null_window.c", "null_joystick.c",
]
GLFW_SOURCES = GLFW_COMMON + ([
    "cocoa_time.c", "posix_thread.c", "posix_module.c",
    "cocoa_init.m", "cocoa_joystick.m", "cocoa_monitor.m", "cocoa_window.m", "nsgl_context.m",
] if IS_MAC else [
    "win32_time.c", "win32_thread.c", "win32_module.c",
    "win32_init.c", "win32_joystick.c", "win32_monitor.c", "win32_window.c", "wgl_context.c",
])

def glfw_object_path(name: str) -> Path:
    return GLFW_OBJ_DIR / (Path(name).stem + ".o")

def build_glfw() -> list[Path]:
    objects = []
    for src_name in GLFW_SOURCES:
        src = GLFW_DIR / "src" / src_name
        obj = glfw_object_path(src_name)
        if not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
            print(f"  compiling glfw/{src_name}")
            language = "objective-c" if src.suffix == ".m" else "c"
            subprocess.run(third_party_c(language) + ["-c", str(src), "-o", str(obj)], check=True)
        else:
            print(f"  up to date glfw/{src_name}")
        objects.append(obj)
    return objects


# -----------------------------------------------------------------------------
# ImGui (core + GLFW + OpenGL3 backends) — compiled once as C++ sources

IMGUI_SOURCES = [
    "imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
    "backends/imgui_impl_glfw.cpp", "backends/imgui_impl_opengl3.cpp",
]

def imgui_object_path(name: str) -> Path:
    # Flatten: imgui_imgui.o, imgui_draw.o, ..., imgui_impl_glfw.o, imgui_impl_opengl3.o
    stem = Path(name).stem
    if "imgui_impl_" in name:
        return IMGUI_OBJ_DIR / (stem + ".o")
    return IMGUI_OBJ_DIR / (stem + ".o")

def build_imgui() -> list[Path]:
    objects = []
    for src_name in IMGUI_SOURCES:
        src = IMGUI_DIR / src_name
        obj = imgui_object_path(src_name)
        if not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
            print(f"  compiling imgui/{src_name}")
            subprocess.run(third_party_cxx() + ["-c", str(src), "-o", str(obj)], check=True)
        else:
            print(f"  up to date imgui/{src_name}")
        objects.append(obj)
    return objects


# -----------------------------------------------------------------------------
# HUD executable

def hud_source_files() -> list[Path]:
    return sorted(HUD_DIR.glob("*.cpp"))

def hud_object_path(src: Path) -> Path:
    return OBJ_DIR / "hud" / (src.stem + ".o")

def build_hud_objects(engine_objects: list[Path]) -> list[Path]:
    """Compile engine/hud/*.cpp with our flags (including -Werror)."""
    hud_obj_dir = OBJ_DIR / "hud"
    hud_obj_dir.mkdir(parents=True, exist_ok=True)

    # Headers that hud sources depend on: engine/src/*.h + hud_window.h
    hud_headers = header_files() + sorted(HUD_DIR.glob("*.h"))

    objects = []
    for src in hud_source_files():
        obj = hud_object_path(src)
        if needs_rebuild(src, obj, hud_headers):
            print(f"  compiling {src.name}")
            # Include GLFW and ImGui headers
            cmd = our_cxx() + [
                "-I", str(GLFW_DIR / "include"),
                "-I", str(IMGUI_DIR),
                "-I", str(HUD_DIR),
                "-isystem", str(STB_DIR),   # stb_image_write: third-party, its warnings are not ours
                "-c", str(src), "-o", str(obj)
            ]
            subprocess.run(cmd, check=True)
        else:
            print(f"  up to date {src.name}")
        objects.append(obj)
    return objects


def link_hud(engine_objects: list[Path], glfw_objects: list[Path],
             imgui_objects: list[Path], hud_objects: list[Path]) -> None:
    inputs = [*engine_objects, *glfw_objects, *imgui_objects, *hud_objects]
    if not HUD_EXE.exists() or any(f.stat().st_mtime > HUD_EXE.stat().st_mtime for f in inputs):
        print(f"  linking {HUD_EXE.name}")
        cmd = our_cxx() + [
            *map(str, engine_objects),
            *map(str, glfw_objects),
            *map(str, imgui_objects),
            *map(str, hud_objects),
            *HUD_LINK_FLAGS,
            "-o", str(HUD_EXE),
        ]
        subprocess.run(cmd, check=True)
    else:
        print(f"  up to date {HUD_EXE.name}")


# What the HUD links against: the system's windowing and OpenGL libraries.
if IS_MAC:
    HUD_LINK_FLAGS = ["-framework", "Cocoa", "-framework", "IOKit", "-framework", "OpenGL",
                      "-framework", "CoreFoundation", "-framework", "QuartzCore"]
else:
    HUD_LINK_FLAGS = ["-lopengl32", "-lgdi32", "-luser32", "-lshell32", "-lwinmm", "-Wl,--subsystem,windows"]


def build_hud(engine_objects: list[Path]) -> None:
    """Build the HUD executable if GLFW and ImGui are present."""
    if not GLFW_DIR.exists() or not IMGUI_DIR.exists():
        print("  HUD skipped: .toolchain/glfw or .toolchain/imgui not found")
        return

    print("Building HUD...")
    glfw_objs = build_glfw()
    imgui_objs = build_imgui()
    hud_objs = build_hud_objects(engine_objects)
    link_hud(engine_objects, glfw_objs, imgui_objs, hud_objs)


def run_tests(test_exes: list[Path]) -> int:
    failed = 0
    for exe in test_exes:
        print(f"  running {exe.name}")
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        if result.returncode != 0:
            print(f"  FAILED: {exe.name} exited with {result.returncode}")
            failed += 1
    return failed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--test", action="store_true", help="build and run tests")
    args = parser.parse_args(argv)

    ensure_dirs()
    objects = build_objects()

    # The console program: sessionscope record / list / report / simulate.
    build_tools(objects)

    # HUD (optional, needs .toolchain/glfw and .toolchain/imgui)
    build_hud(objects)

    if args.test:
        test_exes = build_tests(objects)
        failed = run_tests(test_exes)
        if failed:
            print(f"\n{failed} test(s) failed")
            return 1
        print("\nall tests passed")

    return 0


if __name__ == "__main__":
    sys.exit(main())