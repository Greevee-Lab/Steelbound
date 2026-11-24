/* overlay_style.h - W461 OVERLAY-CORE: gemeinsamer Look und Zeichenweg aller eingeblendeten Elemente.
 *
 * Vorbild ist das ESC-Menue (sb_overlay.c). Kein Overlay definiert eigene Schriften, Farben, Abstaende,
 * DIB-/Textur-Verwaltung oder Spielbox-Rechnung mehr, sondern nimmt sie von hier
 * (Beschreibung und Werte: orchestra/briefs/_overlay_style.md).
 *
 * Zeichenweg (unveraendert seit W122): GDI zeichnet in eine 32-Bit-DIB (ovs_canvas_t), daraus wird
 * vormultipliziertes BGRA (ovs_finish fuer den gruenen SB-UI-Look, ovs_pm_span fuer Text auf freier
 * Flaeche), das per ovs_tex_upload in eine kleine Textur geht und von sb_ovl_quad 1:1 (nie gestreckt) auf
 * den Backbuffer kommt.
 *
 * Masseinheiten: Overlays rechnen in Einheiten und skalieren mit einem Faktor f auf Pixel (ovs_px).
 *  - SB-UI (ESC, F2/F3, Netz-HUD): f = Hoehe der Spielbox / 540, mindestens 1 (ovs_scale_ui),
 *    d. h. 1080p -> f = 2 (Bezug 1080p = 2 Pixel je Einheit).
 *  - LoC-Lobby-Elemente (Netzpanel, Suchliste, Chat): Menueraum 640x480, f = Boxhoehe / 480.
 *
 * Threads: Font-Cache ist gesperrt (SRW) und darf von Render- und VR-Thread genutzt werden. Canvas,
 * Texturen und Belegungsplan gehoeren dem Aufrufer (Render-Thread bzw. unter ctx_lock). */
#ifndef OVERLAY_STYLE_H
#define OVERLAY_STYLE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ Tokens */
/* Farben (GDI COLORREF), gruenes SB-UI (ESC-Menue) */
#define OVS_C_BG     RGB(4, 22, 11)     /* Flaeche */
#define OVS_C_TXT    RGB(140, 255, 150) /* Text hell, Eckwinkel */
#define OVS_C_DIM    RGB(58, 120, 68)   /* Text gedimmt */
#define OVS_C_LINE   RGB(80, 200, 100)  /* Rahmen */
#define OVS_C_HEAD   RGB(70, 190, 88)   /* Kopfzeile (Text darin in OVS_C_BG) */
#define OVS_C_SEL    RGB(24, 84, 40)    /* Auswahlbalken */
#define OVS_C_GRAPH  RGB(2, 14, 7)      /* Diagrammgrund (F2) */
#define OVS_C_ACCENT RGB(255, 196, 92)  /* Akzent Orange (Experimental-Marke, Toast-Titel) */
#define OVS_C_WARN   RGB(255, 176, 64)  /* Warnzeile im SB-UI (F2/F3) */
#define OVS_C_REC    RGB(255, 100, 100) /* W467: Clip-Aufnahme (REC) */
#define OVS_C_REC_BG RGB(24, 8, 8)       /* W467: Flaeche der REC-Marke */
#define OVS_ALPHA    0.88f              /* Deckung des SB-UI */
#define OVS_SCANLINE 0.86f              /* jede zweite Zeile ab Faktor 2 */
/* Status (0xRRGGBB, fuer ovs_pm_span und Status-Punkte; LoC-Elemente) */
#define OVS_RGB_OK    0x78DC8Cu /* gruen: gut/direkt */
#define OVS_RGB_MID   0xFFD21Eu /* gelb: Hinweis/Wert */
#define OVS_RGB_BAD   0xFFA028u /* orange-rot: schlecht (Ping > 150 ms, Arcade) */
#define OVS_RGB_TEXT  0xE6E6E6u /* Text hell auf dunkler Flaeche */
#define OVS_RGB_DIM   0xC8C8C8u /* Text gedimmt auf dunkler Flaeche */
#define OVS_RGB_PANEL 0x080C10u /* dunkle Flaeche der LoC-Elemente */
/* W452 Reiter (Chat, weitere Overlays mit Tabs): Akzent = OVS_C_ACCENT als 0xRRGGBB */
#define OVS_RGB_ACCENT  0xFFC45Cu /* Unterstrich aktiver Reiter, Zaehler, Eingabeaufforderung */
#define OVS_RGB_TAB     0x10161Cu /* Reiter inaktiv */
#define OVS_RGB_TAB_ACT 0x3A2A12u /* Reiter aktiv (gefuellt) */
#define OVS_RGB_TAB_HOT 0x6A4818u /* Reiter pulst (Neues im nicht aktiven Tab) */
#define OVS_RGB_TAB_TXT 0xA8AEB4u /* W467: Reiter-Beschriftung (verfuegbar) */
#define OVS_RGB_TAB_OFF 0x60666Cu /* W467: Reiter-Beschriftung (nicht verfuegbar) */
#define OVS_RGB_TAB_SEL 0xFFF2DCu /* W467: Beschriftung aktiver Reiter */
/* W467 OVERLAY-UNIFY: weitere Status-/Markenfarben der LoC-Elemente (vorher je Overlay eigene Werte) */
#define OVS_RGB_INFO      0x8CC8FFu /* blau: Sim / neutrale Info (Lobby-Infopanel) */
#define OVS_RGB_SIM_BG    0x3C6E9Bu /* Marke SIM (Suchliste, Widget) */
#define OVS_RGB_ARCADE_BG 0xD2780Fu /* Marke ARCADE */
#define OVS_RGB_TAG_TXT   0xFFFFFFu /* Text auf Marken */
#define OVS_RGB_SYS       0xC4CED4u /* Systemzeile (Chat) */
#define OVS_RGB_COMP      0xFFDEA0u /* IME-Komposition (Chat) */
#define OVS_RGB_RULE      0x6A7480u /* Trennlinie zwischen Verlauf und Eingabe (Chat) */
#define OVS_TAG_ALPHA     0.92f     /* Deckung der Marken */
/* Versions-Wasserzeichen (dezent, ohne Flaeche) */
#define OVS_RGB_WM        0xE1F0E1u
#define OVS_WM_ALPHA      0.60f     /* Schrift */
#define OVS_WM_SHADOW     0.45f     /* dunkler Schatten, 0,5 Einheiten versetzt */
/* 0xRRGGBB -> GDI COLORREF */
#define OVS_CREF(rgb) RGB(((rgb) >> 16) & 255u, ((rgb) >> 8) & 255u, (rgb) & 255u)

