/* loc_chat.h - W437 LOC-CHAT: Textchat in LoC-Lobby und -Match (GitHub #77), reiner Kern ohne Spiel/Fenster/Netz.
 *
 * Paket "SBCH" (Spielport 1262, nur als SBT2-Datagramm des eigenen Transports, nie an das Spiel, net/SECURITY.md
 * Abschnitt W437), Little Endian:
 *   0  "SBCH"     4 B Kennung
 *   4  ver   u8   = 1
 *   5  kind  u8   = 1 (Text)
 *   6  len   u16  Textlaenge in Byte, 1..LCH_MAX_TEXT_BYTES
 *   8  nonce u32  Kennung des sendenden Prozesses (!= 0)
 *  12  seq   u32  laufende Nummer je Absender (!= 0)
 *  16  text  UTF-8, genau len Byte (keine Null)
 * Paketlaenge exakt 16 + len. Kein Absendername im Paket: der Name kommt aus der Sitzung (Pilotname aus peer_info,
 * an die Schluessel gebunden, SECURITY.md §4.3).
 *
 * Bereinigung (Senden und Empfangen): strenges UTF-8 (keine Ueberlangen, keine Surrogate, <= U+10FFFF), entfernt
 * Steuerzeichen (C0, DEL, C1), Bidi-Steuerzeichen, Nullbreiten-/unsichtbare Zeichen, Zeilen-/Absatztrenner,
 * Tag-Zeichen, Nichtzeichen, private Nutzung; hoechstens 2 kombinierende Zeichen in Folge; Leerraum -> ein
 * Leerzeichen, Anfang/Ende gekuerzt; hoechstens LCH_MAX_CHARS Zeichen.
 * Ratenbremse: LCH_RATE_N Nachrichten je LCH_RATE_MS je Absender (Senden und Empfangen), darueber verworfen.
 * Nichts davon wird geloggt; der Kern zaehlt nur. */
#pragma once
#include <stdint.h>
#include <wchar.h>

#define LCH_MAGIC0 'S'
#define LCH_MAGIC1 'B'
#define LCH_MAGIC2 'C'
#define LCH_MAGIC3 'H'
#define LCH_VER 1
#define LCH_KIND_TEXT 1
#define LCH_HDR 16
#define LCH_MAX_CHARS 240                       /* Zeichen (Codepunkte) je Nachricht (W452: 120 -> 240) */
#define LCH_MAX_TEXT_BYTES (LCH_MAX_CHARS * 4)  /* 960 (SBCH 976 Byte < SBT2-Nutzlast 1168 - 22 - 10) */
#define LCH_PKT_MAX (LCH_HDR + LCH_MAX_TEXT_BYTES)
#define LCH_PORT_GP 1262                        /* eigener Spielport (Quelle = Ziel), nicht an das Spiel */
#define LCH_RATE_N 3
#define LCH_RATE_MS 5000u
#define LCH_RATE_SLOTS 16
#define LCH_SHOW_MS 8000u                       /* neue Nachricht sichtbar, danach ausgeblendet */
#define LCH_FADE_MS 1000u                       /* letzte Sekunde blendet aus */
#define LCH_HIST 8                              /* Zeilen im Verlauf (beim Tippen sichtbar) */
#define LCH_MUTE_MAX 16
#define LCH_NAME_MAX 16                         /* Pilotname (ASCII, wie im Transkript) */
#define LCH_LINE_W (LCH_MAX_CHARS * 2 + 1)      /* UTF-16 inkl. Surrogatpaare + Null */
#define LCH_DEDUP 32

typedef struct lch_pkt {
    uint32_t nonce, seq;
    int len;
    const uint8_t *text; /* zeigt in das Paket (len Byte) */
} lch_pkt;

/* Paket bauen: text = UTF-8 (bereits bereinigt), len 1..LCH_MAX_TEXT_BYTES. Rueckgabe Paketlaenge oder 0. */
int lch_build(uint8_t *out, int cap, uint32_t nonce, uint32_t seq, const uint8_t *text, int len);
/* Paket pruefen (alle Felder vor jeder Uebernahme). 1 = gueltig, 0 = verwerfen. */
int lch_parse(const uint8_t *d, int len, lch_pkt *o);

