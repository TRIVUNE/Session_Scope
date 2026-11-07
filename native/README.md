# Native components

| Folder | What | Status |
| --- | --- | --- |
| `../hosts/reaper/extension/` | `reaper_sessionscope.dll` (REAPER only): times REAPER's own processing of each audio block through the documented audio hook, and writes `host.csv` into the trace the recorder script is writing, and publishes every block live into the shared-memory ring `SessionScope_host_v1` for the HUD ([decision 0003](../docs/decisions/0003-native-next.md)) | **built and validated** in REAPER 7.77 |
| `vst3_sensor/` | `SessionScope.vst3`: stereo pass-through effect for the master track; publishes one record per block into the shared-memory ring `SessionScope_v2` (layout 2: adds the host's bar start) that the HUD window reads | **built and verified** in REAPER 7.77 (needs the VST3 SDK: `python tools/get_vst3sdk.py`) |
| `clap_sensor/` | `SessionScope.clap`: the same sensor for CLAP hosts; `test_clap_sensor.cpp` is a small host that checks the records it writes | **built and validated** with `clap-validator` 0.4.1 (needs the CLAP headers: `python tools/get_clap.py`) |
| `../hosts/reaper/third_party/reaper-sdk/` | `reaper_plugin.h` from github.com/justinfrankel/reaper-sdk at `b2ca1ec` (zlib licence, notice kept in the file) | |

The sensor exists as VST3 and as CLAP; both are thin wrappers around `sensor_core/sensor_ring.h`,
so they write the same records.

## Build (Windows)

With the pinned Zig toolchain (a self-contained clang; nothing is installed system-wide):

```sh
python tools/get_zig.py        # once: downloads Zig 0.16.0 into .toolchain/ and checks its SHA-256
python tools/build_native.py   # builds build/native/reaper_sessionscope.dll
```

Or with Visual Studio Build Tools and CMake:

```sh
cmake -S native -B build/native -A x64
cmake --build build/native --config Release
```

The DLL exports only `ReaperPluginEntry` and needs nothing beyond Windows 10 or 11 (C++ runtime
linked in, C runtime = the Universal CRT that ships with Windows). It uses only REAPER's plain C
structures, so a MinGW-ABI build works; the SDK's C++ interfaces are not used.

## Install

Copy `build/native/reaper_sessionscope.dll` into `<REAPER resource path>/UserPlugins/` and restart
REAPER. `python tools/setup.py` installs it when REAPER is present; `python hosts/reaper/tools/reaper_validate.py` does this automatically for its isolated REAPER only.

## What has been checked (REAPER 7.77, dummy audio 48 kHz / 256)

- Loads, registers, announces itself (`host_extension: "0.1.0"` in every trace's meta.json).
- Times every block: about as many rows in `host.csv` as the sensor has blocks, none lost.
- Fixture F7b (anticipative FX off): REAPER's per-bar realtime load follows the Workload JSFX's
  known cost within about 1.5 percentage points at every step from 0 % to 103 %.
- Fixture F7a (anticipative FX on, REAPER's default): the audio thread stays at about 0.6 % while
  worker threads render ahead, until the load passes 100 % and the render-ahead runs dry
  (then 104–555 % in bar 24, depending on the run).
  See docs/research/D-timing.md.

## Still to check

1. Spike E: REAPER's Performance Meter with and without the extension on fixture F2.
2. A real audio interface instead of the dummy device.
3. Unload while recording (T04): REAPER must quit cleanly; `host.csv` ends where it ends.
