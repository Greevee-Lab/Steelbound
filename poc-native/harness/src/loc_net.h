/* loc_net.h - W286 LOC-SYSLINK: XNet-/Winsock-HLE fuer Line of Contact (System Link).
 * Nur im LoC-Profil ([expert] loc_boot=1) und mit [expert] loc_net=1 (Standard).
 * Beide Fassungen (W292 LOC-COMM); mit loc_net=0 meldet LoC "Netz nicht bereit" (W275-Stubs). */
#pragma once

/* Installiert die Netz-Hooks (Bytevergleich vor dem Patchen, sonst exit 7).
 * Rueckgabe 0 = Netz aus (loc_net=0 oder keine Adresse) -> Aufrufer setzt die
 * W275-Stubs "Netz nicht bereit". */
int loc_net_install(void);
void loc_net_tick(int waited_s);
void loc_net_bye(void); /* W366: Client endet - Mitspielern BYE senden (eigener Transport) */
/* W297: Netz-Zustand fuer Testfolgen: 0 = Netz aus, 1 = Typ nicht gesehen, 2 = Typ gesehen (typ >= 256: nur Zaehler);
 * typ 512: 3 = Lockstep aktiv (eigener 0x34-Rahmen < 1 s alt). */
int loc_net_test_state(long *accepts, long *connects, int out, unsigned typ);
void loc_net_test_rearm(void); /* W375 TEST: Merkliste/Zaehler der Testfolgen neu (naechste Runde) */
/* W338 LOC-ROLLBACK-R2: Netz-Kern (Adresse, Portbasis, Mitspieler, Verzoegerungsstufe) fuer den eigenen
 * Eingabekanal; NULL = Netz aus. */
struct locnet *loc_net_core(void);
/* W353 LOC-DURCHSTICH: Netzwerte fuer das F11-Paket (Transport, Treffpunkt, Weg, Ping, Rollback) als eine Zeile;
 * nur Zustaende und Zahlen (keine Adressen, kein Raumcode, keine Eingaben). 0 = kein LoC-Netz. */
int loc_net_diag_line(char *buf, int cap);
/* W374 LOC-NET-OVERLAY (F3): Transportwerte fuer das Netz-Overlay (Abtast-Thread, hoechstens 2x/s; nur Zahlen und
 * gekuerzte Pilotnamen, keine Adressen, kein Raumcode). 0 = kein LoC-Netz (SB-Profil oder loc_net=0). */
#define LOC_NET_HUD_PEERS 8
typedef struct loc_net_hud {
    int tun;             /* 1 = eigener Transport (udp), 0 = Original-TCP */
    int known, up;       /* Mitspieler bekannt / verbunden (eigener Transport) */
    int route;           /* 0 keiner, 1 direkt, 2 Relay, 3 gemischt */
    double ping, ping_min, jitter; /* schlechtester verbundener Mitspieler (ms) */
    int loss_in, loss_out;         /* Promille, Maximum ueber die Mitspieler */
    int npeer;                     /* Zeilen in peer[] */
    struct { char name[24]; double ping; int up; } peer[LOC_NET_HUD_PEERS];
    /* W395 LOC-ROUTE: Wege des schlechtesten verbundenen Mitspielers (aktiv + gemessene Alternativen) */
    char route_name[24];           /* "direct" bzw. Relayname, leer = unbekannt */
    int nalt, nswitch;
    struct { char name[24]; int rtt, state, active; } alt[4]; /* rtt < 0 = keine Antwort; state 0 aus, 1 ?, 2 gut */
} loc_net_hud;
int loc_net_hud_read(loc_net_hud *o);
/* W440 LOC-LOBBY-MODES (loc_assist.h): wirksamer Spielmodus des laufenden Spiels (LAS_FREE in SB bzw. LoC ohne Netz,
 * LAS_SIM, LAS_ARCADE), src = Quelle (LAS_SRC_*). Logzeile LOCMODE bei jedem Wechsel. */
int loc_net_assist_mode(int *src);
/* W440: wirksamer Wert einer Hilfe (aim_assist, auto_gear) vor dem Eingabewort: in Sim 0, sonst cfg_value
 * (Steel Battalion: immer cfg_value). Die gespeicherte Einstellung bleibt unveraendert. */
int loc_assist_gate(int cfg_value);
/* W357 LOC-PAUSE-NET: 1 = Mitspieler verbunden (eigener Transport mit Verbindung, eigener Lockstep-Rahmen oder
 * Empfang auf den Spiel-Sockets in den letzten SB_PN_HOLD_MS); nur lesend, je Bild vom Spiel-Thread (Overlay). */
int loc_net_peer_live(void);
/* W447 LOC-NET-AUTO: GetTickCount des letzten eigenen Lockstep-Rahmens (0 = nie / kein LoC-Netz). */
long loc_net_last34_ms(void);
/* W447: offene Spiel-TCP-Stroeme (> 0 = Sitzung: Lobby oder Match). */
long loc_net_streams(void);
/* W350 LOC-REJOIN (DIAG, nur loc_net_diag=1): Belegung der Verbindungstabelle des Spiels je Bild lesen und bei
 * Aenderung eine Zeile LOCROSTER loggen (Plaetze, Seiten-Zaehler; keine Namen, keine Inhalte). */
void loc_net_lobby_frame(unsigned f);