/* W467 OVERLAY-UNIFY: LoC-Panel-Familie (Lobby-Infopanel, Widget, Replay-Hilfe, Chat). Dunkle Flaeche OVS_RGB_PANEL,
 * Akzentlinie links in OVS_C_ACCENT, Kopfzeile mit gedimmter Akzent-Trennlinie, Zeilen in Status-/Textfarbe.
 * Masse in Einheiten des Menueraums 640x480 (f = Boxhoehe / 480). Eine Deckung fuer alle Panels: OVS_ALPHA wie das
 * ESC-Menue (W467: Netzpanel/Replay-Hilfe vorher 0,62 - auf hellem Himmel in Enhanced zu wenig Kontrast). */
#define OVS_LP_ALPHA      OVS_ALPHA /* Flaeche aller LoC-Panels (auch Chat beim Schreiben) */
#define OVS_LP_ALPHA_SOFT 0.66f     /* Chat-Verlauf ohne Eingabe (dezent, Spielbild scheint durch) */
#define OVS_LP_FS_HEAD    10.0f     /* Kopfzeile, FW_SEMIBOLD */
#define OVS_LP_FS_ROW     9.5f      /* Zeilen, FW_NORMAL (Chat: Segoe UI FW_SEMIBOLD) */
#define OVS_LP_FS_TAG     8.0f      /* Marken/Reiter, FW_BOLD */
#define OVS_LP_PAD        4.0f      /* Innenabstand */
#define OVS_LP_HEAD       14.0f     /* Hoehe der Kopfzeile */
#define OVS_LP_LH         12.0f     /* Zeilenhoehe */
#define OVS_LP_BAR        2.0f      /* Akzentlinie links ... */
#define OVS_LP_BAR_ALPHA  0.9f      /* ... in OVS_C_ACCENT mit dieser Deckung */
#define OVS_LP_INSET      2.0f      /* Text-Einzug hinter der Akzentlinie (zusaetzlich zu OVS_LP_PAD) */
#define OVS_LP_GAP        4.0f      /* Abstand gestapelter Elemente (SB-UI-Faktor, wie OVS_MARGIN) */
#define OVS_LP_W_MIN      120.0f    /* Breite nach Text: mindestens ... */
#define OVS_LP_W_MAX      240.0f    /* ... hoechstens (lange Namen kuerzt DT_END_ELLIPSIS) */
#define OVS_LP_W_WIDE     300.0f    /* hoechstens bei Tastenhilfen (Replay-Hilfe) */
#define OVS_LP_W_HINT     60.0f     /* mindestens bei einer einzelnen Hinweiszeile */
/* Schrift-Groessen (Einheiten, x f = Pixel) */
#define OVS_FS_MENU  10.5f /* ESC-Menue */
#define OVS_FS_HUD   9.0f  /* F2/F3-Panel, Netz-HUD */
/* Abstaende (Einheiten) */
#define OVS_BORDER   1.0f  /* Rahmenstaerke */
#define OVS_MARGIN   8.0f  /* Rand zur Bildschirmecke (Backbuffer, nicht Spielbox) */
#define OVS_RADIUS   0     /* Ecken eckig (HUD-Winkel statt Rundung) */
#define OVS_FADE_MS  0     /* kein Ein-/Ausblenden: Overlays erscheinen sofort (wie ESC-Menue) */
#define OVS_REF_UI   540.0f /* Spielbox-Hoehe fuer Faktor 1 (SB-UI) */
#define OVS_REF_LOC  480.0f /* Menueraum-Hoehe fuer Faktor 1 (LoC-Lobby) */

