/* launcher_gui_loc.c - Line of Contact: Spielauswahl (W276), Werte je Spiel (W402), Online-Karte steelnet (W334), Seite Netzwerk (W392), Online-/Versionssperre (W400/W421).
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
/* W276 LOCTAB: Spielauswahl Steel Battalion | Line of Contact          */
/* Umschalter in der Titelleiste (gemerkt in user.cfg, launcher_games). */
/* Gemeinsam: Anzeige, Grafik, Steuerung, Ton (dieselben Elemente und   */
/* dieselben user.cfg-Werte). Nur Steel Battalion: Spielen-Inhalt,      */
/* Packs, Erweitert (Spielstaende, Datenort, Diagnose). Nur LoC: eigene */
/* Spielen-Seite mit Abbild, Datenordner <Daten>\loc, Online-Platzhalter*/
/* und gesperrtem Start ("Coming soon"), bis der Client LoC startet.    */
/* W293: LoC-Spielen wie SB (Bereitschaft + Band unten), Abbild/Ordner/  */
/* Online auf LoC > Erweitert (dort SB-Teile aus, gemeinsame Teile an).  */
/* ------------------------------------------------------------------ */
int gametab_id(int id) /* W398: Wortmarken-Tabs im Kopf */
{
    return id == ID_GAME_SB || id == ID_GAME_LOC;
}
#define ID_LOC_FIRST ID_LOC_START
#define ID_LOC_LAST ID_LOC_SBLOOK /* W459: 2779 (ID_LOC_GO0 .. ID_LOC_SMOOTH lueckenlos, dann SBLOOK) */
/* W392 LAUNCHER-NETTAB: Seite "Netzwerk" im LoC-Bereich (Navigationsplatz von Packs, den es bei LoC nicht gibt).
 * Dort liegen Rollback, eigene Bewegung sofort, Gegner glaetten (ID_LOC_SMOOTH, frueher unter dem Start) und
 * niedrigere Eingabeverzoegerung sowie "Standard wiederherstellen". */
#define ID_LNET_FIRST ID_LNET_RB
#define ID_LNET_LAST ID_LNET_MTEAM /* W454; W473: Reiter + Chat im Spiel */
/* W293 LOC-LAYOUT: Spielen = Kopf, Start, Bereitschaft (wie SB), Hinweisband
 * unten; Disc, Spielstaende & Daten und Online liegen links auf Erweitert
 * (rechts die gemeinsamen Teile Feinabstimmung + Launcher). */
#define MP_MATCH_Y CY0 /* W397: Karte Match (links oben) */
#define MP_MATCH_H LOC_ONL_H /* W473: 184 (Zeile Verbindung), gleich hoch wie die Konto-Karte */
#define MP_LBL_W 100                      /* W473: Beschriftung links (Verbindung, Lobby, Raumcode / Raum, Passwort) */
#define MP_ROW(i) (MP_MATCH_Y + 44 + (i) * 34) /* W473: Zeilen der Karte Match */
#define MP_INFO_Y (MP_MATCH_Y + 146)
#define MP_INFO_H 34
/* W326 LOC-WIDE: Karte "Bildformat" rechts unter Feinabstimmung/Launcher
 * (dort, wo im SB-Bereich die Diagnose steht), unten buendig mit Online. */
/* W334 LOC-SEC: Karte "Online (steelnet)" auf LoC-Spielen rechts neben dem
 * Start (frueher "Gemeinsam mit Steel Battalion"; der Hinweis steht jetzt
 * dezent im Kopf von "Startbereit"), Vertrauenshinweis knapp darunter. */
lgame_state_t g_loc;        /* LoC-Einstellungen (user.cfg launcher_*) */
lgame_img_t g_loc_res;      /* letzte Abbild-Pruefung */
char g_loc_res_path[512];   /* ... gilt nur fuer diesen Pfad */
int g_loc_checked;
char g_locfix_path[512];    /* Shots/Selbsttest: Pruef-Fixture fuer genau einen Pfad */
lgame_img_t g_locfix;
char g_loc_last_cmd[1600];  /* Selbsttest: Startweg (Trockenlauf) */
char g_loc_last_cfg[1200];  /* W402: Einstellungsdatei des letzten LoC-Starts (SB_USER_CFG) */

void loctab_wide_sync(void); /* W326 */
void onl_load(void);         /* W334 */
int acc_visible(int id, int page); /* W328 */
int acc_view_active(void);        /* W328 */
void acc_layout(void);            /* W399 */
wchar_t *acc_launch_env(char *set, size_t cap); /* W328 */
/* W336 LOC-TESTPARTNER: Knoepfe im LoC-Tab (nur interne Ausgabe; IDs auch in der Community-Ausgabe
 * bekannt, damit der Selbsttest dort "kein Knopf" pruefen kann). Code: launcher_loctp_gui.inc. */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
int loctp_visible_id(int id, int page);
int loctp_command(int id, int code);
#endif

int loctab_may_write(void)
{
    return g_o.shot == 0 || g_ucfg_test[0]; /* Shots speichern nie (Selbsttest: Temp-Datei) */
}

static int g_pg_migrated; /* W402: Ergebnis der Uebernahme beim Start (1 = gerade uebernommen) */
int g_loc169_migrated; /* W414: Ergebnis der einmaligen 16:9-/Fade-out-Umstellung (1 = gerade umgestellt) */
int g_locgfx_migrated; /* W465: Ergebnis der einmaligen Umstellung auf Enhanced (1 = gerade umgestellt) */

void loctab_load(void)
{
    char up[1200], lp[1200];
    lgame_state_t lt;
    user_cfg_path(up, sizeof up);
    game_cfg_path(LGAME_LOC, lp, sizeof lp);
    if (loctab_may_write()) { /* W402: einmalige Uebernahme der gemeinsamen Werte (Shots schreiben nie) */
        g_pg_migrated = lgame_pergame_migrate(up, lp);
        /* W414: danach (die LoC-Datei kann eben entstanden sein) einmal 16:9 + Fade out, sofort gespeichert */
        g_loc169_migrated = lgame_loc169_migrate(lp);
        /* W465 LOC-ENHANCED: einmal Optik -> Enhanced, loc_sblook=0, Marke gfx_loc_rev (sofort gespeichert) */
        g_locgfx_migrated = lgame_loc_gfx_migrate(lp);
    }
    lgame_cfg_load(up, &g_loc);
    if (lgame_cfg_load(lp, &lt) == 0) { /* W402: Bildformat steht in user_loc.cfg (vor der Uebernahme: user.cfg) */
        g_loc.loc_widescreen = lt.loc_widescreen;
        g_loc.loc_sblook = lt.loc_sblook; /* W459 */
    } else {
        g_loc.loc_sblook = 0;
    }
    g_game = g_loc.game;
    g_loc_checked = 0;
    g_loc_res_path[0] = '\0';
    loctab_wide_sync(); /* W326 (vor build_controls: nur Wert pruefen) */
    g_loc.loc_sblook = g_loc.loc_sblook ? 1 : 0; /* W459 */
    onl_load();        /* W334: Schalter "Online-Spiel" */
}

