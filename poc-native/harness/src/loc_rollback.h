/* loc_rollback.h - W325 LOC-ROLLBACK-SPIKE: Machbarkeit Rollback im LoC-HLE messen
 * (loc_rb_spike=1/2, Standard 0, nur LoC-Profil + Netz-HLE). */
#pragma once
#include <stdint.h>

int loc_rb_install(void);   /* nach loc_sync_install/loc_pace_install (loc_boot.c) */
void loc_rb_tick(int waited_s); /* Watchdog-Takt (Sekunden), Logzeilen LOCRB alle 10 s */
/* W353 LOC-DURCHSTICH: Rollback-Zustand fuer das F11-Paket (nur Zahlen/Zustaende). 0 = Eingabeschicht aus. */
int loc_rb_diag(char *buf, int cap);
/* W411 LOC-FREEZE-SUSP: Buchfuehrung Render-Thread-Anhalten (Zahlen) fuer Haenger-Bericht; 0 = Ring aus. */
int loc_rb_susp_diag(char *buf, int cap);
void loc_rb_stall_diag(void); /* Watchdog-STALL: eine Zeile LOCRB: STALL-Diag */

/* W330 LOC-ROLLBACK-R1 (loc_rb=1, nur LoC-Profil). Ohne loc_rb bleiben die Zeiger 0/Null: SB-Pfad unveraendert. */
extern void (*volatile g_loc_rb_prewrite)(uintptr_t a, unsigned n); /* vor Kernel-Schreibzugriffen in Gast-Puffer */
extern int g_loc_rb_ww;                 /* 1 = NtAllocateVirtualMemory mit MEM_WRITE_WATCH reservieren */
extern volatile long g_loc_rb_memev;    /* Freigabe/Schutzwechsel im Gast-Speicher -> Bereiche neu */
/* W346 R3: Allokation im Spiel-Thread waehrend R2 (NtAllocateVirtualMemory, base 0). op 0 = beim Neurechnen aus der
 * Aufzeichnung bedienen (1 = bedient, *io = Adresse, Inhalt genullt), op 1 = Ergebnis *io aufzeichnen. NULL = aus.
 * W355: Commit in einen reservierten Bereich (base != 0): op 2 = beim Neurechnen bedienen (*io = base ein, Adresse aus,
 * frische Seiten genullt, kein Registereintrag), op 3 = Ergebnis aufzeichnen (*io = Adresse | 1 wenn frisch). */
extern int (*volatile g_loc_rb_alloc)(int op, unsigned size, unsigned type, unsigned prot, unsigned *io);
#define LOC_RB_PREWRITE(a, n) do { if (g_loc_rb_prewrite) g_loc_rb_prewrite((uintptr_t)(a), (unsigned)(n)); } while (0)
/* W367 LOC-WMA: 1 = Spiel-Thread rechnet gerade neu (Rollback-Wiedergabe); sonst 0 (auch ohne Rollback). */
int loc_rb_resim(void);
/* W390 LOC-OWN-PREDICT: 1 = der aufrufende Thread ist der Spiel-Thread im spekulativen Vorlauf (nur Anzeige, wird danach
 * verworfen; loc_rb_resim() ist dabei ebenfalls 1). HLE-Einstiege mit Wirkung nach aussen (Ton, Freigabe, Schreiben,
 * Schliessen) lassen sie dann aus. loc_rb_spec_note zaehlt solche Faelle: 1 Lesen, 2 Schreiben, 3 Schliessen,
 * 4 Datei-Info, 5 Freigabe. */
int loc_rb_spec(void);
void loc_rb_spec_note(int kind);
/* W390 DIAG (syscall_trace=2): Kernel-Aufruf je Ordinal zaehlen (Vorlauf bzw. Live-Ticks des Spiel-Threads). */
void loc_rb_sc_count(unsigned ordinal);
/* W390: F3-Overlay: 1 = Vorlauf aktiv in dieser Mission; mode 1 an, 2 Pause (zu teuer), 3 aus; Vorsprung (ms); Kosten p95 (us). */
int loc_rb_ownpred_hud(int *mode, int *lead_ms, int *cost_us);
/* W374 LOC-NET-OVERLAY (F3): Rollback-Zustand fuer das Netz-Overlay, nur vorhandene Zaehler (Lesen aus dem
 * Abtast-Thread des Overlays, hoechstens 2x/s; keine Eingaben). 0 = Eingabeschicht aus. */
