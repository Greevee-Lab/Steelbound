# Steelbound – Graphics and packs

All graphics options live on the launcher pages **Display** and
**Graphics**; fine-tuning in game is on the ESC menu. Terms below are
exactly the launcher terms.

## Original look and experimental options

The default is the **original look**: the unchanged Xbox picture in 4:3
with black bars, never stretched. Everything that changes this picture is
marked as experimental: in the launcher with a small **flask icon** on the
card and behind the preset name (hover it for the reason), in the ESC menu
with the same flask icon (or **EXP** where the icon font is missing; amber
when it is active). Experimental options can cause artefacts – for example
at the sides in widescreen or with HD texture packs.

| Experimental (not the original look) | Neutral (never marked) |
|---|---|
| World lighting, shadows, fog, bloom, light, colors, sharpening, anti-aliasing, ambient occlusion | Frame rate (30/60/120) and in-between frames |
| Effects (explosion light, embers, soft smoke) and the fun effects (wireframe, vaporwave) | Display mode (window/fullscreen), resolution, refresh rate |
| Cockpit monitor effect off or reduced (**Clear view**) | Render scaling |
| HD textures from texture packs, effect packs | Controls, aim assist, automatic gears |
| Widescreen sides (Full width, Fade out), cockpit camera, panorama, cockpit width | Sound, VR (own page), saves |
| Hide cockpit, target label avoids readouts | |

- While any experimental option is active, the **Play** page shows a
  warning triangle and *"You're not playing the original look"* with the
  list (hover the triangle to see each active setting). The **Graphics**,
  **Display** and **Packs** entries in the navigation show the same small
  triangle while something experimental is active there. **Back to
  original** resets only these graphics options (4:3, no HD textures,
  original lighting/effects/monitor/cockpit) and saves; frame rate,
  resolution, controls, sound and VR stay. Undo (Ctrl+Z) brings your
  values back.
- **Start original once** starts one game in original mode: no
  experimental options, no packs, 4:3. Your saved settings are not
  changed.
- In game, **Shift + F8** switches live between the original look and your
  settings, with a short message at the top ("Original look" / "Your
  settings"). Use it to check whether an artefact comes from an
  experimental option. Not switched live: the pack list (packs stay as they
  are, only HD textures and effect-pack values switch) and "target label
  avoids readouts" (needs a restart). Opening the ESC menu always returns
  to your settings first, so **Save as default** never saves the
  comparison. The key can be changed in the launcher: **Controls** page,
  **Assign …**, entry "Original look ↔ your settings" (also a combination
  such as Ctrl + F7, or a gamepad button). Without an entry in the layout,
  `orig_key` in `user.cfg` still applies (for example `orig_key=CTRL+F7`,
  `orig_key=OFF`).
- At the start of the game a short message appears when experimental
  options are active.

## Graphics preset

One choice on the **Graphics** page. Remastered presets may reduce FPS.

| Preset | Meaning |
|---|---|
| Original | Unchanged Xbox look, smooth at your monitor's refresh rate (Monitor (auto)). Recommended. |
| Performance | Original look for older / weaker PCs: 60 FPS, native world resolution. Resolution applies after restart. Not experimental (only frame rate and resolution). |
| Remastered subtle (experimental) | Better lighting, effects and textures. May reduce FPS. |
| Remastered strong (experimental) | The strongest remastered look. May reduce FPS. |
| Clear view (experimental) | Easier to see: no lighting, no monitor noise. Not original. |
| Custom | Appears when you change an option. Based on a preset; **Reset** returns to it. |

Every preset sets **Widescreen sides** to **4:3 (original)**. Widescreen
is your own choice on the **Display** page (experimental); it makes the
preset Custom.

## Display options (Display page)

- **Display mode**: Window, Borderless fullscreen, or Exclusive
  fullscreen (Alt + Enter switches in game).
- **Resolution**: window size. Fullscreen uses your monitor's resolution.
- **Refresh rate**: only in exclusive fullscreen.
- **Frame rate**: 30 (original), 60, or 120 (smooth). 60 and 120 add
  smooth in-between frames; the game logic always runs at 30.
- **Aspect ratio**: never stretched. The original picture is 4:3 with
  black bars; menus, movies and HUD always stay 4:3.
- **Widescreen sides** (experimental): what the mission shows left and
  right of the 4:3 area: **4:3 (original)** (default), Full width, or Fade
  out (with **Fade width** and **Fade strength**, default 8 %, 100 %).
  Older settings files keep the widescreen choice you made; the Play page
  then shows the experimental note.
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
- **ESC** opens the game menu (**Continue**, **Settings**, **Photo
  mode** (also **Shift + F12**), **Feedback** (also **F11** / **F9**), **Quit**); the game is paused while it is
  open (setting "ESC menu pauses"). In **Settings** the pages are tabs in
  the first row: **General**, **World**, **Image**, **Effects**, **VR**;
  **TAB / Shift+TAB**, the arrows on the tab row, a mouse click or the
  gamepad shoulder buttons switch pages. Mouse, keyboard and gamepad work
  in all menus. Arrows change values, **B** compares with the
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
- **Fits your game** tells whether the pack works with your copy. Packs
  work with **every supported version** (Europe and USA): textures are
  matched by their content, and both versions share the same textures.
  You'll see for example "yes (all Steel Battalion versions)", "yes (made
  for your version)" or "yes (made for Europe, works with USA – matching
  textures only)". A texture that differs between versions is simply left
  original – never a wrong image. Packs for another game show "no".
- **Making your own pack**: packs with your own artwork can say
  `"game": { "family": "steel-battalion" }` in `manifest.json` instead of
  a game hash; the Pack Creator writes this for you.

## Legal note

Packs must contain only their authors' **own artwork**, never textures
taken from the game. Packs are created and shared by their users, who are
solely responsible for them. The launcher only
checks packs technically, never legally. This notice is not legal
advice.