static int loctab_save(void) /* 0 = gespeichert, 1 = nicht geschrieben (Shot), -1 = Fehler */
{
    char up[1200];
    if (!loctab_may_write()) {
        return 1;
    }
    user_cfg_path(up, sizeof up);
    g_loc.game = g_game;
    /* W402: Spielwahl/LoC-Abbild in user.cfg, Bildformat nur in user_loc.cfg */
    if (lgame_cfg_save_ex(up, &g_loc, LGAME_K_LAUNCHER, LGAME_K_WIDE | LGAME_K_SBLOOK) != 0) {
        set_status_k(LT("gui.loc_save_err"), 2);
        return -1;
    }
    game_cfg_path(LGAME_LOC, up, sizeof up);
    if (u8_GetFileAttributes(up) == INVALID_FILE_ATTRIBUTES) {
        /* W414: fehlt + 16:9 = Standard, nichts anlegen; jede andere Wahl legt die Datei mit Markierung und
         * den Rand-Standardwerten (Fade out 8/100) an - sonst stellte die Einmal-Umstellung sie spaeter um */
        if ((g_loc.loc_widescreen != LGAME_WIDE_169 || g_loc.loc_sblook) &&
            lgame_loc169_mark(up, g_loc.loc_widescreen, 1) != 0) {
            set_status_k(LT("gui.loc_save_err"), 2);
            return -1;
        }
        if (g_loc.loc_sblook && lgame_cfg_save_ex(up, &g_loc, LGAME_K_SBLOOK, LGAME_K_LAUNCHER) != 0) {
            set_status_k(LT("gui.loc_save_err"), 2);
            return -1;
        }
        return 0;
    }
    if (lgame_cfg_save_ex(up, &g_loc, LGAME_K_WIDE | LGAME_K_SBLOOK, LGAME_K_LAUNCHER) != 0 ||
        lgame_loc169_mark(up, g_loc.loc_widescreen, 0) != 0) { /* W414: Markierung, falls noch nicht da */
        set_status_k(LT("gui.loc_save_err"), 2);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* W402 LAUNCHER-PERGAME: eigene Werte je Spiel. g_s gehoert immer dem  */
/* aktiven Spiel; das andere ruht in g_pg_s (inkl. ungespeicherter      */
/* Aenderungen) samt Abgleich-Basis. Global bleiben nur die SB-Pfade    */
/* (Spieldaten) und die Sprache (Zahnrad).                              */
/* ------------------------------------------------------------------ */
static launcher_settings_t g_pg_s[LGAME_N], g_pg_base[LGAME_N];
int g_pg_ok[LGAME_N], g_pg_base_ok[LGAME_N];

void pergame_mark(void)
{
    char up[1200];
    if (!loctab_may_write()) {
        return;
    }
    user_cfg_path(up, sizeof up);
    if (!lgame_pergame_marked(up)) {
        (void)lgame_cfg_save_ex(up, &g_loc, LGAME_K_PERGAME, 0);
    }
}

static void pergame_globals(launcher_settings_t *dst, const launcher_settings_t *src)
{
    strcpy_s(dst->xbe_path, sizeof dst->xbe_path, src->xbe_path);
    strcpy_s(dst->media_root, sizeof dst->media_root, src->media_root);
    strcpy_s(dst->hdd_root, sizeof dst->hdd_root, src->hdd_root);
    strcpy_s(dst->game_image, sizeof dst->game_image, src->game_image);
    strcpy_s(dst->lang, sizeof dst->lang, src->lang);
}

/* Werte eines Spiels frisch laden: Standard -> sb_native.cfg -> Datei des Spiels (wie app_load_effective, das
 * Standard-Profil gilt nur fuer Steel Battalion). */
void pergame_load(int game, launcher_settings_t *s)
{
    char p[1200];
    launcher_settings_t d;
    if (game != LGAME_LOC) {
        app_load_effective(g_client_dir, s, NULL, 0, NULL, NULL);
        return;
    }
    lcfg_defaults(s);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/sb_native.cfg", g_client_dir);
    lcfg_load_file(p, s);
    lcfg_defaults(&d); /* W87/W170: Spieler-Standard statt Testwerte aus sb_native.cfg */
    s->mouse_mode = d.mouse_mode;
    s->rt_scale = d.rt_scale;
    s->rt_scale_dyn = d.rt_scale_dyn;
    s->sb_allied_squads = d.sb_allied_squads; /* W424: Testwert aus sb_native.cfg nicht uebernehmen */
    s->gfx_preset_rev = 0;
    game_cfg_path(LGAME_LOC, p, sizeof p);
    s->game_tick = 30; /* nach dem Laden erneut: PAL gibt es in LoC nicht */
    if (lcfg_load_file(p, s) != 0) { /* noch keine LoC-Datei: Fenster wie beim Erststart pruefen */
        RECT wa;
        int ww = 0, wh = 0, dpi = g_dpi >= 96 ? g_dpi : 96;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            ww = (wa.right - wa.left) * 96 / dpi;
            wh = (wa.bottom - wa.top) * 96 / dpi;
        }
        (void)app_firstrun_defaults(s, 0, ww, wh);
        /* W465 LOC-ENHANCED: neue LoC-Einstellungen - Grafik-Preset Enhanced (alle Preset-Werte aus der Tabelle) */
        lcfg_gfx_apply(s, LGP_ENHANCED);
        /* W414: neue LoC-Einstellungen - Breitbild-Raender "Fade out" 8 %/100 % (Bildformat 16:9 = g_loc) */
        s->edge_mode = LGAME_LOC169_EDGE_MODE;
        s->edge_fade_width = LGAME_LOC169_EDGE_W;
        s->edge_fade_strength = LGAME_LOC169_EDGE_S;
    }
    s->game_tick = 30; /* W402: PAL (Spieltakt 25) gibt es in LoC nicht, auch nicht aus der Uebernahme */
    lcfg_gfx_migrate_fps(s);
}

/* Beim Start (gui_fill nach loctab_load): ist LoC gewaehlt, gilt dessen Datei. */
void pergame_init(void)
{
    if (g_game != LGAME_LOC) {
        return;
    }
    g_pg_s[LGAME_SB] = g_s;
    g_pg_ok[LGAME_SB] = 1;
    g_pg_base_ok[LGAME_SB] = 0;
    pergame_load(LGAME_LOC, &g_s);
    pergame_globals(&g_s, &g_pg_s[LGAME_SB]);
}

/* Umschalten (g_game ist schon "to"): Oberflaechenwerte des alten Spiels ruhen lassen, die des neuen zeigen. */
static void pergame_switch(int from, int to)
{
    launcher_settings_t cur = g_s;
    int now = g_game;
    g_game = from; /* Oberflaeche gehoert noch dem alten Spiel (z. B. PAL nur in SB) */
    ui_to_settings(&cur);
    g_game = now;
    g_pg_s[from] = cur;
    g_pg_ok[from] = 1;
    g_pg_base[from] = g_disk_base;
    g_pg_base_ok[from] = g_disk_base_ok;
    if (g_pg_ok[to]) {
        g_s = g_pg_s[to];
        pergame_globals(&g_s, &cur);
        g_disk_base = g_pg_base[to];
        g_disk_base_ok = g_pg_base_ok[to];
        if (g_disk_base_ok) {
            (void)disk_merge(&g_s); /* waehrenddessen im Spiel (ESC-Menue) gespeicherte Werte */
        }
    } else {
        pergame_load(to, &g_s);
        pergame_globals(&g_s, &cur);
        g_disk_base_ok = 0;
    }
    if (!g_disk_base_ok) {
        disk_base_sync(&g_s);
    }
    ui_from_settings(&g_s);
    update_aspect_info();
    gfx_sync_ui();
}

/* ------------------------------------------------------------------ */
/* W334 LOC-SEC-CLIENT: Online-Spiel ueber steelnet (Protokoll v2,      */
/* SECURITY.md §3.5 Raumcode, §6.6 Punkt 6, §9, §13 F2). Karte "Online  */
/* (steelnet)" auf LoC-Spielen: Schalter (user.cfg launcher_loc_online, */
/* Standard aus), Raumcode (leer = eigene Lobby mit Raumcode). Start    */
/* mit loc_transport=udp;loc_lobby=<Host>:<Port>;loc_lobby_code=<Code>. */
/* Waehrend/nach dem Lauf liest die Karte loc_net_status.txt (sec=,     */
/* lobby_code=). Dienst nicht eingerichtet (beide SPKI in               */
/* steelnet_pins.h leer) = Schalter grau, kein Online-Start.            */
/* ------------------------------------------------------------------ */
#define ONL_KEY "launcher_loc_online"
#define ONL_PUB_KEY "launcher_loc_public" /* W373 */
int g_loc_online;             /* 1 = Online-Spiel an (gemerkt) */
int g_loc_public = 1;         /* W373: 1 = oeffentliche Lobby (Standard), 0 = privat mit Raumcode (gemerkt) */
/* W387 LOC-SMOOTH-OPP: Client-Schluessel loc_smooth_opp (nur Anzeige fremder Objekte etwas spaeter, damit
 * Netzkorrekturen/Rollback unsichtbar bleiben): 0 = aus, 1 = auto (Standard), 2 = fest 1 Tick. Gemerkt in
 * user.cfg launcher_loc_smooth_opp (fehlt/ungueltig = 1), an jeden LoC-Start (lokal, Testpartner, online) im
 * --set angehaengt, nie an Steel Battalion. */
#define ONL_SMOOTH_KEY "launcher_loc_smooth_opp"
int g_loc_smooth_opp = 1;     /* W387 */
/* W392 LAUNCHER-NETTAB: Netzwerk-Schalter fuer jeden LoC-Start (nie Steel Battalion), gemerkt in user.cfg
 * (fehlt/ungueltig = 1 = an). Rollback aus -> loc_rb_input=0 (je Mission ausgehandelt: eine Seite aus = alle
 * Lockstep); eigene Bewegung -> loc_own_predict; niedrigere Verzoegerung aus -> lowd=0 in loc_rb_opts. */
#define LNET_RB_KEY "launcher_loc_rollback"
#define LNET_OWN_KEY "launcher_loc_own_predict"
#define LNET_LOWD_KEY "launcher_loc_low_delay"
int g_loc_rb_on = 1;   /* W392 (0 = Netzmodus Lockstep) */
/* W447 LOC-NET-AUTO (GitHub #82, Nutzerentscheid 23.11.): Netzmodus Auto (Standard) / Rollback / Lockstep als eine
 * Auswahl (ID_LNET_RB). Lockstep = launcher_loc_rollback=0 (wie W392), Auto/Rollback = launcher_loc_net_auto (fehlt/
 * ungueltig = 1). Nur "Rollback (manuell)" haengt loc_net_auto=0 an (Standard des Clients ist Auto). */
#define LNET_AUTO_KEY "launcher_loc_net_auto"
int g_loc_net_auto = 1; /* W447 */
int g_loc_own_pred = 1; /* W392 */
int g_loc_lowd = 1;    /* W392 */
/* W420 LOC-CLIPDUMP: Match-Regel des Hosts "Clip dumping unterbinden" (Nutzerentscheid 22.11.: Standard an), gemerkt in
 * user.cfg (fehlt/ungueltig = 1), als loc_rule_clipdump_block an jeden LoC-Start (beim Testpartner an beide Seiten:
 * dort gilt die lokale Einstellung). Keine Grafik-Abweichung, sondern Spielregel (W278: kein Experimental). */
#define LNET_CLIP_KEY "launcher_loc_clipdump_block"
int g_loc_clip = 1;
/* W437 LOC-CHAT: Textchat in LoC online (Nutzerwunsch 22.11.: Standard an), gemerkt in user.cfg (fehlt/ungueltig = 1),
 * als loc_chat an jeden LoC-Start (beim Testpartner an beide Seiten). Aus = weder Anzeige noch Senden. */
#define LNET_CHAT_KEY "launcher_loc_chat"
int g_loc_chat = 1;
/* W446 STEELNET-CHAT (GitHub #81, Nutzerwunsch 23.11.): steelnet-weiter Chat, getrennt vom Lobby-Chat, Standard an,
 * gemerkt in user.cfg (fehlt/ungueltig = 1), als loc_snchat an jeden LoC-Start. Aus = kein Abonnement beim Dienst. */
#define LNET_SNCHAT_KEY "launcher_loc_snchat"
int g_loc_snchat = 1;
/* W440 LOC-LOBBY-MODES (GitHub #79, Nutzerentscheid 23.11.): Modus der Lobbys, die ich hoste - 0 = Sim (Standard:
 * keine Zielhilfe, keine Gang-Automatik fuer alle), 1 = Arcade (jeder nur seine eigenen Hilfen). Gemerkt in user.cfg
 * (fehlt/ungueltig = Sim), als loc_play_mode=sim|arcade an jeden LoC-Start (loc_rule_set_append; Testpartner: --set). */
int g_loc_arcade = 0;
/* W454 LOC-LAN-MODE (GitHub #91, Nutzerwunsch 23.11.): Multiplayer ueber steelnet (Standard) oder nur im LAN ohne
 * Internet. Gemerkt in user.cfg launcher_loc_lan (fehlt/ungueltig = 0) und launcher_loc_lan_room (nur druckbares
 * ASCII, 1..16, leer = "LAN"); das Raumpasswort bleibt nur im Speicher des Launchers und geht als Umgebung
 * SB_LAN_PASS an den Kindprozess (nie Datei/Kommandozeile/Log). LAN: kein steelnet-Kontakt beim Start (keine
 * Konto-, Versions-, Relay- oder Chat-Verbindung), Start ohne Stufe steelnet erlaubt. */
#define LNET_LAN_KEY "launcher_loc_lan"
#define LNET_LANROOM_KEY "launcher_loc_lan_room"
int g_loc_lan = 0;
char g_loc_lan_room[17];
static char g_loc_lan_pass[64];
/* W473 MPPOLISH: Chat waehrend des Spiels (Nutzer 24.11.). Schluessel des Clients (W474) stehen direkt in
 * user_loc.cfg (die Datei, die der LoC-Client per SB_USER_CFG liest); fehlt/ungueltig = Standard: steelnet 0,
 * Lobby 1, Team 1. Kein --set: so bleiben ESC-Aenderungen des Clients gueltig. Aeltere Clients ignorieren die
 * unbekannten Schluessel (hcfg.c: nur Log "unbekannter Key"). */
#define LNET_MSN_KEY "loc_chat_mission_sn"
#define LNET_MLOBBY_KEY "loc_chat_mission_lobby"
#define LNET_MTEAM_KEY "loc_chat_mission_team"
int g_loc_msn = 0;    /* W473: steelnet-Chat im Spiel (Standard aus) */
int g_loc_mlobby = 1; /* W473: Lobby-Chat im Spiel */
int g_loc_mteam = 1;  /* W473: Team-Chat im Spiel */
/* W473: Team-Reiter im Spiel (W474 Teil B) ist bei diesem Stand nicht gemergt -> Schalter grau mit Hinweis
 * "coming later" (Wert wird trotzdem gelesen/geschrieben). 1 setzen, sobald der Client den Team-Reiter hat. */
#define LNET_HAVE_TEAM_TAB 0
int g_lnet_tab; /* W473: Reiter der Netzwerk-Karte (0 = Network, 1 = Lobby & chat), nicht gespeichert */

int loc_tip_id(int id) /* W387: Tooltip am Element (tips_build, Text in xtip_text) */
{
    return id == ID_LOC_SMOOTH || id == ID_CTRL_AIMASSIST || id == ID_CTRL_AUTOGEAR; /* W440: Hinweis nur in LoC */
}
int g_onl_fix_srv = -1;       /* Shots/Selbsttest: -1 = echte Pins, sonst 0/1 */
int g_onl_fixture;            /* Shots: Status vorgegeben, nie von der Platte */
char g_onl_sec[24];           /* letzter sec= ("" = unbekannt) */
char g_onl_code[16];          /* letzter lobby_code= (normalisiert, "" = keiner) */
char g_onl_status_test[1200]; /* Selbsttest: Statusdatei statt Lauf-Ordner */
ULONGLONG g_onl_since;        /* FILETIME: aeltere Statusdateien (frueherer Lauf) gelten nicht */

/* sec= -> Meldung (ok/unbekannt: keine). */
static const char *const kOnlSec[][2] = {
    { "selftest_failed", "gui.loc_onl_sec_selftest" },
    { "pin_failed", "gui.loc_onl_sec_pin" },
    { "no_pin", "gui.loc_onl_nosrv" },
    { "upgrade_required", "gui.loc_onl_sec_upgrade" },
    { "version_required", "gui.loc_onl_sec_upgrade" }, /* W420: Text mit Version kommt aus ver=/need_ver= */
};
#define ONL_SEC_N ((int)(sizeof kOnlSec / sizeof kOnlSec[0]))
char g_onl_err[12]; /* W353: letzter err= ("" = keiner) */
static char g_onl_ver[8];       /* W420: ver= ("server"/"peer", "" = keine Versionsablehnung) */
static char g_onl_need_ver[16]; /* W420: need_ver= (nur [0-9A-Za-z.+-]) */
static char g_lan_st[40];       /* W454: "lan:<gefunden>:<verbunden>" aus der Statusdatei ("" = kein LAN-Lauf) */

int onl_srv_on(void)
{
    static const char kP1[] = SB_STEELNET_SPKI1, kP2[] = SB_STEELNET_SPKI2;
    if (g_onl_fix_srv >= 0) {
        return g_onl_fix_srv;
    }
    return kP1[0] != '\0' || kP2[0] != '\0';
}

ULONGLONG onl_now_ft(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* Raumcode normalisieren: Gross/Klein, '-' und Leerzeichen egal, I/L -> 1 und
 * O -> 0 (wie der Dienst, §3.5), sonst nur Crockford-Base32 (0-9, A-Z ohne
 * I L O U); hoechstens 9 Zeichen ohne Leerzeichen, genau 8 Codezeichen.
 * 0 = leer (eigene Lobby), 1 = gueltig (out = "XXXX-XXXX"), -1 = ungueltig. */
int onl_code_norm(const char *in, char *out, size_t cap)
{
    char c8[8];
    int n = 0, sig = 0;
    out[0] = '\0';
    if (!in) {
        return 0;
    }
    for (; *in; in++) {
        char ch = *in;
        if (ch == ' ' || ch == '\t') {
            continue;
        }
        if (++sig > 9) {
            return -1;
        }
        if (ch == '-') {
            continue;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (ch == 'I' || ch == 'L') {
            ch = '1';
        } else if (ch == 'O') {
            ch = '0';
        }
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')) || ch == 'U' || n >= 8) {
            return -1;
        }
        c8[n++] = ch;
    }
    if (sig == 0) {
        return 0;
    }
    if (n != 8) {
        return -1;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%.4s-%.4s", c8, c8 + 4);
    return 1;
}

/* --set des LoC-Starts: mit Online (an + Dienst eingerichtet) Transport,
 * Treffpunkt und Raumcode anhaengen (leerer Code = eigene Lobby anlegen).
 * W373: pub = 1 und kein Code -> loc_lobby_vis=public (oeffentliche Lobbys:
 * Search Match zeigt alle offenen Spiele); ein Code tritt immer der privaten
 * Lobby bei. 0 = ok (ggf. unveraendert), -1 = Raumcode ungueltig. */
int onl_set_append(char *set, size_t cap, int on, int srv, const char *raw, int pub)
{
    char code[16];
    size_t l = strlen(set);
    if (!on || !srv) {
        return 0;
    }
    if (onl_code_norm(raw, code, sizeof code) < 0) {
        return -1;
    }
    /* W353: hier stand loc_rb_input=1 fest. W392: loc_rb_input kommt jetzt fuer jeden LoC-Start aus dem
     * Schalter "Rollback-Netcode" (loc_net_set_append, Standard an, je Mission ausgehandelt). */
    _snprintf_s(set + l, cap - l, _TRUNCATE, "%sloc_transport=udp;loc_lobby=%s:%d;loc_lobby_code=%s%s",
                l ? ";" : "", SB_STEELNET_HOST, (int)SB_STEELNET_PORT, code,
                pub && !code[0] ? ";loc_lobby_vis=public" : "");
    return 0;
}

/* W387: Wert aus user.cfg (Text nach "=") -> 0..2, alles andere (fehlt, Buchstaben, ausserhalb) = 1 (auto). */
int loc_smooth_parse(const char *v)
{
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) {
        return 1;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        end++;
    }
    return (*end == '\0' && n >= 0 && n < LOC_SMOOTH_N) ? (int)n : 1;
}

/* W387: loc_smooth_opp=N an den --set eines LoC-Starts haengen (jeder LoC-Start, nie SB). */
void loc_smooth_set_append(char *set, size_t cap, int v)
{
    size_t l = strlen(set);
    if (v < 0 || v >= LOC_SMOOTH_N) {
        v = 1;
    }
    _snprintf_s(set + l, cap - l, _TRUNCATE, "%sloc_smooth_opp=%d", l ? ";" : "", v);
}

/* W392: Schalter 0/1 aus user.cfg (Text nach "="); fehlt/ungueltig = 1 (an). */
int lnet_bool_parse(const char *v)
{
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) {
        return 1;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
        end++;
    }
    return (*end == '\0' && (n == 0 || n == 1)) ? (int)n : 1;
}

/* W392: letzter Wert von key in einer --set-Kette ("\;" = maskiertes Semikolon wie hcfg_apply_set),
 * unmaskiert nach out. 1 = gefunden. */
int lnet_set_last(const char *set, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key), n;
    const char *p = set;
    int found = 0;
    while (p && *p) {
        const char *s = p;
        while (*p && !(*p == ';' && (p == set || p[-1] != '\\'))) {
            p++;
        }
        if ((size_t)(p - s) > kl && strncmp(s, key, kl) == 0 && s[kl] == '=') {
            const char *v = s + kl + 1;
            n = 0;
            while (v < p && n + 1 < cap) {
                if (v[0] == '\\' && v + 1 < p && v[1] == ';') {
                    v++;
                }
                out[n++] = *v++;
            }
            out[n] = '\0';
            found = 1;
        }
        if (*p) {
            p++;
        }
    }
    return found;
}

/* W392: loc_rb_opts zusammenfuehren. base = bisheriger Wert (Optionen mit ";" getrennt). Mit lowd_on = 1
 * bleibt alles unveraendert (0 = nichts anhaengen, der Client behaelt seinen Wert). Mit lowd_on = 0: alle
 * vorhandenen Optionen bleiben in ihrer Reihenfolge, nur "lowd=..." faellt weg, am Ende kommt "lowd=0"
 * (spaetere Option gewinnt im Client ohnehin). Ergebnis maskiert ("\;") fuer die --set-Kette; unmaskiert
 * hoechstens LNET_RBOPTS_MAX Zeichen (Client-Puffer), zu lange Restoptionen fallen von hinten weg, lowd=0 bleibt.
 * 1 = out anhaengen. */
int lnet_rbopts_merge(const char *base, int lowd_on, char *out, size_t cap)
{
    char tok[16][LNET_RBOPTS_MAX + 1];
    int nt = 0, i;
    size_t raw = 0;
    const char *p = base ? base : "";
    out[0] = '\0';
    if (lowd_on) {
        return 0;
    }
    while (*p && nt < 16) {
        const char *s;
        size_t n;
        while (*p == ';' || *p == ' ') {
            p++;
        }
        s = p;
        while (*p && *p != ';') {
            p++;
        }
        n = (size_t)(p - s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
            n--;
        }
        if (!n || n > LNET_RBOPTS_MAX || strncmp(s, "lowd=", 5) == 0) {
            continue;
        }
        memcpy(tok[nt], s, n);
        tok[nt][n] = '\0';
        nt++;
    }
    /* Platz: Optionen + je ";" + "lowd=0" (6 Zeichen) */
    for (i = 0; i < nt; i++) {
        if (raw + strlen(tok[i]) + 1 + 6 > LNET_RBOPTS_MAX) {
            nt = i;
            break;
        }
        raw += strlen(tok[i]) + 1;
    }
    for (i = 0; i < nt; i++) {
        strcat_s(out, cap, tok[i]);
        strcat_s(out, cap, "\\;");
    }
    strcat_s(out, cap, "lowd=0");
    return 1;
}

/* W392: bisheriger loc_rb_opts-Wert wie im Client: sb_native.cfg des Client-Ordners, dann user.cfg (gewinnt);
 * steht er schon in der Kette (z. B. Testpartner), gilt der dortige Wert. "" = keiner. */
static void lnet_base_rbopts(const char *set, char *out, size_t cap)
{
    char p[1300], u[1024];
    out[0] = '\0';
    if (set && lnet_set_last(set, "loc_rb_opts", out, cap)) {
        return;
    }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/sb_native.cfg", g_client_dir);
    cfg_read_key(p, "loc_rb_opts", out, cap);
    game_cfg_path(LGAME_LOC, p, sizeof p); /* W402: LoC-Client-Schluessel stehen in user_loc.cfg */
    if (cfg_read_key(p, "loc_rb_opts", u, sizeof u)) {
        strcpy_s(out, cap, u);
    }
}

/* W392 LAUNCHER-NETTAB: Netzwerk-Schalter an den --set eines LoC-Starts haengen (jeder LoC-Start: lokal,
 * online, Nutzer-Spiel des Testpartners; nie Steel Battalion): loc_rb_input, loc_own_predict, loc_smooth_opp
 * (W387) und - nur mit "niedrigere Verzoegerung" aus - loc_rb_opts mit lowd=0 (vorhandene Optionen bleiben). */
void loc_net_set_append(char *set, size_t cap)
{
    char base[LNET_RBOPTS_MAX * 2 + 8], m[LNET_RBOPTS_MAX * 2 + 8];
    size_t l = strlen(set);
    _snprintf_s(set + l, cap - l, _TRUNCATE, "%sloc_rb_input=%d;loc_own_predict=%d", l ? ";" : "",
                g_loc_rb_on ? 1 : 0, g_loc_own_pred ? 1 : 0);
    loc_smooth_set_append(set, cap, g_loc_smooth_opp);
    if (!g_loc_lowd) {
        lnet_base_rbopts(set, base, sizeof base);
        if (lnet_rbopts_merge(base, 0, m, sizeof m)) {
            l = strlen(set);
            _snprintf_s(set + l, cap - l, _TRUNCATE, ";loc_rb_opts=%s", m);
        }
    }
    if (g_loc_rb_on && !g_loc_net_auto) { /* W447: Rollback manuell (Auto ist Standard des Clients) */
        l = strlen(set);
        _snprintf_s(set + l, cap - l, _TRUNCATE, ";loc_net_auto=0");
    }
}

/* W420: Host-Regel an den --set eines LoC-Starts haengen (eigene Funktion: die Kette der Netzwerk-Schalter bleibt) */
void loc_rule_set_append(char *set, size_t cap)
{
    size_t l = strlen(set);
    _snprintf_s(set + l, cap - l, _TRUNCATE, "%sloc_rule_clipdump_block=%d", l ? ";" : "", g_loc_clip ? 1 : 0);
    l = strlen(set);
    _snprintf_s(set + l, cap - l, _TRUNCATE, ";loc_chat=%d", g_loc_chat ? 1 : 0); /* W437 */
    l = strlen(set);
    _snprintf_s(set + l, cap - l, _TRUNCATE, ";loc_snchat=%d", g_loc_snchat ? 1 : 0); /* W446 */
    l = strlen(set);
    _snprintf_s(set + l, cap - l, _TRUNCATE, ";loc_play_mode=%s", g_loc_arcade ? "arcade" : "sim"); /* W440 */
}

/* W392: Stimmen alle Netzwerk-Schalter mit dem Standard ueberein? (Hinweis auf LoC-Spielen) */
int lnet_is_default(void)
{
    return g_loc_rb_on && g_loc_net_auto && g_loc_own_pred && g_loc_lowd && g_loc_smooth_opp == 1 && g_loc_clip && g_loc_chat &&
           g_loc_snchat && !g_loc_arcade && /* W440: Standard Sim; W446; W447 */
           !g_loc_msn && g_loc_mlobby && g_loc_mteam; /* W473: Chat im Spiel (steelnet aus, Lobby/Team an) */
}

