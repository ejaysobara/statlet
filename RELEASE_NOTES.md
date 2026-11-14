## Statlet 2.0.0

A Dynamic Island-style floating status pill for Windows 11. It shows the time, CPU and RAM, and opens for volume, battery,
network, a Focus timer and the music that is playing. Nothing is sent anywhere: Statlet makes no network calls.

### What is new since 1.0.0

**Music card**
- Five progress bar styles: Wavy (default), Solid, Segmented, Hairline and Gradient. Pick one in Settings > Now playing.
- An optional playhead dot on the bar: never, on hover, or always.
- Click the time on the right of the bar to switch between time left and the track length. Statlet remembers your choice.
- Album art colour: the bar, equalizer and play button can take the cover's main colour. Or follow your Windows accent, or pick
  any colour (white is new).

**Rest line**
- The weekday beside the clock ("Thu Nov 13  5:56 AM"). The pill is sized to the day, so it never twitches during the day.
  Turn it off with Show weekday.
- A small status slot at the start of the rest line shows:
  - a download icon while a file downloads into your Downloads folder, and a check when it finishes;
  - a Bluetooth icon for 3 seconds when a paired Bluetooth device connects;
  - a camera icon after a screenshot.
- Choose the order of status, clock and usage in Settings > General > Rest line order.

**Screenshots**
- Right-click the tray icon > Copy screenshot or Save screenshot. It captures the monitor your cursor is on, without the pill.
  Saved files go to Pictures\Screenshots as "Statlet <date> <time>.png".

**Sounds**
- Optional Windows sounds for a screenshot, a finished download and a finished Focus timer, one switch each.
  They use the sounds from your Windows sound scheme.

**Under the hood**
- Every polled source (usage, music, downloads, Bluetooth) starts only while it is wanted and stops when the screen is off
  or locked, so the island stays cheap when idle.

### Known limits

- Windows 11 x64 only. The installer is unsigned: SmartScreen shows "More info" then "Run anyway".
- Screenshots work only from the tray menu. They do not react to Snipping Tool or the Print Screen key yet.
- Downloads are seen in the root of your Downloads folder only ("ask where to save" downloads are not seen). Stalled downloads
  (no growth for 60 seconds) are ignored.
- Bluetooth shows connect events of paired devices only (no disconnect, no battery, no device name).
- Tested on one laptop with one external monitor.

### Install, upgrade, uninstall

- Per-user install to %LOCALAPPDATA%\Programs\Statlet, no admin prompt. Installing over 1.x keeps your settings.
- Start with Windows is off by default; tick it in the installer or in Settings.
- Uninstall removes the program and also deletes %LOCALAPPDATA%\Statlet (your settings and the log).
- Updates: watch the repository's Releases (GitHub Watch > Custom > Releases), or use Settings > About > Releases page.

**Feedback:** open an issue. Attach %LOCALAPPDATA%\Statlet\island.log if something went wrong.

SHA256 of `Statlet-Setup-2.0.0.exe`: `92a88130af83c21b09e6f7a352111edb02ec64cf5695068860157800bd49138e`
