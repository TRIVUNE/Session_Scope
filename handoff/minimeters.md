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

## Not found

No strings about CPU, cores, PDH or `NtQuerySystemInformation`: MiniMeters has no CPU meter.
Bounded: only strings Ghidra defined were searched.

## What SessionScope took from it

The swap chain setup (`engine/hud/renderer_d3d11.cpp`), the textured heatmap, and the idea of a
strip of modules. Not copied: its look or code.