/* loc_net_status.txt lesen (Schluessel=Wert je Zeile, letzte Zeile gewinnt).
 * Nur bekannte sec-Werte und gueltige Raumcodes werden uebernommen.
 * 1 = gelesen, 0 = fehlt. */
int onl_status_parse(const char *path, char *sec, size_t sc, char *code, size_t cc)
{
    FILE *f = NULL;
    char line[256];
    sec[0] = code[0] = '\0';
    if (!path || u8_fopen_s(&f, path, "r") != 0 || !f) {
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) {
            *--e = '\0';
        }
        if (strncmp(line, "sec=", 4) == 0) {
            int i;
            sec[0] = '\0';
            if (strcmp(line + 4, "ok") == 0) {
                strncpy_s(sec, sc, "ok", _TRUNCATE);
            }
            for (i = 0; i < ONL_SEC_N; i++) {
                if (strcmp(line + 4, kOnlSec[i][0]) == 0) {
                    strncpy_s(sec, sc, kOnlSec[i][0], _TRUNCATE);
                }
            }
        } else if (strncmp(line, "lobby_code=", 11) == 0) {
            if (onl_code_norm(line + 11, code, cc) != 1) {
                code[0] = '\0';
            }
        }
    }
    fclose(f);
    return 1;
}

/* W353: err= aus loc_net_status.txt (letzte Zeile gewinnt); nur bekannte Werte, sonst leer. */
void onl_status_err(const char *path, char *err, size_t cap)
{
    FILE *f = NULL;
    char line[128];
    err[0] = '\0';
    if (!path || u8_fopen_s(&f, path, "r") != 0 || !f) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        int i;
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) {
            *--e = '\0';
        }
        if (strncmp(line, "err=", 4) != 0) {
            continue;
        }
        err[0] = '\0';
        for (i = 0; i < ONL_ERR_N; i++) {
            if (strcmp(line + 4, kOnlErr[i][0]) == 0) {
                strncpy_s(err, cap, kOnlErr[i][0], _TRUNCATE);
            }
        }
    }
    fclose(f);
}

/* W420: ver=/need_ver= aus loc_net_status.txt (Versionsablehnung steelnet); nur bekannte Werte bzw. Zeichen. */
static void onl_status_ver(const char *path, char *ver, size_t vc, char *need, size_t nc)
{
    FILE *f = NULL;
    char line[128];
    ver[0] = need[0] = '\0';
    if (!path || u8_fopen_s(&f, path, "r") != 0 || !f) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line), *q;
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) {
            *--e = '\0';
        }
        if (strcmp(line, "ver=server") == 0 || strcmp(line, "ver=peer") == 0 || strcmp(line, "ver=lan") == 0) { /* W454: lan */
            strncpy_s(ver, vc, line + 4, _TRUNCATE);
        } else if (strncmp(line, "need_ver=", 9) == 0 && strlen(line + 9) < 16) {
            for (q = line + 9; *q && (isalnum((unsigned char)*q) || *q == '.' || *q == '+' || *q == '-'); q++) {
            }
            if (!*q) strncpy_s(need, nc, line + 9, _TRUNCATE);
        }
    }
    fclose(f);
}

/* Lauf-Ordner des Clients: SB_OUT_DIR (erbt der Kindprozess; relativ = zum
 * Client-Ordner, dem Arbeitsordner des Clients), sonst der Client-Ordner. */
static void onl_status_path(char *out, size_t cap)
{
    char od[1024];
    if (g_onl_status_test[0]) {
        strcpy_s(out, cap, g_onl_status_test);
        return;
    }
    if (u8_GetEnv("SB_OUT_DIR", od, sizeof od) > 0 && od[0]) {
        int abs = od[0] == '\\' || od[0] == '/' || (od[0] && od[1] == ':');
        if (abs) {
            _snprintf_s(out, cap, _TRUNCATE, "%s\\loc_net_status.txt", od);
        } else {
            _snprintf_s(out, cap, _TRUNCATE, "%s\\%s\\loc_net_status.txt", g_client_dir, od);
        }
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s\\loc_net_status.txt", g_client_dir);
}

/* W454: mode=lan, lan_peers=, lan_up= -> "lan:N:M" (nur Zahlen 0..99), sonst leer. */
static void lan_status_read(const char *path, char *out, size_t cap)
{
    FILE *f = NULL;
    char line[64];
    int lan = 0, np = -1, nu = -1;
    out[0] = '\0';
    if (!path || u8_fopen_s(&f, path, "r") != 0 || !f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "mode=lan", 8) == 0) lan = 1;
        else if (strncmp(line, "lan_peers=", 10) == 0) np = atoi(line + 10);
        else if (strncmp(line, "lan_up=", 7) == 0) nu = atoi(line + 7);
    }
    fclose(f);
    if (lan && np >= 0 && np < 100 && nu >= 0 && nu < 100) _snprintf_s(out, cap, _TRUNCATE, "lan:%d:%d", np, nu);
}

/* Statusdatei nachlesen (nur Dateien seit g_onl_since). 1 = Anzeige geaendert. */
int onl_status_refresh(void)
{
    char p[1200], sec[24] = "", code[16] = "", err[12] = "", ver[8] = "", need[16] = "", lst[40] = "";
    wchar_t w[1200];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (g_onl_fixture || (g_o.shot != 0 && !g_onl_status_test[0])) {
        return 0; /* Shots: nie die echte Datei */
    }
    onl_status_path(p, sizeof p);
    u8_to_w(p, w, 1200);
    if (GetFileAttributesExW(w, GetFileExInfoStandard, &fa)) {
        ULONGLONG t = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        if (t >= g_onl_since) {
            onl_status_parse(p, sec, sizeof sec, code, sizeof code);
            onl_status_err(p, err, sizeof err); /* W353 */
            onl_status_ver(p, ver, sizeof ver, need, sizeof need); /* W420 */
            lan_status_read(p, lst, sizeof lst);                  /* W454 */
        }
    }
    if (strcmp(sec, g_onl_sec) == 0 && strcmp(code, g_onl_code) == 0 && strcmp(err, g_onl_err) == 0 &&
        strcmp(ver, g_onl_ver) == 0 && strcmp(need, g_onl_need_ver) == 0 && strcmp(lst, g_lan_st) == 0) {
        return 0;
    }
    strcpy_s(g_lan_st, sizeof g_lan_st, lst); /* W454 */
    strcpy_s(g_onl_ver, sizeof g_onl_ver, ver);
    strcpy_s(g_onl_need_ver, sizeof g_onl_need_ver, need);
    strcpy_s(g_onl_sec, sizeof g_onl_sec, sec);
    strcpy_s(g_onl_code, sizeof g_onl_code, code);
    strcpy_s(g_onl_err, sizeof g_onl_err, err);
    return 1;
}

/* ------------------------------------------------------------------ */
/* W454 LOC-LAN-MODE: Hilfen (Raumname, Start, Anzeige, Felder)         */
/* ------------------------------------------------------------------ */
static int onl_save_keystr(const char *key, const char *val);
void lan_kind_set(int v);
int acc_local_name(char *out, size_t cap); /* launcher_gui_account.c: Name des lokalen Kontos (ohne Netz) */
/* Raumname wie der Client (loc_lan.c llan_room_norm): Rand-Leerzeichen egal, leer = Standard, sonst druckbares
 * ASCII 0x20..0x7E, 1..16 Zeichen. 1 = gueltig. */
int lan_room_valid(const char *r)
{
    size_t a = 0, e = strlen(r), i;
    while (a < e && r[a] == ' ') a++;
    while (e > a && r[e - 1] == ' ') e--;
    if (e - a > 16) return 0;
    for (i = a; i < e; i++) {
        unsigned char c = (unsigned char)r[i];
        if (c < 0x20 || c > 0x7E) return 0;
    }
    return 1;
}

/* --set des LoC-Starts im LAN: Transport udp, LAN an, Raumname (ohne Rand-Leerzeichen; ';' und '=' nicht erlaubt,
 * weil sie das --set trennen). 0 = ok, -1 = Raumname ungueltig. */
int lan_set_append(char *set, size_t cap)
{
    char r[17] = "";
    size_t l = strlen(set), a = 0, e;
    if (!lan_room_valid(g_loc_lan_room)) return -1;
    e = strlen(g_loc_lan_room);
    while (a < e && g_loc_lan_room[a] == ' ') a++;
    while (e > a && g_loc_lan_room[e - 1] == ' ') e--;
    memcpy(r, g_loc_lan_room + a, e - a);
    r[e - a] = 0;
    if (strchr(r, ';') || strchr(r, '=') || strchr(r, '\\')) return -1;
    _snprintf_s(set + l, cap - l, _TRUNCATE, "%sloc_transport=udp;loc_lan=1%s%s", l ? ";" : "", r[0] ? ";loc_lan_room=" : "", r);
    return 0;
}

/* Pilotname im LAN: Name des steelnet-Kontos, falls auf diesem PC vorhanden (lokal, kein Netz), sonst Standard des
 * Clients ("Pilot"). Nie der Schluessel. */
void lan_pilot_append(char *set, size_t cap)
{
    char nm[40] = "";
    if (acc_local_name(nm, sizeof nm) && nm[0] && !strchr(nm, ';') && !strchr(nm, '=')) {
        _snprintf_s(set + strlen(set), cap - strlen(set), _TRUNCATE, ";loc_lobby_pilot=%s", nm);
    }
}

/* Raumpasswort nur fuer den Kindprozess (on = 1 setzen, 0 loeschen). */
void lan_env(int on)
{
    wchar_t w[64];
    int i;
    if (on && g_loc_lan && g_loc_lan_pass[0]) {
        for (i = 0; i < 63 && g_loc_lan_pass[i]; i++) w[i] = (wchar_t)(unsigned char)g_loc_lan_pass[i];
        w[i] = 0;
        SetEnvironmentVariableW(L"SB_LAN_PASS", w);
        SecureZeroMemory(w, sizeof w);
    } else {
        SetEnvironmentVariableW(L"SB_LAN_PASS", NULL);
    }
}

/* Statuszeile LAN (Karte Match und Spielen): nach einem Lauf die Zahl der gefundenen/verbundenen Mitspieler aus
 * loc_net_status.txt (mode=lan), sonst Hinweis. */
int lan_info(char *out, size_t cap, COLORREF *col)
{
    int np = 0, nu = 0;
    *col = C_DIM;
    if (strcmp(g_onl_ver, "lan") == 0) { /* Client meldet einen Mitspieler mit anderer Version */
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.lan_ver_bad"), g_onl_need_ver[0] ? g_onl_need_ver : "?");
        *col = C_WARN;
        return 1;
    }
    if (strcmp(g_onl_err, "lan") == 0) { /* Entdeckungs-Port belegt bzw. Raumname */
        strcpy_s(out, cap, LT("gui.lan_err"));
        *col = C_WARN;
        return 1;
    }
    if (sscanf_s(g_lan_st, "lan:%d:%d", &np, &nu) == 2) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.lan_status"), np, nu);
        *col = nu > 0 ? C_OK : C_DIM;
        return 1;
    }
    strcpy_s(out, cap, LT(g_loc_lan_pass[0] ? "gui.lan_hint_pass" : "gui.lan_hint"));
    return 1;
}

void lan_kind_fill(void)
{
    HWND c = ctl(ID_LNET_KIND);
    if (!c) return;
    g_updating++;
    ComboBox_ResetContent(c);
    combo_add(c, LT("gui.lan_kind_online"));
    combo_add(c, LT("gui.lan_kind_lan"));
    ComboBox_SetCurSel(c, g_loc_lan ? 1 : 0);
    g_updating--;
}

void lan_cues(void)
{
    wchar_t w[80];
    if (ctl(ID_LNET_LANROOM)) {
        u8_to_w(LT("gui.lan_room_cue"), w, 80);
        SendMessageW(ctl(ID_LNET_LANROOM), EM_SETCUEBANNER, TRUE, (LPARAM)w);
    }
    if (ctl(ID_LNET_LANPASS)) {
        u8_to_w(LT("gui.lan_pass_cue"), w, 80);
        SendMessageW(ctl(ID_LNET_LANPASS), EM_SETCUEBANNER, TRUE, (LPARAM)w);
    }
}

void lan_build_init(void)
{
    if (ctl(ID_LNET_LANROOM)) {
        SendMessageW(ctl(ID_LNET_LANROOM), EM_LIMITTEXT, 16, 0);
        g_updating++;
        ui_settext(ctl(ID_LNET_LANROOM), g_loc_lan_room);
        g_updating--;
    }
    if (ctl(ID_LNET_LANPASS)) {
        SendMessageW(ctl(ID_LNET_LANPASS), EM_LIMITTEXT, 63, 0);
    }
    lan_kind_fill();
    lan_cues();
}

/* Auswahl Online/LAN (UI) bzw. Felder geaendert. */
void lan_kind_pick(void)
{
    int v = ctl(ID_LNET_KIND) ? ComboBox_GetCurSel(ctl(ID_LNET_KIND)) == 1 : 0;
    if (v == g_loc_lan) return;
    lan_kind_set(v);
}

void lan_kind_set(int v)
{
    g_loc_lan = v ? 1 : 0;
    onl_save_key(LNET_LAN_KEY, g_loc_lan);
    onl_sync_ui();
    loc_gate_sync(); /* LoC-Start sofort frei bzw. gesperrt */
    set_status_k(LT(g_loc_lan ? "gui.lan_on" : "gui.lan_off"), 1);
    ui_refresh_visibility();
    redraw_all();
}

void lan_field_changed(int id)
{
    char t[80];
    if (!ctl(id)) return;
    ui_gettext(ctl(id), t, sizeof t);
    if (id == ID_LNET_LANROOM) {
        if (lan_room_valid(t) && !strchr(t, ';') && !strchr(t, '=') && !strchr(t, '\\')) {
            strcpy_s(g_loc_lan_room, sizeof g_loc_lan_room, t);
            onl_save_keystr(LNET_LANROOM_KEY, t);
        }
    } else if (id == ID_LNET_LANPASS) {
        size_t i, ok = 1;
        for (i = 0; t[i]; i++) ok &= (unsigned char)t[i] >= 0x20 && (unsigned char)t[i] <= 0x7E;
        if (ok) strcpy_s(g_loc_lan_pass, sizeof g_loc_lan_pass, t);
        else g_loc_lan_pass[0] = 0;
    }
    SecureZeroMemory(t, sizeof t);
    onl_redraw();
}

/* Infozeile der Karte (Text, Farbe). Rueckgabe 1 = Raumcode der eigenen
 * Lobby wird gezeigt (Knopf "Kopieren" sichtbar). Reihenfolge: Dienst fehlt,
 * Eingabe ungueltig, Sicherheitsmeldung des Clients, eigener Raumcode, Hinweis. */
int onl_info(char *out, size_t cap, COLORREF *col)
{
    char raw[64] = "", code[16];
    int i;
    if (!onl_srv_on()) {
        strncpy_s(out, cap, LT("gui.loc_onl_nosrv"), _TRUNCATE);
        *col = C_DIM;
        return 0;
    }
    if (ctl(ID_LOC_CODE)) {
        ui_gettext(ctl(ID_LOC_CODE), raw, sizeof raw);
    }
    if (g_loc_online && onl_code_norm(raw, code, sizeof code) < 0) {
        strncpy_s(out, cap, LT("gui.loc_code_bad"), _TRUNCATE);
        *col = C_WARN;
        return 0;
    }
    if (loc_ver_gate()) { /* W421: Launcher laut /v1/info veraltet (vor der Meldung des letzten Laufs) */
        char m[300];
        loc_gate_msg(m, sizeof m);
        strncpy_s(out, cap, m, _TRUNCATE);
        *col = C_ERR;
        return 0;
    }
    if (g_onl_ver[0]) { /* W420: steelnet nur mit der neuesten Version (Treffpunkt bzw. Mitspieler) */
        _snprintf_s(out, cap, _TRUNCATE, LT(strcmp(g_onl_ver, "peer") == 0 ? "gui.loc_onl_ver_peer" : "gui.loc_onl_ver"),
                    g_onl_need_ver[0] ? g_onl_need_ver : "?");
        *col = C_ERR;
        return 0;
    }
    for (i = 0; i < ONL_SEC_N; i++) {
        if (strcmp(g_onl_sec, kOnlSec[i][0]) == 0) {
            strncpy_s(out, cap, LT(kOnlSec[i][1]), _TRUNCATE);
            *col = strcmp(g_onl_sec, "no_pin") == 0 ? C_WARN : C_ERR;
            return 0;
        }
    }
    for (i = 0; i < ONL_ERR_N; i++) { /* W353: Fehlergrund vor dem Raumcode (was jetzt zu tun ist) */
        if (strcmp(g_onl_err, kOnlErr[i][0]) == 0) {
            strncpy_s(out, cap, LT(kOnlErr[i][1]), _TRUNCATE);
            *col = (strcmp(g_onl_err, "lost") == 0 || strcmp(g_onl_err, "rate") == 0) ? C_WARN : C_ERR;
            return 0;
        }
    }
    if (g_onl_code[0]) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.loc_onl_code"), g_onl_code);
        *col = C_OK;
        return 1;
    }
    if (g_loc_public && !raw[0]) { /* W373: oeffentlich (Standard) - Vertrauenshinweis */
        strncpy_s(out, cap, LT("gui.loc_onl_trust"), _TRUNCATE);
        *col = C_WARN;
        return 0;
    }
    strncpy_s(out, cap, LT(g_loc_public ? "gui.loc_code_hint_pub" : "gui.loc_code_hint"), _TRUNCATE);
    *col = C_DIM;
    return 0;
}

