<p align="center">
  <img src="docs/img/logo.png" alt="Statlet logo" width="96">
</p>

<h1 align="center">Statlet <sub>(working name)</sub></h1>

<p align="center">
  <img alt="Windows 11" src="https://img.shields.io/badge/Windows-11%20x64-0078D4">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-green">
  <img alt="Pre-release" src="https://img.shields.io/badge/release-v0.1.0--friends%20(pre--release)-orange">
</p>

<p align="center">
  <img src="docs/img/hero.gif" alt="The island resting, opening on hover to show CPU and RAM, then tucking away" width="386">
</p>

A small floating "island" at the top-center of your Windows 11 screen, in the style of the iPhone's Dynamic Island.
It sits quietly as a thin status pill, then opens for the things you care about: volume, battery, network, a Focus
timer and the music that's playing. It is built to cost almost nothing while idle: it wakes only when something changes.

**Status: early test build (v0.1.0-friends).** Rough edges expected. Statlet is a working name and may change.

## What it does

**Always visible: the rest line.** Date and time, plus two mini bars for CPU (top) and RAM (bottom). Bars turn amber at
80% and red at 95%. The pill resizes to fit what it shows.

**Hover to open.** Hovering opens a larger view with labeled CPU and RAM rows. If CPU or RAM stays above 90% for 30
seconds, the island opens by itself so you notice.

**Takeovers.** Short cards that open and then tuck away on their own:
- volume changes (with the output device),
- battery low, critical, charging, unplugged,
- connection lost and back,
- Focus timer (25 min by default; start and stop it from the tray menu).

**Now playing.** Reads Windows' media sessions, so it works with Spotify, Apple Music, browsers and any player in the
Windows media flyout. A new track peeks with album art, title and artist. Hover for the media card: progress bar,
elapsed and remaining time, previous / play-pause / next.

**Settings app.** Tray icon > Settings. Choose how the island looks (glass tint and strength, accent color, shape and
text), how far it sits from the top, the Focus length, the CPU/RAM warning levels, what the rest line shows while music
plays, and whether Statlet starts with Windows. A live preview shows changes as you make them.

**Behaves with your setup.** Follows display scaling and monitor changes, stays out of the way of full-screen apps,
and pauses its polling while the screen is off or the PC is locked or asleep.

## Screenshots

The rest line, always visible: date, time, and CPU / RAM mini bars.

![The island's rest line](docs/img/rest-line.png)

Hover to open it: labeled CPU and RAM rows, with a notch at the 80% warning level.

![The island opened on hover](docs/img/hover.png)

Hover while music plays for the media card: art (blurred here), title, artist, progress, controls and CPU / RAM.

![The media card](docs/img/media-card.png)

Takeovers, like this battery card, open on their own and tuck away:

![A battery takeover card](docs/img/takeover.png)

When CPU or RAM runs high, the bars turn red and the island opens on its own:

![The island showing CPU 97% and RAM 100% with warning marks](docs/img/high-usage.png)

The settings app (tray icon > Settings), where changes apply to the island right away:

![Statlet Settings, General page](docs/img/settings.png)

## Install, run, quit

1. Download `Statlet-friends-*.zip` from [Releases](../../releases) and extract it (right-click > Extract All).
2. Open the `Statlet` folder and double-click `statlet.exe`.
3. Windows will say "Windows protected your PC" because the build is unsigned: **More info > Run anyway**.
4. Quit: tray icon > **Quit Statlet**.

Nothing to install: no .NET, Visual C++ or Windows App SDK runtime is needed. There is no installer yet.

**Uninstall:** turn off "Start with Windows" in Settings if you turned it on, quit Statlet, delete the folder, and delete
`%LOCALAPPDATA%\Statlet`.

## Requirements and known limits

- Windows 11, 64-bit (Intel/AMD). Not tested on Windows 10 or ARM.
- Unsigned, so SmartScreen warns. No installer or auto-update.
- Autostart is opt-in (Settings > General > Start with Windows). It adds one value under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`; turning it off removes it.
- Tested on one laptop with one external monitor, so other multi-monitor setups may misbehave.
- Statlet only writes to `%LOCALAPPDATA%\Statlet`: your settings and a small event log (`island.log`, app events only).
  Nothing is sent anywhere.

## Feedback

Open an issue on the [Issues](../../issues) page: what you saw, what you did, your Windows version and monitor setup.
Attach `%LOCALAPPDATA%\Statlet\island.log` if the island vanished or behaved oddly.

## Build from source

Needs Visual Studio with the C++ x64 tools and the .NET 8 SDK.

```
.\build.ps1        build and test the island (native C++) and the settings app (WinUI 3)
.\package.ps1      build the portable zip into dist\
```

See `island/README.md` for how the island works and `docs/adr/` for design decisions.

## License

MIT, see `LICENSE`. The bundled Manrope font is under the SIL Open Font License (`island/assets/OFL.txt`).
