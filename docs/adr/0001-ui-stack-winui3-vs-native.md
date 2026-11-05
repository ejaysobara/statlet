# ADR-0001: Build the island on native Win32 + DirectComposition (C++)

**Date**: 2026-11-03
**Status**: proposed
**Deciders**: Ejay (pending), Claude (benchmark and recommendation)

## Context

Ejay's priorities for the island are responsiveness, minimal resource use and correct behavior. The shell in `IslandBar/` is WinUI 3 (.NET 8, unpackaged), with acrylic shaped by a rounded window region. The question is whether to stay on WinUI 3 or move to native Win32 + DirectComposition.

Round 1 compared WinUI 3 with a minimal native prototype. The rule agreed afterwards: move WinUI's animation off XAML layout and re-measure. If animating still costs more than 5% of a core, go native.

Machine: Ryzen 7 250 (3.26 GHz cycle counter), 15 GB RAM, 2560 px panel at 144 Hz and 125% scale. The panel is driven by the RTX 5060 dGPU. Dynamic Edge was running throughout, and the test island sat 300 epx below it.

What was benchmarked:
- `bench/native/island.cpp`: the native pill, in C++ built with MSYS2 g++. It has a black fill, a rectangular region and the round-1 spring.
- `bench/winui-before/`: the shell at HEAD, using the current design (acrylic, rounded region, current spring).
- `bench/winui-after/`: the shell with the composition-driven animation. That is the same change now uncommitted in `IslandBar/`.
- `bench/run-bench.ps1` is the harness. Raw results are in `bench/results*/`.

## Results

### Round 2: animation off XAML layout

Each value is the median of 5 animation runs and 2 idle runs. CPU is counted exactly from CPU cycles (`QueryProcessCycleTime`). The round-1 counter, `TotalProcessorTime`, is charged per 15.6 ms clock tick, which aliases with 144 Hz frame work. It swung between 1% and 20% across identical native runs, so round 1's CPU column is not trustworthy.

| Metric | Native C++ | WinUI before | WinUI after (no XAML layout) | WinUI after, no per-frame region |
|---|---|---|---|---|
| CPU during expand/collapse (% of one core, toggling every 1 s) | **6.8%** | 20.1% | 18.6% | 15.2% |
| Share of time spent animating | 76% | 48% | 48% | 48% |
| CPU per animated frame | **≈0.6 ms** | ≈2.9 ms | ≈2.7 ms | ≈2.2 ms |
| Idle CPU | **0.20%** | 0.75% | 0.74% | n/a |
| Frame interval p99 / max | 7.4–8.0 / 8.3–13.8 ms | 7.3–7.5 / 9.6–14 ms | 7.4–7.6 / 9.2–13.2 ms | n/a |
| Startup to first frame (round 2, 10 runs) | **233 ms** | 322 ms | 319 ms | n/a |
| Idle private working set / commit | **14 / 58 MB** | 31 / 93 MB | 32 / 94 MB | n/a |

Native animates longer because its older, softer spring takes more frames, which is why the per-frame row matters.

- **Removing XAML layout saved only about 7%** of WinUI's animation cost (20.1% → 18.6%).
- **The window region costs about 3.4 points.** Native pays it too.
- **About 15% of a core is WinUI's own per-frame tick.** That covers the `CompositionTarget.Rendering` loop, the XAML render pass and managed code. The WinUI shell cannot drop that tick: the acrylic is shaped by the window region, and only the app can update the region every frame, so WinUI must run code each frame.
- **Native is about 4× cheaper per animated frame and 3.7× cheaper at idle.**
- **Frame pacing is a tie.** Both stacks hold 144 Hz, with p99 under 8 ms.

### Round 1 (kept for memory and startup)

Startup was 260 ms native vs 313 ms WinUI ReadyToRun vs 355 ms WinUI JIT. About 225 ms of that, in both stacks, is creating the Direct3D device on the NVIDIA GPU: `D3D11CreateDevice` alone took 219–234 ms. Idle private working set was 14 MB vs 29–30 MB, and commit 58 MB vs 84–86 MB.

## Decision