/* Rahmen mit Kopfzeile (Einheiten): Eckwinkel, Kopfhoehe, Innenabstand, Titel-Einzug */
typedef struct {
    float bracket, head, pad, inset;
} ovs_frame_t;
extern const ovs_frame_t OVS_FRAME_MENU; /* ESC-Menue: 8 / 20 / 6 / 2 */
extern const ovs_frame_t OVS_FRAME_HUD;  /* F2/F3-Panel: 7 / 15 / 5 / 1 */

/* ------------------------------------------------------------------ Skalierung */
/* Einheiten -> Pixel, mindestens 1 */
int ovs_px(float f, float units);
/* SB-UI-Faktor aus der Spielbox-Hoehe (bh / 540, mindestens 1) */
float ovs_scale_ui(unsigned box_h);

/* ------------------------------------------------------------------ Spielbox und Ecken */
/* Box mit Seitenverhaeltnis aspect mittig in w x h (Balken links/rechts bzw. oben/unten), mindestens 1x1 */
void ovs_fit_box(float aspect, unsigned w, unsigned h, unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh);
/* LoC-Spielbild im Backbuffer: 16:9 (g_loc_wide) bzw. 4:3 */
void ovs_loc_box(unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh);

typedef enum { OVS_TOP_RIGHT = 0, OVS_TOP_LEFT = 1, OVS_BOTTOM_LEFT = 2, OVS_BOTTOM_RIGHT = 3 } ovs_corner;
/* Ecke des Backbuffers (Reihenfolge wie perf_hud_pos/net_hud_pos: 0 oben rechts, 1 oben links, 2 unten links,
 * 3 unten rechts), Abstand margin Pixel zur Bildschirmecke. */
void ovs_anchor(ovs_corner c, unsigned w, unsigned h, unsigned margin, unsigned *x, unsigned *y);

/* ------------------------------------------------------------------ Belegungsplan
 * Damit sich Overlays nicht ueberlappen, meldet jedes sichtbare Overlay je Bild seine Flaeche an.
 * Feste Zuordnung (Nutzerwunsch 23.11., _overlay_style.md):
 *   oben rechts   F2-Leistungs-Panel (Standard), darunter F3-Netz-HUD; LoC-Netzpanel (oben rechts in der Spielbox)
 *   oben links    frei (F2/F3 wahlweise per Shift+F2/F3)
 *   unten links   Chat (W437/W452)
 *   unten rechts  Lobby-Infopanel (W453)
 *   rechts Mitte  Lobby-Widget Hauptmenue/MatchMaking (W460)
 *   Wasserzeichen unten rechts, Original-Vergleich-Toast oben mittig in der Spielbox (melden sich ebenfalls an)
 * Neue Overlays nehmen ovs_layout_place: Platz in ihrer Ecke, von der Ecke weg gestapelt hinter allem, was
 * in diesem Bild dort schon angemeldet ist. Overlays mit eigener Lage melden sich mit ovs_layout_claim an.
 * Der Plan leert sich automatisch, sobald ein neues Present (g_present_no) beginnt. Zeichenreihenfolge in
 * d3d11_render.c bestimmt den Vorrang (fruehere Anmeldung behaelt ihren Platz). */