void lnet_mission_load(void); /* W473 */
void onl_load(void)
{
    char up[1200], line[256];
    FILE *f = NULL;
    size_t kl = strlen(ONL_KEY);
    g_loc_online = 0;
    g_loc_public = 1; /* W373: Standard oeffentlich */
    g_loc_smooth_opp = 1; /* W387: Standard auto */
    g_loc_rb_on = g_loc_own_pred = g_loc_lowd = 1; /* W392: Standard an */
    g_loc_net_auto = 1; /* W447: Standard Auto */
    g_loc_clip = 1; /* W420: Standard an */
    g_loc_chat = 1; /* W437: Standard an */
    g_loc_snchat = 1; /* W446: Standard an */
    g_loc_arcade = 0; /* W440: Standard Sim */
    g_loc_lan = 0;    /* W454: Standard steelnet */
    g_loc_lan_room[0] = 0;
    if (!g_onl_since) { /* laufendes Spiel (Launcher neu geoeffnet) zaehlt, alte Laeufe nicht */
        g_onl_since = onl_now_ft() - 600ULL * 10000000ULL;
    }
    user_cfg_path(up, sizeof up);
    if (u8_fopen_s(&f, up, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, ONL_KEY, kl) == 0 && line[kl] == '=') {
                g_loc_online = atoi(line + kl + 1) ? 1 : 0; /* letzte Zeile gewinnt */
            }
            if (strncmp(line, ONL_PUB_KEY "=", sizeof ONL_PUB_KEY) == 0) { /* W373: fehlt = oeffentlich */
                g_loc_public = atoi(line + sizeof ONL_PUB_KEY) ? 1 : 0;
            }
            if (strncmp(line, ONL_SMOOTH_KEY "=", sizeof ONL_SMOOTH_KEY) == 0) { /* W387: fehlt/ungueltig = 1 */
                g_loc_smooth_opp = loc_smooth_parse(line + sizeof ONL_SMOOTH_KEY);
            }
            if (strncmp(line, LNET_RB_KEY "=", sizeof LNET_RB_KEY) == 0) { /* W392: fehlt/ungueltig = 1 */
                g_loc_rb_on = lnet_bool_parse(line + sizeof LNET_RB_KEY);
            }
            if (strncmp(line, LNET_OWN_KEY "=", sizeof LNET_OWN_KEY) == 0) {
                g_loc_own_pred = lnet_bool_parse(line + sizeof LNET_OWN_KEY);
            }
            if (strncmp(line, LNET_AUTO_KEY "=", sizeof LNET_AUTO_KEY) == 0) { /* W447: fehlt/ungueltig = 1 (Auto) */
                g_loc_net_auto = lnet_bool_parse(line + sizeof LNET_AUTO_KEY);
            }
            if (strncmp(line, LNET_LOWD_KEY "=", sizeof LNET_LOWD_KEY) == 0) {
                g_loc_lowd = lnet_bool_parse(line + sizeof LNET_LOWD_KEY);
            }
            if (strncmp(line, LNET_CLIP_KEY "=", sizeof LNET_CLIP_KEY) == 0) { /* W420: fehlt/ungueltig = 1 */
                g_loc_clip = lnet_bool_parse(line + sizeof LNET_CLIP_KEY);
            }
            if (strncmp(line, LNET_CHAT_KEY "=", sizeof LNET_CHAT_KEY) == 0) { /* W437: fehlt/ungueltig = 1 */
                g_loc_chat = lnet_bool_parse(line + sizeof LNET_CHAT_KEY);
            }
            if (strncmp(line, LNET_SNCHAT_KEY "=", sizeof LNET_SNCHAT_KEY) == 0) { /* W446: fehlt/ungueltig = 1 */
                g_loc_snchat = lnet_bool_parse(line + sizeof LNET_SNCHAT_KEY);
            }
            if (strncmp(line, LNET_LAN_KEY "=", sizeof LNET_LAN_KEY) == 0) { /* W454: nur genau 1 = LAN */
                const char *lv = line + sizeof LNET_LAN_KEY;
                g_loc_lan = lv[0] == '1' && (lv[1] == 0 || lv[1] == '\r' || lv[1] == '\n' || lv[1] == ' ');
            }
            if (strncmp(line, LNET_LANROOM_KEY "=", sizeof LNET_LANROOM_KEY) == 0) { /* W454: ungueltig = Standard */
                char r[64];
                size_t rl;
                strncpy_s(r, sizeof r, line + sizeof LNET_LANROOM_KEY, _TRUNCATE);
                rl = strlen(r);
                while (rl > 0 && (r[rl - 1] == '\n' || r[rl - 1] == '\r')) r[--rl] = 0;
                if (!lan_room_valid(r)) r[0] = 0;
                strcpy_s(g_loc_lan_room, sizeof g_loc_lan_room, r);
            }
            if (strncmp(line, LNET_MODE_KEY "=", sizeof LNET_MODE_KEY) == 0) { /* W440: nur genau 1 = Arcade */
                const char *mv = line + sizeof LNET_MODE_KEY;
                g_loc_arcade = mv[0] == '1' && (mv[1] == 0 || mv[1] == '\r' || mv[1] == '\n' || mv[1] == ' ');
            }
        }
        fclose(f);
    }
    lnet_mission_load(); /* W473: Chat im Spiel (user_loc.cfg) */
}

/* launcher_loc_online (W373: bzw. launcher_loc_public) in user.cfg setzen
 * (erste Zeile ersetzen, Doppelte weg, sonst anhaengen; fremde Zeilen
 * bleiben). 0 = gespeichert, 1 = nicht geschrieben (Shot), -1 = Fehler.
 * W387: Wert wird unveraendert geschrieben (launcher_loc_smooth_opp 0..2; Schalter uebergeben 0/1). */
static int onl_save_keystr(const char *key, const char *val);
int onl_save_key(const char *key, int val)
{
    char v[24];
    _snprintf_s(v, sizeof v, _TRUNCATE, "%d", val);
    return onl_save_keystr(key, v);
}

/* W454: wie onl_save_key mit Text (nur fuer gepruefte Werte ohne Zeilenumbruch, z. B. den LAN-Raumnamen).
 * W473: Kern mit Dateipfad (cfg_file_setkey), damit die Chat-im-Spiel-Schluessel in user_loc.cfg landen. */
static int cfg_file_setkey(const char *up, const char *key, const char *val);
static int onl_save_keystr(const char *key, const char *val)
{
    char up[1200];
    if (!loctab_may_write()) {
        return 1;
    }
    user_cfg_path(up, sizeof up);
    return cfg_file_setkey(up, key, val);
}

static int cfg_file_setkey(const char *up, const char *key, const char *val)
{
    char line[1200], *out;
    FILE *f = NULL;
    size_t n = 0, cap = 1024 * 1024, kl = strlen(key);
    int done = 0;
    out = (char *)malloc(cap);
    if (!out) {
        return -1;
    }
    if (u8_fopen_s(&f, up, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            size_t l;
            if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
                if (done) {
                    continue;
                }
                _snprintf_s(line, sizeof line, _TRUNCATE, "%s=%s\n", key, val);
                done = 1;
            }
            l = strlen(line);
            if (n + l + 64 >= cap) {
                fclose(f);
                free(out);
                return -1;
            }
            memcpy(out + n, line, l);
            n += l;
        }
        fclose(f);
    }
    if (!done) {
        if (n > 0 && out[n - 1] != '\n') {
            out[n++] = '\n';
        }
        n += (size_t)_snprintf_s(out + n, cap - n, _TRUNCATE, "%s=%s\n", key, val);
    }
    if (u8_fopen_s(&f, up, "w") != 0 || !f) {
        free(out);
        set_status_k(LT("gui.loc_save_err"), 2);
        return -1;
    }
    fwrite(out, 1, n, f);
    fclose(f);
    free(out);
    return 0;
}

int onl_save(void)
{
    return onl_save_key(ONL_KEY, g_loc_online);
}

/* Schalter/Feld zum Zustand: ohne Dienst grau (und aus), Feld nur mit Online an. */
int lnet_id_grey(int id); /* W473 */
void onl_sync_ui(void)
{
    int srv = onl_srv_on();
    if (ctl(ID_LOC_ONL)) {
        ui_set_checked(ID_LOC_ONL, srv && g_loc_online);
        EnableWindow(ctl(ID_LOC_ONL), srv ? TRUE : FALSE);
    }
    if (ctl(ID_LOC_CODE)) {
        EnableWindow(ctl(ID_LOC_CODE), srv && g_loc_online ? TRUE : FALSE);
    }
    if (ctl(ID_LOC_VIS)) { /* W373 */
        g_updating++;
        if (ComboBox_GetCurSel(ctl(ID_LOC_VIS)) != (g_loc_public ? 0 : 1)) {
            ComboBox_SetCurSel(ctl(ID_LOC_VIS), g_loc_public ? 0 : 1);
        }
        g_updating--;
        EnableWindow(ctl(ID_LOC_VIS), srv && g_loc_online ? TRUE : FALSE);
    }
    if (ctl(ID_LOC_SMOOTH)) { /* W387: gilt fuer jeden LoC-Start (W392: grau ohne Rollback) */
        g_updating++;
        if (ComboBox_GetCurSel(ctl(ID_LOC_SMOOTH)) != g_loc_smooth_opp) {
            ComboBox_SetCurSel(ctl(ID_LOC_SMOOTH), g_loc_smooth_opp);
        }
        g_updating--;
        EnableWindow(ctl(ID_LOC_SMOOTH), g_loc_rb_on ? TRUE : FALSE);
    }
    /* W392: Netzwerk-Schalter; ohne Rollback wirken die drei anderen nicht -> grau (Wert bleibt gemerkt) */
    if (ctl(ID_LNET_RB)) { /* W447: Auswahl Netzmodus */
        lnet_netmode_fill();
    }
    if (ctl(ID_LNET_OWN)) {
        ui_set_checked(ID_LNET_OWN, g_loc_own_pred);
        EnableWindow(ctl(ID_LNET_OWN), g_loc_rb_on ? TRUE : FALSE);
    }
    if (ctl(ID_LNET_LOWD)) {
        ui_set_checked(ID_LNET_LOWD, g_loc_lowd);
        EnableWindow(ctl(ID_LNET_LOWD), g_loc_rb_on ? TRUE : FALSE);
    }
    if (ctl(ID_LNET_CLIP)) { /* W420: unabhaengig vom Rollback */
        ui_set_checked(ID_LNET_CLIP, g_loc_clip);
    }
    if (ctl(ID_LNET_CHAT)) { /* W437: unabhaengig vom Rollback */
        ui_set_checked(ID_LNET_CHAT, g_loc_chat);
    }
    if (ctl(ID_LNET_SNCHAT)) { /* W446: unabhaengig vom Rollback; W454: im LAN nicht verfuegbar (grau, Wert bleibt) */
        ui_set_checked(ID_LNET_SNCHAT, g_loc_snchat);
        EnableWindow(ctl(ID_LNET_SNCHAT), g_loc_lan ? FALSE : TRUE);
    }
    { /* W473: Chat im Spiel (grau wie lnet_row_grey; Wert bleibt) */
        static const int kM[3] = { ID_LNET_MSN, ID_LNET_MLOBBY, ID_LNET_MTEAM };
        int k;
        for (k = 0; k < 3; k++) {
            if (ctl(kM[k])) {
                ui_set_checked(kM[k], k == 0 ? g_loc_msn : k == 1 ? g_loc_mlobby : g_loc_mteam);
                EnableWindow(ctl(kM[k]), lnet_id_grey(kM[k]) ? FALSE : TRUE);
            }
        }
        ui_set_checked(ID_LNET_TAB0, g_lnet_tab == 0);
        ui_set_checked(ID_LNET_TAB1, g_lnet_tab == 1);
    }
    if (ctl(ID_LNET_KIND)) { /* W454 */
        g_updating++;
        if (ComboBox_GetCurSel(ctl(ID_LNET_KIND)) != (g_loc_lan ? 1 : 0)) ComboBox_SetCurSel(ctl(ID_LNET_KIND), g_loc_lan ? 1 : 0);
        g_updating--;
    }
    if (ctl(ID_LNET_MODE)) { /* W440: unabhaengig vom Rollback */
        g_updating++;
        ComboBox_SetCurSel(ctl(ID_LNET_MODE), g_loc_arcade ? 1 : 0);
        g_updating--;
    }
    if (ctl(ID_LNET_RESET)) {
        EnableWindow(ctl(ID_LNET_RESET), lnet_is_default() ? FALSE : TRUE);
    }
}

void onl_redraw(void)
{
    if (g_page == P_LNET) { /* W397: Match auf Multiplayer */
        redraw_rect(CX0, MP_MATCH_Y, COLW, MP_MATCH_H + 4);
        return;
    }
    redraw_rect(LOC_NET_X, LOC_NET_Y, LOC_NET_W, LOC_NET_H + 4);
}

static int onl_copy_visible(void)
{
    char t[300];
    COLORREF c;
    return onl_info(t, sizeof t, &c);
}

void onl_copy(void)
{
    wchar_t w[16];
    HGLOBAL hg;
    if (!g_onl_code[0]) {
        return;
    }
    if (g_o.shot != 0) { /* Test: Zwischenablage des Nutzers nie anfassen */
        printf("[clipboard] Raumcode (%u Zeichen)\n", (unsigned)strlen(g_onl_code));
        set_status_k(LT("gui.loc_code_copied"), 1);
        return;
    }
    u8_to_w(g_onl_code, w, 16);
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        hg = GlobalAlloc(GMEM_MOVEABLE, (wcslen(w) + 1) * sizeof(wchar_t));
        if (hg) {
            memcpy(GlobalLock(hg), w, (wcslen(w) + 1) * sizeof(wchar_t));
            GlobalUnlock(hg);
            if (!SetClipboardData(CF_UNICODETEXT, hg)) {
                GlobalFree(hg);
            }
        }
        CloseClipboard();
        set_status_k(LT("gui.loc_code_copied"), 1);
    }
}

/* W373 LOC-PUBLIC-LOBBY: Auswahl Oeffentlich (0) / Privat mit Raumcode (1), gemerkt in user.cfg. */
void onl_vis_pick(void)
{
    int pub = ComboBox_GetCurSel(ctl(ID_LOC_VIS)) != 1;
    if (pub == g_loc_public) {
        return;
    }
    g_loc_public = pub;
    onl_save_key(ONL_PUB_KEY, g_loc_public);
    set_status_k(LT(g_loc_public ? "gui.loc_vis_pub_on" : "gui.loc_vis_priv_on"), 1);
    ui_refresh_visibility();
    onl_redraw();
}

/* W387 LOC-SMOOTH-OPP: Eintraege in Reihenfolge der Werte loc_smooth_opp (0 aus, 1 auto, 2 fest 1 Tick). */
const char *const kLocSmooth[LOC_SMOOTH_N] = { "gui.loc_smooth_off", "gui.loc_smooth_auto",
                                                      "gui.loc_smooth_fix" };

/* W387: Auswahl Gegner glaetten, gemerkt in user.cfg (wirkt beim naechsten Start). */
void loc_smooth_pick(void)
{
    char t[300];
    int v = ComboBox_GetCurSel(ctl(ID_LOC_SMOOTH));
    if (v < 0 || v >= LOC_SMOOTH_N || v == g_loc_smooth_opp) {
        return;
    }
    g_loc_smooth_opp = v;
    if (onl_save_key(ONL_SMOOTH_KEY, g_loc_smooth_opp) >= 0) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.loc_smooth_set"), LT(kLocSmooth[v]));
        set_status_k(t, 1);
    }
}

/* W387: Eintraege (Sprache), Auswahl nach g_loc_smooth_opp. */
static void loc_smooth_fill(void)
{
    HWND c = ctl(ID_LOC_SMOOTH);
    int i;
    if (!c) {
        return;
    }
    g_updating++;
    ComboBox_ResetContent(c);
    for (i = 0; i < LOC_SMOOTH_N; i++) {
        combo_add(c, LT(kLocSmooth[i]));
    }
    ComboBox_SetCurSel(c, g_loc_smooth_opp);
    g_updating--;
    EnableWindow(c, g_loc_rb_on ? TRUE : FALSE); /* W392 */
}

/* ------------------------------------------------------------------ */
/* W392 LAUNCHER-NETTAB: Seite "Netzwerk" (nur LoC, Navigationsplatz    */
/* von Packs). Eine Karte: Rollback-Netcode, darunter eingerueckt die   */
/* drei Teile, die nur mit Rollback wirken (ohne Rollback grau, Wert    */
/* bleibt gemerkt), je ein kurzer Hinweis, "Standard wiederherstellen". */
/* ------------------------------------------------------------------ */
#define LNET_Y (CY0 + 196) /* W397: unter Match/Account, volle Breite (W473: Karten darueber 184 hoch) */
#define LNET_H 308
#define LNET_IND 28           /* Einrueckung abhaengiger Zeilen (Rollback bzw. Chat) */
#define LNET_LBL_W 150        /* W473: Beschriftung links der Auswahlfelder */
#define LNET_COMBO_W 202      /* W473: Auswahlfeld zeigt nur den Wert */
#define LNET_CTL_END (CX0 + 18 + LNET_LBL_W + LNET_COMBO_W) /* rechte Kante der Elemente (Schalter + Auswahl) */
#define LNET_HINT_X (LNET_CTL_END + 16)
#define LNET_HINT_W (CX0 + CW - 18 - LNET_HINT_X)
#define LNET_HINT_H 30        /* hoechstens zwei kurze Zeilen */
#define LNET_TAB_W0 120       /* W473: Reiter im Kartenkopf (Segment) */
#define LNET_TAB_W1 150
#define LNET_BTN_W 180        /* Standard wiederherstellen */
#define LNET_ROWS 7
/* W473 MPPOLISH: zwei Reiter statt einer langen Karte. Network = alles, was das Netzverhalten im Match bestimmt
 * (Netzmodus und die drei Rollback-Teile); Lobby & chat = Regeln der Lobbys, die ich hoste, und alles zum Chat
 * (inkl. Chat im Spiel). Zeile = (Reiter, y, Element, Beschriftung links (Auswahl) oder NULL (Schalter), Hinweis,
 * abhaengig von: 1 = Rollback, 2 = Chat, 3 = steelnet-Chat). */
