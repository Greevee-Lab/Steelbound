#pragma once
/* sb_fpsrate.h - W223 FPS-RATES: waehlbare Bildraten (Client-ESC + Launcher).
 *
 * interp_fps in der cfg: 0 = 30 Original (keine Zwischenbilder), n > 0 =
 * Zwischenbilder mit n Bildern/s, SB_FPS_AUTO (-1) = Wiederholrate des
 * Monitors (Client loest beim Laden auf, danach live im Fenster/randlos,
 * exklusiv folgt fs_interp_match bzw. auto dem Modus).
 * Der Echtzeit-Takt (render_interp.c) ist zeitbasiert (1/interp_fps, QPC,
 * t aus der Zustandszeit), daher gehen auch Nicht-Vielfache von 30.
 * Alte cfg-Werte 0/60/120 bleiben gueltig. ASCII only. */
#include <windows.h>

#define SB_FPS_AUTO (-1)
#define SB_FPS_AUTO_MIN 60
#define SB_FPS_AUTO_MAX 240
#define SB_FPS_NCHOICE 10

/* Reihenfolge der Auswahl (Launcher-Liste, ESC links/rechts). */
static const int kSbFpsChoice[SB_FPS_NCHOICE] = { SB_FPS_AUTO, 0, 60, 75, 90, 100, 120, 144, 165, 240 };

/* Index in kSbFpsChoice, -1 = kein Listenwert. */
static int sb_fps_choice_idx(int v)
{
    int i;
    for (i = 0; i < SB_FPS_NCHOICE; i++) {
        if (kSbFpsChoice[i] == v) { return i; }
    }
    return -1;
}

/* Monitor-Hz -> Bildrate fuer auto: 59/74/.../239 (gerundete NTSC-Raten,
 * EnumDisplaySettings schneidet ab) auf die gaengige Rate, 0/1 (Treiber-
 * Standard, unbekannt) -> 60, sonst auf [60, 240] begrenzt. */
static int sb_fps_auto_from_hz(int hz)
{
    static const int common[] = { 60, 75, 90, 100, 120, 144, 165, 240 };
    int i;
    if (hz <= 1) { return SB_FPS_AUTO_MIN; }
    for (i = 0; i < (int)(sizeof common / sizeof common[0]); i++) {
        if (hz + 1 == common[i]) { return common[i]; }
    }
    if (hz < SB_FPS_AUTO_MIN) { return SB_FPS_AUTO_MIN; }
    if (hz > SB_FPS_AUTO_MAX) { return SB_FPS_AUTO_MAX; }
    return hz;
}

/* Aktuelle Wiederholrate eines Monitors (NULL = Hauptmonitor), 0 = unbekannt. */
static int sb_fps_monitor_hz(HMONITOR hm)
{
    DEVMODEW dm;
    MONITORINFOEXW mi;
    const wchar_t *dev = NULL;
    if (hm) {
        ZeroMemory(&mi, sizeof mi);
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(hm, (MONITORINFO *)&mi)) { dev = mi.szDevice; }
    }
    ZeroMemory(&dm, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm)) { return 0; }
    return dm.dmDisplayFrequency > 1 ? (int)dm.dmDisplayFrequency : 0;
}