typedef enum {
    OVS_SLOT_PERF = 0,   /* F2 Leistungs-Panel */
    OVS_SLOT_NETHUD,     /* F3 Netz-HUD */
    OVS_SLOT_NETPANEL,   /* LoC-Netzpanel (Lobby) */
    OVS_SLOT_LOBBYLIST,  /* LoC-Suchliste (Marken im Spielbild) */
    OVS_SLOT_CHAT,       /* Chat unten links */
    OVS_SLOT_LOBBYINFO,  /* Lobby-Infopanel unten rechts (W453) */
    OVS_SLOT_LOBBYWIDGET,/* Lobby-Widget rechts (W460) */
    OVS_SLOT_REPLAYHELP, /* LoC-Replay-Bedienhilfe oben rechts (W457) */
    OVS_SLOT_WATERMARK,  /* Versions-Wasserzeichen */
    OVS_SLOT_TOAST,      /* Original-Vergleich-Hinweis */
    OVS_SLOT_N
} ovs_slot;
/* Flaeche anmelden (Pixel im Backbuffer); ersetzt eine fruehere Anmeldung desselben Slots in diesem Bild */
void ovs_layout_claim(ovs_slot id, unsigned x, unsigned y, unsigned w, unsigned h);
/* Platz in Ecke c suchen (Abstand margin zur Bildschirmecke, gap zwischen Elementen), anmelden.
 * 1 = gefunden (x/y gesetzt), 0 = passt nicht mehr auf den Bildschirm. */
int ovs_layout_place(ovs_slot id, ovs_corner c, unsigned w, unsigned h, unsigned margin, unsigned gap, unsigned *x,
                     unsigned *y);
/* Anmeldung dieses Bildes abfragen (r = x, y, w, h). 1 = angemeldet. */
int ovs_layout_get(ovs_slot id, unsigned r[4]);
/* 1 = Rechteck ueberlappt eine Anmeldung dieses Bildes (ausser self) */
int ovs_layout_hits(ovs_slot self, unsigned x, unsigned y, unsigned w, unsigned h);

/* ------------------------------------------------------------------ Font-Cache */
typedef enum {
    OVS_FACE_MONO = 0, /* Consolas (SB-UI, Wasserzeichen, Toast) */
    OVS_FACE_COND,     /* Bahnschrift SemiCondensed (LoC-Netzpanel, Suchliste) */
    OVS_FACE_UI,       /* Bahnschrift */
    OVS_FACE_SEGOE,    /* Segoe UI (Chat, CJK-Ersatz ueber GDI-Font-Linking) */
    OVS_FACE_ICON,     /* Segoe Fluent Icons (Symbol-Marken im ESC-Menue) */
    OVS_FACE_N
} ovs_face;
/* Halter einer Schrift. Gleiche Schriften (Familie, Gewicht, Pixel) werden gemeinsam genutzt (Zaehler).
 * Wichtig: die bisherige Schrift des Halters darf beim Wechsel in keiner DC mehr ausgewaehlt sein. */
typedef struct {
    HFONT f;
    int face, weight, px, own;
} ovs_font_t;
/* Schrift (face, weight FW_*, Hoehe px) fuer den Halter; legt sie nur bei Aenderung neu an. NULL = gescheitert. */
HFONT ovs_font_get(ovs_font_t *s, ovs_face face, int weight, int px);
/* Halter freigeben (Schrift darf nicht mehr ausgewaehlt sein) */
void ovs_font_put(ovs_font_t *s);
/* Familienname (fuer Logs/Pruefungen) */
const wchar_t *ovs_face_name(ovs_face face);

/* ------------------------------------------------------------------ Zeichenflaeche (GDI-DIB) */
typedef struct {
    HDC dc;
    HBITMAP bmp, bmp_old;
    unsigned *bits;  /* 32 Bit, top-down */
    unsigned w, h;
    ovs_font_t font; /* Hauptschrift (in dc ausgewaehlt) */
    HFONT font_old;
    int font_px;
    float f;         /* Faktor Einheiten -> Pixel des Aufrufers */
} ovs_canvas_t;
/* DIB auf w x h bringen (DC bei Bedarf anlegen, TRANSPARENT). 1 = ok. Gleiche Groesse: nichts zu tun.
 * Bei neuer Groesse wird die Hauptschrift abgewaehlt (font_px = 0) - danach ovs_canvas_font. */
int ovs_canvas_size(ovs_canvas_t *c, unsigned w, unsigned h);
/* nur die DC (z. B. zum Messen vor ovs_canvas_size). NULL = gescheitert. */
HDC ovs_canvas_dc(ovs_canvas_t *c);
/* Hauptschrift waehlen (Cache). 1 = ok. */
int ovs_canvas_font(ovs_canvas_t *c, ovs_face face, int weight, int px);
void ovs_canvas_free(ovs_canvas_t *c);

