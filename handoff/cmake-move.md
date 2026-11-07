# The move from Python to CMake: where it stands

The person decided (7 Nov 2026) that the build moves from the Python scripts in `tools/` and
`hosts/reaper/tools/` to CMake, and Python is then removed. Phases 1 and 2 are done; **phase 3 is
not started: the person said to stop there for now. Ask before starting it.**

## How to build now

CMake 4.4.5 and Ninja 1.13.2 are installed portable in `C:\tools\cmake` and `C:\tools\ninja`, and
on the user PATH (a terminal opened before 7 Nov may not see them yet).

```
cmake --preset windows              # once (and after adding files): sets up build/cmake
cmake --build --preset windows      # builds what changed
ctest --preset windows              # unit tests: 19 pass
ctest --preset windows-plugins      # VST3 validator, clap-validator, CLAP test host: 3 pass
                                    # (they use the real shared memory: close any DAW first)
```

Outputs are where they always were: `build/engine/` (sessionscope.exe, SessionScopeHUD.exe,
tests/) and `build/native/` (SessionScope.vst3.dll and a SessionScope.vst3 bundle folder,
SessionScope.clap, reaper_sessionscope.dll, validator.exe). A clean rebuild has 0 warnings.

## What each file does

| File | Replaces | What |
| --- | --- | --- |
| `CMakeLists.txt` | | top level: includes the rest |
| `CMakePresets.json` | | the `windows` / `mac` presets above |
| `cmake/zig-toolchain.cmake` | `get_zig.py` | fetches Zig 0.16.0 (SHA-256 checked), makes it the compiler |
| `cmake/fetch.cmake` | the `get_*.py` scripts | pinned git clones (commit checked), files and zip members (SHA-256 checked) into `.toolchain/` |
| `cmake/third_party.cmake` | `get_hud_deps.py` | GLFW, Dear ImGui, stb, Inter |
| `cmake/embed_fonts.cmake` | part of `build_engine.py` | the HUD's fonts into C++ |
| `engine/CMakeLists.txt` | `build_engine.py` | engine, console program, HUD, tests |
| `native/CMakeLists.txt` | `build_native.py`, `run_vst3_validator.py`, `run_clap_validator.py` | sensors, REAPER extension, validators as tests |
| `native/mac.cmake`, `native/mac/*` | `build_native_mac.py`, `check_mac_plugins.py` | macOS bundles, Audio Unit, auval: **written, never run on a Mac** |

Things learned: object libraries don't pass their compiled files on through another object
library (the HUD links `glfw` directly); a list on a custom command's line is cut apart by
Windows' shell (`;` and `|` both fail), so the font list goes through a file; clap-validator colours
its numbers, so its check reads the `--json` report.

## Phase 3: what's left (not started)

1. **Installing**, from `tools/install_paths.py`, `install_plugins.py` and `setup.py`: copy the
   sensor into the per-user plug-in folders (required) and the shared ones (optional, needs
   administrator rights), skip files that are already identical, say clearly when the DAW has a file
   open; uninstall; the REAPER extension into `%APPDATA%\REAPER\UserPlugins` if REAPER is installed.
   Plan: a `sessionscope install` / `sessionscope install --uninstall` command in the C++ console
   program (`engine/tools/sessionscope.cpp`), run by a CMake target or after building.
2. **REAPER tools**, from `hosts/reaper/tools/reaper_install.py` (link or copy the JSFX and scripts
   into a REAPER resource folder; a junction on Windows; never touch folders it didn't make) and
   `reaper_validate.py` (an isolated REAPER with the dummy audio device, runs
   `hosts/reaper/tests/integration/validate.lua`, prints a summary from `result.json`, then the
   analyser's report per trace). Plan: `sessionscope reaper install|uninstall|validate`; the engine
   already has a JSON reader (`engine/src/json.h`). Read the whole Python script first: its summary
   part is long.
3. **One-command setup** replacing `setup.py`, e.g. a CMake workflow preset (configure, build,
   test, install).
4. **Delete** every `.py` in `tools/` and `hosts/reaper/tools/`, then update README (quick start,
   install, tests), `guide.md` (build commands), `native/README.md`, `STATUS.md`, this note.
   Search for "python" afterwards: nothing should still point to a deleted script. Still pointing
   at Python now: README's quick start (`python tools/setup.py`) and install section
   (`install_plugins.py`), guide.md's REAPER validation line and its "Trying the HUD" paragraph,
   `native/README.md`. Build and test commands in README, guide and `engine/hud/README.md` already
   use CMake. There is no CI in the repository (an old README line claimed there was; removed).

Until phase 3 is done the Python scripts still work and are still the way to **install** the
plug-ins (`python tools/setup.py`) and run the **REAPER** tools; for **building and testing**, use
CMake.