typedef struct loc_rb_hud {
    int mode;            /* 0 Lobby, 1 Lockstep, 2 Rollback (R2), 3 R2-Ende (allein), 4 Angebot, 5 Warten */
    int why;             /* Rueckfall-Grund (RBN_WHY_*, bei Lockstep; Host-Grund hat Vorrang), 0 = keiner */
    const char *why_key; /* lang-Schluessel des Grundes (ovl.rb_why_*) */
    int delay;           /* Eingabeverzoegerung in Ticks (50 ms), 0 = unbekannt */
    int rtt_ms;          /* RTT des Eingabekanals (max ueber Mitspieler), <= 0 = unbekannt */
    unsigned rb_total;   /* Rollbacks gesamt */
    unsigned taken, predicted; /* Entnahmen / davon vorhergesagt (gesamt) */
    double adv;          /* geglaetteter Vorsprung in Slots (Zeitabgleich), Wert des letzten Live-Ticks */
    unsigned ts_waits;   /* Zeitabgleich: Stau-Ticks gesamt */
    int npeers;          /* Mitspieler der Mission (-1 = unbekannt) */
    unsigned depth_p95;  /* Rollback-Tiefe in Ticks, p95 der letzten 10 s (0 = keine) */
    unsigned depth_n;    /* Rollbacks in diesem Fenster */
    int tsync;           /* W372: Art des Zeitabgleichs (2 = ueber gemessenen Puffer) */
    double tsync_ms;     /* W372: Vorsprung laut Zeitabgleich (ms, + = wir liegen vorn) */
    int buf_ms;          /* W372: Puffer-Median zum Mitspieler (ms), -999 = noch unbekannt */
    int net_mode;        /* W447: gewaehlter Netzmodus 0 Auto, 1 Rollback, 2 Lockstep */
    int prof;            /* W447: Profil der laufenden Mission (RBN_PROF_* 0 fest, 1 direkt, 2 rollback), -1 = keine */
    int prof_rtt, prof_var; /* W447: Messwerte beim Missionsbeginn (ms) */
} loc_rb_hud;
int loc_rb_hud_read(loc_rb_hud *o);
/* W447 LOC-NET-AUTO: Netzmodus zur Laufzeit (ESC-Menue): 0 Auto, 1 Rollback, 2 Lockstep; wirkt ab der naechsten Mission
 * (Hallo-Wunsch). Ohne Eingabeschicht (Start mit loc_rb_input=0) nur Lockstep: Rueckgabe 0 = nicht moeglich. */
int loc_rb_net_mode(void);
int loc_rb_net_mode_set(int mode);
/* W447: Lobby-Anzeige (Panel): was Auto waehlen wuerde bzw. gewaehlt hat. 0 = keine Eingabeschicht. */
#define LOC_RB_LOBBY_PEERS 8
typedef struct loc_rb_lobby {
    int net_mode;        /* 0 Auto, 1 Rollback, 2 Lockstep */
    int in_mission;      /* 1 = Mission laeuft (R2 oder Lockstep), 0 = Lobby/Menue */
    int agreed;          /* Auto mit allen frischen Mitspielern vereinbart */
    int prof;            /* RBN_PROF_* (Vorschau in der Lobby bzw. laufende Mission) */
    int d_start, d_target; /* Verzoegerung in Ticks (Start / Ziel nach dem Ringstart) */
    int rtt, var;        /* groesste RTT (Mittel + 2 Schwankung) / Schwankung, ms (-1 = unbekannt) */
    int npeer;
    struct { int pidx, rtt, wauto; } peer[LOC_RB_LOBBY_PEERS]; /* wauto 0 Lockstep, 1 Rollback, 2 Auto */
} loc_rb_lobby;
int loc_rb_lobby_read(loc_rb_lobby *o);
/* W474 CHATTABS: 1 = Mission laut Eingabeschicht (wie loc_rb_lobby.in_mission, ohne Sperre); 0 ohne Eingabeschicht. */
int loc_rb_in_mission(void);
/* W396: wirksame Eingabeverzoegerung im Rollback (Ticks, = F3-Zeile), 0 = nicht im Rollback-Modus */
int loc_rb_eff_delay(void);
/* W469 LOC-RB-HITCH: je Lockstep-Rahmen (ls_record, Spiel-Thread): Abstand messen, bei Haenger LOCHITCH mit Aufteilung.
 * stallf = Stauframe-Zaehler des Spiels. Nur Ganzzahlen. */
void loc_rb_hitch_frame(unsigned stallf);
/* W420 LOC-CLIPDUMP: GetTickCount|1 des letzten eigenen Rahmens (FUN_0004EC80, auch im Rollback, wo kein TCP-0x34
 * gesendet wird), 0 = nie. Nur mit loc_rb_input=1 (sonst zeigt locnet.last34_ms die Mission). */
extern volatile long g_loc_rb_own_ms;
