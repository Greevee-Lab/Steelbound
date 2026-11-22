# Steelbound – Controls

Keyboard and mouse always work. A gamepad (XInput) and the original
Steel Battalion controller (USB) work in parallel. Input only works
while the game window has focus.

## Keyboard and mouse (Standard layout)

| What | Key |
|---|---|
| Menus: select / confirm / START | Arrows / SPACE / ENTER |
| Skip briefing movie | L or middle mouse button |
| Close cockpit hatch | H |
| Ignition | I |
| Toggle switches (at the status check) | 1 2 3 4 5 |
| START (all bars above the line) | ENTER |
| Main weapon (primary fire) | Left mouse button or SPACE |
| Sub-weapon | Right mouse button or F |
| Lock-on | Middle mouse button or L |
| Reload magazine | R |
| Chaff / Override | C / O |
| Night vision / FSS | N / V |
| Zoom in / out | Z / X |
| Mode / Map zoom | TAB / M |
| Throttle / Brake | W / S |
| Turn left / right | A / D |
| Slide-step (dodge, steer with A/D) | Left Ctrl or mouse X1/X2 |
| Gear down / up (from N: E = 1st gear) | Q / E |
| Tuner up / down | T / G |
| Eject (only when the game tells you, otherwise your save is lost) | Shift + Delete |
| Aim | Mouse (or arrows; Shift + arrows = sight only) |
| Free / capture mouse | F10 |
| Game menu (Continue, Settings, Feedback, Quit; pauses the game) | ESC |
| Pause / continue | Pause key (assignable: Controls page, "pause") |
| Feedback with screenshot / with 5-second clip | F11 / F9 |
| Screenshot (quick, uses the photo-mode settings, no menu) | F12 |
| Photo mode (menu; in photo mode F12 takes the photo) | Shift+F12 |
| Original look ↔ your settings (compare live) | Shift + F8 (assignable: Controls page, "orig_view") |
| Exit a running replay (the mouse no longer ends replays; gamepad, original controller and joystick work as in the original) | Backspace (assignable: Controls page, "replay_exit") |
| Performance overlay | F2 (Shift + F2 moves it) |
| Network overlay in Line of Contact online play (route, ping, loss, rollback, smoothing; see LINE_OF_CONTACT.md) | F3 (Shift + F3 moves it; assignable: Controls page, "Network overlay") |
| Window / fullscreen | Alt + Enter |
| VR: recenter view | F8 |

Mouse aiming is **Absolute** by default (the crosshair follows the
mouse); **Sensitivity** (1–16) is on the **Controls** page next to the
mode. EJECT is Shift + Delete in the Standard layout: a combination, so
you do not press it by accident (Ctrl is already the slide-step pedal).
Own layouts can be edited
with **Assign …** on the **Controls** page.

**Looking around with the mouse (Line of Contact):** with `mouse_sight=1`
in `user.cfg` the mouse moves the sight lever instead of the crosshair.
The game treats this lever like the stick on the original controller: how
far you push it sets how fast the view turns. Move the mouse away from the
centre to look around and back to the centre to stop; small movements near
the centre do nothing. The view stops at about 90° to either side and 45°
up or down and stays where you left it. When you drop into a mission, the
lever starts at the centre.

The comparison key (**Original look ↔ your settings**) is listed with the
client keys in **Assign …**. To learn a combination, hold Shift, Ctrl or
Alt and press the key; releasing Shift, Ctrl or Alt on its own assigns
just that key. A gamepad, joystick or original-controller button also
works. **Clear all …** keeps it, like the other client keys. Layouts saved
before this change keep working: without their own entry, Shift + F8 (or
`orig_key` in `user.cfg`) applies.

## Gamepad (XInput)

| Input | Function |
|---|---|
| Left stick | Aiming |
| Right stick | Rotation and sight |
| LT / RT (analog) | Brake / Throttle |
| LB / RB | Lock-on / Main weapon |
| LB + RB | Magazine |
| A / B | Chaff / Sub-weapon |
| X / Y | Sub-weapon select / Main weapon select |
| L3 / R3 | Fire extinguisher / Sight button |
| D-pad up / down | Tuner up / down |
| D-pad left / right | Sub-monitor mode |
| START | START |
| BACK + A / X | Ignition / Cockpit hatch |
| BACK + B | Override |
| BACK + LB / RB | Gear down / up |
| BACK + D-pad | Four toggle switches (filter, oxygen, fuel, buffer) |
| BACK + R3 | Toggle switch: VT location |
| BACK + L3 | Night vision |
| BACK + START | Washer |
| BACK + LT | Slide-step |

