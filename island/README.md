# Statlet island (native)

The always-running island: Win32 + Windows.UI.Composition in C++ (C++/WinRT), built with the MSVC Build Tools.
The settings app (WinUI 3) is separate and comes later.

```
build.cmd                     builds out\statlet.exe and out\statlet-tests.exe (/W4 /WX: a warning fails the build)
out\statlet.exe               runs the island (tray icon > Quit Statlet to exit)
out\statlet-tests.exe         doctest suites: unit, render, screen (run from this folder)
out\statlet-tests.exe --e2e   also checks click-through on the real island (moves the mouse)
out\statlet-tests.exe --ci    unit suite only (no desktop needed)
```

## Build and test everything

From the repository root, `.\build.ps1` builds the island and the settings app, runs the island tests and fails on any
warning in either project. `.\build.ps1 -Ci` skips the suites that need a desktop; CI runs that. If `out\statlet.exe` is
running, the island is built into `out\gate` instead.

`.\tools\e2e-settings.ps1` (needs a desktop, not part of the gate) opens the settings app twice and checks that one
copy runs. Opening Settings starts the island if it is not running; the island never opens Settings by itself.

A missing golden image fails its test. After an intended drawing change, run the tests with `STATLET_NEW_GOLDEN=1`
(it writes the missing files), look at the new images in `tests\golden`, then commit them.

## Layout

- `src/tokens.h`: every look value from the Island Design Spec, in epx.
- `src/shape_math.h`: spring model, rounded-rect hit test, accent contrast.
- `src/gfx.h`, `gfx.cpp`: devices, bundled Manrope, device-loss recovery, and the five drawing pieces
  (`pieces.cpp`): **RoundedShape** (clip, edge, shadow), **Text**, **Icon**, **Bar**, **Acrylic**.
- `src/main.cpp`: the window: placement, hover, click-through, settings, tray, bench mode.

## How it stays cheap

- The compositor runs the spring (`SpringScalarNaturalMotionAnimation`) and the content fades, so the app runs no code per frame.
- The window never resizes; the shape is a composition clip. Outside the shape the window is click-through
  (`WS_EX_LAYERED | WS_EX_TRANSPARENT`, toggled by the 40 ms cursor poll and by mouse moves).
- Direct2D draws only when text changes, on DPI change, or after a GPU device reset. The D3D device is created on the low-power GPU.

## Gotchas

- Animations on a `CompositionPropertySet` stop once the app releases it; keep property sets alive.
- A `CompositionNineGridBrush` only stretches its surface source when that brush's `Stretch` is `Fill`.
- Text and RoundedShape register redraw callbacks with `Gfx` by address and unregister in their destructors.

## Rest-line widgets

The collapsed island shows the date and time plus CPU (top) and RAM (bottom) mini bars. Bars turn amber at 80% and red at 95%.
Its width fits what it shows (content plus padding, rounded up to 4 epx, 124-260 epx), so it springs only when that value
changes, e.g. to 176 epx for Focus. Hovering opens a 244x68 view with CPU over RAM as labeled rows (bar with a notch at 80%,
value at the end; a warning mark past 80% so the level does not rely on color). If CPU or RAM stays above 90% for 30 s
the island opens by itself, and closes when usage dips. Usage is polled every 2 s and redrawn only when a whole percent
changes. All values live in `src/tokens.h`; the logic is in `src/stats.h`. The window name (what screen readers announce)
carries the rest line, CPU and memory, and what is playing.

## Takeovers

Volume change, battery low/critical/charging/unplugged, connection lost/back and the Focus timer each open the island with a
104 epx card for 1.5-4 s, then it returns to the rest line (a hover or click keeps it open). A newer card replaces the one
showing. Cards with a bar show it under the title with its value beside it, and the detail line holds the output device
or the battery time left. Sources are OS notifications that post a window message, so nothing polls: the default output
device's endpoint volume callback (`src/events.cpp`), `WM_POWERBROADCAST` for battery and AC, and
`NotifyNetworkConnectivityHintChange` for the connection ("offline" waits 3 s so a blip says nothing). Focus starts and
stops from the tray menu (25 min, one timer that fires at the end; the rest line shows `Focus Nm` in accent in place of
the date and updates with the clock). Rules and card text are in `src/takeover.h` and covered by `TestTakeover`.
`STATLET_DEMO=battery|net|focus` plays that takeover with made-up readings, and `STATLET_FOCUS_SECONDS` shortens Focus,
for screenshots.

## Now playing

`src/media.cpp` reads Windows' media sessions (GlobalSystemMediaTransportControls: Spotify, Apple Music, browsers, any player
in the Windows media flyout) when a session event fires; nothing polls. A new track, or playback starting, peeks for 4 s:
the island widens to fit the art, title and artist (capped at 260 epx, trimmed with an ellipsis). Afterwards the rest line
follows `Tokens::afterPeek`: `Tuck` (default) puts the art before the time and an equalizer after the bars (the date steps
aside), `Clock` keeps the plain rest line, `Music` shows art, title and equalizer instead of the clock (210 epx cap). Paused
music dims the art, stills the equalizer and hides after 30 s. Hovering while music shows opens the 368x168 media card: art,
title, artist, elapsed and remaining time, the progress bar and previous / play-pause / next buttons. The progress bar is
`Tokens::progressStyle`: `Wave` (default; a stroke 3 epx either side of the line, 32 epx wavelength, drifting while playing
and flattening when paused, then a gap, the track and a stop dot) or `Line`. Progress runs as one compositor keyframe to
the end of the track; the wave drift and the equalizer are compositor loops that run only while visible, so the app stays
asleep. The after-peek view and the progress style come from the settings file (see Settings below). Rules (`FormatTime`, `RowFor`, `ShouldPeek`) are header-only in `src/media.h` and covered by `TestMedia`.

## Settings

The island reads `%LOCALAPPDATA%\Statlet\settings.json` at start, watches that folder, and rebuilds its visuals from the
new tokens when the file changes (running state carries over). `src/settings.h` lists the keys; each is optional and
clamped, so the file can be edited by hand. The settings app (`settings/`, opened from the tray menu's Settings) writes
it, and sends `WM_APP + 6` (wParam 0 release, 1 hover, 2 card, 3 music) to hold a view open for its preview.
Benchmarks ignore the file; `STATLET_TOP_OFFSET` still overrides the gap.
