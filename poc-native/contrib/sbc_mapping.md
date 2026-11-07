# SBC-Steuerungs-Spec für den Harness-`XInputGetState`-Hook

Stand: 29.10.2026 (Zulieferung Ansatz-1-Agent, verifiziert gegen Cxbx-Reloaded-Source
+ eigenes lauffähiges Tastaturprofil im Emulator).
Ziel: Direkt als `XInputGetState`-Hook + CFG-Default im Harness übernehmbar.

## 1. Geräte-Fakten

- SBC = Standard-XInput-Gerätetyp (`XINPUT_DEVTYPE_STEELBATTALION`), kein Spezialtreiber.
- USB: VID `0x0A7B`, PID `0xD000`, USB 1.1.
- 56 Controls (Indizes 0–55, Namen aus Cxbx `button_sbc_names`).

## 2. Toggle-Semantik (wichtig!)

Folgende Controls sind **Toggle-Schalter**, kein Halten: Zustand wechselt bei
abgeschlossener Drücken→Loslassen-Transaktion (Flanke bei Release):

- Index 4 `CockpitHatch`, 5 `Ignition`
- Index 34–38 (`ToggleFilterControl`, `ToggleOxygenSupply`, `ToggleFuelFlowRate`,
  `ToggleBufferMaterial`, `ToggleVTLocation`)
- Index 52–53 (`TunerDial Up/Down`), 54–55 (`GearLever Up/Down`)

Alle anderen Buttons: Zustand = gedrückt (1) / losgelassen (0).

Referenzimplementierung: `tools/cxbx-src/src/common/input/InputManager.cpp`,
`UpdateInputSBC` (benutzt `last_in_state`-Bits pro Toggle).

## 3. Analog-Kodierung

- **Aiming X (39/40), Aiming Y (41/42):** uint8, Mitte = `0x7F`.
  X: Plus→`0xFF`, Minus→`0x00`. Y: Plus→`0x00`, Minus→`0xFF` (Y-Polarität im
  Spiel verifizieren, ggf. invertieren!).
- **RotationLever (43 Le→A / 44 Ri):** int8, links negativ, rechts positiv.
- **SightChange X (45/46):** int8, links positiv, rechts negativ
  (d. h. Index 45 = LINKS, 46 = RECHTS — trotz „X+“-Name!).
- **SightChange Y (47 Up / 48 Down):** int8, hoch negativ, runter positiv.
- **Pedale (49 links / 50 mitte / 51 rechts):** uint8, 0–255 (Taste = 0/255).

## 4. Empfohlenes Default-Mapping (Tastatur+Maus, erprobt im Emulator)

Maus: Linksklick=MainWeapon, Rechtsklick=SubWeapon (Fire), Mittelklick=LockOn,
Mausposition absolut = Aiming X/Y.

| Control | Taste | Control | Taste |
|---|---|---|---|
| RightJoyMainWeapon | Linksklick | LeverLeft/Right | A / D |
| RightJoyFire | Rechtsklick | LeftPedal/Middle/Right | S / SPACE / W |
| RightJoyLockOn | Mittelklick | GearLever Up/Down | E / Q |
| Ignition / Start | I / ENTER | TunerDial Up/Down | R / F |
| CockpitHatch | H | SightChange | Pfeiltasten |
| Eject | RÜCKTASTE | LeftJoySightChange | TAB |
| Filter/Oxygen/Fuel/Buffer/VT | 3/4/5/6/7 | Washing/Exting./Chaff | C / X / LSTRG |
| Monitore Open/Zoom/Mode/Sub | M/N/B/V | MainMon Zoom +/- | BILD-HOCH/-RUNTER |
| FSS/Manip./Linienfarbe | F4/F5/F6 | TankDetach/Override/Night | F7/F8/F9 |
| Function F1–F3 | F1/F2/F3 | Weapon Main/Sub/Magazin | 1/2/G |
| Comm1–5 | NUMPAD 1–5 | | |

Hinweis DE-Layout: Keine Y/Z-Belegung (positionsabhängig), Rest liegt identisch.

## 5. Vorgeschlagenes CFG-Format (für `sb_native.cfg`)

```ini
[input]
device = SBC
; Format: <ControlName> = <HostInput>
RightJoyMainWeapon = MouseLeft
Aiming_X+ = MousePosX+
LeverLeft = A
Ignition = I
GearLever_Up = E
...
```

Host-Namen frei wählbar; Maus unterstützt idealerweise Tasten (`MouseLeft/…`),
absolute Position (`MousePosX±/Y±`, normiert auf Fenster) und relativ
(`MouseDX±/…`) — absolute Position als Default für Aiming.

## 6. Validierungs-Reihenfolge im Harness

1. `XGetDevices(SBC)` → Maske melden; `XInputOpen` → Handle (vgl. PoC-Client-Log).
2. Titelbildschirm: MainWeapon-Button muss Titel überspringen.
3. Startsequenz: Toggles (Ignition, 3–7) müssen Lampen schalten.
4. Fahren: Gas/Bremse/Gänge/Pedale analog prüfen.
5. Zielen: Aiming-Polarität (Y!) und Sight-Richtungen gegen Emulator-Referenz
   vergleichen (Ansatz-1-Setup steht dafür bereit).

## 7. Quellen (alle lokal)

- `tools/cxbx-src/src/common/input/layout_xbox_device.h` (Control-Namen)
- `tools/cxbx-src/src/common/input/InputManager.cpp` → `UpdateInputSBC`
- `tools/cxbx-src/src/common/input/DInputKeyboardMouse.*` (Host-Namen)
- `DOKU_TASTATUR.md` (User-Doku der Belegung)
- `tools/cxbx_profile.txt` (Maschinen-Profil)
