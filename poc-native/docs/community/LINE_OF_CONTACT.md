# Steelbound – Playing Line of Contact

Steelbound also runs **Steel Battalion: Line of Contact**, the online sequel – including online matches
against other pilots through our own meeting point. Line of Contact support is new and marked
**experimental**: it plays well in our tests, but please tell us about anything odd (see
[Reporting problems](#reporting-problems)).

No game data is included – you need your own copy of *Steel Battalion: Line of Contact*.

All VTs, missions (maps) and items are always unlocked for every player, as with the original unlock code back
then, so everyone in a match has the same choice; pilots you already have get the missing VTs the next time they load.

## What you need

- Windows 10 or 11 (64-bit), mouse and keyboard, a gamepad or the original controller.
- Your own Line of Contact disc as an image: an `.iso`/`.xiso` file, the extracted disc folder, or
  its `default.xbe`.
- Supported: the regular release (version 1.0, with or without the media patch). Not supported: the
  Japanese *Pilot Test* disc and a game with the **title update** applied – the launcher tells you
  so and asks for the unmodified disc.
- For online play: an internet connection. No account needed – you play as a guest.

## Set up (once)

1. Start `sb_launcher.exe` and switch to **Line of Contact** at the top.
2. Under **Advanced → Your Line of Contact disc**, pick your image and press **Check**.
   A green line (*"Line of Contact found … Ready to play"*) means it is fine.
3. If you want to play over the internet: open the **launcher settings** with the gear at the top right
   and choose **Connect to steelnet** under *Online & updates* (on first start the launcher asks you
   this once). Optional: create a **steelnet account** under **Multiplayer › Account** (sub-item in
   the sidebar while Multiplayer is open) – no password, no email. New here? Pick an account name
   (shown to other players) and press *Create account*. Already have one? *Sign in with Discord*, or
   use your login and password, or a code from another PC or from the admin. Afterwards the page shows
   one step: secure your account with *Secure with Discord* (recommended), a login and password
   (private, only for signing in) or by writing down your recovery code. Redeeming a code, signing out
   and deleting the account sit under *Manage*.
4. Press **Play Line of Contact**.

Each game has its own settings: display, graphics, controls, audio and extras you change while
**LINE OF CONTACT** is selected apply to Line of Contact only, and Steel Battalion keeps its own. On the
first start after the update both games begin with your previous settings. Options that do not exist in
Line of Contact stay visible but greyed out, with the reason in the tooltip – for example the 50 Hz TV
speed (PAL), the cockpit view options, aim assist and automatic gear shifts, replay labels and texture
packs. Launcher settings (gear: online & updates, language, window size, background, data folder) are
the same for both. Line of Contact keeps its own saves (see **Saves & data** on the LoC page).

## Online lobbies

On the **Multiplayer** page, the *Match* card lets you choose **Public** (default) or **Private (room code)**.
If the launcher is set to *Check version only* or *Offline*, the card shows a note with a button to the
launcher settings. Line of Contact needs steelnet: without *Connect to steelnet* (gear at the top right,
Online & Updates) **Play Line of Contact** is greyed out and the page shows why; the network settings can
still be changed.

- **Public:** leave the room code empty and press **Play Line of Contact**. In the game the host creates a match
  (**System Link Play → MatchMaking → Create Match**); it shows up for every pilot under
  **Search Match** with the host's name and an estimated ping (e.g. `~Falcon 120ms`, `~` = guest).
  Pick it to join. Full matches and matches already in a mission are not listed.
  **You may meet players you don't know.**
- **Private (room code):** the host chooses *Private (room code)* and presses **Play Line of Contact**. After a few
  seconds the *Match* card (and the *Multiplayer* card on the Play page) shows *Your lobby's room code: XXXX-XXXX* – press **Copy** and send it to your
  friends. They type the code into **Room code** and press **Play Line of Contact**; a code always joins that
  private lobby.
- If Windows asks whether `sb_ldr.exe` may use the network, allow it (private networks are enough
  at home).
- First start only: the game asks you to create a pilot.
- After a mission: confirm the result screens with the main weapon button and **A** until
  **DEBRIEFING**, leave with **SIGN OUT** → **YES**, then **NEXT**. You are back in the main menu.
  For another round nobody needs to restart: the host creates a new match, the others search again.

The connection goes through the meeting point `relay.steelbound.dev`. If two PCs cannot reach each
other directly, the game traffic is relayed automatically. Game traffic is encrypted end to end.

### Rollback and input delay

Steelbound negotiates **rollback** for every mission automatically: your own inputs show at once
and the game corrects itself when a fellow pilot's input arrives late. If rollback cannot be used,
the game shows *Rollback off – higher input delay* with the reason for a few seconds and plays the
mission in the original lockstep. Nothing pauses online, just like on the console: F11 and the Esc
menu only open on **your** screen while the match keeps running.

## Network settings

In the Line of Contact tab of the launcher, the *Network* card on the **Multiplayer** page lets you switch the network features
on or off, for example to compare them while testing. Everything is **on** by default; changes take
effect at the next start, and **Restore defaults** turns everything back on.

| Setting | What it does |
|---|---|
| **Rollback netcode** (on) | Turn off only for testing; if any player disables it, the match uses the classic lockstep mode. The three settings below only work with rollback and are greyed out without it. |
| **Instant own movement** (on) | Your VT reacts immediately; target markers stay at the confirmed position. |
| **Smooth opponents** – Off / **Auto** (recommended) / Fixed | Hides small network corrections of other VTs; they are shown slightly later instead of jumping. |
| **Adaptive lower input delay** (on) | Lowers the input delay by one tick when the connection allows it and raises it again when corrections pile up. |

If a setting is not at its default, the Play page shows a short note below **Play Line of Contact**.

## Launcher settings (gear)

The gear at the top right opens settings that apply to both games: **Online & updates**
(*Connect to steelnet* = online play, server status and version hint; *Check version only* = one request
to GitHub at startup; *Offline* = no network access), language and window size, background image,
test tools, data folder and version information. The version check only shows a hint and a link to the
release page – it never downloads or installs anything. Next to the gear, a small status shows
*steelnet online/offline* and *Update x.y.z available* when the matching option is on.

## Options

On the Line of Contact pages of the launcher:

| Option | What it does |
|---|---|
| **Picture format** – 4:3 (Original) / **16:9 (game mode)** | **16:9 is the default**: the console's own widescreen mode, never stretched, not marked as experimental. The widescreen sides default to *Fade out* (visible on screens wider than 16:9). Saved settings switch to 16:9 once; you can still choose 4:3 under Advanced. Takes effect on the next start. |
| Frame rate | *Original rate* shows the mission exactly as the console (20 pictures per second, least graphics power); higher rates add in-between frames. |

Your own looking around and crosshair react immediately, and corrections of your fellow pilots are
smoothed – both are on by default (see *Network settings*). **Game speed never depends on your frame rate**: the mission
always runs at the console's 20 ticks per second.

## F3 – network info

Press **F3** in the game for a small panel: route (direct or relay), ping, packet loss, mode
(rollback or lockstep), input delay, rollbacks per second and smoothing. **Shift+F3** moves it to
another corner. When you report lag, a screenshot with the panel open helps a lot.

## If the game runs slowly

- **Laptops with two graphics chips:** in Windows *Settings → System → Display → Graphics*, add
  `sb_ldr.exe` and choose *High performance*.
- **Weaker PCs:** choose the frame rate *Original rate* and render scaling 1, or a window size of
  1920 × 1080. On a weak graphics card you can also lower *Menu frame limit* (e.g. to 60).
- **Short stutters in the first minute of your very first match** are normal: the game prepares its
  graphics programs once and keeps them for later.

## If something goes wrong

The launcher's online card tells you what is wrong, for example:

| Message | What to do |
|---|---|
| No internet connection / meeting point cannot be found | Check your internet connection. |
| Meeting point not reachable | Check firewall or antivirus; try again in a minute. |
| Room code not found | Check the code; the host's game must still be running. |
| The host closed the lobby | The host's game has ended. Ask for a new room code. |
| Fellow pilot cannot be reached | Allow `sb_ldr.exe` in the firewall on **both** PCs. |
| Too many attempts | Wait a minute and try again. |
| A newer version is required | Download the latest release. |

If your partner quits or loses the connection during a mission, you get the console's result
(**VICTORY** or **CONNECTION ERROR**) and return to the lobby.

## Reporting problems

Press **F11** in the game (or **F9** for a 5-second clip) and choose **Bug report**. The game
creates a feedback package in its data folder (game picture, end of the log, system and network
values – never your key presses or anything from your desktop) and opens a pre-filled GitHub issue
with **Which game? = Line of Contact**. Drag the files from the folder into the issue and submit
it. Nothing is sent automatically. Without the game: on the project's GitHub page open
*Issues → New issue → Bug report* and choose *Line of Contact* under *Which game?*.

Please mention: online or offline, public or private lobby, how many pilots, mission and VT, and
whether it happens again. Never attach game files (ISO, XBE, saves).

Controls: see [CONTROLS.md](CONTROLS.md). Have fun, pilot!
