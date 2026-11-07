# Steelbound

Steelbound is a game preservation project: it keeps the original-Xbox
classic **Steel Battalion (Europe)** (Capcom, 2002) faithfully playable on
modern hardware. It is a launcher and a client for **your own copy** of the
game — an Xbox high-level-emulation layer (32-bit, D3D8 → D3D11) plus a
starter application. No game content is included or distributed.

This repository is source-available (see [License](#license)). It is not an
official product and is not affiliated with Capcom or Microsoft.

## What Steelbound is

- A **launcher** (`sb_launcher.exe`): pick your own game copy, configure
  settings, manage texture and effect packs, then play.
- A **client** (`sb_ldr.exe` + `sb_emu.dll`): runs the game natively with
  high-level emulation of kernel, XDK and D3D8 calls.
- An **open package format** (`.sbpack`, see `PACK_FORMAT.md`) for community
  texture and effect packs: images and parameters, never code.
- A **Pack Creator**: builds your own packs locally from your own copy of
  the game (dump, upscale, review, export).

## What Steelbound is not

- It contains **no game content**: no XBE or disc-image parts, no textures,
  no screenshots, no packs. You need your own copy of the game.
- It performs **no downloads, updates or telemetry** and has no network
  access at all. Every tool ships with the release.
- It does not modify your game copy: your disc image is only ever read.

## Features

- Faithful **original look** (default, pixel-checked against references) and
  optional remaster presets.
- **120 FPS** via interpolated frames on a real-time tick.
- Any resolution with correct aspect ratio everywhere: 2D screens, movies
  and menus get black bars (never stretched); the mission view uses true
  Hor+ widescreen.
- Shading and lighting after the original GPU semantics, monitor effects,
  image borders.
- In-game settings menu (ESC) and performance overlay (F2).
- Controls: keyboard/mouse, gamepads, and the original Steel Battalion
  controller over USB (you install the WinUSB driver yourself).
- Window, borderless and exclusive-fullscreen modes.
- Texture/effect packs (`.sbpack`) with per-pack order, on/off switch,
  quality level and conflict display.
- English and German UI (English default).
- Self-tests for input, rendering and pack validation.

## System requirements

- Windows 10 or later (64-bit), x86-64 CPU.
- A Direct3D 11 capable GPU.
- Your own copy of Steel Battalion (Europe, original Xbox). Only the PAL
  release is supported; other versions are refused with a clear message.
- Microsoft Visual C++ Redistributable 2015–2022, **x86** (32-bit) version
  (also on 64-bit Windows).
- To build: Visual Studio 2022 (32-bit build is required), CMake 3.24 or
  later and Python 3 (generates the built-in English texts).

## Documentation

Player guides (English) are in `poc-native/docs/community/` (in a release
archive: `docs/`): Getting started, Controls, Graphics and packs, VR, FAQ,
and Alpha testing (what to test, where logs and crash reports are).

## Quickstart

1. Build the client and the launcher (see [Building](#building)), or use a
   release archive. Place `sb_launcher.exe` next to `sb_ldr.exe`
   (or pass `--client-dir <folder>`).
2. Start the launcher and select your own game copy (the folder containing
   `default.xbe` and `Media/`).
3. Configure display, audio and controls, then play.

Your saves, settings, caches and packs live in your user profile
(`%LOCALAPPDATA%\Steelbound\`); the program folder stays read-only. For a
portable setup (USB stick), place an empty file named `portable.txt` next
to the exe — then everything stays in the program folder.

## Packs and the Pack Creator

Texture and effect packs (`.sbpack`) are optional add-ons made by users:
upscaled or hand-made textures and effect parameters. Without any pack the
game looks exactly like the original.

The Pack Creator builds such packs locally from your own copy: dump the
original textures, upscale them on your own GPU, reconstruct text and lines
without AI smearing, review every variant side by side, and export a
validated `.sbpack`. Packs made from game textures are created and shared
by their users, who are solely responsible for them (see
[Legal notice](#legal-notice)).

## Building (Visual Studio 2022, 32-bit is required)

```
cmake -S poc-native/harness  -B poc-native/harness/build-community  -G "Visual Studio 17 2022" -A Win32 -DSB_EDITION=community
cmake --build poc-native/harness/build-community --config Release
cmake -S poc-native/launcher -B poc-native/launcher/build-community -G "Visual Studio 17 2022" -A Win32 -DSB_EDITION=community
cmake --build poc-native/launcher/build-community --config Release
```

## Self-tests

- `sb_launcher.exe --selftest` (for the validation tests, point the test at
  your own copy: `SB_TEST_XBE=<…>/default.xbe`, `SB_TEST_MEDIA=<…>`)
- `input_selftest.exe`, `render_selftest.exe`
- `python poc-native/tools/edition/tests/test_edition.py`

## Cleanliness check

`python poc-native/tools/edition/clean_check.py <folder> --report report.md`
checks an export for game content, secrets, machine paths and network
access (it compares against the **local** `default.xbe` of your own copy:
`--xbe <path>`).

## License

Our own code is **source-available** under the **PolyForm Noncommercial
License 1.0.0** (see `LICENSE`). In short, and without replacing the
license text:

- free noncommercial use and modification by the community;
- no commercial use by third parties without the author's written permission
  (see `COMMERCIAL.md`);
- voluntary donations to the author change nothing about the license
  (see `DONATE.md`).

“Source-available” is deliberate: this license is not OSI-approved, so this
is not called “Open Source”.

- The `.sbpack` format specification (`PACK_FORMAT.md`) is openly documented
  under **CC BY 4.0**, so anyone can build their own tools for it.
- Bundled or referenced third-party components keep their own licenses;
  see `THIRD_PARTY.md`.
- Contributions by third parties require a rights agreement; see
  `CONTRIBUTING.md` (draft, pending legal review).

## Legal notice

> This project ships no game content. You need your own copy of the game.
> Texture and effect packs derived from game textures are created and shared
> by their users, who are solely responsible for them. This notice is not
> legal advice.

The launcher shows the author and license of every pack. Packs from others
are only checked technically, never legally.

## Trademarks

“Steel Battalion” is a trademark of Capcom. “Xbox” and “DirectX” are
trademarks of Microsoft. This project is a fan-made preservation effort; it
is not an official product and is not connected to or endorsed by Capcom or
Microsoft.
