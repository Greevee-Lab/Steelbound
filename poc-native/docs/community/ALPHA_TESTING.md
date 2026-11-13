# Steelbound – Alpha testing

Thank you for testing the Steelbound public alpha. This is an early
build: it runs well on the machines it has been tried on so far, but your
PC, GPU, monitor and controller may show problems nobody has seen yet.
Your reports decide what gets fixed first. See also
[Getting started](GETTING_STARTED.md), [Controls](CONTROLS.md),
[Graphics and packs](GRAPHICS_AND_PACKS.md), [VR](VR.md) and the
[FAQ](FAQ.md).

## System requirements

- Windows 10 or 11 (64-bit).
- A Direct3D 11 capable GPU with current drivers.
- Microsoft Visual C++ Redistributable for Visual Studio 2015–2022,
  **x86** (32-bit) version. Install the x86 package even on 64-bit
  Windows – the game runtime is a 32-bit program. The launcher starts
  without it and tells you before the game starts if it is missing
  (`VCRUNTIME140.dll`).
- Your own disc image of **Steel Battalion (Europe, PAL)** as `.iso` or
  `.xiso`. No game data is included in this package.

There are no hard minimum specs. On an older or weaker PC choose the
graphics preset **Original (HD) – Performance** (see below).

**Windows warning on first start:** Windows Defender SmartScreen may show
*“Windows protected your PC”* for `sb_launcher.exe`. Click **More info →
Run anyway**. Steelbound is an unsigned hobby project; code signing is
planned. Only use the archive from the official Releases page.

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

## Hotkeys at a glance

| Key | Function |
|---|---|
| **Esc** | Game menu: Continue, Settings, Photo mode, Feedback, Quit (pauses the game) |
| **F11** | Feedback with a screenshot |
| **F9** | Feedback with a 5-second video clip |
| **F12** | Photo mode: open / take photo (also Esc → Photo mode) |
| **Pause** | Pause / continue the game |
| **F2** | Performance overlay (Shift + F2 moves it) |
| **F10** | Free / capture the mouse |
| **Alt + Enter** | Window / fullscreen |
| **F8** | VR: recenter view |

These keys can be reassigned on the launcher's **Controls** page
(**Assign …**). The full layout is in [Controls](CONTROLS.md).

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
6. **Graphics presets:** Original (HD) – 120 FPS, Original (HD) –
   Performance, Remastered subtle, Remastered strong – look and frame
   rate (press **F2** in game).
7. **Controls:** keyboard/mouse, an XInput gamepad, and – if you have
   one – the original Steel Battalion controller (see
   [Controls](CONTROLS.md)).
8. **In-game menu:** pause, settings, photo mode (F12), feedback (F11/F9).
9. **Cockpit perspective** on wide monitors (see below).
10. **Optional:** importing saves, VR (experimental), texture/effect packs.

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

**In the game (recommended):** press **F11** (feedback with screenshot),
**F9** (feedback with a 5-second clip) or open **Esc → Feedback**. The game
pauses, asks for *Bug report* or *Idea* and a short text, and creates a
package in the data folder (`feedback/<date-time>/`): game screenshot or
`clip.mp4`, the end of the log with your user name and paths removed,
version/GPU/settings, your text. It shows what is inside before anything
else happens; **Open GitHub issue + folder** opens a pre-filled issue page
in your browser and the folder. Drag the files into the issue and submit
it. Nothing is uploaded automatically.

Please add to the issue:

- the **mission** and the **VT** you were piloting,
- the **steps** that led to the problem and whether it happens again,
- **settings** you changed (preset, display mode, resolution, frame rate,
  controller).

**By hand:**

1. Right after the problem, open the data folder (see above).
2. Copy `logs/sb_harness.log` and any new file from `crash/`. Do not
   start the game again first – the log is overwritten at each start.
3. Open a new issue with the **Bug report** template on the project's
   GitHub page, attach the files and add `VERSION.txt` from the program
   folder, your Windows version and your GPU model.

The files contain settings, file paths on your PC (for example your
Windows user name) and technical data. They contain no game data. Look
through them before sending if you are unsure, and remove anything you
do not want to share.

## "Unable to load … Record" at the title screen

