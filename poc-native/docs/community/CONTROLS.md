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
| Aim | Mouse (or arrows; Shift + arrows = sight only) |
| Free / capture mouse | F10 |
| Settings overlay (game keeps running) | ESC |
| Performance overlay | F2 (Shift + F2 moves it) |
| Window / fullscreen | Alt + Enter |
| VR: recenter view | F8 |

Mouse aiming is **Absolute** by default (the crosshair follows the
mouse); **Sensitivity** (1–16) is on the **Controls** page next to the
mode. EJECT has no key in the Standard layout. Own layouts can be edited
with **Assign …** on the **Controls** page.

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

Warning: BACK + Y is Eject. Only use it when the game tells you to;
otherwise your save is lost.

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