/* ------------------------------------------------------------------ Textur (vormultipliziertes BGRA) */
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
typedef struct {
    struct ID3D11Texture2D *tex;
    struct ID3D11ShaderResourceView *srv;
    unsigned w, h;
} ovs_tex_t;
/* Textur auf w x h bringen. 0 = gescheitert, 1 = vorhanden, 2 = neu angelegt (Inhalt hochladen). */
int ovs_tex_ensure(ovs_tex_t *t, unsigned w, unsigned h);
/* Bild (w x h = Texturgroesse, top-down) hochladen. 1 = ok. Unter ctx_lock. */
int ovs_tex_write(ovs_tex_t *t, const unsigned *px);
/* ensure + write. 1 = ok. */
int ovs_tex_upload(ovs_tex_t *t, const unsigned *px, unsigned w, unsigned h);
void ovs_tex_free(ovs_tex_t *t);

/* ------------------------------------------------------------------ Zeichenhelfer (GDI auf dc) */
void ovs_fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c);
/* Text an x/y (oben), right = rechtsbuendig an x */
void ovs_text(HDC dc, int x, int y, const wchar_t *s, COLORREF c, int right);
/* Text mit dunklem Schatten (off Pixel nach rechts unten) */
void ovs_text_shadow(HDC dc, int x, int y, const wchar_t *s, COLORREF c, COLORREF shadow, int off, int right);
/* Panel: Flaeche OVS_C_BG, Rahmen OVS_C_LINE, HUD-Eckwinkel oben links/unten rechts in OVS_C_TXT */
void ovs_panel(HDC dc, float f, int W, int H, const ovs_frame_t *fr);
/* Kopfzeile (Balken OVS_C_HEAD, Titel links, tag rechts, Text in OVS_C_BG). Rueckgabe: Unterkante (Pixel). */
int ovs_head(HDC dc, float f, int W, int font_px, const ovs_frame_t *fr, const wchar_t *title, const wchar_t *tag);
/* Panel + Kopfzeile, Rueckgabe wie ovs_head */
int ovs_frame(HDC dc, float f, int W, int H, int font_px, const ovs_frame_t *fr, const wchar_t *title,
              const wchar_t *tag);
/* Zeile: Text links bei pad, optional Wert rechtsbuendig bei W - pad */
void ovs_row(HDC dc, int pad, int W, int y, const wchar_t *left, COLORREF cl, const wchar_t *right, COLORREF cr);
/* Status-Punkt (Quadrat-Punkt, Kantenlaenge d Pixel) mit Mitte cx/cy */
void ovs_dot(HDC dc, int cx, int cy, int d, COLORREF c);

/* ------------------------------------------------------------------ Umrechnung nach vormultipliziertem BGRA */
/* SB-UI: Deckung OVS_ALPHA, Scanlines ab Faktor 2 (in place) */
void ovs_finish(unsigned *bits, unsigned w, unsigned h, float f);
/* Weisser GDI-Text (Abdeckung = groesster Kanal) als Farbe col (0xRRGGBB) ueber Flaeche bg (0xRRGGBB) mit Deckung
 * bga, n Pixel von src nach dst */
void ovs_pm_span(unsigned *dst, const unsigned *src, int n, unsigned col, unsigned bg, float bga);
/* Zielpuffer (w*h) bei Bedarf vergroessern. 1 = ok. */
int ovs_buf(unsigned **buf, unsigned *cap, unsigned n);
/* Ein vormultipliziertes BGRA-Pixel aus Farbe (0xRRGGBB) und Deckung */
unsigned ovs_pm_px(unsigned rgb, float a);

/* ------------------------------------------------------------------ LoC-Panel (W467) */
/* Lage einer Zeile im Panel (Pixel, f = Menueraum-Faktor): Zeile 0 = Kopfzeile (wenn head), danach Zeilen der Hoehe
 * OVS_LP_LH. Rueckgabe: Oberkante; *bot = Unterkante (optional). */
int ovs_lp_row_top(float f, int head, int i, int *bot);
/* Panelhoehe fuer nrow Zeilen (inkl. Kopfzeile, wenn head) in Pixeln */
unsigned ovs_lp_height(float f, int head, int nrow);
/* Panel fertigstellen: Flaeche OVS_RGB_PANEL (OVS_LP_ALPHA), Akzentlinie links, Text (weisser GDI-Text in bits) je Zeile
 * in col[i], bei head gedimmte Akzent-Trennlinie unter der Kopfzeile. pm/bits: w x h. */
void ovs_lp_finish(unsigned *pm, const unsigned *bits, unsigned w, unsigned h, float f, int head, const unsigned *col,
                   int nrow);

#ifdef __cplusplus
}
#endif
#endif /* OVERLAY_STYLE_H */