If the game shows **Unable to load KIA Record.** (or *Player*,
*Mission*, *RELEGATED* Record / *SYSTEM DATA*) after you press START on
the title screen, one of your save files could not be read – for
example a save copied from a real Xbox or another emulator (Steelbound
signs saves differently), or a save that was cut off while the game was
closing. Press the main weapon button to continue; the game then
starts that list empty.

The game checks the save files before every start and repair them where nothing is lost (a
foreign signature is rewritten, the data stays). A file that cannot be
repaired is moved to `hdd\_backup\repair-<date>-<time>\` so the
message no longer appears. Nothing is deleted.

To start over completely: **Advanced › Saves & data › Reset saves…**.
After a confirmation, all pilots, records and replays are moved to
`hdd\_backup\saves-<date>-<time>\` and the game starts like a fresh
install (settings and cache stay). Close the game first.

## Protecting your pilot while testing

Steel Battalion deletes a pilot who dies without ejecting – that is the
original game rule. While testing you can switch on **Protect saves
(test aid)**: in the launcher under **Advanced › Diagnostics**, or in
the game under **ESC › Settings › General**.

While it is on, the game plays on a throw-away copy of your saves
(`hdd\_savelock\`). Inside the session everything behaves normally
(a dead pilot is gone, records are written), but your real saves are
never touched. The next start begins again from your real saves.
*Saves protected (test aid)* is shown in the ESC menu and in the F2
panel as long as the protection is active.

- Switching it on in the ESC menu takes effect immediately.
- Switching it off in the ESC menu keeps the protection until you quit
  (shown as *On until exit*), so nothing half-protected reaches your
  real saves.
- **Keep protected changes** (ESC › Settings › General, press Enter
  twice) copies the current session into your real saves; the previous
  saves are moved to `hdd\_backup\savelock-<date>-<time>\` first.
- *Back to title* (a game restart) stays in the same session.

## Importing and exporting saves

Under **Advanced › Saves & data** in the launcher:

- **Import…** takes a folder, a ZIP or a single SAV file with Steel
  Battalion saves (for example from a real Xbox or another emulator).
  The launcher first shows a preview (number of saves, pilots, replays and
  conflicting files). For conflicts, **Yes** overwrites, **No** merges and
  keeps your existing files, **Cancel** imports nothing.
- Your current saves are backed up to `hdd\_backup\import-<time>\`
  before anything is changed. Saves of other games (different Xbox title
  ID) are refused.
- **Export…** writes all saves and replays into one ZIP file.

Close the game before importing. Foreign signatures are repaired by the
save check on the next start.

## Photo mode

Press **F12** in game (or **Esc → Photo mode**). The game pauses and the
photo panel opens; **F12** again or **Take photo** saves the picture. The
photo folder, resolution and format are on the launcher page **Audio &
extras** (card *Extras – photo mode*), which also shows the default
folder. Photo mode is not available in VR yet.

## Pause and in-game menu

- **Pause** stops and continues the game (assignable on the **Controls**
  page, action *Pause*).
- **Esc** opens the game menu and pauses the game while it is open
  (setting *ESC menu pauses*). **Save as default** keeps changed settings
  for the next start.

## Cockpit perspective (wide monitors)

On wide and ultrawide monitors the mission view uses the full width
(Hor+, never stretched). **Cockpit** on the launcher page **Display**
(card *Aspect ratio*) moves the cockpit camera back and zooms in, so more
of the cockpit monitors stays visible. **Original** is the unchanged view.
In game the same setting is **Cockpit: camera back** under
**Esc › Settings**. It has no effect in VR.

## Performance preset

**Original (HD) – Performance** on the launcher page **Graphics** (also in
**Esc › Settings**) is meant for older or weaker PCs: original look,
60 FPS and native world resolution. Resolution changes apply after a
restart. Check the result with **F2**.

## Known issues

- **Textures or shaders break after several missions in a row.** Quit and
  restart the game; your saved progress is kept.
- **Landscape at the screen edge pops in** in some missions.
- **Replays:** importing and viewing replays does not fully work yet.
- **Only the European (PAL) release** of Steel Battalion is supported.
  Other versions and regions are refused with a message; US and Japanese
  releases are planned.
- **VR is experimental.** Expect rough edges with headsets, runtimes and
  performance.
- The legal texts (`LICENSE`, `THIRD_PARTY.md`) are drafts.
- Steelbound does not use the network: no updates, no telemetry, no
  automatic crash upload. Reports reach us only when you send them.
