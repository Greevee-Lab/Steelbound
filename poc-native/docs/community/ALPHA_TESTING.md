# Steelbound – Alpha testing

Thank you for testing Steelbound Alpha 1. This is an early build: it
works well on the developer's PC, but it has not yet been tried on other
machines. Your reports decide what gets fixed first. See also
[Getting started](GETTING_STARTED.md), [Controls](CONTROLS.md),
[Graphics and packs](GRAPHICS_AND_PACKS.md), [VR](VR.md) and the
[FAQ](FAQ.md).

## System requirements

- Windows 10 or 11 (64-bit).
- A Direct3D 11 capable GPU with current drivers.
- Microsoft Visual C++ Redistributable for Visual Studio 2015–2022,
  **x86** (32-bit) version. Install the x86 package even on 64-bit
  Windows – the launcher and the game runtime are 32-bit programs.
  Without it, Windows reports a missing `VCRUNTIME140.dll`.
- Your own disc image of **Steel Battalion (Europe, PAL)** as `.iso` or
  `.xiso`. No game data is included in this package.

## What is in the package

| File / folder | Purpose |
|---|---|
| `sb_launcher.exe` | Launcher – start here |
| `sb_ldr.exe`, `sb_emu.dll` | Game runtime (started by the launcher) |
| `sb_native.cfg` | Default settings (read-only; your changes go to `user.cfg`) |
| `profiles/` | Default control layouts (keyboard, Xbox pad) |
| `lang/` | Languages (English is built in, German as a file) |
| `docs/` | This guide and the player documentation |
| `README.md`, `LICENSE`, `THIRD_PARTY.md` | Project notes and licence texts (drafts) |
| `VERSION.txt` | Build date, commit and file checksums |

## Quick start

1. Unpack the archive into a new, empty folder.
2. Optional, recommended for testing: create an empty file named
   `portable.txt` next to `sb_launcher.exe`. Saves, settings, logs and
   crash reports then stay in that folder, which makes them easy to find
   and to delete.
3. Start `sb_launcher.exe`. On a fresh install the **Play** page shows
   **Set up your game**: choose your disc image and press **Check**.
4. Press **LAUNCH GAME**.

## What to test

Please tell us what you tried, even if everything worked.

1. **First start:** does the launcher open, is the text readable (DPI,
   scaling), does **Set up your game** accept your PAL disc image?
2. **Boot to the menus:** intro movies, main menu, sound.
3. **One full mission:** start the mech (hatch, ignition, the five
   toggle switches, START), drive, fight, finish or fail the mission.
4. **Saving and loading:** save, quit, start again, load.
5. **Display modes:** window, borderless and exclusive fullscreen; 60
   and 120 FPS; your monitor's aspect ratio (4:3 menus, wider missions,
   never stretched).
6. **Graphics presets:** Original (HD) – 120 FPS, Remastered subtle,
   Remastered strong – look and frame rate (press **F2** in game).
7. **Controls:** keyboard/mouse, an XInput gamepad, and – if you have
   one – the original Steel Battalion controller (see
   [Controls](CONTROLS.md)).
8. **Optional:** VR (experimental), texture/effect packs.

For each problem, note: what you did, what you expected, what happened,
and whether it happens again.

## Logs and crash reports

The launcher shows where your data lives:

- **Advanced › Saves & data** shows the data location and has
  **Open folder**.
- **Advanced › Diagnostics › Show** lists the game client folder, the
  settings file (`user.cfg`) and the active settings.

Inside the data folder:

| Folder / file | Content |
|---|---|
| `logs/sb_harness.log` | Log of the last game start (overwritten at each start) |
| `crash/crash_<date>_<time>_<n>.txt` | Short crash report (error address, module, registers) |
| `user.cfg` | Your settings |

Default data location: `%LOCALAPPDATA%\Steelbound\`. In portable mode
(`portable.txt` next to the exe) everything is in the program folder.

### Sending a report

1. Right after the problem, open the data folder (see above).
2. Copy `logs/sb_harness.log` and any new file from `crash/`. Do not
   start the game again first – the log is overwritten at each start.
3. Add `VERSION.txt` from the program folder, your Windows version and
   your GPU model.
4. Send everything to the contact who gave you the alpha package, the
   way you agreed with them.

The files contain settings, file paths on your PC (for example your
Windows user name) and technical data. They contain no game data. Look
through them before sending if you are unsure, and remove anything you
do not want to share.

## Known limitations

- **Only the European (PAL) release** of Steel Battalion is supported.
  Other versions and regions are refused with a message.
- **Tested on one PC so far.** Other GPUs, drivers, monitors and
  Windows setups may show problems that have not been seen yet – that is
  what this alpha is for.
- **VR is experimental.** Expect rough edges with headsets, runtimes and
  performance.
- The Pack Creator is not part of Alpha 1.
- The legal texts (`LICENSE`, `THIRD_PARTY.md`) are drafts.
- Steelbound does not use the network: no updates, no telemetry, no
  automatic crash upload. Reports reach us only when you send them.