typedef struct {
    int tab, row, id;
    const char *lbl, *hint;
    int dep;
} lnet_row_t;
static const lnet_row_t kLnetRows[] = {
    { 0, 0, ID_LNET_RB, "gui.lnet_rb", "gui.lnet_rb_hint", 0 },
    { 0, 1, ID_LNET_OWN, NULL, "gui.lnet_own_hint", 1 },
    { 0, 2, ID_LOC_SMOOTH, "gui.lnet_smooth", "gui.lnet_smooth_hint", 1 },
    { 0, 3, ID_LNET_LOWD, NULL, "gui.lnet_lowd_hint", 1 },
    { 1, 0, ID_LNET_MODE, "gui.lnet_mode", "gui.lnet_mode_hint", 0 },
    { 1, 1, ID_LNET_CLIP, NULL, "gui.lnet_clip_hint", 0 },
    { 1, 2, ID_LNET_CHAT, NULL, "gui.lnet_chat_hint", 0 },
    { 1, 3, ID_LNET_MLOBBY, NULL, "gui.lnet_mlobby_hint", 2 },
#if LNET_HAVE_TEAM_TAB
    { 1, 4, ID_LNET_MTEAM, NULL, "gui.lnet_mteam_hint", 2 },
    { 1, 5, ID_LNET_SNCHAT, NULL, "gui.lnet_snchat_hint", 0 },
    { 1, 6, ID_LNET_MSN, NULL, "gui.lnet_msn_hint", 3 },
#else /* Nutzer 24.11.: ohne Team-Reiter im Spiel kein Team-Schalter (Reiter -1 = nie sichtbar, Wert bleibt in der cfg) */
    { -1, 4, ID_LNET_MTEAM, NULL, "gui.lnet_mteam_hint", 2 },
    { 1, 4, ID_LNET_SNCHAT, NULL, "gui.lnet_snchat_hint", 0 },
    { 1, 5, ID_LNET_MSN, NULL, "gui.lnet_msn_hint", 3 },
#endif
};
#define LNET_NROWS ((int)(sizeof kLnetRows / sizeof kLnetRows[0]))
/* y der Zeile: Network weiter auseinander (4 Zeilen), Lobby & chat enger (7 Zeilen) */
static int lnet_row_y(int tab, int row) { return LNET_Y + 72 + row * (tab == 0 ? 40 : 32); }
static const lnet_row_t *lnet_row_of(int id)
{
    int i;
    for (i = 0; i < LNET_NROWS; i++) {
        if (kLnetRows[i].id == id) return &kLnetRows[i];
    }
    return NULL;
}
/* W473: Element gehoert zum gerade nicht gewaehlten Reiter (Chat privacy = Lobby & chat, Release-Seite = Network) */
int lnet_tab_hidden(int id)
{
    const lnet_row_t *r = lnet_row_of(id);
    if (id == ID_LNET_PRIV) return g_lnet_tab != 1;
    if (id == ID_LNET_VERPAGE) return g_lnet_tab != 0;
    return r && r->tab != g_lnet_tab;
}
/* Zeile ist grau (abhaengig und Voraussetzung aus; Team-Reiter im Spiel noch nicht da; steelnet im LAN) */
static int lnet_row_grey(const lnet_row_t *r)
{
    if (r->id == ID_LNET_MTEAM && !LNET_HAVE_TEAM_TAB) return 1;
    if ((r->id == ID_LNET_SNCHAT || r->id == ID_LNET_MSN) && g_loc_lan) return 1;
    return (r->dep == 1 && !g_loc_rb_on) || (r->dep == 2 && !g_loc_chat) || (r->dep == 3 && !g_loc_snchat);
}
int lnet_id_grey(int id)
{
    const lnet_row_t *r = lnet_row_of(id);
    return r && lnet_row_grey(r);
}
static const char *lnet_row_hint(const lnet_row_t *r)
{
    if (r->id == ID_LNET_MTEAM && !LNET_HAVE_TEAM_TAB) return LT("gui.lnet_mteam_later");
    if ((r->id == ID_LNET_SNCHAT || r->id == ID_LNET_MSN) && g_loc_lan) return LT("gui.lan_na");
    if (r->dep == 1 && !g_loc_rb_on) return LT("gui.lnet_needs_rb");
    if (r->dep == 2 && !g_loc_chat) return LT("gui.lnet_needs_chat");
    if (r->dep == 3 && !g_loc_snchat) return LT("gui.lnet_needs_snchat");
    return LT(r->hint);
}

static void lnet_redraw(void)
{
    redraw_rect(CX0, LNET_Y, CW, LNET_H + 4);
}

/* W447 LOC-NET-AUTO: Eintraege (Sprache) der Auswahl Netzmodus, Auswahl nach g_loc_rb_on/g_loc_net_auto. */
static int lnet_netmode_cur(void) { return !g_loc_rb_on ? 2 : (g_loc_net_auto ? 0 : 1); }
void lnet_netmode_fill(void)
{
    HWND c = ctl(ID_LNET_RB);
    if (!c) {
        return;
    }
    g_updating++;
    ComboBox_ResetContent(c);
    combo_add(c, LT("gui.lnet_nm_auto"));
    combo_add(c, LT("gui.lnet_nm_rb"));
    combo_add(c, LT("gui.lnet_nm_ls"));
    ComboBox_SetCurSel(c, lnet_netmode_cur());
    g_updating--;
}
/* 1 = Auswahl steht nicht auf Lockstep (frueher: Schalter Rollback an) */
int lnet_rb_ui(void) { return ctl(ID_LNET_RB) && ComboBox_GetCurSel(ctl(ID_LNET_RB)) != 2; }

/* W447: Auswahl Netzmodus, sofort in user.cfg gemerkt (wirkt beim naechsten Start). */
void lnet_netmode_pick(int v)
{
    static const char *const kSt[3] = { "gui.lnet_nm_auto_on", "gui.lnet_nm_rb_on", "gui.lnet_nm_ls_on" };
    int bad = 0;
    if (v < 0 || v > 2 || v == lnet_netmode_cur()) {
        return;
    }
    g_loc_rb_on = v != 2;
    if (v != 2) {
        g_loc_net_auto = v == 0;
    }
    onl_sync_ui();
    bad |= onl_save_key(LNET_RB_KEY, g_loc_rb_on) < 0;
    bad |= onl_save_key(LNET_AUTO_KEY, g_loc_net_auto) < 0;
    if (!bad) {
        set_status_k(LT(kSt[v]), 1);
    }
    lnet_redraw();
}

/* W440 LOC-LOBBY-MODES: Eintraege (Sprache) der Auswahl Lobby-Modus, Auswahl nach g_loc_arcade. */
static void lnet_mode_fill(void)
{
    HWND c = ctl(ID_LNET_MODE);
    if (!c) {
        return;
    }
    g_updating++;
    ComboBox_ResetContent(c);
    combo_add(c, LT("gui.lnet_mode_sim"));
    combo_add(c, LT("gui.lnet_mode_arcade"));
    ComboBox_SetCurSel(c, g_loc_arcade ? 1 : 0);
    g_updating--;
}

/* W440: Auswahl Lobby-Modus, sofort in user.cfg gemerkt (gilt fuer Lobbys, die ich ab dem naechsten Start anlege). */
void lnet_mode_pick(void)
{
    int v = ComboBox_GetCurSel(ctl(ID_LNET_MODE)) == 1;
    if (v == g_loc_arcade) {
        return;
    }
    g_loc_arcade = v;
    if (onl_save_key(LNET_MODE_KEY, g_loc_arcade) >= 0) {
        set_status_k(LT(g_loc_arcade ? "gui.lnet_mode_arcade_on" : "gui.lnet_mode_sim_on"), 1);
    }
    lnet_redraw();
}

static int *lnet_var(int id, const char **key, const char **name)
{
    if (id == ID_LNET_RB) {
        *key = LNET_RB_KEY;
        *name = "gui.lnet_rb";
        return &g_loc_rb_on;
    }
    if (id == ID_LNET_OWN) {
        *key = LNET_OWN_KEY;
        *name = "gui.lnet_own";
        return &g_loc_own_pred;
    }
    if (id == ID_LNET_CLIP) { /* W420 */
        *key = LNET_CLIP_KEY;
        *name = "gui.lnet_clip";
        return &g_loc_clip;
    }
    if (id == ID_LNET_CHAT) { /* W437 */
        *key = LNET_CHAT_KEY;
        *name = "gui.lnet_chat";
        return &g_loc_chat;
    }
    if (id == ID_LNET_SNCHAT) { /* W446 */
        *key = LNET_SNCHAT_KEY;
        *name = "gui.lnet_snchat";
        return &g_loc_snchat;
    }
    *key = LNET_LOWD_KEY;
    *name = "gui.lnet_lowd";
    return &g_loc_lowd;
}

/* W473: Chat im Spiel - Schluessel/Variable je Schalter (Bit fuer "vorgemerkt"). */
static int *lnet_mvar(int id, const char **key, const char **name, int *bit)
{
    if (id == ID_LNET_MSN) {
        *key = LNET_MSN_KEY, *name = "gui.lnet_msn", *bit = 1;
        return &g_loc_msn;
    }
    if (id == ID_LNET_MLOBBY) {
        *key = LNET_MLOBBY_KEY, *name = "gui.lnet_mlobby", *bit = 2;
        return &g_loc_mlobby;
    }
    if (id == ID_LNET_MTEAM) {
        *key = LNET_MTEAM_KEY, *name = "gui.lnet_mteam", *bit = 4;
        return &g_loc_mteam;
    }
    return NULL;
}

/* W473: Wahl solange vorgemerkt, wie user_loc.cfg noch fehlt (Bitmaske 1/2/4). Die Datei legt der Launcher erst mit
 * Speichern bzw. dem ersten LoC-Start an; eine Datei nur mit diesen Schluesseln haette die Erststart-Werte von LoC
 * (Fenster, Enhanced, 16:9) verdraengt (pergame_load liest dann die Datei statt der Erststart-Regel). */
int g_lnet_m_pending;

/* W473: einen Chat-im-Spiel-Schluessel in user_loc.cfg schreiben. 0 = geschrieben oder vorgemerkt, 1 = Shot,
 * -1 = Fehler. */
static int lnet_mission_save(int id)
{
    const char *key, *name;
    char lp[1200];
    int bit, *v = lnet_mvar(id, &key, &name, &bit);
    if (!v) return -1;
    if (!loctab_may_write()) return 1;
    game_cfg_path(LGAME_LOC, lp, sizeof lp);
    if (u8_GetFileAttributes(lp) == INVALID_FILE_ATTRIBUTES) {
        g_lnet_m_pending |= bit;
        return 0;
    }
    g_lnet_m_pending &= ~bit;
    return cfg_file_setkey(lp, key, *v ? "1" : "0");
}

/* W473: nach dem Speichern der LoC-Datei (do_save; auch vor jedem LoC-Start) die vorgemerkten Werte nachtragen. */
void lnet_mission_flush(void)
{
    static const int kId[3] = { ID_LNET_MSN, ID_LNET_MLOBBY, ID_LNET_MTEAM };
    int i;
    for (i = 0; i < 3 && g_lnet_m_pending; i++) {
        const char *key, *name;
        int bit;
        lnet_mvar(kId[i], &key, &name, &bit);
        if (g_lnet_m_pending & bit) {
            lnet_mission_save(kId[i]);
        }
    }
}

/* W473: Werte aus user_loc.cfg (letzte Zeile gewinnt; nur genau 0/1 zaehlt, sonst Standard). */
void lnet_mission_load(void)
{
    static const char *const kK[3] = { LNET_MSN_KEY "=", LNET_MLOBBY_KEY "=", LNET_MTEAM_KEY "=" };
    char lp[1200], line[256];
    FILE *f = NULL;
    int *kv[3];
    g_loc_msn = 0;
    g_loc_mlobby = g_loc_mteam = 1;
    g_lnet_m_pending = 0;
    kv[0] = &g_loc_msn;
    kv[1] = &g_loc_mlobby;
    kv[2] = &g_loc_mteam;
    game_cfg_path(LGAME_LOC, lp, sizeof lp);
    if (u8_fopen_s(&f, lp, "r") != 0 || !f) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        int k;
        for (k = 0; k < 3; k++) {
            size_t kl = strlen(kK[k]);
            const char *v = line + kl;
            if (strncmp(line, kK[k], kl) == 0 && (v[0] == '0' || v[0] == '1') &&
                (v[1] == 0 || v[1] == '\r' || v[1] == '\n' || v[1] == ' ')) {
                *kv[k] = v[0] == '1';
            }
        }
    }
    fclose(f);
}

/* Schalter umlegen, sofort in user.cfg merken (wirkt beim naechsten Start). Ohne Rollback sind die
 * abhaengigen Schalter grau; ein Klick (z. B. per Test) aendert dann nichts. W473: Chat im Spiel -> user_loc.cfg,
 * grau ohne Chat/steelnet-Chat bzw. im LAN (steelnet) und ohne Team-Reiter (Team). */
void lnet_toggle(int id)
{
    const char *key, *name;
    int bit, *v = lnet_mvar(id, &key, &name, &bit);
    char t[300];
    if (v) {
        const lnet_row_t *r = lnet_row_of(id);
        if (r && lnet_row_grey(r)) {
            onl_sync_ui();
            return;
        }
        *v = !*v;
        onl_sync_ui();
        if (lnet_mission_save(id) >= 0) {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT(*v ? "gui.lnet_set_on" : "gui.lnet_set_off"), LT(name));
            set_status_k(t, 1);
        }
        lnet_redraw();
        return;
    }
    v = lnet_var(id, &key, &name);
    if (id != ID_LNET_RB && id != ID_LNET_CLIP && id != ID_LNET_CHAT && id != ID_LNET_SNCHAT &&
        !g_loc_rb_on) { /* W420/W437/W446: auch im Lockstep */
        onl_sync_ui();
        return;
    }
    *v = !*v;
    onl_sync_ui();
    if (onl_save_key(key, *v) >= 0) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT(*v ? "gui.lnet_set_on" : "gui.lnet_set_off"), LT(name));
        set_status_k(t, 1);
    }
    lnet_redraw();
}

/* Alles auf Standard (an, Gegner glaetten Auto) und merken. W473: Chat im Spiel = steelnet aus, Lobby/Team an. */
void lnet_reset(void)
{
    int bad = 0;
    g_loc_rb_on = g_loc_own_pred = g_loc_lowd = 1;
    g_loc_net_auto = 1; /* W447 */
    g_loc_smooth_opp = 1;
    g_loc_clip = 1; /* W420 */
    g_loc_chat = 1; /* W437 */
    g_loc_snchat = 1; /* W446 */
    g_loc_arcade = 0; /* W440: Sim */
    g_loc_msn = 0; /* W473 */
    g_loc_mlobby = g_loc_mteam = 1;
    onl_sync_ui();
    bad |= onl_save_key(LNET_CLIP_KEY, 1) < 0;
    bad |= onl_save_key(LNET_CHAT_KEY, 1) < 0;
    bad |= onl_save_key(LNET_SNCHAT_KEY, 1) < 0;
    bad |= onl_save_key(LNET_MODE_KEY, 0) < 0;
    bad |= onl_save_key(LNET_RB_KEY, 1) < 0;
    bad |= onl_save_key(LNET_AUTO_KEY, 1) < 0; /* W447 */
    bad |= onl_save_key(LNET_OWN_KEY, 1) < 0;
    bad |= onl_save_key(LNET_LOWD_KEY, 1) < 0;
    bad |= onl_save_key(ONL_SMOOTH_KEY, 1) < 0;
    bad |= lnet_mission_save(ID_LNET_MSN) < 0; /* W473 */
    bad |= lnet_mission_save(ID_LNET_MLOBBY) < 0;
    bad |= lnet_mission_save(ID_LNET_MTEAM) < 0;
    if (!bad) {
        set_status_k(LT("gui.lnet_reset_done"), 1);
    }
    lnet_redraw();
}

/* W473: Reiter der Netzwerk-Karte waehlen (Sichtbarkeit je Reiter, Segment gewaehlt = Akzent). */
void lnet_tab_pick(int tab)
{
    g_lnet_tab = tab ? 1 : 0;
    ui_set_checked(ID_LNET_TAB0, g_lnet_tab == 0);
    ui_set_checked(ID_LNET_TAB1, g_lnet_tab == 1);
    if (g_game == LGAME_LOC && g_page == P_LNET) {
        ui_refresh_visibility();
    }
    lnet_redraw();
}

static void mp_paint_match(void); /* W397 */
void upd_open_page(void); /* W420 */
int acc_paint_card(void);
void acc_paint_view(void);

