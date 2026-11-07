# What REA found in MiniMeters

`C:\Program Files\MiniMeters\MiniMeters.exe`, SHA-256
`525a8f0c0e4073bdc8e813560367b1bd3d98c9c928dbed4c82f646b5d93d9074`, read with REA 6.8.0 and
Ghidra 12.1.4 on 2026-11-07. Static analysis only: nothing was run or observed live.

## Observed

- A Rust application with a C++ renderer class `D3D11Renderer` (RTTI names), plus OpenGL 3.3 / ES
  and Vulkan back ends; shaders are cross-compiled with SPIRV-Cross (HLSL, GLSL, Metal text in the
  binary).
- Windows render setup (decompiled `FUN_140166990`):
  `CreateDXGIFactory1` -> `IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` ->
  `D3D11CreateDevice` -> `CreateSwapChainForComposition` (B8G8R8A8, 2 buffers, flip, premultiplied
  alpha, frame-latency waitable object flag) -> `DCompositionCreateDevice`, `CreateTargetForHwnd`
  (topmost), `CreateVisual`, `SetContent(swap chain)`, `Commit` -> `SetMaximumFrameLatency(1)` ->
  `GetFrameLatencyWaitableObject` (a message box and exit if that fails).
- Present (`FUN_1400ff210`): presents, and recreates the swap chain when the window size changed
  or the device was lost.
- Also imports `CreateWaitableTimerExW` (high-resolution waitable timer) and `timeBeginPeriod`.
- Shaders: a heatmap/spectrogram shader that reads values from one texture and colours them
  through a 1-D lookup texture; text from a single-channel glyph atlas (discard below alpha
  0.04); round dots from a smoothstep circle.

## Second pass (same day): layout, settings and window features

Observed in strings and C++ type names (RTTI), not in running code:

- **Layout system.** Classes `LayoutManager`, `LayoutQuad`, `LayoutChild` (with
  `recurse_draw_resize_handles`), `SingleModule`, `Module`, and prefs presets. Layouts are named
  "Horizontal", "Vertical" and "Quad". Strings "Created from a split on the left, increase right
  module size", "Insert Module", "Remove Module", "Select Module", "Add modules to the layout":
  modules sit in splits with **drag handles between them to resize**.
- **Per-module settings saved as text.** The default preferences embedded in the exe, per module:
  `Module.Spectrogram { bool enabled  int order  bool popout.enabled  int size }`. So each module
  has an on/off, an order, a **size in pixels**, and whether it is **popped out into its own
  window** ("Creating Pop-Out Module for", "Module is Popped-Out.").
  Defaults: "Horizontal" 1920 x 128 at the top of the screen, "Vertical" 200 x 1080.
- **FPS limit** menu (`build_fps_limit_menu`) with "VSync On [Default]" and lower rates ("Lowering
  the FPS"), and an `FpsPanel` that prints "FPS: %.2f".
- **Global speed** (`global_settings { speed }`) that each module can follow or override
  (`speed_use_global`).
- **Tray icon** ("MiniMeters Tray Icon", `Shell_NotifyIconA`), **hide from taskbar**
  (`HIDE_FROM_TASKBAR`, "useful if you have many popped-out windows"), **global hotkeys**
  (`RegisterHotKey`, `GlobalHotKey`), **window opacity** (0/25/50/75/100 %) and blur
  (`DwmEnableBlurBehindWindow`), `SHAppBarMessage` imported (docking / taskbar queries; usage not
  traced).
- **Themes** as text files ("primary = r,g,b", "secondary", "selection", "text"...) in a themes
  folder, with a **colour map editor** for gradients ("Gradient LUT Texture", "Color Map Editor").
- **Peak hold** markers: "peak indicator threshold in dBFS", spectrum "decay".
- Bundled libraries named in its licence text: pffft and MuFFT (FFT), Microsoft Detours, GLM,
  sse2neon, json.cpp, VST3 SDK.

## Not found

No strings about CPU, cores, PDH or `NtQuerySystemInformation`: MiniMeters has no CPU meter.
Bounded: only strings Ghidra defined were searched.

## What SessionScope took from it

The swap chain setup (`engine/hud/renderer_d3d11.cpp`), the textured heatmap, and the idea of a
strip of modules. Not copied: its look or code.
