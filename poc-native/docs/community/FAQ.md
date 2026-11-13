# Steelbound – FAQ

## Why do I need my own disc?

Steelbound is a preservation project: launcher and client only, without
any game content. Your own copy keeps the project on the safe side, and
your disc image is only ever read, never modified. See
[Getting started](GETTING_STARTED.md).

## Which version is supported?

Only the European (PAL) release of Steel Battalion, as a `.iso` or
`.xiso` disc image. Other versions are refused with a clear message
instead of starting. Support for the US release is planned.

## Windows says “Windows protected your PC”. Is that safe?

Windows Defender SmartScreen warns about new programs that are not
signed. Steelbound is an unsigned hobby project; code signing is planned.
Click **More info → Run anyway**. Only use the archive from the official
Releases page of the project.

## My PC is old or weak. Will it run?

There are no hard minimum specs. Choose the graphics preset **Original
(HD) – Performance** on the **Graphics** page: original look, 60 FPS and
native world resolution. It runs fine on an 11-year-old laptop. Press
**F2** in game to check the frame rate.

## How do I get 120 FPS?

Select **Frame rate** 120 on the **Display** page (part of the default
preset **Original (HD) – 120 FPS**). The game logic still runs at 30;
the extra frames are smooth in-between frames. On a 60 Hz display
without VRR, 120 FPS may judder; use 60 then. Press **F2** in game to
check the frame rate.

## Is any game data included?

No. No disc-image parts, textures, screenshots, audio, movies, saves or
packs. Everything visible or audible in the game comes from your own
copy.

## How do I report a bug?

In the game press **F11** (with screenshot), **F9** (with a 5-second
clip) or **Esc → Feedback**. The game creates a feedback package and
opens a pre-filled GitHub issue plus the folder; drag the files in and
submit. Nothing is sent automatically. Please add the mission, the VT,
the steps and your settings. Details:
[Alpha testing](ALPHA_TESTING.md#sending-a-report).

## Which keys are there besides driving and fighting?

**Esc** game menu (pauses), **Pause** pause/continue, **F11** / **F9**
feedback, **F12** photo mode, **F2** performance overlay, **F10** free
the mouse, **Alt + Enter** window/fullscreen. All are listed in
[Alpha testing](ALPHA_TESTING.md#hotkeys-at-a-glance) and
[Controls](CONTROLS.md).

## How does photo mode work?

Press **F12** (or **Esc → Photo mode**); the game pauses and the photo
panel opens. **F12** again or **Take photo** saves the picture. The photo
folder is shown on the launcher page **Audio & extras**. Not available in
VR yet.

## Where are my saves?

In your user profile: `%LOCALAPPDATA%\Steelbound\` (folder **Save
games**). Settings (`user.cfg`), packs, caches and logs live there too.
In portable mode (`portable.txt` next to the exe), everything stays in
the program folder instead.

## Can I use saves from my Xbox or another emulator?

Yes: **Advanced › Saves & data › Import…** takes a folder, a ZIP or a SAV
file. Your current saves are backed up first, and you decide whether
conflicting files are overwritten or kept. **Export…** writes all saves
and replays into a ZIP. See
[Alpha testing](ALPHA_TESTING.md#importing-and-exporting-saves).

## How do I protect my pilot while testing?

A pilot who dies without ejecting is deleted – that is the original game
rule. Switch on **Protect saves (test aid)** (launcher **Advanced ›
Diagnostics**, or in game **Esc › Settings › General**); the session then
plays on a throw-away copy of your saves. See
[Alpha testing](ALPHA_TESTING.md#protecting-your-pilot-while-testing).

## I see less of the cockpit on my wide monitor.

Missions use the full width (Hor+, never stretched). **Cockpit** on the
**Display** page (or **Cockpit: camera back** in **Esc › Settings**)
moves the camera back so more of the cockpit monitors stays visible.
**Original** is the unchanged view. Desktop only, no effect in VR.

## Do I need the original controller?

No. Keyboard/mouse and any XInput gamepad work. The original Steel
Battalion controller via USB adapter is optional; setup is described in
[Controls](CONTROLS.md).

## Can I share packs I made?

Yes, as long as the pack contains only **your own artwork** – textures you
created yourself – and never textures taken from the game. Sharing packs is
your responsibility; see the legal note in
[Graphics and packs](GRAPHICS_AND_PACKS.md).

## Where do I change settings?

Everyday settings are on the launcher pages (**Play**, **Display**,
**Graphics**, **Packs**, **Controls**, **Audio**, **Advanced**). In-game
fine-tuning is on the ESC menu (**Save as default** keeps it). Complete
sets of settings can be stored as **Saved setups** under **Advanced**.
