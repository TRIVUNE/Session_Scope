# The HUD's code: a map

The HUD is the always-on-top window that shows how a DAW session is doing. This folder is its
code; the numbers it shows come from `engine/src/`. Every file starts with a comment that says what
it does and where its ideas and system calls come from. Start reading at `main.cpp`.

## How one second goes

```
10 times a second                          every screen refresh (60-144 times a second)
-----------------                          ----------------------------------------------
HudEngine::tick()      sensor data         HudWindow::begin_frame()   waits for the screen
SystemMonitor::tick()  this computer       HudView::draw()            lays out and draws the modules
                                           HudWindow::end_frame()     puts the frame on screen
```

Data arrives ten times a second, but moving things (the heatmap) are placed by time and drawn every
refresh, so they slide smoothly instead of jumping.

## Files

| File | What it does |
| --- | --- |
| `main.cpp` | Program start: reads the command line (`--demo`, `--replay`, `--screenshot`) |
| `hud_app.h/.cpp` | Makes the window, engine and readings, runs the main loop, saves settings |
| `hud_view.h/.cpp` | Lays the modules out in a strip, drag and drop, the right-click menu |
| `modules/module.h` | What a module is; the list of modules |
| `modules/module_list.*` | Makes every module, in the default order |
| `modules/draw_helpers.*` | Shared pieces: framed boxes, number formatting, jet colours, meters, cards |
| `modules/peak_meter.cpp` ... | One file per module (see `module.h` for the list) |
| `window/hud_window.*` | One window (GLFW): frames, its own ImGui context, opacity, frame-rate limit, screenshots. The main strip and every pop-out are each one |
| `window/menu.*` | Right-click menus: real Windows menus (they reach past the window), ImGui elsewhere |
| `window/desktop.*` | Windows: tray icon, show/hide hotkey, taskbar button |
| `window/renderer.h` | What drawing on the graphics card must do |
| `window/renderer_d3d11.cpp` | Windows: Direct3D 11 + DirectComposition |
| `window/renderer_gl.cpp`, `window/gl.h` | macOS: OpenGL 3.3 |
| `look/theme.h` | Colours and fonts |
| `look/fonts_data.h` | The fonts built into the program |

Data sources in `engine/src/`: `hud_engine.*` (the sensor plug-in's data: lateness, transport,
dropouts), `system_monitor.*` (this computer: `core_cpu.*`, `process_cpu.*`,
`process_resources.*`, `cpu_clock.*`), `hud_settings.*` (`hud.ini`).

## Where things come from

| What | From |
| --- | --- |
| Window, mouse, keyboard | GLFW 3 (github.com/glfw/glfw, zlib licence) |
| Drawing boxes, text, menus | Dear ImGui 1.91.9 (github.com/ocornut/imgui, MIT licence) |
| PNG screenshots | stb_image_write (github.com/nothings/stb, public domain) |
| Font | Inter 4.1 (github.com/rsms/inter, SIL Open Font Licence) |
| Swap chain setup on Windows, modules in a strip, values-in-a-texture heatmap, colour lookup table | MiniMeters, read with REA + Ghidra (`handoff/minimeters.md`); our code is our own |
| Using the screen's own GPU instead of the fastest | Measured here: 120 fps instead of 52 on a two-GPU laptop (top of `renderer_d3d11.cpp`) |
| Per-core load, process CPU, memory, CPU speed | Windows and macOS system calls, named in each file in `engine/src/` |
| Jet colours | The classic spectrum-analyser / MATLAB "jet" scale, picked by the person from a screenshot |
| Light panel look | A plug-in screenshot the person gave (white boxes, thin dark borders) |
| Resize handles, pop-outs, FPS limit, tray icon, hotkey, hide from taskbar, opacity, own menus | Features MiniMeters has (seen with REA, `handoff/minimeters.md`); built here from Windows' documented APIs |

## Cost

Measured with `--demo` on a 120 Hz laptop screen: about 15 % of one core at the screen's frame
rate, 6 % at 60 fps, 2 % at 30 fps (right-click > Frame rate); 0.2 % while minimised or hidden (it
keeps measuring but stops drawing); about 76 MB of memory.

## Building and checking

```sh
cmake --build --preset windows                            # build (after `cmake --preset windows` once)
ctest --preset windows                                    # run every test
build/engine/SessionScopeHUD.exe --demo                   # try it without a DAW
build/engine/SessionScopeHUD.exe --demo --screenshot out.png --wait 9000   # one picture
build/engine/SessionScopeHUD.exe --demo --screenshot out.png --wait 6000 --test-drag 1607,150,1757,150
                                                          # ...after playing a mouse drag in
build/engine/SessionScopeHUD.exe --demo --settings test.ini   # live test with its own settings file
```

Testing tips: screenshot runs never read or write `hud.ini`. Live tests must pass `--settings`
so they never change the person's own `hud.ini`. On a 200 % screen Windows rescales a test
script's mouse, so aim with `--test-drag` instead of moving the real mouse.

The build compiles every `.cpp` in this folder and its subfolders; a new file is picked up
automatically.