void paint_lnet(void)
{
    int x = CX0, y = LNET_Y, w = CW, i;
    if (acc_view_active()) { /* W397: Account verwalten = Unterseite von Multiplayer */
        acc_paint_view();
        return;
    }
    mp_paint_match();
    if (!acc_paint_card()) { /* steelnet in dieser Ausgabe nicht eingerichtet */
        card(ACC_X, LOC_ONL_Y, COLW, LOC_ONL_H, LT("gui.acc_title"));
        label(F_TINY, LT("gui.loc_onl_nosrv"), ACC_X + 18, LOC_ONL_Y + 40, COLW - 36, 50, C_DIM);
    }
    card(x, y, w, LNET_H, ""); /* W473: Titel = Reiter (Segmente im Kopf) */
    label1(F_TINY, LT("gui.lnet_intro"), x + 18, y + 46, w - 36, 18, C_DIM);
    for (i = 0; i < LNET_NROWS; i++) { /* W473: Beschriftung links der Auswahl, je Zeile ein kurzer Hinweis rechts */
        const lnet_row_t *r = &kLnetRows[i];
        int ry = lnet_row_y(r->tab, r->row), grey = lnet_row_grey(r), ind = r->dep ? LNET_IND : 0;
        COLORREF c = grey ? C_FAINT : C_DIM;
        if (r->tab != g_lnet_tab) {
            continue;
        }
        if (r->id == ID_LNET_RB && !g_loc_rb_on) {
            c = C_WARN; /* W392: Lockstep sichtbar machen */
        }
        if (r->lbl) {
            char t[120];
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s:", LT(r->lbl));
            label1(F_SMALL, t, x + 18 + ind, ry, LNET_LBL_W - ind - 8, 28, grey ? C_FAINT : C_TEXT);
        }
        if (ui_text_fits(F_TINY, lnet_row_hint(r), LNET_HINT_X, ry, LNET_HINT_W, 28, 1)) { /* eine Zeile: mittig */
            L_text(F_TINY, lnet_row_hint(r), LNET_HINT_X, ry, LNET_HINT_W, 28, c,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        } else {
            label(F_TINY, lnet_row_hint(r), LNET_HINT_X, ry - 1, LNET_HINT_W, LNET_HINT_H, c);
        }
    }
}

/* W473: Passt ein Text in ein logisches Rechteck (Pixel wie beim Zeichnen, Layout a/b)? single = eine Zeile
 * (Breite), sonst mit Umbruch (Hoehe). need/have (optional) = benoetigt/verfuegbar in Pixeln. */
int ui_text_need(int font, const char *u8, int x, int y, int w, int h, int single, int *need, int *have)
{
    RECT q, m;
    wchar_t *wt;
    HDC dc;
    int nd, hv;
    if (lay_b()) {
        lay_paint_px(x, y, w, h, &q);
    } else {
        SetRect(&q, SX(x), SX(y), SX(x + w), SX(y + h));
    }
    wt = u8_wdup(u8 ? u8 : "");
    dc = GetDC(NULL);
    SetRect(&m, 0, 0, q.right - q.left, 0);
    SelectObject(dc, g_f[font]);
    DrawTextW(dc, wt ? wt : L"", -1, &m, DT_CALCRECT | DT_NOPREFIX | (single ? DT_SINGLELINE : DT_WORDBREAK));
    ReleaseDC(NULL, dc);
    free(wt);
    nd = single ? m.right : (m.right > q.right - q.left ? 1 << 20 : m.bottom); /* ueberlanges Wort = passt nicht */
    hv = single ? q.right - q.left : q.bottom - q.top;
    if (need) *need = nd;
    if (have) *have = hv;
    return nd <= hv;
}
int ui_text_fits(int font, const char *u8, int x, int y, int w, int h, int single)
{
    return ui_text_need(font, u8, x, y, w, h, single, NULL, NULL);
}

/* W473: Selbsttest - Lage des Hinweises (logisch) und Text je Zeile; -1 = Zeile im anderen Reiter */
int lnet_hint_rect(int i, RECT *rc, const char **txt, int *id)
{
    const lnet_row_t *r;
    if (i < 0 || i >= LNET_NROWS) return 0;
    r = &kLnetRows[i];
    *id = r->id;
    if (r->tab != g_lnet_tab) return -1;
    rc->left = LNET_HINT_X;
    rc->top = lnet_row_y(r->tab, r->row) - 1;
    rc->right = rc->left + LNET_HINT_W;
    rc->bottom = rc->top + LNET_HINT_H;
    *txt = lnet_row_hint(r);
    return 1;
}
int lnet_row_count(void) { return LNET_NROWS; }
/* W473: Selbsttest - Beschriftung links (logisch) je Auswahlzeile; 0 = keine */
int lnet_label_rect(int i, RECT *rc, char *txt, size_t cap)
{
    const lnet_row_t *r;
    int ind;
    if (i < 0 || i >= LNET_NROWS || !kLnetRows[i].lbl || kLnetRows[i].tab != g_lnet_tab) return 0;
    r = &kLnetRows[i];
    ind = r->dep ? LNET_IND : 0;
    rc->left = CX0 + 18 + ind;
    rc->top = lnet_row_y(r->tab, r->row);
    rc->right = rc->left + LNET_LBL_W - ind - 8;
    rc->bottom = rc->top + 28;
    _snprintf_s(txt, cap, _TRUNCATE, "%s:", LT(r->lbl));
    return 1;
}
/* W450 LAUNCHER-PRIVACY-INFO: kleines Fenster (Dialogstil wie Replays) mit dem Datenschutz-Hinweis zu Chat und
 * steelnet. Inhalt = Faktenpruefung in BERICHT_M2 "W450". mode 0 = normal, 1 = Bild (aussen, ohne Aktivierung),
 * 2 = Selbsttest (aussen, schliesst sich ueber den Schliessen-Knopf). Rueckgabe 1 = Fenster war da, Texte vorhanden. */
#define IDPV_TEXT 720
#define IDPV_CLOSE 721
int g_pv_done, g_pv_mode;

static LRESULT CALLBACK pv_proc(HWND hd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDPV_CLOSE || LOWORD(wp) == IDCANCEL) {
            g_pv_done = 1;
        }
        return 0;
    case WM_DRAWITEM:
        draw_item((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLORSTATIC:
        return ctlcolor((HDC)wp, (HWND)lp, 1);
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hd, &r);
        if (!g_br_dialog) {
            g_br_dialog = CreateSolidBrush(C_DIALOG);
        }
        FillRect((HDC)wp, &r, g_br_dialog);
        return 1;
    }
    case WM_TIMER:
        if (wp == 9 && g_pv_mode != 0) {
            KillTimer(hd, 9);
            if (g_pv_mode == 1) {
                char path[1200];
                int rc;
                RedrawWindow(hd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                _snprintf_s(path, sizeof path, _TRUNCATE, "%s/w450_privacy_%s_%d.png", g_o.out_dir, sb_lang_code(),
                            g_dpi * 100 / 96);
                rc = shot_window(hd, path, 1);
                printf("privacy-shot: %s%s\n", rc == 0 ? "" : "FEHLER ", path);
                g_pv_done = 1;
            } else {
                SendMessageW(hd, WM_COMMAND, MAKEWPARAM(IDPV_CLOSE, BN_CLICKED), 0); /* Schliessen-Knopf */
            }
        }
        return 0;
    case WM_CLOSE:
        g_pv_done = 1;
        return 0;
    }
    return DefWindowProcW(hd, msg, wp, lp);
}

int privacy_texts(char *out, int cap)
{
    static const char *const kK[5] = { "gui.lnet_priv_1", "gui.lnet_priv_2", "gui.lnet_priv_3", "gui.lnet_priv_4",
                                       "gui.lnet_priv_5" };
    int i, l = 0, ok = 1;
    out[0] = 0;
    for (i = 0; i < 5; i++) {
        const char *t = LT(kK[i]);
        if (!t || !t[0] || strncmp(t, "gui.", 4) == 0) {
            ok = 0; /* Schluessel fehlt */
            continue;
        }
        l += _snprintf_s(out + l, (size_t)(cap - l), _TRUNCATE, "%s%s", i ? "\r\n\r\n" : "", t);
    }
    return ok;
}

int privacy_dialog_modal(int mode)
{
    static int reg;
    HWND hd;
    RECT rc = { 0, 0, 0, 0 };
    MSG msg;
    int ui_mark = g_ui_n, ok;
    DWORD exs = mode ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) : 0;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    char txt[2400];
    wchar_t title[96];
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = pv_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"SbPrivacy";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        reg = 1;
    }
    ok = privacy_texts(txt, (int)sizeof txt);
    g_pv_done = 0;
    g_pv_mode = mode;
    rc.right = SX(540);
    rc.bottom = SX(312);
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, exs);
    if (mode) {
        offscreen_pos(&x, &y);
    } else if (g_hwnd) {
        RECT pr;
        GetWindowRect(g_hwnd, &pr);
        x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
        y = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;
    }
    u8_to_w(LT("gui.lnet_priv_title"), title, 96);
    hd = CreateWindowExW(exs, L"SbPrivacy", title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y,
                         rc.right - rc.left, rc.bottom - rc.top, g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!hd) {
        return 0;
    }
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    }
    mk(hd, K_STATIC, IDPV_TEXT, PG_DIALOG, 20, 16, 500, 240, txt, WS_VISIBLE);
    mk(hd, K_ACCENT, IDPV_CLOSE, PG_DIALOG, 400, 268, 120, 30, LT("gui.btn_close"), WS_VISIBLE);
    if (g_hwnd) {
        EnableWindow(g_hwnd, FALSE);
    }
    if (mode) {
        if (!is_offscreen(hd)) {
            printf("privacy-shot: FEHLER Fenster nicht au" U8_SZ "erhalb des Bildschirms - Abbruch\n");
            g_pv_done = 1;
            ok = 0;
        } else {
            ShowWindow(hd, SW_SHOWNOACTIVATE);
            SetTimer(hd, 9, mode == 1 ? 400 : 30, NULL);
        }
    } else {
        ShowWindow(hd, SW_SHOW);
        SetFocus(bctl(hd, IDPV_CLOSE));
    }
    UpdateWindow(hd);
    while (!g_pv_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hwnd) {
        EnableWindow(g_hwnd, TRUE);
    }
    DestroyWindow(hd);
    g_ui_n = ui_mark;
    return ok;
}

static void lnet_build(HWND w)
{
    int x = CX0 + 18, xr = CX0 + CW - 18, i;
    /* W473: Kopf = zwei Reiter (Segmente) links, rechts Standard wiederherstellen und je Reiter ein Knopf
     * (Network: Release-Seite nach Versionsablehnung; Lobby & chat: Chat privacy) */
    mk(w, K_SEG, ID_LNET_TAB0, P_LNET, x, LNET_Y + 10, LNET_TAB_W0, 28, LT("gui.lnet_tab_net"), 0);
    mk(w, K_SEG, ID_LNET_TAB1, P_LNET, x + LNET_TAB_W0 + 4, LNET_Y + 10, LNET_TAB_W1, 28, LT("gui.lnet_tab_lobby"), 0);
    mk(w, K_BTN, ID_LNET_RESET, P_LNET, xr - LNET_BTN_W, LNET_Y + 10, LNET_BTN_W, 28, LT("gui.lnet_reset"), 0); /* W397 */
    mk(w, K_BTN, ID_LNET_PRIV, P_LNET, xr - LNET_BTN_W - 12 - 150, LNET_Y + 10, 150, 28, LT("gui.lnet_priv_btn"), 0); /* W450 */
    mk(w, K_BTN, ID_LNET_VERPAGE, P_LNET, xr - LNET_BTN_W - 12 - 170, LNET_Y + 10, 170, 28, LT("gui.lnet_verpage"), 0);
    for (i = 0; i < LNET_NROWS; i++) { /* Zeilen aus kLnetRows (Auswahl rechts der Beschriftung, Schalter mit Text) */
        const lnet_row_t *r = &kLnetRows[i];
        int ry = lnet_row_y(r->tab, r->row), ind = r->dep ? LNET_IND : 0;
        if (r->lbl) {
            mk(w, K_COMBO, r->id, P_LNET, x + LNET_LBL_W, ry, LNET_COMBO_W, 28, "", 0);
        } else {
            static const struct {
                int id;
                const char *key;
            } kT[] = { { ID_LNET_OWN, "gui.lnet_own" },       { ID_LNET_LOWD, "gui.lnet_lowd" },
                       { ID_LNET_CLIP, "gui.lnet_clip" },     { ID_LNET_CHAT, "gui.lnet_chat" },
                       { ID_LNET_SNCHAT, "gui.lnet_snchat" }, { ID_LNET_MSN, "gui.lnet_msn" },
                       { ID_LNET_MLOBBY, "gui.lnet_mlobby" }, { ID_LNET_MTEAM, "gui.lnet_mteam" } };
            int k;
            for (k = 0; k < (int)(sizeof kT / sizeof kT[0]); k++) {
                if (kT[k].id == r->id) {
                    mk(w, K_TOGGLE, r->id, P_LNET, x + ind, ry, LNET_CTL_END - x - ind, 28, LT(kT[k].key), 0);
                }
            }
        }
    }
    lnet_netmode_fill(); /* W447 */
    lnet_mode_fill();    /* W440 */
    ui_set_checked(ID_LNET_TAB0, g_lnet_tab == 0);
    ui_set_checked(ID_LNET_TAB1, g_lnet_tab == 1);
}

static void lnet_relabel(void)
{
    static const struct {
        int id;
        const char *key;
    } kL[] = { { ID_LNET_OWN, "gui.lnet_own" },         { ID_LNET_LOWD, "gui.lnet_lowd" },
               { ID_LNET_RESET, "gui.lnet_reset" },     { ID_LNET_CLIP, "gui.lnet_clip" },
               { ID_LNET_CHAT, "gui.lnet_chat" },       { ID_LNET_SNCHAT, "gui.lnet_snchat" },
               { ID_LNET_VERPAGE, "gui.lnet_verpage" }, { ID_LNET_PRIV, "gui.lnet_priv_btn" },
               { ID_LNET_TAB0, "gui.lnet_tab_net" },    { ID_LNET_TAB1, "gui.lnet_tab_lobby" },
               { ID_LNET_MSN, "gui.lnet_msn" },         { ID_LNET_MLOBBY, "gui.lnet_mlobby" },
               { ID_LNET_MTEAM, "gui.lnet_mteam" } };
    int i;
    lnet_netmode_fill(); /* W447 */
    for (i = 0; i < (int)(sizeof kL / sizeof kL[0]); i++) {
        if (ctl(kL[i].id)) ui_settext(ctl(kL[i].id), LT(kL[i].key));
    }
    lnet_mode_fill(); /* W440 */
}
/* Timer (2 s) und Seitenwechsel: Status des laufenden/letzten Spiels. */
void onl_tick(void)
{
    if (g_game != LGAME_LOC || (g_page != P_PLAY && g_page != P_LNET) || !g_hwnd || IsIconic(g_hwnd)) {
        return; /* W397: auch auf Multiplayer (Raumcode kopieren) */
    }
    if (onl_status_refresh()) {
        ui_refresh_visibility();
        onl_redraw();
    }
}

/* W397: LoC > Spielen rechts: Erstabfrage oder kurze Karte "Multiplayer" (Stufe, Lobby, Raumcode/Meldung). */
void mp_card_acc_line(char *t, size_t cap); /* W403 */
void onl_paint(void)
{
    char t[400];
    COLORREF col = C_DIM;
    int x = LOC_NET_X, y = LOC_NET_Y, w = LOC_NET_W;
    if (g_upd_ask) {
        netq_paint(x, y, w);
        return;
    }
    card(x, y, w, LOC_NET_H, LT("gui.page_mp"));
    if (g_loc_lan) { /* W454: LAN-Modus */
        label1(F_SMALL, LT("gui.mp_sum_lan"), x + 18, y + 38, w - 36, 22, C_TEXT);
        lan_info(t, sizeof t, &col);
        label(F_TINY, t, x + 18, y + 62, w - 36, 32, col);
    } else if (netm_get() == NETM_STEEL) {
        label1(F_SMALL, LT(g_loc_public ? "gui.mp_sum_pub" : "gui.mp_sum_priv"), x + 18, y + 38, w - 36, 22, C_TEXT);
        onl_info(t, sizeof t, &col);
        if (col != C_DIM) { /* Raumcode der eigenen Lobby, Fehler, Vertrauenshinweis (Eingabehinweise: Multiplayer) */
            label(F_TINY, t, x + 18, y + 62, w - 36, 32, col);
        }
    } else { /* W403: ohne steelnet nur der Konto-Stand (Grund + Sprung stehen unter dem Startknopf) */
        mp_card_acc_line(t, sizeof t);
        label1(F_SMALL, t, x + 18, y + 38, w - 36, 22, C_DIM);
        label(F_TINY, LT("gui.mp_card_off"), x + 18, y + 62, w - 36, 32, C_FAINT);
    }
}

/* W397: LoC > Multiplayer, Karte Match (Sichtbarkeit, Raumcode, Meldung; ohne steelnet Hinweis + Sprung). */
static void mp_paint_match(void)
{
    char t[400];
    COLORREF col = C_DIM;
    int x = CX0, y = MP_MATCH_Y, w = COLW, on = onl_srv_on() && g_loc_online;
    card(x, y, w, MP_MATCH_H, LT("gui.mp_match"));
    /* W473: Online/LAN als eigene Zeile "Verbindung" (vorher im Kopf, Wert abgeschnitten); Beschriftung links */
    label1(F_SMALL, LT("gui.mp_conn"), x + 18, MP_ROW(0), MP_LBL_W - 6, 28, C_DIM);
    if (g_loc_lan) { /* W454: LAN - Raum, Passwort, Hinweis (steelnet-Funktionen entfallen) */
        label1(F_SMALL, LT("gui.lan_room"), x + 18, MP_ROW(1), MP_LBL_W - 6, 28, C_DIM);
        label1(F_SMALL, LT("gui.lan_pass"), x + 18, MP_ROW(2), MP_LBL_W - 6, 28, C_DIM);
        lan_info(t, sizeof t, &col);
        label(F_TINY, t, x + 18, MP_INFO_Y, w - 36, MP_INFO_H, col);
        return;
    }
    label1(F_SMALL, LT("gui.mp_lobby"), x + 18, MP_ROW(1), MP_LBL_W - 6, 28, on ? C_DIM : C_FAINT);
    label1(F_SMALL, LT("gui.loc_code"), x + 18, MP_ROW(2), MP_LBL_W - 6, 28, on ? C_DIM : C_FAINT);
    if (!on) {
        label(F_TINY, LT(onl_srv_on() ? "gui.mp_off" : "gui.loc_onl_nosrv"), x + 18, MP_INFO_Y, w - 36, MP_INFO_H,
              C_WARN);
        return;
    }
    onl_info(t, sizeof t, &col);
    label(F_TINY, t, x + 18, MP_INFO_Y, w - 36, MP_INFO_H, col);
}