/* 1 = Codepunkt darf angezeigt/gesendet werden (Positivregel). */
int lch_cp_ok(uint32_t cp);
/* 1 = kombinierendes Zeichen (Begrenzung auf 2 in Folge). */
int lch_cp_combining(uint32_t cp);
/* UTF-8 bereinigen -> Codepunkte (hoechstens cap, hoechstens LCH_MAX_CHARS). Rueckgabe Anzahl (0 = leer). */
int lch_sanitize_u8(const uint8_t *in, int n, uint32_t *cps, int cap);
/* Codepunkte (bereits bereinigt) bereinigen wie oben (fuer die Eingabezeile). Rueckgabe Anzahl. */
int lch_sanitize_cps(const uint32_t *in, int n, uint32_t *cps, int cap);
/* Codepunkte -> UTF-8 (ohne Null). Rueckgabe Byte oder -1 (zu klein). */
int lch_cps_to_u8(const uint32_t *cps, int n, uint8_t *out, int cap);
/* Codepunkte -> UTF-16 mit Null. Rueckgabe Einheiten ohne Null oder -1. */
int lch_cps_to_w(const uint32_t *cps, int n, wchar_t *out, int cap);
/* Pilotname fuer die Anzeige: nur druckbares ASCII, 1..LCH_NAME_MAX, sonst '?' bzw. "?". */
void lch_name_clean(const char *in, char *out, int cap);

/* ---------------- Ratenbremse je Absender ---------------- */
typedef struct lch_rate {
    struct { uint32_t key, last, t[LCH_RATE_N]; int used, n; } e[LCH_RATE_SLOTS];
} lch_rate;
/* 1 = erlaubt (und gezaehlt), 0 = verwerfen. key = Absender (vip bzw. 0 = selbst). */
int lch_rate_take(lch_rate *r, uint32_t key, uint32_t now);

/* ---------------- Stummschalten (je Sitzung, nur im Speicher) ---------------- */
typedef struct lch_mute {
    char name[LCH_MUTE_MAX][LCH_NAME_MAX + 1];
    int n;
} lch_mute;
/* 1 = neu stumm, 0 = war schon, -1 = ungueltig/voll */
int lch_mute_add(lch_mute *m, const char *name);
/* 1 = entfernt, 0 = nicht vorhanden */
int lch_mute_del(lch_mute *m, const char *name);
int lch_muted(const lch_mute *m, const char *name);

/* ---------------- Verlauf ---------------- */
enum { LCH_L_OTHER = 0, LCH_L_SELF = 1, LCH_L_SYS = 2 };
typedef struct lch_line {
    wchar_t name[LCH_NAME_MAX + 1];
    wchar_t text[LCH_LINE_W];
    uint32_t t;
    int kind;
} lch_line;
typedef struct lch_hist {
    lch_line l[LCH_HIST];
    int head, n; /* head = naechster Platz */
    uint32_t gen;
} lch_hist;
void lch_hist_push(lch_hist *h, int kind, const char *name, const wchar_t *text, uint32_t now);
/* k-te neueste Zeile (0 = neueste) oder NULL */
const lch_line *lch_hist_get(const lch_hist *h, int k);

/* ---------------- Eingabezeile ---------------- */
typedef struct lch_input {
    uint32_t cp[LCH_MAX_CHARS];
    int n;
    uint16_t hs; /* ausstehendes hohes Surrogat aus WM_CHAR */
} lch_input;
void lch_input_clear(lch_input *in);
/* UTF-16-Einheit aus WM_CHAR. 1 = Zeichen uebernommen, 0 = verworfen (Steuerzeichen, voll, ungueltig). */
int lch_input_wchar(lch_input *in, unsigned u);
void lch_input_back(lch_input *in);
/* Zeile als UTF-16 (Anzeige). */
int lch_input_w(const lch_input *in, wchar_t *out, int cap);

/* ---------------- Befehle (nie gesendet) ---------------- */
enum { LCH_CMD_NONE = 0, LCH_CMD_MUTE, LCH_CMD_UNMUTE, LCH_CMD_MUTES, LCH_CMD_HELP, LCH_CMD_BAD };
/* Zeile beginnt mit '/': Befehl erkennen, arg = Name (ASCII, ohne Leerzeichen am Rand). */
int lch_cmd(const lch_input *in, char *arg, int cap);

/* ---------------- Zustand + Ablauf ---------------- */
typedef struct lch_dd { uint32_t key, nonce, seq; } lch_dd;
typedef struct lch_state {
    lch_rate rx_rate, tx_rate;
    lch_mute mute;
    lch_hist hist;
    lch_dd dd[LCH_DEDUP];
    int dd_pos;
    uint32_t nonce, seq;
    /* Zaehler (nur Zahlen, keine Inhalte) */
    uint32_t rx_ok, rx_bad, rx_rate_drop, rx_muted, rx_empty, rx_dup, tx_ok, tx_rate_drop, tx_empty;
} lch_state;
void lch_init(lch_state *s, uint32_t nonce);
/* Empfang: Paket von Absender key (vip) mit Sitzungsname pilot. 1 = in den Verlauf, 0 = verworfen. */
int lch_rx(lch_state *s, uint32_t now, uint32_t key, const char *pilot, const uint8_t *d, int len);
/* Senden: Eingabezeile -> Paket (out, cap >= LCH_PKT_MAX), eigene Zeile in den Verlauf. Rueckgabe Paketlaenge,
 * 0 = leer, -1 = Ratenbremse (nichts gesendet, Zeile bleibt). */
