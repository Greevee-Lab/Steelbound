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

/* ------------------------------------------------------------------ */
/* W309 FPS-MONITOR: nur Bildraten anbieten, die zum Monitor passen     */
/* (gleiche Regel fuer Launcher-Liste und ESC links/rechts).            */
/* - Immer: Monitor (auto) und Original (30/25).                        */
/* - Feste Werte nur bis zur Monitorrate (59/143/... = 60/144/...).     */
/* - Werte, die die Monitorrate nicht glatt teilen (Rate / Wert keine   */
/*   ganze Zahl), bleiben mit Kennzeichen SB_FPS_F_JUDDER: ohne VRR     */
/*   (G-Sync/FreeSync) stehen Bilder ungleich lange. Nicht ausgeblendet,*/
/*   weil Performance (60) waehlbar bleiben muss und VRR-Monitore sie   */
/*   gleichmaessig zeigen; VRR ist nicht verlaesslich erkennbar.        */
/* - Ein Wert aus der cfg, der nicht mehr passt (z. B. 240 auf 144 Hz), */
/*   bleibt als eigener Eintrag mit SB_FPS_F_ABOVE (nie still aendern).  */
/* - Monitorrate unbekannt (0): volle Liste ohne Kennzeichen wie bisher. */
/* ------------------------------------------------------------------ */
#define SB_FPS_F_JUDDER 1 /* teilt die Monitorrate nicht glatt (ohne VRR Ruckeln) */
#define SB_FPS_F_ABOVE 2  /* ueber der Monitorrate, nur als alter/gewaehlter Wert */
#define SB_FPS_LIST_MAX SB_FPS_NCHOICE

typedef struct {
    int v;     /* Wert wie interp_fps (SB_FPS_AUTO, 0, n) */
    int flags; /* SB_FPS_F_* */
} SbFpsItem;

/* Abgeschnittene Monitorrate (EnumDisplaySettings, 59 statt 59,94) auf die
 * Nennrate: 59/74/89/99/119/143/164/239 -> +1, sonst unveraendert. */
static int sb_fps_nominal_hz(int hz)
{
    static const int common[] = { 60, 75, 90, 100, 120, 144, 165, 240 };
    int i;
    if (hz <= 1) { return 0; }
    for (i = 0; i < (int)(sizeof common / sizeof common[0]); i++) {
        if (hz + 1 == common[i]) { return common[i]; }
    }
    return hz;
}

/* Kennzeichen eines festen Werts v (> 0) fuer Monitorrate hz (roh). */
static int sb_fps_flags(int hz, int v)
{
    int hn = sb_fps_nominal_hz(hz), k;
    if (v <= 0 || hn <= 0) { return 0; }
    if (v > hn) { return SB_FPS_F_ABOVE; }
    k = (hn + v / 2) / v; /* naechstes ganzes Vielfaches; Toleranz +-0,5 Hz = exakt bei ganzen Hz */
    return (k >= 1 && k * v == hn) ? 0 : SB_FPS_F_JUDDER;
}

/* Angebotene Liste fuer Monitorrate hz (roh, 0 = unbekannt). keep = Wert,
 * der auf jeden Fall drin bleibt (aktuelle cfg-Wahl; SB_FPS_AUTO/0 sind
 * ohnehin drin, Nicht-Listenwerte werden ignoriert). Rueckgabe: Anzahl. */
static int sb_fps_list(int hz, int keep, SbFpsItem *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < SB_FPS_NCHOICE && n < cap; i++) {
        int v = kSbFpsChoice[i], f = sb_fps_flags(hz, v);
        if ((f & SB_FPS_F_ABOVE) && v != keep) { continue; }
        out[n].v = v;
        out[n].flags = f;
        n++;
    }
    return n;
}

/* Position von v in der Liste, -1 = fehlt. */
static int sb_fps_list_idx(const SbFpsItem *l, int n, int v)
{
    int i;
    for (i = 0; i < n; i++) {
        if (l[i].v == v) { return i; }
    }
    return -1;
}

/* Hoechste Rate des Monitors bei der aktuellen Aufloesung (0 = unbekannt),
 * z. B. 144-Hz-Monitor, der in Windows auf 60 steht. */
static int sb_fps_monitor_max_hz(HMONITOR hm)
{
    DEVMODEW cur, dm;
    MONITORINFOEXW mi;
    const wchar_t *dev = NULL;
    DWORD m;
    int best = 0;
    if (hm) {
        ZeroMemory(&mi, sizeof mi);
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(hm, (MONITORINFO *)&mi)) { dev = mi.szDevice; }
    }
    ZeroMemory(&cur, sizeof cur);
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &cur)) { return 0; }
    for (m = 0; m < 4096; m++) {
        ZeroMemory(&dm, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsW(dev, m, &dm)) { break; }
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight &&
            dm.dmDisplayFrequency > 1 && (int)dm.dmDisplayFrequency > best) {
            best = (int)dm.dmDisplayFrequency;
        }
    }
    if (best == 0 && cur.dmDisplayFrequency > 1) { best = (int)cur.dmDisplayFrequency; }
    return best;
}