Warning: BACK + Y (keyboard: Shift + Delete) is Eject. Only use it when
the game tells you to; otherwise your save is lost.

In Line of Contact, Eject works the same way (Shift + Delete, gamepad
BACK + Y, the EJECT button of the original controller, or your own key via
**Assign …**). It acts at once, without a confirmation: you leave your
VT, your team loses one VT from its VT count, and you choose a new
sortie on the map. In online play it is sent like any other input.

## Original controller (USB)

You need the original Steel Battalion controller and an
Xbox-male → USB-A-female adapter. No cable cutting: the adapter is enough.
The controller is not a HID device, so Windows needs a one-time driver
binding. Admin rights are needed once for this step; afterwards
everything runs as a normal user.

1. Plug the controller in via the adapter.
2. Get Zadig from its official site and start it.
3. In Zadig: **Options → List All Devices**, then select the device
   `0A7B:D000`.
4. Install the **WinUSB** driver for this device.

Warning: only ever select the device `0A7B:D000`. Check the ID twice
before installing; binding WinUSB to the wrong device breaks that
device's driver until you reinstall it. This binding only affects your
PC; the controller still works on the original Xbox.

5. In the launcher, on the **Controls** page, turn on **Original
   controller (USB)**.
6. Open **Test & calibrate …** and check the **Live** page: every button
   and axis must react.

Keyboard and mouse stay active in parallel. **Turn off mouse aiming
while the controller is used** disables mouse aiming only.

## Calibration wizard (Test & calibrate …)

- **Live**: live view of all buttons, axes, tuner and gear.
- **Guided test**: press everything once in order; the result can be
  exported for support.
- **Calibrate**: measures rest position, ends and deadzone of the
  spring-loaded axes and pedals, then saves the values. Gear and tuner
  are never calibrated; only their notches are checked.
- **Lamps**: lamp test (all 37 lamps in order).
- **Gamepad**: XInput status and stick/trigger deadzones.

## Export and import your controls

On the **Controls** page, the **Control layout** card has **Export …** and
**Import …**. Use them to back up your controls, take them to another PC
or share them.

- **Export …** saves one text file, `<layout>.sbcontrols`. It contains the
  selected layout (keys, mouse, gamepad, joystick and original-controller
  buttons) and the control settings: mouse mode and sensitivity, stick
  aiming, aim assist, automatic gears, original controller on/off,
  invert, dead zones and calibration. No paths, account data or other
  settings (graphics, audio, network) are included.
- **Import …** checks the file first. A broken or unknown file is refused
  with a short message, and nothing changes. Otherwise the launcher asks
  whether to import it as a new layout (`user_<name>`; if the name exists,
  `-2`, `-3` … is added). Existing layouts are never overwritten. The new
  layout is selected; click **Save** (or **Launch**) to keep the selection
  and the page settings. Dead zones and calibration are saved right away.
- Joysticks or the original controller that are not connected stay in
  the layout and work as soon as they are plugged in; the question lists
  them by name. Joystick calibration belongs to its device model and is
  never moved to another device.

## Starting the mech

Wait briefly between steps.

1. **Cockpit hatch**: close it (H, or the hatch switch on the
   controller).
2. **Ignition**: turn it on (I, or the ignition switch). The OS boots;
   wait until the **Primary Systems Status Check** screen stands with
   its five rows.
3. **Toggle switches**: only on that screen, flip all five (1 2 3 4 5,
   or the five switches: filter, oxygen, fuel, buffer, VT location).
   Each accepted row animates to OK. Pressing 1–5 earlier does not count;
   just press them again on this screen.
4. **START** (ENTER, or the START button) once **every bar is above the
   line**. Too early stalls the start; wait until the bars rise again
   and press START again.

After START the mech is online and weapons and driving work (see the
tables above).