int lch_tx(lch_state *s, uint32_t now, const lch_input *in, const char *self_name, uint8_t *out, int cap);
/* Alles loeschen (Absturz: vor dem Minidump; Sitzungsende). */
void lch_wipe(lch_state *s);

/* ---------------- W446 STEELNET-CHAT: steelnet-weiter Chat ueber den TLS-Steuerkanal (JSON-Zeilen) ----------------
 * Senden: {"t":"chat","text":"<UTF-8>"} (kein Absender: der Dienst setzt den Pilotnamen der Sitzung). Empfang:
 * {"t":"chat","id":N,"from":"<Pilot>","text":"...","ts":N[,"hist":1]} - Text wird hier noch einmal bereinigt, der Name
 * auf druckbares ASCII gekuerzt; /mute gilt fuer beide Tabs. Eigene Zeile erst mit dem Echo des Dienstes (zeigt den
 * bereinigten Text, bestaetigt die Zustellung). Nichts davon wird geloggt. */
#define LCH_SN_LINE_MAX 1100 /* {"t":"chat","text":"..."} + 960 Byte UTF-8 bzw. 240 x 2 Byte Escape (W452) */
typedef struct lch_sn {
    lch_hist hist;          /* Verlauf des steelnet-Tabs (der Lobby-Verlauf bleibt lch_state.hist) */
    lch_rate tx_rate;       /* eigene Ratenbremse (3 je 5 s, strenger als der Dienst) */
    uint32_t rx_ok, rx_bad, rx_muted, rx_empty, rx_hist, tx_ok, tx_rate_drop, tx_empty, status_n;
} lch_sn;
/* JSON-Zeichenkette (v zeigt auf '"', ve = Ende des Wertes) streng nach UTF-8: Escapes \" \\ \/ \b \f \n \r \t
 * \uXXXX (Surrogatpaare zusammengesetzt, einzelne Surrogate verworfen), rohe Bytes >= 0x20 unveraendert (die
 * Bereinigung prueft UTF-8). Rueckgabe Byteanzahl oder -1 (kein String, kaputtes Escape, Steuerbyte, zu lang). */
int lch_json_str_u8(const char *v, const char *ve, uint8_t *out, int cap);
/* Codepunkte -> JSON-Zeichenkettenkoerper (ohne Anfuehrungszeichen, UTF-8, " \ und < 0x20 escaped). -1 = zu klein. */
int lch_json_text(const uint32_t *cps, int n, char *out, int cap);
/* Empfang einer steelnet-Zeile (Felder bereits aus dem JSON geholt). hist = Verlaufszeile des Dienstes; self_name =
 * eigener Pilotname (Zeile als eigene markieren). 1 = in den Verlauf, 0 = verworfen. */
int lch_sn_rx(lch_sn *sn, const lch_mute *mute, uint32_t now, const char *from, const uint8_t *text, int len, int hist,
              const char *self_name);
/* Senden: Eingabezeile -> JSON-Objekt ohne Zeilenumbruch (out, cap >= LCH_SN_LINE_MAX). Rueckgabe Laenge, 0 = leer,
 * -1 = Bremse. */
int lch_sn_tx(lch_sn *sn, uint32_t now, const lch_input *in, char *out, int cap);
void lch_sn_wipe(lch_sn *sn);

/* ---------------- W474 CHATTABS: Lobby-/Match-Zustand und Reiter (rein, Selbsttest) ----------------
 * Ein zentraler Zustand "in einer Lobby bzw. im Match dieser Lobby" fuer Chat-Reiter "Lobby" und Lobby-Infopanel
 * (loc_net_lobby_state). Eingaben: offene Spiel-TCP-Stroeme (das Spiel ist in einer Sitzung), Szene u16 [0x727988],
 * Seite der Szene 8 (lch_page: Text-Id der ersten Menuezeile + Zeilenzahl, Werte W460), eigene Lockstep-Rahmen in
 * den letzten 3 s bzw. Mission laut Eingabeschicht. LoC kehrt nach dem Match nicht in die Lobby zurueck: Ergebnis und
 * Nachbesprechung gehoeren noch zur Sitzung (Lobby-Zustand), erst SIGN OUT beendet sie (Stroeme zu). Reihenfolge: ohne Strom keine Lobby; Mission/Lockstep = Match;
 * Szene 8 auf MAIN MENU, MatchMaking oder Suche = keine Lobby, sonst Lobby (SESSION, Seitenwahl, VT-Auswahl,
 * Briefing); Menues ausserhalb des Hubs (Szene 0/1/3/4/5/6: Logos, Titel, Hauptmenue, Benutzerdaten) = keine
 * Lobby; jede andere Szene mit offenem Strom (Laden, Ergebnis) = Match. */