Build the island on **native Win32 + DirectComposition in C++**, compiled with the MSYS2 UCRT64 g++ that is already installed. This applies the agreed rule: WinUI still costs 18.6% of a core while animating, against 6.8% for native. The gap comes from WinUI's per-frame tick, not from anything in the shell's code. Get WinRT APIs (GSMTC, Bluetooth, clipboard history) through C++/WinRT, from the MSYS2 `mingw-w64-ucrt-x86_64-cppwinrt` package. `bench/native/island.cpp` is the starting point.

## Alternatives Considered

### Stay on WinUI 3 with the composition-driven animation
- **Pros**: first-class C# APIs for every planned widget. XAML text, images and controls. UI Automation accessibility for free. The existing shell and design-token work carries over.
- **Cons**: 3× native's CPU while animating, about 4× per frame. 3.7× native's idle CPU. 2× the private memory, 36 MB more commit, and about 90 ms slower startup.
- **Why not**: it misses the agreed 5% bar by almost 4×. The remaining cost is the framework's own frame loop, so the shell has nothing left to optimize.

### Native in Rust (`windows` crate)
- **Pros**: same resource profile as C++. The WinRT projection is built in.
- **Cons**: Rust is not installed, and it has the same hand-drawn UI cost as C++.
- **Why not**: C++ builds today with no new toolchain. Revisit if C++/WinRT under MinGW proves painful.

## Consequences

### Positive
- About 0.6 ms of CPU per animated frame and 0.2% idle CPU.
- 14 MB private memory, no runtime, and a 0.5 MB binary.
- Fastest startup. The remaining startup time is mostly the GPU device.

### Negative
- Every widget's UI is hand-built in Direct2D/DirectWrite: text, album art (WIC), buttons, hover and pressed states, sliders, hit testing and DPI changes. Expect roughly 3–5× the UI code of XAML.
- Accessibility (UI Automation providers) must be written by hand. It must not be skipped.
- Acrylic must be reproduced. Options are a DComp backdrop or `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)`, the second shaped by the region exactly as today.
- The WinUI shell in `IslandBar/` becomes a reference for design tokens and behavior. Its uncommitted composition-animation change is moot unless this ADR is rejected.

### Risks
- **C++/WinRT on MinGW is less traveled than on MSVC.** Mitigation: spike GSMTC first. Fall back to the MSVC Build Tools or Rust if it fights back.
- **Both stacks create their D3D device on the default adapter, which is the NVIDIA dGPU here.** That keeps the dGPU awake. Prefer `DXGI_GPU_PREFERENCE_MINIMUM_POWER` where the iGPU drives the panel: the 780M device took 75–153 ms to create here. The battery and dGPU power impact is not yet measured.
- **Found along the way:** the shell's trimmed Release publish (its default profile) crashes at launch with `E_NOINTERFACE` in combase. Untrimmed publish works. This only matters if WinUI is kept.

## Addendum (2026-11-03): the native Statlet island, measured

Ejay chose the hybrid: the island is native C++, and the settings app is a separate WinUI 3 app. `island/` is the shell that was built. It differs from the decision above in three ways:
- It builds with the MSVC Build Tools and the Windows SDK's C++/WinRT, not MSYS2 g++.
- It uses Windows.UI.Composition instead of raw DirectComposition, so it gets host-backdrop acrylic.
- The compositor runs the spring, so the app runs no code per frame. A CPU copy of the spring is kept only for hit testing.

Bench settings: `bench/run-bench.ps1 -StartupRuns 5 -IdleRuns 2 -IdleSeconds 30 -CycleRuns 3 -CycleSeconds 20 -Only statlet,native,winui-after`. Raw log: `bench/results-log-statlet.txt`.

| Metric | Statlet (acrylic, compositor spring) | Native prototype (black, per-frame region) | WinUI after (acrylic) |
|---|---|---|---|
| CPU while toggling every 1 s (% of one core) | **0.23%** | 7.4% | 19–26% |
| Idle CPU | **0.17%** | 0.22% | 0.77% |
| DWM GPU while toggling | 6.7% | 4.7% | 3.3–6.7% |
| Startup to first frame (median of 5) | 290 ms | 235 ms | 315 ms |
| Idle private working set / commit | 20 / 36 MB | 14 / 57 MB | 32 / 94 MB |

DWM CPU swings 7–34% between runs of the same target, because other apps share DWM, so it is left out of the table. The acrylic blur adds about 2 points of DWM GPU over the black prototype. Statlet records no frame times, because the compositor paces its frames.