/* W473: Selbsttest - Beschriftungen der Karte Match passen einzeilig (Verbindung, Lobby, Raumcode, Raum, Passwort).
 * Rueckgabe NULL = alle passen, sonst der zu breite Text. */
const char *mp_labels_fit(void)
{
    static const char *const kK[5] = { "gui.mp_conn", "gui.mp_lobby", "gui.loc_code", "gui.lan_room", "gui.lan_pass" };
    int i;
    for (i = 0; i < 5; i++) {
        if (!ui_text_fits(F_SMALL, LT(kK[i]), CX0 + 18, MP_ROW(0), MP_LBL_W - 6, 28, 1)) {
            return LT(kK[i]);
        }
    }
    { /* Hinweiszeile unter den Feldern (LAN-Hinweise mit/ohne Passwort, Status, Fehler; online aus) - auch auf der
       * Karte Multiplayer unter Spielen (gleiche Texte, gleiche Breite, 2 Zeilen) */
        static const char *const kI[7] = { "gui.lan_hint", "gui.lan_hint_pass", "gui.lan_status", "gui.lan_ver_bad",
                                           "gui.lan_err", "gui.mp_off", "gui.loc_onl_nosrv" };
        static char t[400];
        for (i = 0; i < 7; i++) {
            if (i == 2) {
                _snprintf_s(t, sizeof t, _TRUNCATE, LT(kI[i]), 12, 12); /* %d gefunden / %d verbunden */
            } else if (i == 3) {
                _snprintf_s(t, sizeof t, _TRUNCATE, LT(kI[i]), "0.4.12"); /* %s = Versionsnummer */
            } else {
                strcpy_s(t, sizeof t, LT(kI[i]));
            }
            if (!ui_text_fits(F_TINY, t, CX0 + 18, MP_INFO_Y, COLW - 36, MP_INFO_H, 0) ||
                !ui_text_fits(F_TINY, t, LOC_NET_X + 18, LOC_NET_Y + 62, LOC_NET_W - 36, 32, 0)) {
                return t;
            }
        }
    }
    return NULL;
}

/* W373: Eintraege der Sichtbarkeits-Auswahl (Sprache), Auswahl nach g_loc_public. */
static void onl_vis_fill(void)
{
    HWND c = ctl(ID_LOC_VIS);
    if (!c) {
        return;
    }
    g_updating++;
    ComboBox_ResetContent(c);
    combo_add(c, LT("gui.loc_vis_pub"));
    combo_add(c, LT("gui.loc_vis_priv"));
    ComboBox_SetCurSel(c, g_loc_public ? 0 : 1);
    g_updating--;
}

void onl_build(HWND w)
{
    int x = CX0, y = MP_MATCH_Y, cw = COLW;
    /* W397: Schalter "Online-Spiel" entfaellt (Stufe steelnet unter dem Zahnrad); Match auf Multiplayer */
    int fx = x + 18 + MP_LBL_W, fw = cw - 18 - 18 - MP_LBL_W; /* W473: Felder rechts der Beschriftung */
    (void)y;
    /* W454: Auswahl Online/LAN (W473: eigene Zeile "Verbindung", volle Feldbreite), LAN-Felder in den Zeilen */
    mk(w, K_COMBO, ID_LNET_KIND, P_LNET, fx, MP_ROW(0), fw, 28, "", 0);
    mk(w, K_COMBO, ID_LOC_VIS, P_LNET, fx, MP_ROW(1), fw, 28, "", 0); /* W373 */
    mk(w, K_EDIT, ID_LOC_CODE, P_LNET, fx, MP_ROW(2), fw - 84 - 8, 28, "", ES_UPPERCASE);
    mk(w, K_BTN, ID_LOC_CODE_COPY, P_LNET, x + cw - 18 - 84, MP_ROW(2) + 1, 84, 26, LT("gui.loc_code_copy"), 0);
    mk(w, K_BTN, ID_MP_GOSET, P_LNET, x + cw - 18 - 150, y + 9, 150, 26, LT("gui.mp_goset"), 0);
    mk(w, K_EDIT, ID_LNET_LANROOM, P_LNET, fx, MP_ROW(1), fw, 28, "", 0);
    mk(w, K_EDIT, ID_LNET_LANPASS, P_LNET, fx, MP_ROW(2), fw, 28, "", 0);
    mk(w, K_BTN, ID_LOC_MP_GO, P_PLAY, LOC_NET_X + LOC_NET_W - 18 - 150, LOC_NET_Y + LOC_NET_H - 40, 150, 28,
       LT("gui.mp_go"), 0);
    /* W387: Gegner glaetten lag unter dem Startknopf; W392: jetzt auf der Seite Netzwerk */
    lnet_build(w);
    onl_vis_fill();
    loc_smooth_fill(); /* W387 */
    if (ctl(ID_LOC_CODE)) {
        SendMessageW(ctl(ID_LOC_CODE), EM_LIMITTEXT, 12, 0);
        SendMessageW(ctl(ID_LOC_CODE), EM_SETCUEBANNER, TRUE, (LPARAM)L"XXXX-XXXX");
    }
    lan_build_init(); /* W454 */
    onl_sync_ui();
}

void onl_relabel(void)
{
    lan_kind_fill(); /* W454 */
    lan_cues();
    onl_vis_fill(); /* W373 */
    loc_smooth_fill(); /* W387 */
    lnet_relabel();    /* W392 */
    if (ctl(ID_LOC_CODE_COPY)) ui_settext(ctl(ID_LOC_CODE_COPY), LT("gui.loc_code_copy"));
}

/* W326 LOC-WIDE: Auswahl "Bildformat" (LoC > Erweitert). Eintraege = die
 * angebotenen Modi (lgame_wide_count; Index = Wert loc_widescreen). Die Wahl
 * wird sofort in user.cfg gemerkt (wie Abbild/Spielwahl) und wirkt beim
 * naechsten Spielstart; 16:9/720p sind experimentell (Kolben an der Karte,
 * Warnzeile darunter). */
void loctab_wide_fill(void)
{
    HWND c = ctl(ID_LOC_WIDE);
    int i, n = lgame_wide_count();
    if (!c) {
        return;
    }
    g_updating++;
    ComboBox_ResetContent(c);
    for (i = 0; i < n; i++) {
        combo_add(c, LT(lgame_wide_label_key(i)));
    }
    ComboBox_SetCurSel(c, lgame_wide_clamp(g_loc.loc_widescreen));
    g_updating--;
}

void loctab_wide_sync(void)
{
    HWND c = ctl(ID_LOC_WIDE);
    g_loc.loc_widescreen = lgame_wide_clamp(g_loc.loc_widescreen);
    if (c && ComboBox_GetCurSel(c) != g_loc.loc_widescreen) {
        g_updating++;
        ComboBox_SetCurSel(c, g_loc.loc_widescreen);
        g_updating--;
    }
}

int loctab_wide_val(void)
{
    return lgame_wide_clamp(g_loc.loc_widescreen);
}

int pergame_box_169(void) /* W402: Bildformat des aktiven Spiels ist 16:9 (nur LoC) */
{
    return g_game == LGAME_LOC && loctab_wide_val() != LGAME_WIDE_43;
}

void loctab_wide_pick(void)
{
    int v = lgame_wide_clamp(ComboBox_GetCurSel(ctl(ID_LOC_WIDE)));
    if (v == g_loc.loc_widescreen) {
        return;
    }
    g_loc.loc_widescreen = v;
    if (loctab_save() == 0) {
        set_status_k(LT("gui.loc_wide_saved"), 1);
    }
    update_aspect_info(); /* W402: Vorschau 16:9/4:3 auf Anzeige */
    ui_apply_rules(); /* W326: Experimental-Hinweis (Breitbild) im LoC-Tab */
    redraw_rect(COL2, LOC_WIDE_Y, COLW, LOC_WIDE_H);
}

/* W459 LOC-FX-LIGHT: loc_sblook in user_loc.cfg (an = Experimental-Hinweis, Gruppe "Licht und Shader", siehe
 * ui_apply_rules in settings.c). W465 LOC-ENHANCED: der Schalter ist aus der Oberflaeche genommen (das Preset
 * Enhanced ersetzt ihn); gelesen wird der Wert weiter, die Umstellung auf Enhanced setzt ihn auf 0. */
void loctab_sblook_sync(void)
{
    g_loc.loc_sblook = g_loc.loc_sblook ? 1 : 0;
    if (ctl(ID_LOC_SBLOOK)) {
        ui_set_checked(ID_LOC_SBLOOK, g_loc.loc_sblook);
    }
}

/* W459: "Back to original" im LoC-Tab schaltet SB look (Experimental) aus. 1 = geaendert. */
int loctab_sblook_reset(void)
{
    if (!g_loc.loc_sblook) { return 0; }
    g_loc.loc_sblook = 0;
    loctab_sblook_sync();
    loctab_save();
    return 1;
}

/* LoC-Datenordner: <Datenordner>\loc (Profil/portabel), legacy <Client>\loc. */
void loctab_data_dir(char *out, size_t cap)
{
    char acp[1024], base[1100];
    if (sb_userdata_path_peek(SB_UD_ROOT, acp, sizeof acp) == 1) {
        u8_from_acp(acp, base, sizeof base);
    } else {
        strcpy_s(base, sizeof base, g_client_dir);
    }
    lgame_loc_dir(base, out, cap);
}

/* W293: Erweitert-Elemente, die auch fuer LoC gelten (gemeinsame user.cfg-
 * bzw. Launcher-Einstellungen). SB-Teile (Spieldateien, Spielstaende,
 * Gespeicherte Setups, Diagnose mit Spielstand-Schutz) bleiben bei LoC weg. */
static int loctab_adv_shared(int id)
{
    return id == ID_VSYNC || id == ID_FPS || id == ID_PACING; /* W468: Bildtakt; W397: Hintergrund/Versionspruefung jetzt unter dem Zahnrad */
}

/* ------------------------------------------------------------------ */
/* W400 LAUNCHER-LOC-ONLINE-GATE: LoC ist ein reines Mehrspieler-Spiel.  */
/* Ohne Stufe "Mit steelnet verbinden" (Zahnrad > Online & Updates, auch */
/* aus der Erstabfrage) ist der LoC-Start gesperrt: Knopf grau (wie ohne */
/* startbaren Client), Grund + Sprung zum Zahnrad unter dem Knopf, Zeile */
/* Online warnt, jeder Startweg (Knopf, Launch-Leiste, Enter) prueft in  */
/* loctab_start. Testpartner (Loopback, eigener Startweg) und SB frei.   */
/* ------------------------------------------------------------------ */
int g_locgate_skip; /* nur Selbsttest: aeltere Startweg-Faelle pruefen die Kette ohne steelnet */
int g_in_gui_selftest; /* W403: Selbsttest laeuft (nie das echte Konto lesen) */

int loc_net_ok(void)
{
    return g_locgate_skip || netm_get() == NETM_STEEL || g_loc_lan; /* W454: LAN ohne steelnet */
}

/* Sperrgrund steelnet sichtbar? (nur mit startbarem Client, sonst gilt "Demnaechst") */
int loc_gate_shown(void)
{
    return lgame_loc_client_ready() && !loc_net_ok();
}

/* W403 LAUNCHER-R1-POLISH: ohne (gepruefte) LoC-Disc ist der Start gesperrt wie ohne steelnet: Knopf grau,
 * Grund unter dem Knopf, Sprung "Disc-Image waehlen" (LoC > Erweitert). Gleiche Bedingung wie loctab_start
 * (lgame_loc_start_gate mit dem Pruefergebnis des gemerkten Abbilds). m: Grund (ohne Pfad "kein Abbild",
 * sonst die Meldung der Startsperre). Nur mit startbarem Client (sonst gilt "Demnaechst"). */