enum { LCH_SES_NONE = 0, LCH_SES_LOBBY = 1, LCH_SES_MATCH = 2 };
enum { LCH_PAGE_OTHER = 0, LCH_PAGE_MAIN = 1, LCH_PAGE_MATCH = 2, LCH_PAGE_SEARCH = 3 };
#define LCH_TID_MAIN 0x1B4   /* MISSION SELECT (MAIN MENU, 7 Zeilen) - wie LW_TID_MAIN */
#define LCH_TID_MATCH 0x3A0  /* Search Match (MatchMaking, 2 Zeilen) - wie LW_TID_MATCH */
#define LCH_TID_SEARCH 0x432 /* Suchergebnis und "Cancel?"-Dialog (1 Zeile) */
int lch_page(int scene, int text_id, int nitems);
int lch_session_from(int streams, int scene, int page, int lockstep, int rb_mission);
/* 1 = man steht selbst ausserhalb der Lobby-Seiten (MAIN MENU, MatchMaking, Suche, Menues ausserhalb des Hubs). Nur dann
 * wird der Lobby-Verlauf geleert; geht nur der letzte Gast (Ersteller allein in seiner Lobby), verschwindet der Reiter,
 * der Verlauf bleibt. */
int lch_out_of_lobby(int scene, int page);

/* Reiter in Anzeige- und Tab-Reihenfolge steelnet | Lobby | Team (Nutzer 24.11.). */
enum { LCH_TAB_SN = 0, LCH_TAB_LOBBY = 1, LCH_TAB_TEAM = 2, LCH_TAB_N = 3 };
typedef struct lch_tabin {
    int ses;            /* LCH_SES_* */
    int lobby_link;     /* Lobby-Chat technisch moeglich: Schalter loc_chat, sec ok, Mitspieler verbunden, Treffpunkt/LAN/direkt */
    int sn_ready;       /* steelnet-Chat abonniert (loc_snchat, Treffpunkt verbunden) */
    int team_known;     /* Team-Zuordnung sicher belegt (W474: nie, Teil B offen) */
    int mis_sn, mis_lobby, mis_team; /* Schluessel loc_chat_mission_* (gelten nur im Match) */
} lch_tabin;
/* Bitmaske 1 << LCH_TAB_* der verfuegbaren Reiter. Lobby/Team nur in Lobby bzw. Match; im Match zusaetzlich je
 * Schluessel loc_chat_mission_*; steelnet ueberall, im Match nur mit loc_chat_mission_sn. */
unsigned lch_tabs_avail(const lch_tabin *in);
/* Tab-Taste: naechster verfuegbarer Reiter nach cur in Reihenfolge (zyklisch); keiner sonst -> cur. */
int lch_tab_next(int cur, unsigned avail);
/* Automatischer Wechsel: Lobby neu verfuegbar -> Lobby; aktiver Reiter weg -> erster verfuegbarer; sonst cur. */
int lch_tab_auto(int cur, unsigned avail, unsigned prev_avail);
/* Verlauf leeren (Lobby verlassen: der naechste Beitritt beginnt leer). */
void lch_hist_clear(lch_hist *h);

/* ---------------- W508 CHATBAR: Chat in den schwarzen Seitenbalken (rein, Selbsttest) ----------------
 * Spielbild (bx, bw) mittig im Backbuffer der Breite bb_w (Pillarbox). Der breitere Balken (links bei Gleichstand)
 * nimmt den Chat auf, wenn er abzueglich je eines Randes (m_units * f) noch LCH_BAR_MIN_UNITS bei Standardschrift
 * fasst. Standardschrift = SB-UI-Faktor 2 (1080p); bei groesserem Faktor darf die Schrift im Balken bis auf diese
 * Groesse schrumpfen (fe in [min(f, 2), f]), damit z. B. 16:9 auf 3440x1440 noch passt. Breite = Balken minus Raender,
 * hoechstens die normale Chatbreite (LCH_W_UNITS * fe); der Chat liegt an der Bildkante (Abstand ein Rand).
 * Rueckgabe 0 = kein Balken bzw. zu schmal (heutige Lage), 1 = linker, 2 = rechter Balken; x/w in Pixeln, fe Faktor. */
#define LCH_W_UNITS 280.0f       /* normale Chatbreite (Einheiten, 1080p: 560 px) */
#define LCH_BAR_MIN_UNITS 180.0f /* Mindestbreite im Balken (Einheiten, Standardschrift 1080p: 360 px) */
#define LCH_BAR_STD_F 2.0f       /* Standardschrift (SB-UI-Faktor 1080p) */
int lch_bar_fit(unsigned bb_w, unsigned bx, unsigned bw, float f, float m_units, unsigned *x, unsigned *w, float *fe);
