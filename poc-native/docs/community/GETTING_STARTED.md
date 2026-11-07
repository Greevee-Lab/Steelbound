# Steelbound – Getting started

Steelbound plays your own copy of Steel Battalion (Europe) on modern
hardware. No game content is included. See also [Controls](CONTROLS.md),
[Graphics and packs](GRAPHICS_AND_PACKS.md), [VR](VR.md) and the
[FAQ](FAQ.md).

## Requirements

- Windows 10 or 11 (64-bit).
- A Direct3D 11 capable GPU.
- The current Microsoft Visual C++ Redistributable for Visual Studio 2015–2022, **x86** (32-bit) version – the game runtime is a 32-bit program, also on 64-bit Windows.
- Your own disc image of the PAL release (Steel Battalion, Europe) as
  `.iso` or `.xiso`. Only this version is supported.

## Install

1. Unpack the Steelbound archive into any folder. The program folder
   stays read-only; saves, settings, packs and caches live in your user
   profile (`%LOCALAPPDATA%\Steelbound\`).
2. For a portable setup (USB stick), place an empty file named
   `portable.txt` next to the exe. Everything then stays in the program
   folder.
3. Start `sb_launcher.exe`.

## Choose your disc image

1. On the **Play** page, open **Set up your game**.
2. **Browse …** to your disc image (`.iso` or `.xiso`), then **Check**.
3. The launcher shows the detected game version. Only the European (PAL)
   release can start; anything else is refused with a message.

The image is only ever read. It is never modified or unpacked. If you
have an extracted game folder instead (with `default.xbe` and `Media/`),
select it under **Advanced** → **Game files (advanced)**.

## First launch

1. On the **Play** page, press **LAUNCH GAME**.
2. The recommended preset is **Original (HD) – 120 FPS** (the default):
   the unchanged Xbox look, sharp and smooth.
3. Start the mech as described in [Controls](CONTROLS.md): hatch, ignition,
   the five toggle switches, then START.

Language (English/German) is selected at the bottom of the launcher and
applies immediately.

## Troubleshooting

- **No game data set / game data not found.** Choose your disc image on
  the **Play** page (**Set up your game** → **Browse …** → **Check**), or
  an extracted game folder under **Advanced**. The launcher starts only
  when the game version was checked.
- **This game version is not supported.** Only the European (PAL) release
  of Steel Battalion is supported. No other version or region can start.
- **Judder at 120 FPS.** On a 60 Hz display without VRR, 120 FPS may
  judder; use **Frame rate** 60 on the **Display** page instead. The
  remastered presets (**Remastered subtle**, **Remastered strong**) may
  reduce FPS; **Original (HD) – 120 FPS** is the fastest. **Render
  scaling** on **Auto** lowers the resolution briefly under load.
  Press **F2** in game to check the frame rate (see
  [Graphics and packs](GRAPHICS_AND_PACKS.md)).
