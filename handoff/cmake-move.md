# The move from Python to CMake: done

The person decided (7 Nov 2026) that the build moves from the Python scripts in `tools/` and
`hosts/reaper/tools/` to CMake, and Python is then removed. All three phases are done (phase 3 on
11 Nov 2026, after the person said to go ahead): there is no Python left in the repository.

## How to build now

CMake 4.4.5 and Ninja 1.13.2 must be on the PATH. On the person's current computer they come from
scoop (`scoop install cmake ninja`); an earlier computer had them portable in `C:\tools`.

```
cmake --preset windows              # once (and after adding files): sets up build/cmake
cmake --build --preset windows      # builds what changed
ctest --preset windows              # unit tests
ctest --preset windows-plugins      # VST3 validator, clap-validator, test hosts
                                    # (they use the real shared memory: close any DAW first)
cmake --workflow --preset windows   # all of the above (not the plug-in checks), then install
build/engine/sessionscope install   # only install (--uninstall removes)
build/engine/sessionscope reaper install|uninstall|validate
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

## Phase 3: what replaced what

| Python script | Now |
| --- | --- |
| `tools/install_paths.py`, `install_plugins.py`, install steps of `setup.py` | `sessionscope install [--uninstall] [--from DIR]`: `engine/src/install.h/.cpp`, tested by `engine/tests/test_install.cpp` in a scratch folder |
| `hosts/reaper/tools/reaper_install.py` | `sessionscope reaper install [--copy] [--resource DIR]` and `reaper uninstall`: `engine/src/reaper_tools.h/.cpp` (a junction on Windows, made with `FSCTL_SET_REPARSE_POINT`) |
| `hosts/reaper/tools/reaper_validate.py` | `sessionscope reaper validate [--reaper PATH] [--timeout S] [--work DIR] [--keep]`, same file; the summary is `validation_summary()`, tested by `engine/tests/test_reaper_tools.cpp` |
| `tools/setup.py` | `cmake --workflow --preset windows` (configure, build, test, then the `install_sessionscope` target, which runs `sessionscope install`) |
| everything else in `tools/` | already replaced in phases 1 and 2 (table above) |

Things learned in phase 3:

- `clap-validator`'s denormals test compares two timings of a plug-in that takes well under a
  microsecond per block; on this computer it warns on every run. It now runs as its own check
  (`clap_validator_denormals`) where a warning is allowed; the other tests still fail on any warning.
- `validate.lua` saves `result.json` after every step, so "finished_utc is there" never meant
  "finished". The last job now sets `complete = true`, and the command looks for that.
- The console program finds the repository and `build/native` from its own location
  (`build/engine/..`); `--from` / `--repo` override that.
- The first `reaper validate` on a computer is slow: REAPER scans every installed plug-in once
  into `build/reaper-validate`.