int loc_img_gate(char *m, size_t cap)
{
    char t[600];
    const lgame_img_t *r = (g_loc_checked && strcmp(g_loc_res_path, g_loc.loc_image) == 0) ? &g_loc_res : NULL;
    if (m && cap) {
        m[0] = '\0';
    }
    if (!lgame_loc_client_ready() || lgame_loc_start_gate(r, t, sizeof t) == 0) {
        return 0;
    }
    if (m && cap) {
        strcpy_s(m, cap, g_loc.loc_image[0] ? t : LT("gui.loc_sum_none"));
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* W421 LOC-VERSION-GATE (Nutzerentscheid 22.11.): online nur mit der    */
/* neuesten Version - ist dieser Launcher laut steelnet aelter als       */
/* min_client_version (GET /v1/info, Kopf-Chip-Pruefung snp_*), ist der  */
/* LoC-Start gesperrt (Knopf, Fussleiste, Startmenue, Enter), mit Grund  */
/* und Knopf zur Release-Seite (kein Download, kein Selbst-Update). Nur  */
/* Stufe steelnet; Server nicht erreichbar, Feld fehlt/leer/kaputt ->    */
/* frei wie bisher. Ergebnis nur im Speicher (Launcher-Sitzung), jede    */
/* Pruefung ersetzt es. SB nie betroffen. Eigene Version wie der Client: */
/* poc-native/VERSION (SB_NET_VERSION), Vergleich sb_netver_* (W420).    */
/* ------------------------------------------------------------------ */
char g_locver_min[16];        /* min_client_version der letzten Pruefung ("" = keine Mindestversion) */
const char *g_locver_own_fix; /* Shots/Selbsttest: eigene Version (sonst SB_NET_VERSION) */
int g_ver_open_n;             /* Shots/Selbsttest: Release-Seite "geoeffnet" (nur gezaehlt) */

const char *locver_own(void)
{
    return g_locver_own_fix ? g_locver_own_fix : SB_NET_VERSION;
}

int loc_ver_gate(void)
{
    /* W454: im LAN prueft nur der Client die Version gegen die Mitspieler (kein steelnet) */
    return !g_loc_lan && netm_get() == NETM_STEEL && sb_netver_outdated(locver_own(), g_locver_min);
}

/* Shots/Selbsttest: steelnet-Pruefung erreichbar mit dieser Mindestversion (Attrappe, nie Netz); eigene Version
 * fest 0.3.2 (Beleg unabhaengig von poc-native/VERSION). */
void locver_fixture(const char *min_ver)
{
    lacc_probe_t pr;
    memset(&pr, 0, sizeof pr);
    pr.ok = 1;
    pr.ms = 38;
    strcpy_s(pr.version, sizeof pr.version, "2.4");
    strcpy_s(pr.min_ver, sizeof pr.min_ver, min_ver);
    g_locver_own_fix = "0.3.2";
    snp_apply(&pr);
}

/* Launch (Fussleiste der Unterseiten) im Bereich LoC: frei ohne startbaren Client ("Coming soon"),
 * sonst nur mit gepruefter Disc und steelnet - dieselbe Bedingung wie Play Line of Contact.
 * W421: und nicht veraltet laut steelnet. */
int loc_launch_ok(void)
{
    return !lgame_loc_client_ready() || (loc_net_ok() && !loc_img_gate(NULL, 0) && !loc_ver_gate());
}

/* Sperrgrund LoC (Spielen-Seite und Fussleiste): W421 Version zuerst (gilt auch mit Disc), dann Disc, dann
 * steelnet kurz. */
void loc_gate_msg(char *m, size_t cap)
{
    if (loc_ver_gate()) {
        _snprintf_s(m, cap, _TRUNCATE, LT("gui.loc_ver_gate"), g_locver_min);
        return;
    }
    if (loc_img_gate(m, cap) && loc_gate_shown()) {
        size_t k = strlen(m);
        _snprintf_s(m + k, cap - k, _TRUNCATE, "%s %s", k && m[k - 1] != '.' ? "." : "",
                    LT("gui.loc_gate_short"));
    } else if (!m[0]) {
        strcpy_s(m, cap, LT("gui.loc_gate_short"));
    }
}

void loc_gate_sync(void)
{
    static int busy; /* W403: EnableWindow kann Fokus-/Zeichenwege ausloesen -> kein Wiedereintritt */
    if (ctl(ID_LOC_START) && !busy) {
        static const int kFoot[] = { ID_START, ID_START_MENU };
        int i;
        busy = 1;
        BOOL en = lgame_loc_client_ready() && loc_net_ok() && !loc_img_gate(NULL, 0) && !loc_ver_gate() ? TRUE : FALSE;
        if ((IsWindowEnabled(ctl(ID_LOC_START)) != FALSE) != (en != FALSE)) {
            EnableWindow(ctl(ID_LOC_START), en);
        }
        for (i = 0; g_game == LGAME_LOC && i < 2; i++) { /* Fussleiste: grau wie Play Line of Contact */
            BOOL fe = loc_launch_ok() ? TRUE : FALSE;
            if (ctl(kFoot[i]) && (IsWindowEnabled(ctl(kFoot[i])) != FALSE) != (fe != FALSE)) {
                EnableWindow(ctl(kFoot[i]), fe);
                InvalidateRect(ctl(kFoot[i]), NULL, FALSE);
            }
        }
        busy = 0;
    }
}

int loctab_visible(const uictl_t *u, int page)
{
    if (u->id == ID_LOC_WIDE_XI && !lgame_wide_experimental(LGAME_WIDE_720P)) {
        return 0; /* W414: Kolben nur, solange ein experimentelles Bildformat angeboten wird (720p, LOC_WIDE_720P) */
    }
    if (u->id == ID_XEXP_DISP && pergame_box_169() && !lgame_wide_experimental(loctab_wide_val())) {
        return 0; /* W414: LoC 16:9 - Karte Breitbild ohne experimentelle Option (Raender folgen der 16:9-Box) */
    }
    if (u->id == ID_NAVSUB_MATCH || u->id == ID_NAVSUB_ACC) { /* W399: nur LoC, nur auf Multiplayer */
        return g_game == LGAME_LOC && page == P_LNET;
    }
    if (u->id == ID_LOC_VER_GO) { /* W421: Versionssperre zuerst (ein Sprung) */
        return g_game == LGAME_LOC && page == P_PLAY && lgame_loc_client_ready() && loc_ver_gate();
    }
    if (u->id == ID_LOC_IMG_GO) { /* W403: Disc zuerst */
        return g_game == LGAME_LOC && page == P_PLAY && loc_img_gate(NULL, 0) && !loc_ver_gate();
    }
    if (u->id == ID_LOC_GATE_GO) { /* W400; W403: erst, wenn die Disc passt (ein Sprung, Disc zuerst) */
        return g_game == LGAME_LOC && page == P_PLAY && loc_gate_shown() && !loc_img_gate(NULL, 0) && !loc_ver_gate();
    }
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
    if (u->id == ID_LOCTP_STOP && (loc_gate_shown() || loc_img_gate(NULL, 0) || loc_ver_gate()) && !loctp_active()) {
        return 0; /* W400: Platz fuer den Sprung zum Zahnrad (Stop erst mit laufendem Testpartner) */
    }
#endif
    { /* W328: Pilotenkonto (Karte bzw. Verwaltungsansicht auf LoC > Erweitert) */
        int av = acc_visible(u->id, page);
        if (av >= 0) {
            return av;
        }
    }
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
    { /* W336: Testpartner-Knoepfe auf LoC > Spielen */
        int tv = loctp_visible_id(u->id, page);
        if (tv >= 0) {
            return tv;
        }
    }
#endif
    if (page == P_LNET && lnet_tab_hidden(u->id)) {
        return 0; /* W473: Element des anderen Reiters der Netzwerk-Karte */
    }
    if (u->id >= ID_LOC_FIRST && u->id <= ID_LOC_LAST) {
        if (page == P_LNET && acc_view_active()) {
            return 0; /* W328: Verwaltungsansicht deckt die Seite (W397: Multiplayer) */
        }
        if (u->id == ID_LOC_CODE_COPY && !onl_copy_visible()) {
            return 0; /* W334: nur mit Raumcode der eigenen Lobby */
        }
        if (g_loc_lan && (u->id == ID_LOC_VIS || u->id == ID_LOC_CODE || u->id == ID_LOC_CODE_COPY)) {
            return 0; /* W454: im LAN Raum/Passwort statt Sichtbarkeit/Raumcode */
        }
        /* W387: ohne startbaren Client war Gegner glaetten (unter dem Start) ausgeblendet; W392: auf der Seite
         * Netzwerk immer sichtbar (Einstellung, kein Start) */
        return g_game == LGAME_LOC && page == u->page; /* W293: Start/Change auf Spielen, Disc/Ordner auf Erweitert */
    }
    if (u->id == ID_LNET_VERPAGE && !g_onl_ver[0] && !loc_ver_gate()) {
        return 0; /* W420: nur nach einer Versionsablehnung; W421: oder mit Versionssperre aus /v1/info */
    }
    if ((u->id == ID_LNET_LANROOM || u->id == ID_LNET_LANPASS) && !g_loc_lan) {
        return 0; /* W454: nur im LAN-Modus */
    }
    if (u->id >= ID_LNET_FIRST && u->id <= ID_LNET_LAST) { /* W392: Seite Netzwerk, nur LoC (nie bei SB-Packs) */
        return g_game == LGAME_LOC && page == P_LNET && !acc_view_active(); /* W397: Account verwalten deckt */
    }
    if (u->id == ID_GAME_SB || u->id == ID_GAME_LOC) {
        return -1;
    }
    if (g_game != LGAME_LOC) {
        return -1;
    }
    if (u->page == P_PACKS) {
        return 0; /* W392: Pack-Elemente nie auf der LoC-Seite Netzwerk (gleicher Seitenplatz) */
    }
    if (u->page == P_PLAY) {
        return 0; /* SB-Spielen (Start, VR, Abbild, Bereitschaft, Update-Karte) */
    }
    if (u->page == P_ADV) { /* W293: nur die gemeinsamen Teile (Feinabstimmung) */
        return loctab_adv_shared(u->id) ? -1 : 0;
    }
    return -1;
}

/* Navigation ohne Luecke in der Reihenfolge je Spiel (W397: nav_order; Test tools unter dem Zahnrad). */
/* W399: Ist bei LoC Multiplayer offen, stehen darunter eingerueckt die Unterpunkte Match & network und Account;
 * die folgenden Punkte ruecken nach unten. Sonst bleibt die Leiste zugeklappt (Unterpunkte unsichtbar). */
static int nav_sub_open(void)
{
    return g_game == LGAME_LOC && g_page == P_LNET;
}

void nav_sub_layout(void)
{
    const int *o = nav_order();
    int k, y = TITLE_H + 16, open = nav_sub_open();
    for (k = 0; k < NAV_ORDER_N; k++) {
        uictl_t *u = ui_find(ID_NAV0 + o[k]);
        if (u && u->y != y) {
            u->y = y;
            ui_place(u);
        }
        y += 44;
        if (o[k] == P_LNET && g_game == LGAME_LOC) {
            uictl_t *s0 = ui_find(ID_NAVSUB_MATCH), *s1 = ui_find(ID_NAVSUB_ACC);
            int ys = y - 4;
            if (s0 && s0->y != ys) {
                s0->y = ys;
                ui_place(s0);
            }
            if (s1 && s1->y != ys + NAVSUB_H + 2) {
                s1->y = ys + NAVSUB_H + 2;
                ui_place(s1);
            }
            if (open) {
                y = ys + 2 * (NAVSUB_H + 2) + 6;
            }
        }
    }
    if (ctl(ID_NAVSUB_MATCH)) {
        InvalidateRect(ctl(ID_NAVSUB_MATCH), NULL, FALSE);
    }
    if (ctl(ID_NAVSUB_ACC)) {
        InvalidateRect(ctl(ID_NAVSUB_ACC), NULL, FALSE);
    }
}

void loctab_nav_layout(void)
{
    nav_sub_layout();
    ui_set_checked(ID_GAME_SB, g_game == LGAME_SB);
    ui_set_checked(ID_GAME_LOC, g_game == LGAME_LOC);
    xicon_texts(); /* W392: Navigation Packs <-> Netzwerk (Text inkl. Hinweiszeile, Symbol beim Zeichnen) */
    if (ctl(ID_NAV0 + P_PACKS)) {
        InvalidateRect(ctl(ID_NAV0 + P_PACKS), NULL, FALSE);
    }
    loc_gate_sync(); /* W400: grau ohne startbaren Client oder ohne steelnet */
}

void loctab_set_game(int game)
{
    game = game == LGAME_LOC ? LGAME_LOC : LGAME_SB;
    if (game == g_game) {
        loctab_nav_layout();
        return;
    }
    {
        int from = g_game;
        g_game = game;
        pergame_switch(from, game); /* W402: eigene Werte je Spiel */
    }
    loctab_save();
    loctab_nav_layout();
    ui_apply_rules(); /* W326: Experimental-Hinweis haengt am Bereich (LoC-Bildformat) */
    if (ctl(ID_IPFPS)) { ip_fill(ctl(ID_IPFPS)); } /* W323: "20 (Original)" im LoC-Profil, Wahl bleibt */
    base_build(); /* Marke oben links: STEEL BATTALION / LINE OF CONTACT */
    show_page(g_page); /* Packs/Erweitert -> Spielen; Sichtbarkeit + Bild neu */
    set_status_k(LT(game == LGAME_LOC ? "gui.loc_switched" : "gui.loc_switched_sb"), 0);
}

void loctab_check(void)
{
    char p[512];
    ui_gettext(ctl(ID_LOC_IMAGE), p, sizeof p);
    if (g_locfix_path[0] && strcmp(p, g_locfix_path) == 0) {
        g_loc_res = g_locfix;
    } else {
        lgame_loc_check(p, &g_loc_res);
    }
    strcpy_s(g_loc_res_path, sizeof g_loc_res_path, p);
    g_loc_checked = 1;
    strcpy_s(g_loc.loc_image, sizeof g_loc.loc_image, p);
    loctab_save();
    loc_gate_sync();         /* W403: Disc passt bzw. fehlt -> Startknopf + Sprung passend */
    ui_refresh_visibility();
    redraw_rect(CX0, LOC_DISC_Y, COLW, LOC_DISC_H);
}

void loctab_browse(void)
{
    OPENFILENAMEW ofn;
    static wchar_t file[1024], filter[220], title[128];
    char cur[1024];
    wchar_t f1[128], f2[64];
    size_t n1, n2;
    if (g_o.shot != 0) { /* Test/Shot: nie ein Dialog */
        printf("[browse] LoC\n");
        return;
    }
    ui_gettext(ctl(ID_LOC_IMAGE), cur, sizeof cur);
    u8_to_w(cur, file, 1024);
    u8_to_w(LT("gui.loc_dlg_filter"), f1, 128);
    u8_to_w(LT("gui.dlg_allfiles"), f2, 64);
    u8_to_w(LT("gui.loc_dlg_title"), title, 128);
    n1 = wcslen(f1) + 1;
    n2 = wcslen(f2) + 1;
    memset(filter, 0, sizeof filter);
    if (n1 + 19 + n2 + 5 < 220) { /* "<f1>|*.iso;*.xiso;*.xbe|<f2>|*.*||" */
        wcscpy_s(filter, 220, f1);
        wcscpy_s(filter + n1, 220 - n1, L"*.iso;*.xiso;*.xbe");
        wcscpy_s(filter + n1 + 19, 220 - n1 - 19, f2);
        wcscpy_s(filter + n1 + 19 + n2, 220 - n1 - 19 - n2, L"*.*");
    }
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = filter;
    ofn.lpstrTitle = title;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(ctl(ID_LOC_IMAGE), file);
        loctab_check();
    }
}

void loctab_open_folder(void)
{
    char d[1200];
    wchar_t w[1200];
    loctab_data_dir(d, sizeof d);
    if (g_o.shot != 0) { /* Test/Shot: nie ein Explorer-Fenster, nichts anlegen */
        printf("[open] %s\n", d);
        return;
    }
    lcfg_mkdir_p(d);
    u8_to_w(d, w, 1200);
    ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* Start im Bereich LoC: gesperrt mit Meldung, solange der Client LoC nicht
 * startet (lgame_loc_client_ready). Danach: Abbild + eigener Datenordner. */
void loctab_start(void)
{
    char m[600];
    const lgame_img_t *r = (g_loc_checked && strcmp(g_loc_res_path, g_loc.loc_image) == 0)
                               ? &g_loc_res : NULL;
    g_loc_last_cmd[0] = '\0';
    if (lgame_loc_client_ready() && loc_ver_gate()) { /* W421: veraltet laut steelnet (jeder Startweg) */
        loc_gate_msg(m, sizeof m);
        set_status_k(m, 2);
        return;
    }
    if (lgame_loc_start_gate(r, m, sizeof m) != 0) {
        if (loc_gate_shown()) { /* W400: beide Gruende, der Disc-Grund zuerst */
            size_t k = strlen(m);
            _snprintf_s(m + k, sizeof m - k, _TRUNCATE, " %s", LT("gui.loc_gate_why"));
        }
        set_status_k(m, lgame_loc_client_ready() ? 2 : 0);
        return;
    }
    if (!loc_net_ok()) { /* W400: ohne steelnet kein LoC-Start (gleiche Sperre fuer alle Startwege) */
        set_status_k(LT("gui.loc_gate_why"), 2);
        return;
    }
    { /* W353: intern immer, Community nur im privaten Testpaket (lgame_loc_client_ready) */
        char d[1200], acp[1200], set[4096], raw[64] = "", xbe_u8[1200];
        int onl = g_loc_online && onl_srv_on() && !g_loc_lan; /* W454: LAN = nie steelnet */
        size_t k;
        loctab_data_dir(d, sizeof d);
        strcat_s(d, sizeof d, "\\hdd");
        u8_to_acp(d, acp, sizeof acp);
        wchar_t *env;
        /* W353 LOC-DURCHSTICH: Kette wie das Nutzer-Spiel des Testpartners (loctp_build_set, W336):
         * loc_boot=1 (sonst bricht der Client beim LoC-Abbild ab) und das Abbild. W384 (GH #52): ISO als
         * game_image, .xbe/entpackter Ordner im Ordner-Modus (xbe_path + media_root, game_image leer). */
        lgame_loc_image_set(g_loc.loc_image, set, sizeof set, xbe_u8, sizeof xbe_u8);
        k = strlen(set);
        _snprintf_s(set + k, sizeof set - k, _TRUNCATE, ";vr_enable=0;game_tick=30;hdd_root=%s", acp); /* W402: nie PAL */
        loc_net_set_append(set, sizeof set); /* W387/W392: Netzwerk-Schalter, jeder LoC-Start (lokal und online) */
        loc_rule_set_append(set, sizeof set); /* W420: Host-Regel Clip dumping */
        if (ctl(ID_LOC_CODE)) {
            ui_gettext(ctl(ID_LOC_CODE), raw, sizeof raw);
        }
        /* W334: Online (steelnet) -> Transport, Treffpunkt, Raumcode; ungueltiger Code = kein Start */
        if (onl_set_append(set, sizeof set, onl, onl, raw, g_loc_public) != 0) {
            set_status_k(LT("gui.loc_code_bad"), 2);
            onl_redraw();
            return;
        }
        if (g_loc_lan && lan_set_append(set, sizeof set) != 0) { /* W454: LAN-Modus (Raum, ohne Treffpunkt) */
            set_status_k(LT("gui.lan_room_bad"), 2);
            onl_redraw();
            return;
        }
        if ((onl || g_loc_lan) && g_o.shot == 0) { /* Status ab diesem Lauf (aeltere Datei gilt nicht); W454: auch LAN */
            g_onl_since = onl_now_ft() - 2ULL * 10000000ULL;
            g_onl_sec[0] = g_onl_code[0] = g_onl_err[0] = '\0';
            ui_refresh_visibility();
            onl_redraw();
        }
        { /* W402 PERGAME: LoC-Werte sichern + Spielerbetrieb-Block in user_loc.cfg; der Client liest nur diese Datei */
            char lp[1200];
            wchar_t wlp[1200];
            game_cfg_path(LGAME_LOC, lp, sizeof lp);
            strcpy_s(g_loc_last_cfg, sizeof g_loc_last_cfg, lp);
            if (loctab_may_write()) {
                if (do_save(1) != 0) {
                    return;
                }
                if (lcfg_write_player_block(lp) != 0) {
                    char e[1400];
                    _snprintf_s(e, sizeof e, _TRUNCATE, LT("gui.msg_nowrite"), lp);
                    set_status_k(e, 2);
                    return;
                }
            }
            u8_to_w(lp, wlp, 1200);
            SetEnvironmentVariableW(L"SB_USER_CFG", wlp); /* nur fuer diesen Start (acc_launch_env kopiert den Block) */
        }
        /* W328: Pilotname + SB_LOC_AUTH nur im Kindprozess; W454: im LAN nur der Name (kein Konto, kein Schluessel),
         * Raumpasswort als SB_LAN_PASS nur fuer diesen Start */
        env = g_loc_lan ? (lan_pilot_append(set, sizeof set), (wchar_t *)NULL) : acc_launch_env(set, sizeof set);
        lan_env(1);
        app_client_cmdline(g_client_dir, xbe_u8, set, g_loc_last_cmd,
                           sizeof g_loc_last_cmd);
        if (g_o.shot != 0) {
            SetEnvironmentVariableW(L"SB_USER_CFG", NULL);
            printf("[loc-start] %s\n", g_loc_last_cmd);
            printf("[loc-cfg] %s\n", LGAME_LOC_CFG_NAME); /* W402 */
            if (env) {
                printf("[loc-env] SB_LOC_AUTH (nur Kindprozess)\n");
            }
            if (g_loc_lan && g_loc_lan_pass[0]) {
                printf("[loc-env] SB_LAN_PASS (nur Kindprozess)\n"); /* W454: nie der Wert */
            }
            lan_env(0);
            free(env);
            return;
        }
        lcfg_mkdir_p(d);
        if (app_launch_client_env(g_client_dir, xbe_u8, set, env, m, sizeof m) == 0) {
            set_status_k(m, 1);
        } else {
            set_status_k(m, 2);
        }
        SetEnvironmentVariableW(L"SB_USER_CFG", NULL); /* W402: Steel Battalion liest weiter user.cfg */
        lan_env(0); /* W454 */
        lacc_env_free(env); /* W344: ganzer Block (sortiert, SB_LOC_AUTH steht nicht vorne) */
    }
}


/* ------------------------------------------------------------------ */
