#pragma once
/* launcher_res.h - Launcher-Dateien neben der Exe (W82 LAUNCHER-UI2, W535 AVVT).
 * Hintergrundbild: bis W534 RCDATA-Ressource IDR_BG, seit W535 eine Datei neben sb_launcher.exe
 * (Quelle res/bg_nutzer.png bzw. res/convert_bg.py; Build: CMake-POST_BUILD, Release: make_alpha.py).
 * Fehlt sie, zeichnet der Launcher den einfarbigen Verlauf. */
#define LAUNCHER_BG_REL L"res\\launcher_bg.png"
#define LAUNCHER_BG_MAX (16u * 1024u * 1024u) /* Obergrenze beim Einlesen */
