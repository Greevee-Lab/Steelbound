# Steelbound – Graphics and packs

All graphics options live on the launcher pages **Display** and
**Graphics**; fine-tuning in game is on the ESC menu. Terms below are
exactly the launcher terms.

## Graphics preset

One choice on the **Graphics** page. Remastered presets may reduce FPS.

| Preset | Meaning |
|---|---|
| Original (HD) – 120 FPS | Unchanged Xbox look, sharp and smooth. Recommended. |
| Remastered subtle | Better lighting, effects and textures. May reduce FPS. |
| Remastered strong | The strongest remastered look. May reduce FPS. |
| Custom | Appears when you change an option. Based on a preset; **Reset** returns to it. |

## Display options (Display page)

- **Display mode**: Window, Borderless fullscreen, or Exclusive
  fullscreen (Alt + Enter switches in game).
- **Resolution**: window size. Fullscreen uses your monitor's resolution.
- **Refresh rate**: only in exclusive fullscreen.
- **Frame rate**: 30 (original), 60, or 120 (smooth). 60 and 120 add
  smooth in-between frames; the game logic always runs at 30.
- **Aspect ratio**: never stretched. Menus, movies and HUD stay 4:3 with
  black bars; missions use the full width (Hor+).
- **Widescreen sides**: what the mission shows left and right of the 4:3
  area: Full width, Cut to 4:3, or Fade out (with **Fade width** and
  **Fade strength**).
- **Render scaling**: how sharp the 3D world is drawn. Auto stays sharp
  and lowers the resolution briefly under load.
- **Desktop preview**: used when you choose **PLAY IN VR** (see
  [VR](VR.md)).

## Graphics options (Graphics page)

- **World lighting**: modern light and shade for the outside world
  (On, as on Xbox, or Off, simplified).
- **Shadows** and **Soft shadows**: need World lighting.
- **Effects**: explosion light, embers and soft smoke. Original, Subtle,
  Strong, or Custom.
- **Anti-aliasing**: smooths jagged edges. Off (original), Final image,
  World, or Both. The World part needs World lighting.
- **Cockpit monitor**: grain and interference on the cockpit screens.
  Original (grain + interference), Grain/scanlines only, or Interference
  only.
- **Target label avoids readouts**: moves the lock-on label when it
  would cover RELOAD/MAIN. Applies after restart; off is the original.
- **Textures**: shows the texture detail and the packs it comes from;
  **Packs →** jumps to the Packs page (see below).
- **More fine-tuning in game: press ESC.** Colors, fog, bloom and light
  detail are on the ESC menu.

## Performance overlay (F2) and ESC menu

- **F2** shows the performance overlay (FPS, frame times, CPU/GPU load).
  **Shift + F2** moves it to the next corner. The setting is remembered.
- **ESC** opens the settings overlay; the game keeps running behind it.
  **TAB** switches pages: **SETTINGS**, **WORLD**, **IMAGE**,
  **EFFECTS**, **VR**. Arrows change values, **B** compares with the
  original, **Save as default** keeps the values for the next start.

## Packs

Packs change how the game looks without touching your game files.
**Texture packs** replace textures with sharper ones. **Effect packs**
tune explosions, light and colors. Without packs the game looks exactly
like the original.

- **Install**: on the **Packs** page use **+ Add pack …**, or drop a
  `.sbpack` file onto the launcher window. **Check all** validates every
  pack; **Open folder** opens the packs folder.
- **Order**: higher packs win. Drag packs or press Alt + Up/Down to
  reorder. When two packs change the same texture, the higher one wins
  and the lower one reports it as covered by the higher pack.
- **Texture detail**: Original, 2×, or 4× (best). Sharper textures where
  a pack has them; otherwise the original.
- **Limit by category**: lowers the detail per category (HUD & fonts,
  Cockpit, Ground, Buildings, Vehicles, Effects, Menus, World, Other).
  It can only lower, never raise.
- **Conflicts**: the selected pack lists covered textures and known
  effect values. An effect pack overrides your own colors and effects
  settings when the game starts (shown as a lock on the Graphics page).
- **Fits your game** tells whether the pack was made for your game
  version (yes (PAL), no, or not checked yet).

## Pack Creator overview

The **Pack Creator** (separate app, **Open Pack Creator** on the Packs
page) builds a pack from your own disc image in seven steps: Image,
Dump, Categories, Upscale, Text, Review, Export. You dump the original
textures, upscale them, reconstruct text without AI smearing, review
every variant side by side, and export a validated `.sbpack`. The app
shows which helpers it needs.

## Legal note

Packs are derived from your own disc. Packs are created and shared by
their users, who are solely responsible for them. The launcher only
checks packs technically, never legally. This notice is not legal
advice.
