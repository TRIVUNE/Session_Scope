# 0005: Any DAW, native code: one sensor in every plug-in format, a C++ engine and HUD

- Date: 2026-11-06
- Author: TRIVUNE
- Status: accepted. Supersedes the part of decision 0003 about the native sensor ("CLAP first,
  then VST3"): both exist now, plus an Audio Unit.

## Problem

Until November 2026 the tool was built around REAPER: the sensor was a JSFX, the recorder a
ReaScript, and the reports talked about REAPER. Blueprint decision D06 plans other DAWs for phase 2,
and the VST3 sensor already worked in any host, but the rest still assumed REAPER.

The HUD needed Python (the engine and a local server) and Electron (the window): six processes, a
Python and a Node install for every user, and noticeable load on the machine that is being
measured.

## Evidence

- **Same results.** The C++ engine was compared with the Python one before Python was removed.
  Reports (text and JSON) and the simulator matched byte for byte, apart from last-digit
  differences in about 14 cells per million. The HUD engine matched frame by frame on six traces,
  about 4 million values. Golden reports in `engine/tests/golden/` keep that behaviour fixed.
- **Lighter.** On the same demo, measured over 15 s on the development PC: the native HUD used
  16.5 % of one core and 100 MB in one process. Electron plus Python used 88.4 % and 470 MB in six.
- **Validators.**
  - VST3: Steinberg's `validator`, 47 passed, 0 failed.
  - CLAP: `clap-validator` 0.4.1, 18 passed, 0 failed, 26 skipped (parameters, state, notes and
    presets, which the sensor does not have). A test host reads the records back from shared memory.
  - Audio Unit: Apple's `auval`, run by the macOS CI job.
- **REAPER still works.** The headless REAPER validation passed after the move, with the C++
  analyser printing its reports.

## Alternatives

1. **Keep Python and Electron, add DAWs only through the VST3 sensor.** Least work. But every user
   needs two runtimes, and the HUD costs about five times the CPU of a native one, on the computer
   whose audio deadline we are measuring.
2. **A web HUD in a lighter shell (Tauri or a WebView).** Smaller than Electron, still a browser
   engine and a second language for the engine. Rejected for the same measuring-cost reason.
3. **A native engine with a GPU-drawn window (chosen).** One language, C++, from the sensor to the
   HUD: GLFW, OpenGL and Dear ImGui, all small and permissively licensed, on Windows and macOS. More
   to write and review, which the equivalence checks above paid for once.

## Decision

- **One sensor, every plug-in format.** VST3, CLAP and, on macOS, Audio Unit wrappers around one
  shared core (`native/sensor_core/sensor_ring.h`), which writes the same records whatever the host.
- **REAPER becomes an optional add-on** (`hosts/reaper/`): the JSFX sensor, the ReaScripts and the
  extension for REAPER's realtime CPU. Nothing else depends on it.
- **The engine, the console program (`sessionscope record`, `list`, `report`, `simulate`) and the
  HUD are C++** (`engine/`). Python is used only by build scripts, and Node only by the REAPER Lua
  tests.
- **AAX (Pro Tools) is not built.** It needs an Avid developer account and signing, which we don't
  have. Revisit when that changes.

## Consequences

- **Engineering.**
  - A behaviour change in the engine shows up as a failing golden report, not as a mismatch with
    Python.
  - The macOS builds are proven only by CI, because there is no Mac on the team.
  - Audio Units in sandboxed hosts may not be allowed to open the shared memory. Not tested yet.
- **Cost.** No runtime to bundle for P4: the installer ships the sensor bundles and two
  executables.
- **Customer.** Any VST3, CLAP or Audio Unit host can feed the HUD, and REAPER users keep the extra
  REAPER data.

## Review when

- the sensor is first used in a host other than REAPER (Live, Cubase, Bitwig, Logic): check the
  transport fields and the ASIO-Guard behaviour in Cubase;
- an Avid developer account exists (AAX);
- the macOS CI job fails, or a Mac user reports the HUD or a bundle not loading.
