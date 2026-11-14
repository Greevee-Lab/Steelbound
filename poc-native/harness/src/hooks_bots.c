/* hooks_bots.c - XDK-Hooks: Test-Eingaben (replay=, replay_steps-Parser und
 * -Tick, Film-Skip, aimat/aimfire-Ziel-Bot, driveto-Fahr-Bot).
 * W68 SPLIT: 1:1 aus xdk_hooks.c verschoben. */
#include "hooks_int.h"
#include "input_mouse.h" /* W79: input_source, input_focus_only */
#include "sb_overlay.h" /* W118: Overlay sperrt echte Eingabe */

/* M2b-Replay: "pkt:vk,len;..." simuliert Tastendrucke (unbeaufsichtigte
 * Tests). Max 8 Slots, einmalig geparst. */
/* W214: 192 Slots (Mehrmissions-Ketten seq, bis ~150 Schritte; vorher 128). */
#define REPLAY_MAX 192
static struct {
    int start, vk, len, logged;
} g_replay[REPLAY_MAX];
static int g_replay_n = -1;

static void replay_parse(void)
{
    const char *p;
    if (g_replay_n >= 0) {
        return;
    }
    g_replay_n = 0;
    if (!g_cfg.replay[0]) {
        return;
    }
    p = g_cfg.replay;
    while (*p && g_replay_n < REPLAY_MAX) {
        int st = 0, vk = 0, ln = 0;
        while (*p == ' ' || *p == ';') {
            p++;
        }
        if (!*p) {
            break;
        }
        st = atoi(p);
        while (*p && *p != ':') {
            p++;
        }
        if (*p == ':') {
            p++;
        }
        vk = atoi(p);
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
        ln = atoi(p);
        while (*p && *p != ';') {
            p++;
        }
        g_replay[g_replay_n].start = st;
        g_replay[g_replay_n].vk = vk;
        g_replay[g_replay_n].len = ln;
        g_replay[g_replay_n].logged = 0;
        g_replay_n++;
    }
    HLOG_I("XIN replay: %d Slots (%s)", g_replay_n, g_cfg.replay);
}

/* REPLAY-STEPS (A4, 31.10.): Eingaben an Spielzustaende statt an feste
 * Framenummern binden. Format "trigger>vk,len;...", Schritte strikt der
 * Reihe nach; ein Trigger zaehlt erst ab dem vorigen Schritt:
 *   f+N                  N Frames nach dem vorigen Schritt
 *   file:NAME[+N]        nachdem Datei NAME (Basename) geoeffnet wurde
 *   mem:0xADDR=0xVAL[+N] sobald u32 an ADDR == VAL (Spielzustand)
 *   poke:0xADDR=0xVAL[+N]  N Frames nach dem vorigen Schritt einmal u32
 *     an ADDR schreiben (A11c-Logik-Test, z. B. Missionsende erzwingen);
 *     ohne '>vk,len' (reiner Schrieb, kein Tastenslot)
 *   memall:0xA1,0xA2,..=LINIE[+N] sobald ALLE float-Werte > LINIE
 *     (W21-R2 Regelkreis, z. B. 5 Balken > 0.7); LINIE dezimal (0.7)
 *     oder Hex-Bits (0x3F333333); max. 8 Adressen
 *   epoke:IDX=HP[+N]      N Frames nach dem vorigen Schritt einmal HP
 *     (u16) an Entity IDX ([0x351828]+IDX*0x5C) schreiben (W35-Kampf-
 *     Logik-Test); ohne '>vk,len' (reiner Schrieb, kein Tastenslot)
 *   epoke:IDX,OFF=VAL[+N]  wie oben, aber u32 an +OFF (W39, z. B. Teleport
 *     unserer X-Position: epoke:42,12=0x4414C000)
 *   aimat:IDX[+N]         W59 Ziel-Bot: ab hier fuehrt der Zielhebel
 *     (aiming_x/y) geregelt auf Entity IDX nach (Peilung aus Muendung
 *     [0x3579B4] und Rumpf-Gierung [0x3521C0], Spiel-Skalen 0x260384/
 *     0x260380/0x26037C) und tippt LOCK ON (bi2), sobald der Spiel-Kandidat
 *     [0x357834]==IDX und das Lock-Ziel [0x34700C]!=IDX ist; IDX=0 = aus,
 *     IDX=999 = automatisch der naechste lebende lockbare Gegner; Ziel-
 *     punkt = Lock-Knochen (FUN_00020AE0 +0x130), Fadenkreuz-Regler per
 *     Projektion wie FUN_00014F10
 *   aimfire:R[+N]         W59 Autofeuer zum aimat-Bot: SPACE nur bei Lock
 *     oder Fadenkreuz auf dem Ziel und Distanz < R; Magazin leer ->
 *     MAGAZINE CHANGE (bi27); R=0 = aus
 *     Reiner Testpfad (wie ein Spieler, der das blinkende Fadenkreuz
 *     sieht), ohne '>vk,len'
 *   memedge:0xA1,..=LINIE[+N] W63: wie memall, feuert aber erst, nachdem seit
 *     Schrittbeginn mindestens einmal NICHT alle Werte > LINIE lagen (frische
 *     Flanke). Mission 2: die Startbalken stehen aus M1 noch verriegelt auf
 *     1.0 -> memall feuerte sofort, START vor dem Minispiel (W63_c10)
 *   rel:VK[,VK..][+N]     W63: gehaltene Tasten loslassen - alle laufenden
 *     Slots dieser VKs enden jetzt (max. 8 VKs, ohne '>vk,len'). Kampagne:
 *     die 5 Rastschalter (VK 290-294) bleiben in M1 30000 Frames an
 *     (Sauerstoff!) und muessen vor der M2-Startsequenz aus, sonst sieht die
 *     Schalterstufe keine Flanke (W63_c11/c12: kein M2-START)
 *   aimat:998             W63: wie 999, aber naechstes lebendes Missionsziel
 *     (Entity +0x4A Bit 0 gesetzt, Bit 10 frei = Zaehlregel von 0x4A820,
 *     Zielzaehler [0x356E74]); M1: Gegner-VTs 67 + 71
 *   driveto:IDX,R[,G][+N] W63 Fahr-Bot (Testpfad wie aimat, nur Controller-
 *     Eingaben): Drehhebel geregelt auf die Peilung zu Entity IDX (998 =
 *     naechstes Missionsziel), Gang G (Default 10 = 2. Gang, 9 = 1.) + Gas
 *     (rechtes Pedal), bis Distanz < R; danach Gas los + Bremse. Drehsinn
 *     des Hebels wird an der Rumpf-Gierung [0x3521C0] selbst kalibriert.
 *     IDX=0 = aus. Log: DRIVETO-Zeilen alle 100 Frames.
 * Ein ausgeloester Schritt haengt einen Slot an g_replay an (gleiche
 * Tastenlogik wie replay=). Datei-Ereignisse via replay_note_file(). */
/* A1: 128 = vollstaendiges Standard-Replay (101 Schritte) passt; wie g_replay.
 * W214: 192 fuer Mehrmissions-Ketten (campaign_bot.py seq). */
#define STEP_MAX REPLAY_MAX
u32 g_aimat_idx; /* W59: aimat-Ziel-Bot (0 = aus) */
static int g_aimat_fire; /* W59: aimfire:R = Autofeuer bis Distanz R (0 = aus) */
static float g_aimat_range = 1e9f;
u32 g_drive_idx; /* W63: driveto-Fahr-Bot (0 = aus) */
static float g_drive_r = 100.0f; /* W63: Zieldistanz */
static uint8_t g_drive_gear = 10; /* W63: Gang beim Fahren (9..13 = 1..5) */
enum { STEP_FRAMES, STEP_FILE, STEP_MEM, STEP_POKE, STEP_MEMALL, STEP_EPOKE, STEP_AIMAT, STEP_DRIVE, STEP_REL };
static struct {
    int kind, delay, vk, len;
    char name[64];
    u32 addr, val;
    u32 eoff; /* STEP_EPOKE: Entity-Offset (u32-Modus, W39) */
    int eword; /* STEP_EPOKE: 1 = u32 an +eoff, 0 = HP-Wort (W35) */
    u32 addrs[8]; /* STEP_MEMALL: Adressen */
    int naddrs;   /* STEP_MEMALL: Anzahl (0 = ungueltig) */
    float line;   /* STEP_MEMALL: Schwelle (alle Werte > line) */
    int edge;     /* W63 memedge: 1 = erst nach einem Frame mit "nicht alle > line" */
    int armed;    /* W63 memedge: Flanke scharf (Unterschreitung gesehen) */
} g_steps[STEP_MAX];
static int g_nsteps = -1, g_step_cur, g_step_hit;
static u32 g_step_hit_frame;
static volatile LONG g_file_seq;
static LONG g_step_seq0;
static char g_file_ring[64][64];

void replay_note_file(const char *path)
{
    const char *b = path, *p;
    LONG i;
    char *d;
    unsigned k;
    for (p = path; *p; p++) {
        if (*p == '\\' || *p == '/') b = p + 1;
    }
    i = InterlockedIncrement(&g_file_seq);
    d = g_file_ring[i & 63];
    for (k = 0; k < 63 && b[k]; k++) {
        char c = b[k];
        d[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    d[k] = 0;
}

static void replay_steps_parse(void)
{
    const char *p = g_cfg.replay_steps;
    if (g_nsteps >= 0) return;
    g_nsteps = 0;
    while (*p && g_nsteps < STEP_MAX) {
        char tok[160];
        unsigned n = 0;
        char *gt, *plus;
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        while (*p && *p != ';' && n < sizeof tok - 1) tok[n++] = *p++;
        tok[n] = 0;
        gt = strchr(tok, '>');
        if (!gt && strncmp(tok, "poke:", 5) != 0 && strncmp(tok, "epoke:", 6) != 0 && strncmp(tok, "aimat:", 6) != 0 && strncmp(tok, "aimfire:", 8) != 0 && strncmp(tok, "driveto:", 8) != 0 && strncmp(tok, "rel:", 4) != 0) { HLOG_W("REPLAY-STEP ohne '>': %s", tok); continue; }
        if (gt) *gt = 0; /* A11c: poke ohne '>' laesst tok ganz */
        g_steps[g_nsteps].vk = gt ? atoi(gt + 1) : 0;
        { const char *c = gt ? strchr(gt + 1, ',') : NULL;
          g_steps[g_nsteps].len = c ? atoi(c + 1) : 30; }
        g_steps[g_nsteps].naddrs = 0; /* nur STEP_MEMALL setzt >0 */
        g_steps[g_nsteps].edge = 0;
        g_steps[g_nsteps].armed = 0;
        plus = strchr(tok, '+');
        g_steps[g_nsteps].delay = plus ? atoi(plus + 1) : 0;
        if (plus) *plus = 0;
        if (strncmp(tok, "file:", 5) == 0) {
            unsigned k;
            g_steps[g_nsteps].kind = STEP_FILE;
            for (k = 0; k < 63 && tok[5 + k]; k++) {
                char c = tok[5 + k];
                g_steps[g_nsteps].name[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            g_steps[g_nsteps].name[k] = 0;
        } else if (strncmp(tok, "mem:", 4) == 0) {
            char *eq = strchr(tok, '=');
            g_steps[g_nsteps].kind = STEP_MEM;
            g_steps[g_nsteps].addr = SBA((u32)strtoul(tok + 4, NULL, 0)); /* W230: EU-Adresse -> Profil */
            g_steps[g_nsteps].val = eq ? strtoul(eq + 1, NULL, 0) : 0;
        } else if (strncmp(tok, "memall:", 7) == 0 || strncmp(tok, "memedge:", 8) == 0) {
            /* W21-R2: memall:A1,A2,..=LINIE (alle float > LINIE).
             * W63: memedge: = dto. mit frischer Flanke (s. oben). */
            char *eq = strchr(tok, '=');
            int ed = tok[3] == 'e';
            char *q = tok + (ed ? 8 : 7);
            g_steps[g_nsteps].edge = ed;
            char *end = eq ? eq : tok + strlen(tok);
            g_steps[g_nsteps].kind = STEP_MEMALL;
            g_steps[g_nsteps].naddrs = 0;
            while (q < end && g_steps[g_nsteps].naddrs < 8) {
                if (*q == ',' || *q == ' ') { q++; continue; }
                g_steps[g_nsteps].addrs[g_steps[g_nsteps].naddrs++] =
                    SBA((u32)strtoul(q, NULL, 0)); /* W230 */
                while (q < end && *q != ',') q++;
            }
            if (eq && strchr(eq + 1, '.')) {
                g_steps[g_nsteps].line = (float)strtod(eq + 1, NULL);
            } else {
                u32 lb = eq ? (u32)strtoul(eq + 1, NULL, 0) : 0;
                memcpy(&g_steps[g_nsteps].line, &lb, 4);
            }
        } else if (strncmp(tok, "poke:", 5) == 0) {
            char *eq = strchr(tok, '=');
            g_steps[g_nsteps].kind = STEP_POKE;
            g_steps[g_nsteps].addr = SBA((u32)strtoul(tok + 5, NULL, 0)); /* W230 */
            g_steps[g_nsteps].val = eq ? strtoul(eq + 1, NULL, 0) : 0;
        } else if (strncmp(tok, "epoke:", 6) == 0) {
            /* W35: epoke:IDX=HP (Entity-HP, Wort an Tabellenbasis).
             * W39: epoke:IDX,OFF=VAL (u32 an Tabellenbasis+OFF, z. B.
             *   Teleport: epoke:42,12=0x4414C000 = unser X). */
            char *eq = strchr(tok, '=');
            char *co = strchr(tok + 6, ',');
            g_steps[g_nsteps].kind = STEP_EPOKE;
            g_steps[g_nsteps].addr = strtoul(tok + 6, NULL, 0);
            g_steps[g_nsteps].val = eq ? strtoul(eq + 1, NULL, 0) : 0;
            g_steps[g_nsteps].eword = co ? 1 : 0;
            g_steps[g_nsteps].eoff = co ? strtoul(co + 1, NULL, 0) : 0;
        } else if (strncmp(tok, "aimat:", 6) == 0) {
            g_steps[g_nsteps].kind = STEP_AIMAT; /* W59 Ziel-Bot */
            g_steps[g_nsteps].addr = strtoul(tok + 6, NULL, 0);
            g_steps[g_nsteps].eword = 0;
        } else if (strncmp(tok, "aimfire:", 8) == 0) {
            g_steps[g_nsteps].kind = STEP_AIMAT; /* W59 Autofeuer */
            g_steps[g_nsteps].addr = strtoul(tok + 8, NULL, 0);
            g_steps[g_nsteps].eword = 1;
        } else if (strncmp(tok, "rel:", 4) == 0) {
            /* W63: rel:VK,VK,.. (gehaltene Tasten loslassen). */
            char *q = tok + 4;
            g_steps[g_nsteps].kind = STEP_REL;
            while (*q && g_steps[g_nsteps].naddrs < 8) {
                if (*q == ',' || *q == ' ') { q++; continue; }
                g_steps[g_nsteps].addrs[g_steps[g_nsteps].naddrs++] = (u32)strtoul(q, NULL, 0);
                while (*q && *q != ',') q++;
            }
        } else if (strncmp(tok, "driveto:", 8) == 0) {
            /* W63: driveto:IDX,R[,G] (Fahr-Bot, Gang G, Zieldistanz R). */
            char *c1 = strchr(tok + 8, ',');
            char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
            g_steps[g_nsteps].kind = STEP_DRIVE;
            g_steps[g_nsteps].addr = strtoul(tok + 8, NULL, 0);
            g_steps[g_nsteps].val = c1 ? strtoul(c1 + 1, NULL, 0) : 100u;
            g_steps[g_nsteps].eoff = c2 ? strtoul(c2 + 1, NULL, 0) : 10u;
        } else if (tok[0] == 'f') {
            g_steps[g_nsteps].kind = STEP_FRAMES;
        } else {
            HLOG_W("REPLAY-STEP unbekannter Trigger: %s", tok);
            continue;
        }
        g_nsteps++;
    }
    if (g_nsteps) {
        HLOG_I("REPLAY-STEPS: %d Schritte", g_nsteps);
    }
}

/* W63: movie_skip=2 ueberspringt zusaetzlich jeden spaeteren Film (*.bik
 * im Datei-Ring, z. B. Story-Film opj.bik nach Mission 1, Briefing
 * bri01.bik vor Mission 2) mit je einem neuen 3x-L-Zyklus.
 * W14 FILMTEMPO (movie_skip): Briefing-Film wie ein Spieler per START
 * ueberspringen (schneller Testpfad; run_once.ps1 setzt movie_skip=1,
 * -film schaltet ab). Erkennung: s00ev01j.bik im Datei-Ring; dann 2x
 * 'L' (bi2, Skip-Taste per Disasm 0x46F17/0x48D4C) ueber g_replay-Slots
 * (gleicher Eingabepfad wie REPLAY-STEPS).
 * Begrenzt auf 3 Tastendruecke (danach nie wieder, kein Pause-Risiko). */
static int g_skip_n;
static u32 g_skip_f0;
static LONG g_skip_seq; /* W63: movie_skip=2 - zuletzt behandelter Datei-Eintrag */
static void replay_movieskip_tick(void)
{
    static const int at[3] = { 60, 150, 300 };
    LONG q, last;
    int s;
    if (!g_cfg.movie_skip)
        return;
    if (g_cfg.movie_skip >= 2 && g_skip_n >= 3) {
        /* W63: movie_skip=2 - jeder weitere Film (*.bik, z. B. opj.bik
         * nach dem M1-Sieg, bri01.bik vor Mission 2) bekommt einen neuen
         * Skip-Zyklus (3x L). */
        last = g_file_seq;
        if (last - g_skip_seq > 64) g_skip_seq = last - 64;
        for (q = g_skip_seq + 1; q <= last; q++) {
            size_t ln = strlen(g_file_ring[q & 63]);
            if (ln > 4 && strcmp(g_file_ring[q & 63] + ln - 4, ".bik") == 0) {
                g_skip_n = 0;
                g_skip_f0 = g_frames;
                HLOG_I("REPLAY-SKIP: Film %s erkannt bei Frame %u", g_file_ring[q & 63], g_frames);
                break;
            }
        }
        g_skip_seq = last;
    }
    if (g_skip_n >= 3)
        return;
    if (!g_skip_f0) {
        int seen = 0;
        last = g_file_seq;
        for (q = last > 64 ? last - 64 : 0; q <= last; q++) {
            size_t ln = strlen(g_file_ring[q & 63]);
            if (strcmp(g_file_ring[q & 63], "s00ev01j.bik") == 0 ||
                (g_cfg.movie_skip >= 2 && ln > 4 &&
                 strcmp(g_file_ring[q & 63] + ln - 4, ".bik") == 0)) {
                seen = 1; /* W63: movie_skip=2 - auch erster Film beliebig */
                break;
            }
        }
        if (!seen)
            return;
        g_skip_f0 = g_frames;
        g_skip_seq = last;
        HLOG_I("REPLAY-SKIP: Film erkannt bei Frame %u", g_frames);
    }
    s = g_skip_n;
    if (g_frames < g_skip_f0 + (u32)at[s] + (u32)(g_cfg.movie_skip_delay > 0 ? g_cfg.movie_skip_delay : 0))
        return; /* W137 movie_skip_delay: Film-Messfenster vor dem Skip */
    if (g_replay_n < 0)
        g_replay_n = 0;
    /* 'L' (bi2, Lock-On): Film-Skip = 48CF0(port=1, gruppe=0, button=1)
     * = Bit 1 an Out+0 (Disasm 0x48D4C/0x48F19), ab [0x3B8AB0]>60 (~2 s).
     * Der Mapper setzt Out+0 Bit 1 fuer bi2 (W11_F: L->+0 OR 02); START
     * (bi6), SPACE (bi0) und 'F' (bi1) skippen nicht (skip1/2/3, Kette).
     * 'L' in der Mission = Lock-On (harmlos, keine Pause). */
    if (g_replay_n < REPLAY_MAX) {
        g_replay[g_replay_n].start = (int)g_frames;
        g_replay[g_replay_n].vk = 76; /* 'L' = bi2 */
        g_replay[g_replay_n].len = 60;
        g_replay[g_replay_n].logged = 0;
        g_replay_n++;
    }
    HLOG_I("REPLAY-SKIP %d/3: L(bi2) bei Frame %u", s + 1, g_frames);
    g_skip_n++;
}

void replay_steps_tick(void)
{
    replay_movieskip_tick(); /* W14: Film-Skip (Testpfad) */
    if (g_cfg.rt_from_film > 0 && g_cfg.vblank_hz <= 0) {
        /* TEST(PERF-BENCH,W137): Testtakt bis zum ersten Film, dann Echtzeit
         * (Film, Missions-Load und Mission in Originaltakt messen). */
        LONG q, last = g_file_seq;
        for (q = last > 64 ? last - 63 : 1; q <= last; q++) {
            size_t ln = strlen(g_file_ring[q & 63]);
            if (ln > 4 && strcmp(g_file_ring[q & 63] + ln - 4, ".bik") == 0) {
                extern void sb_timing_go_realtime(int hz);
                HLOG_I("RTFILM: Film %s -> Echtzeit", g_file_ring[q & 63]);
                sb_timing_go_realtime(g_cfg.rt_from_film);
                if (g_cfg.rt_interp_fps > 0) { sb_ovl_request_fps(g_cfg.rt_interp_fps); }
                break;
            }
        }
    }
    replay_steps_parse();
    replay_parse();
    while (g_step_cur < g_nsteps) {
        int s = g_step_cur;
        if (!g_step_hit) {
            int hit = 0;
            if (g_steps[s].kind == STEP_FRAMES || g_steps[s].kind == STEP_POKE || g_steps[s].kind == STEP_EPOKE || g_steps[s].kind == STEP_AIMAT || g_steps[s].kind == STEP_DRIVE || g_steps[s].kind == STEP_REL) {
                hit = 1;
            } else if (g_steps[s].kind == STEP_FILE) {
                LONG q, last = g_file_seq;
                if (last - g_step_seq0 > 64) g_step_seq0 = last - 64;
                for (q = g_step_seq0 + 1; q <= last; q++) {
                    if (strcmp(g_file_ring[q & 63], g_steps[s].name) == 0) {
                        hit = 1;
                        break;
                    }
                }
            } else {
                __try {
                    if (g_steps[s].kind == STEP_MEMALL) {
                        int k;
                        hit = g_steps[s].naddrs > 0 ? 1 : 0;
                        for (k = 0; k < g_steps[s].naddrs; k++) {
                            float fv = *(volatile float *)(uintptr_t)g_steps[s].addrs[k];
                            if (!(fv > g_steps[s].line)) { hit = 0; break; }
                        }
                        if (g_steps[s].edge && !g_steps[s].armed) {
                            /* W63 memedge: erst scharf nach Unterschreitung */
                            if (!hit && g_steps[s].naddrs > 0) g_steps[s].armed = 1;
                            hit = 0;
                        }
                    } else {
                        hit = *(volatile u32 *)(uintptr_t)g_steps[s].addr == g_steps[s].val;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    hit = 0;
                }
            }
            if (!hit) return;
            g_step_hit = 1;
            g_step_hit_frame = g_frames;
        }
        if (g_frames < g_step_hit_frame + (u32)g_steps[s].delay) return;
        if (g_steps[s].kind == STEP_POKE) {
            __try {
                *(volatile u32 *)(uintptr_t)g_steps[s].addr = g_steps[s].val;
                HLOG_I("REPLAY-POKE %d/%d: [0x%X]=0x%X bei Frame %u",
                       s + 1, g_nsteps, g_steps[s].addr, g_steps[s].val,
                       g_frames);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                HLOG_W("REPLAY-POKE %d/%d: [0x%X] nicht schreibbar",
                       s + 1, g_nsteps, g_steps[s].addr);
            }
            g_step_cur++;
            g_step_hit = 0;
            g_step_seq0 = g_file_seq;
            continue;
        }
        if (g_steps[s].kind == STEP_AIMAT) {
            if (g_steps[s].eword) {
                g_aimat_fire = g_steps[s].addr != 0;
                g_aimat_range = g_steps[s].addr ? (float)g_steps[s].addr : 1e9f;
                HLOG_I("REPLAY-AIMFIRE %d/%d: Autofeuer %s (Distanz < %u) bei Frame %u",
                       s + 1, g_nsteps, g_aimat_fire ? "an" : "aus",
                       g_steps[s].addr, g_frames);
            } else {
                g_aimat_idx = g_steps[s].addr;
                HLOG_I("REPLAY-AIMAT %d/%d: Ziel-Bot -> Entity %u bei Frame %u",
                       s + 1, g_nsteps, g_aimat_idx, g_frames);
            }
            g_step_cur++;
            g_step_hit = 0;
            g_step_seq0 = g_file_seq;
            continue;
        }
        if (g_steps[s].kind == STEP_REL) {
            int k, i2, nrel = 0;
            for (i2 = 0; i2 < g_replay_n; i2++) {
                for (k = 0; k < g_steps[s].naddrs; k++) {
                    if (g_replay[i2].vk == (int)g_steps[s].addrs[k] &&
                        (int)g_frames >= g_replay[i2].start &&
                        (int)g_frames < g_replay[i2].start + g_replay[i2].len) {
                        g_replay[i2].len = (int)g_frames - g_replay[i2].start;
                        nrel++;
                    }
                }
            }
            HLOG_I("REPLAY-REL %d/%d: %d gehaltene Slots losgelassen bei Frame %u",
                   s + 1, g_nsteps, nrel, g_frames);
            g_step_cur++;
            g_step_hit = 0;
            g_step_seq0 = g_file_seq;
            continue;
        }
        if (g_steps[s].kind == STEP_DRIVE) {
            g_drive_idx = g_steps[s].addr;
            g_drive_r = (float)g_steps[s].val;
            g_drive_gear = (uint8_t)((g_steps[s].eoff >= 7u && g_steps[s].eoff <= 13u) ? g_steps[s].eoff : 10u);
            HLOG_I("REPLAY-DRIVETO %d/%d: Fahr-Bot -> Entity %u bis %.0f (Gang %u) bei Frame %u",
                   s + 1, g_nsteps, g_drive_idx, (double)g_drive_r, g_drive_gear, g_frames);
            g_step_cur++;
            g_step_hit = 0;
            g_step_seq0 = g_file_seq;
            continue;
        }
        if (g_steps[s].kind == STEP_EPOKE) {
            /* W35: Entity-HP schreiben (Tabellenbasis dynamisch). */
            __try {
                u32 base = *(volatile u32 *)(uintptr_t)SBA(0x351828u);
                if (g_steps[s].eword) {
                    /* W39: u32-Wort an +eoff (z. B. Position). */
                    volatile u32 *w = (volatile u32 *)(uintptr_t)(base + g_steps[s].addr * 0x5Cu + g_steps[s].eoff);
                    u32 old = *w;
                    *w = g_steps[s].val;
                    HLOG_I("REPLAY-EPOKE %d/%d: ent[%u]+0x%X 0x%X->0x%X bei Frame %u",
                           s + 1, g_nsteps, g_steps[s].addr, g_steps[s].eoff,
                           old, g_steps[s].val, g_frames);
                } else {
                    volatile unsigned short *hp = (volatile unsigned short *)(uintptr_t)(base + g_steps[s].addr * 0x5Cu);
                    unsigned old = *hp;
                    *hp = (unsigned short)(g_steps[s].val & 0xFFFFu);
                    HLOG_I("REPLAY-EPOKE %d/%d: ent[%u].hp %u->%u bei Frame %u",
                           s + 1, g_nsteps, g_steps[s].addr, old,
                           g_steps[s].val & 0xFFFFu, g_frames);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                HLOG_W("REPLAY-EPOKE %d/%d: ent[%u] nicht schreibbar",
                       s + 1, g_nsteps, g_steps[s].addr);
            }
            g_step_cur++;
            g_step_hit = 0;
            g_step_seq0 = g_file_seq;
            continue;
        }
        if (g_replay_n < 0) g_replay_n = 0;
        if (g_replay_n < REPLAY_MAX) {
            g_replay[g_replay_n].start = (int)g_frames;
            g_replay[g_replay_n].vk = g_steps[s].vk;
            g_replay[g_replay_n].len = g_steps[s].len;
            g_replay[g_replay_n].logged = 0;
            g_replay_n++;
        }
        HLOG_I("REPLAY-STEP %d/%d: vk=%d len=%d bei Frame %u",
               s + 1, g_nsteps, g_steps[s].vk, g_steps[s].len, g_frames);
        if (g_steps[s].kind == STEP_MEMALL) {
            /* W21-R2: ausloesende Werte einmal loggen (Regelkreis-Beleg). */
            float vv[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            int k;
            __try {
                for (k = 0; k < g_steps[s].naddrs; k++)
                    vv[k] = *(volatile float *)(uintptr_t)g_steps[s].addrs[k];
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
            HLOG_I("REPLAY-ALL %d/%d: %.4f %.4f %.4f %.4f %.4f > %.4f bei Frame %u",
                   s + 1, g_nsteps, vv[0], vv[1], vv[2], vv[3], vv[4],
                   g_steps[s].line, g_frames);
        }
        g_step_cur++;
        g_step_hit = 0;
        g_step_seq0 = g_file_seq;
    }
}

/* A8: Replay aktiv = Slots vorhanden oder Steps konfiguriert (Steps haengen
 * Slots erst zur Laufzeit an, darum zaehlt auch die blosse Konfiguration).
 * Echte Tastatur/Maus nur ohne Replay (M2b: Phantom-Inputs verfaelschen
 * unbeaufsichtigte Tests). */
int input_replay_active(void)
{
    replay_parse();
    return g_replay_n != 0 || g_cfg.replay_steps[0] != '\0' ||
        g_cfg.movie_skip;
}

int key_down(int vk)
{
    int i;
    replay_parse();
    /* M2b: Bei aktivem Replay echte Tastatur ignorieren (Phantom-Inputs
     * durch parallele PC-Nutzung verfaelschen unbeaufsichtigte Tests). */
    if (!input_replay_active()) {
        /* M2c: Record-Mode (Tastenwechsel + Frame fuer Replay-String). */
        static int rec_vk[8]; static unsigned rec_start[8]; static int rec_n;
        /* W79 input_source=1: Fenster-Nachrichten statt GetAsyncKeyState. */
        int down = g_cfg.input_source == 1 ? imsg_key(vk, g_frames)
                                           : ((GetAsyncKeyState(vk) & 0x8000) != 0);
        int i;
        /* W79 input_focus_only: echte Tasten nur bei Fokus des Clients. */
        if (down && g_cfg.input_focus_only && !imouse_has_focus())
            down = 0;
        /* W118 REM-OVERLAY: offenes Overlay -> echte Tasten nicht ans Spiel. */
        if (down && g_cfg.overlay && sb_ovl_input_blocked())
            down = 0;
        for (i = 0; i < rec_n; i++) {
            if (rec_vk[i] == vk) break;
        }
        if (down && i == rec_n && rec_n < 8) {
            rec_vk[rec_n] = vk; rec_start[rec_n] = g_frames; rec_n++;
        } else if (!down && i < rec_n) {
            HLOG_I("XIN record: %u:%d,%u", rec_start[i], vk,
                   g_frames - rec_start[i]);
            rec_vk[i] = rec_vk[rec_n - 1];
            rec_start[i] = rec_start[rec_n - 1]; rec_n--;
        }
        return down;
    }
    /* M2c: Paket-Zaehler (Crash-Run-Setup: erreicht die Mission). */
    /* M2c: Frame-Zaehler (Pakete laufen timing-abhaengig 4x schneller). */
    for (i = 0; i < g_replay_n; i++) {
        if (vk == g_replay[i].vk &&
            (int)g_frames >= g_replay[i].start &&
            (int)g_frames < g_replay[i].start + g_replay[i].len) {
            if (!g_replay[i].logged) {
                g_replay[i].logged = 1;
                HLOG_I("XIN replay: vk=0x%X ab frame=%u (%d lang)",
                       vk, g_frames, g_replay[i].len);
            }
            return 1;
        }
    }
    return 0;
}

/* W59 aimat-Ziel-Bot: Zielhebel geregelt auf Entity g_aimat_idx (Testpfad).
 * Missions-Kette (Disasm): FUN_00037290 liest Aim per 48CF0(1,0,18/19) ->
 * [0x356E1C] = OutX*[0x260384] (Gier-Offset rad), [0x356D38] = OutY*
 * ([0x356E20]?[0x260380]:[0x26037C]) (Nick); FUN_00034BE0 glaettet ->
 * Zielpunkt = Muendung + R*(sin(hy+sx)cos p, -sin p, cos(hy+sx)cos p),
 * hy = [0x3521C0]. Mapper 0x4A05A: OutX = ([0x355F00]-((0x7FFF-ax16)>>8))*
 * [0x25FCD4]/[0x354F90], OutY = (((0x7FFF-ay16)>>8)-[0x35180C])*[0x25FCD4]/
 * [0x347830]. Daraus der benoetigte Hebelwert (aiming_x/y = High-Byte). */
static unsigned g_aimat_lockf, g_aimat_logf;
static float aimat_rdf(u32 a, float dflt)
{
    float f = dflt;
    __try { f = *(volatile float *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { f = dflt; }
    if (!(f == f) || f > 1e6f || f < -1e6f) f = dflt;
    return f;
}
/* Projektion wie das Spiel (FUN_00014F10 -> D3DXVec3Project 0x193B7C mit
 * World [0x358070], View [0x357E60], Proj [0x3B6560], Viewport 0x293938):
 * liefert die Schirmposition, die FUN_0002E350 mit dem Fadenkreuz
 * [0x358560/64] vergleicht (Radius 48 px [0x25FF14]). 0 = hinter Kamera. */
static int aimat_project(const float v[3], float *sx, float *sy)
{
    const float *w = (const float *)(uintptr_t)SBA(0x358070u);
    const float *vw = (const float *)(uintptr_t)SBA(0x357E60u);
    const float *pr = (const float *)(uintptr_t)SBA(0x3B6560u);
    const u32 *vp = (const u32 *)(uintptr_t)SBA(0x293938u);
    float a[4], b[4], c[4];
    int i;
    for (i = 0; i < 4; i++)
        a[i] = v[0] * w[i] + v[1] * w[4 + i] + v[2] * w[8 + i] + w[12 + i];
    for (i = 0; i < 4; i++)
        b[i] = a[0] * vw[i] + a[1] * vw[4 + i] + a[2] * vw[8 + i] + a[3] * vw[12 + i];
    for (i = 0; i < 4; i++)
        c[i] = b[0] * pr[i] + b[1] * pr[4 + i] + b[2] * pr[8 + i] + b[3] * pr[12 + i];
    if (!(c[3] > 0.001f)) return 0;
    *sx = (float)vp[0] + (1.0f + c[0] / c[3]) * (float)vp[2] * 0.5f;
    *sy = (float)vp[1] + (1.0f - c[1] / c[3]) * (float)vp[3] * 0.5f;
    return 1;
}
/* W63: naechstes lebendes Missionsziel (Zaehlregel 0x4A820: +0x4A Bit 0
 * gesetzt, Bit 10 frei; HP 1..0x7FFE) zur Position (mx, mz), ohne uns.
 * 0 = keins (alle Ziele tot oder Tabelle ungueltig). Aufrufer im __try. */
static u32 campaign_target(u32 tab, u32 n, float mx, float mz)
{
    u32 ours = *(volatile uint16_t *)(uintptr_t)SBA(0x356E2Cu), i, best = 0;
    float bd = 1e30f;
    for (i = 1; i < n && i < 4000u; i++) {
        u32 b = tab + i * 0x5Cu;
        uint16_t hp = *(volatile uint16_t *)(uintptr_t)b;
        uint16_t f4a = *(volatile uint16_t *)(uintptr_t)(b + 0x4A);
        float qx, qz;
        if (i == ours || hp == 0 || hp >= 0x7FFF) continue;
        if (!(f4a & 1u) || (f4a & 0x400u)) continue;
        qx = *(volatile float *)(uintptr_t)(b + 0x0C) - mx;
        qz = *(volatile float *)(uintptr_t)(b + 0x14) - mz;
        if (qx * qx + qz * qz < bd) { bd = qx * qx + qz * qz; best = i; }
    }
    return best;
}
static u32 g_aimat_cur;          /* aktuelles Ziel (auto-Modus) */
static float g_aimat_cx, g_aimat_cy; /* Schirm-Regler-Korrektur (Out-Einheiten) */
static unsigned g_aimat_relf;
void aimat_apply(sbc_in_t *s)
{
    __try {
        u32 tab = *(volatile u32 *)(uintptr_t)SBA(0x351828u);
        u32 n = *(volatile u32 *)(uintptr_t)SBA(0x352288u);
        u32 idx = g_aimat_idx, eb, bone;
        float mx = aimat_rdf(SBA(0x3579B4u), 0), my = aimat_rdf(SBA(0x3579B8u), 0);
        float mz = aimat_rdf(SBA(0x3579BCu), 0), hy = aimat_rdf(SBA(0x3521C0u), 0);
        float tp[3], dx, dy, dz, dist, d, p, kx, ky, sx, sy, ox, oy;
        float tsx = 0, tsy = 0, chx, chy, smx, smy;
        int ax, ay, onscr = 0, aligned;
        u32 cand, lck, wp, slot, ammo = 0, mags = 0;
        if (!tab || n > 100000u) return;
        /* W63: nur in der Missionsschleife (nach Missionsende ist die
         * Entity-Tabelle freigegeben -> Muell-Ziele, W63_c5). */
        if (g_cfg.aimat_antiwindup && *(volatile u32 *)(uintptr_t)SBA(0x356F58u) != 1u) return;
        if (idx == 998u) {
            /* W63: naechstes lebendes Missionsziel (M1: 71, danach 67). */
            idx = campaign_target(tab, n, mx, mz);
            if (!idx) return;
        } else if (idx == 999u) {
            /* auto: naechster lebender, lockbarer Gegner (Kriterien wie
             * FUN_0002E350: HP>0, Flags Bit19 gesetzt, Bit21 frei), ohne uns. */
            u32 ours = *(volatile uint16_t *)(uintptr_t)SBA(0x356E2Cu), i, best = 0;
            float bd = 1e30f;
            for (i = 1; i < 400u && i < n; i++) {
                u32 b = tab + i * 0x5Cu, fl;
                uint16_t hp = *(volatile uint16_t *)(uintptr_t)b;
                float qx, qz;
                if (i == ours || hp == 0 || hp >= 0x7FFF) continue;
                fl = *(volatile u32 *)(uintptr_t)(b + 0x28);
                if (!(fl & 0x80000u) || (fl & 0x200000u)) continue;
                qx = *(volatile float *)(uintptr_t)(b + 0x0C) - mx;
                qz = *(volatile float *)(uintptr_t)(b + 0x14) - mz;
                if (qx * qx + qz * qz < bd) { bd = qx * qx + qz * qz; best = i; }
            }
            if (!best) return;
            idx = best;
        }
        if (n < idx + 1) return;
        if (idx != g_aimat_cur) {
            g_aimat_cur = idx;
            g_aimat_cx = g_aimat_cy = 0.0f;
            HLOG_I("AIMAT Ziel -> %u bei f=%u", idx, g_frames);
        }
        eb = tab + idx * 0x5Cu;
        if (*(volatile uint16_t *)(uintptr_t)eb == 0) return; /* tot */
        /* Zielpunkt = Lock-Knochen wie FUN_00020AE0 (+0x130): Modell [e+0x54]
         * + Knochen-Index aus Typtabelle [0x4CA034]+[e+0x34]*0x4E0+4. */
        tp[0] = *(volatile float *)(uintptr_t)(eb + 0x0C);
        tp[1] = *(volatile float *)(uintptr_t)(eb + 0x10) + 2.0f;
        tp[2] = *(volatile float *)(uintptr_t)(eb + 0x14);
        {
            u32 mdl = *(volatile u32 *)(uintptr_t)(eb + 0x54);
            u32 ty = *(volatile uint16_t *)(uintptr_t)(eb + 0x34);
            u32 tt = *(volatile u32 *)(uintptr_t)SBA(0x4CA034u);
            if (mdl > 0x10000u && ty != 0xFFFFu && tt > 0x10000u) {
                bone = *(volatile uint8_t *)(uintptr_t)(tt + ty * 0x4E0u + 4u);
                tp[0] = *(volatile float *)(uintptr_t)(mdl + bone * 0x1A0u + 0x130u);
                tp[1] = *(volatile float *)(uintptr_t)(mdl + bone * 0x1A0u + 0x134u);
                tp[2] = *(volatile float *)(uintptr_t)(mdl + bone * 0x1A0u + 0x138u);
            }
        }
        dx = tp[0] - mx; dy = tp[1] - my; dz = tp[2] - mz;
        dist = sqrtf(dx * dx + dz * dz);
        d = (float)atan2((double)dx, (double)dz) - hy;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        p = -(float)atan2((double)dy, (double)(dist > 0.1f ? dist : 0.1f));
        sx = aimat_rdf(SBA(0x260384u), -0.015f);
        sy = (*(volatile uint16_t *)(uintptr_t)SBA(0x356E20u)) ?
             aimat_rdf(SBA(0x260380u), -0.0061359f) : aimat_rdf(SBA(0x26037Cu), 0.0061359f);
        kx = aimat_rdf(SBA(0x25FCD4u), 1.0f) / aimat_rdf(SBA(0x354F90u), 1.0f);
        ky = aimat_rdf(SBA(0x25FCD4u), 1.0f) / aimat_rdf(SBA(0x347830u), 1.0f);
        if (!(kx > 0.05f && kx < 50.0f)) kx = 1.0f;
        if (!(ky > 0.05f && ky < 50.0f)) ky = 1.0f;
        /* Schirm-Regler: Fadenkreuz [0x358560/64] auf die projizierte
         * Zielposition ziehen (Vorzeichen belegt W59_aim1: Out_x+ -> ch.x+,
         * Out_y+ -> ch.y+). Integrator, langsam gegen die Spielglaettung
         * (k=0.0625/Frame, FUN_00034BE0). */
        chx = aimat_rdf(SBA(0x358560u), 0);
        chy = aimat_rdf(SBA(0x358564u), 0);
        smx = aimat_rdf(SBA(0x356D08u), 0);
        smy = aimat_rdf(SBA(0x3521BCu), 0);
        onscr = aimat_project(tp, &tsx, &tsy);
        if (g_cfg.aimat_antiwindup && !(onscr && chx > 1.0f && chy > 1.0f &&
                                        chx < 511.0f && chy < 479.0f)) {
            /* W63 Anti-Windup: Fadenkreuz ausserhalb des Schirms (Zielwechsel
             * bei drehendem Rumpf, W63_c1: ch.x=-534, Integrator auf -40
             * eingefroren) -> Korrektur abbauen statt einfrieren; die
             * Vorsteuerung (Peilung) zieht das Fadenkreuz zurueck. */
            g_aimat_cx *= 0.9f;
            g_aimat_cy *= 0.9f;
        } else if (onscr && chx > 1.0f && chy > 1.0f) {
            float ex = tsx - chx, ey = tsy - chy;
            float lim = g_cfg.aimat_antiwindup ? 60.0f : 200.0f; /* W63: nur Feinkorrektur */
            if (ex > lim) ex = lim;
            if (ex < -lim) ex = -lim;
            if (ey > lim) ey = lim;
            if (ey < -lim) ey = -lim;
            g_aimat_cx += 0.006f * ex;
            g_aimat_cy += 0.015f * ey;
            if (g_aimat_cx > 40.0f) g_aimat_cx = 40.0f;
            if (g_aimat_cx < -40.0f) g_aimat_cx = -40.0f;
            if (g_aimat_cy > 40.0f) g_aimat_cy = 40.0f;
            if (g_aimat_cy < -40.0f) g_aimat_cy = -40.0f;
        }
        ox = d / sx + g_aimat_cx;   /* benoetigtes Out+0x0C */
        oy = p / sy + g_aimat_cy;   /* benoetigtes Out+0x0D */
        if (ox > 127.0f) ox = 127.0f;
        if (ox < -127.0f) ox = -127.0f;
        if (oy > 127.0f) oy = 127.0f;
        if (oy < -127.0f) oy = -127.0f;
        ax = 0x7F + (int)lroundf(ox / kx);
        ay = 0x7F - (int)lroundf(oy / ky);
        if (ax < 0) ax = 0;
        if (ax > 0xFF) ax = 0xFF;
        if (ay < 0) ay = 0;
        if (ay > 0xFF) ay = 0xFF;
        s->aiming_x = (uint8_t)ax;
        s->aiming_y = (uint8_t)ay;
        /* LOCK ON tippen (Flanke 48CF0(1,6,1)): 3 Frames an, >=6 Frames aus. */
        cand = *(volatile u32 *)(uintptr_t)SBA(0x357834u);
        lck = *(volatile u32 *)(uintptr_t)SBA(0x34700Cu);
        if (g_frames - g_aimat_lockf < 3u) {
            s->buttons[0] |= (1u << 2);
        } else if (g_frames - g_aimat_lockf >= 9u && lck != idx &&
                   (cand == idx || lck != 0)) {
            g_aimat_lockf = g_frames;
            s->buttons[0] |= (1u << 2);
            HLOG_I("AIMAT LOCK-Tipp f=%u cand=%u lock=%u ziel=%u dist=%.1f",
                   g_frames, cand, lck, idx, dist);
        }
        /* Autofeuer (aimfire:1): Hauptwaffe nur, wenn gelockt oder das
         * Fadenkreuz im Lock-Radius auf dem Ziel liegt; Magazin leer ->
         * MAGAZINE CHANGE (bi27, Flanke 48CF0(1,6,0) -> 0x181A20). */
        wp = *(volatile u32 *)(uintptr_t)SBA(0x355000u);
        slot = *(volatile u32 *)(uintptr_t)SBA(0x356C94u);
        if (wp > 0x10000u && slot < 4u) {
            ammo = *(volatile uint16_t *)(uintptr_t)(wp + 0x12u + slot * 2u);
            mags = *(volatile uint16_t *)(uintptr_t)(SBA(0x346FFAu) + slot * 2u);
        }
        aligned = lck == idx ||
                  (onscr && fabsf(tsx - chx) < 24.0f && fabsf(tsy - chy) < 24.0f);
        if (g_aimat_fire) {
            if (ammo == 0 && mags != 0) {
                if (g_frames - g_aimat_relf < 3u) {
                    s->buttons[1] |= (1u << (27 - 16));
                } else if (g_frames - g_aimat_relf >= 60u) {
                    g_aimat_relf = g_frames;
                    s->buttons[1] |= (1u << (27 - 16));
                    HLOG_I("AIMAT MAGAZIN f=%u mags=%u", g_frames, mags);
                }
            } else if (aligned && ammo != 0 && dist < g_aimat_range) {
                s->buttons[0] |= 1u; /* bi0 Hauptwaffe */
            }
        }
        if (g_frames - g_aimat_logf >= 100u) {
            /* Lock-Diagnose: Kandidaten-Vorbedingungen aus FUN_0002E350 -
             * Schleifenlaenge [0x356E2E] (ab [0x356E2C]) und Karten-/Zellen-
             * Test FUN_00021EB0(x, z, 0) fuer die Ziel-Position (reine
             * Abfrage, Spiel-Thread). */
            typedef int (__cdecl *fn21eb0_t)(float, float, int);
            int c21 = -1;
            short cnt2e = *(volatile short *)(uintptr_t)SBA(0x356E2Eu);
            __try {
                c21 = ((fn21eb0_t)(uintptr_t)SBA(0x21EB0u))(
                    *(volatile float *)(uintptr_t)(eb + 0x0C),
                    *(volatile float *)(uintptr_t)(eb + 0x14), 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) { c21 = -2; }
            /* Zellen-Gitter wie FUN_00021EB0 (bl=0-Pfad 0x21FE1): 128x128
             * Zellen a 4 Byte ab [0x35149C], Mitte +0x8100 = eigene Zelle
             * ([0x35206C]/[0x356D50]); Index = (int)(x*0.2) - cx -
             * 128*((int)(z*-0.2) + cz); Byte 0 != 0 = Zelle aktiv. */
            {
                u32 P = *(volatile u32 *)(uintptr_t)SBA(0x35149Cu);
                int cxg = *(volatile int *)(uintptr_t)SBA(0x35206Cu);
                int czg = *(volatile int *)(uintptr_t)SBA(0x356D50u);
                float ex0 = *(volatile float *)(uintptr_t)(eb + 0x0C);
                float ez0 = *(volatile float *)(uintptr_t)(eb + 0x14);
                int ti = (int)(ex0 * 0.2f) - cxg - (((int)(ez0 * -0.2f) + czg) << 7);
                int oi = (int)(mx * 0.2f) - cxg - (((int)(mz * -0.2f) + czg) << 7);
                int tv = -1, ov = -1, nz = 0, k;
                if (P > 0x10000u) {
                    tv = *(volatile uint8_t *)(uintptr_t)(P + 0x8100u + (u32)(ti * 4));
                    ov = *(volatile uint8_t *)(uintptr_t)(P + 0x8100u + (u32)(oi * 4));
                    for (k = 0; k < 128 * 128; k++)
                        if (*(volatile uint8_t *)(uintptr_t)(P + (u32)k * 4u)) nz++;
                }
                HLOG_I("AIMAT LOCKDIAG f=%u ziel=%u cnt2e=%d c21eb0=%d fl=0x%X b48=0x%02X b4b=0x%02X "
                       "grid=0x%X c=%d,%d zelle(ziel)=%d[%d] zelle(wir)=%d[%d] aktiv=%d/16384",
                       g_frames, idx, (int)cnt2e, c21, *(volatile u32 *)(uintptr_t)(eb + 0x28),
                       *(volatile uint8_t *)(uintptr_t)(eb + 0x48),
                       *(volatile uint8_t *)(uintptr_t)(eb + 0x4B), P, cxg, czg, tv, ti, ov, oi, nz);
            }
            g_aimat_logf = g_frames;
            HLOG_I("AIMAT f=%u ziel=%u hp=%u dist=%.1f d=%.3f p=%.3f hy=%.3f ax=%02X ay=%02X "
                   "c=%.1f,%.1f sm=%.3f,%.3f ts=%.1f,%.1f(%d) ch=%.1f,%.1f cand=%u lock=%u "
                   "am=%u mg=%u al=%d",
                   g_frames, idx, *(volatile uint16_t *)(uintptr_t)eb, dist, d, p, hy,
                   ax, ay, g_aimat_cx, g_aimat_cy, smx, smy, tsx, tsy, onscr, chx, chy,
                   cand, lck, ammo, mags, aligned);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

/* W63 driveto-Fahr-Bot (Testpfad, nur mit Replay): Drehhebel + Gang + Gas
 * wie ein Spieler, der auf ein Ziel zufaehrt. Eigene Position = Entity
 * [0x356E2C] (+0x0C/+0x14), Rumpf-Gierung hy = [0x3521C0] (Fahrtrichtung
 * (sin hy, cos hy), belegt W37: (395,609)->(449,592) bei hy=1.873).
 * Peilfehler d = atan2(dx,dz) - hy; Drehhebel = P-Regler, Vorzeichen per
 * Selbstkalibrierung (|Hebel| >= 40 ueber 30 Frames, Gierung muss in
 * Hebelrichtung laufen, sonst einmal umdrehen). Gas nur bei |d| < 0.5,
 * sonst Drehen im Stand. Ab Distanz < R: Gas los, Bremse (mittleres Pedal). */
static u32 g_drive_cur, g_drive_logf, g_drive_calf;
static int g_drive_sign = 1, g_drive_cal; /* 0 = offen, 1 = bestaetigt */
static float g_drive_hy0;
static int g_drive_reached;
/* Festfahr-Erkennung (W63_c1/c2: an (431,592) gegen ein Hindernis gefahren,
 * Gas an, < 1u in 60 Frames): Manoever wie ein Spieler - Rueckwaerts (Gang R
 * + Gas), dann mit vollem Drehhebel abwechselnd links/rechts vorwaerts,
 * danach wieder Zielkurs. Dauer waechst je Versuch (max. 4-fach). */
static u32 g_drive_chkf, g_drive_manf;
static float g_drive_chkx, g_drive_chkz;
static int g_drive_man, g_drive_try; /* 0 = Kurs, 1 = rueckwaerts, 2 = ausweichen */
void drive_apply(sbc_in_t *s)
{
    __try {
        u32 tab = *(volatile u32 *)(uintptr_t)SBA(0x351828u);
        u32 n = *(volatile u32 *)(uintptr_t)SBA(0x352288u);
        u32 ours = *(volatile uint16_t *)(uintptr_t)SBA(0x356E2Cu);
        u32 idx = g_drive_idx, eb, ob;
        float mx, mz, tx, tz, dx, dz, dist, d, hy, rot;
        int rv, gas;
        if (!tab || n > 100000u || ours == 0 || ours >= n) return;
        if (*(volatile u32 *)(uintptr_t)SBA(0x356F58u) != 1u) return; /* nur in der Missionsschleife */
        if (*(volatile u32 *)(uintptr_t)SBA(0x53831Cu) != 1u) return; /* nur mit verriegeltem VT (nach START, W52) */
        ob = tab + ours * 0x5Cu;
        if (*(volatile uint16_t *)(uintptr_t)ob == 0) return; /* eigener VT tot */
        mx = *(volatile float *)(uintptr_t)(ob + 0x0C);
        mz = *(volatile float *)(uintptr_t)(ob + 0x14);
        hy = aimat_rdf(SBA(0x3521C0u), 0);
        if (idx == 998u) {
            idx = campaign_target(tab, n, mx, mz);
            if (!idx) return;
        }
        if (idx >= n) return;
        if (idx != g_drive_cur) {
            g_drive_cur = idx;
            g_drive_reached = 0;
            HLOG_I("DRIVETO Ziel -> %u bei f=%u", idx, g_frames);
        }
        eb = tab + idx * 0x5Cu;
        if (*(volatile uint16_t *)(uintptr_t)eb == 0) return; /* Ziel tot */
        tx = *(volatile float *)(uintptr_t)(eb + 0x0C);
        tz = *(volatile float *)(uintptr_t)(eb + 0x14);
        dx = tx - mx; dz = tz - mz;
        dist = sqrtf(dx * dx + dz * dz);
        d = (float)atan2((double)dx, (double)dz) - hy;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        if (dist < g_drive_r) {
            if (!g_drive_reached) {
                g_drive_reached = 1;
                HLOG_I("DRIVETO erreicht: f=%u ziel=%u dist=%.1f pos=%.1f,%.1f hy=%.3f",
                       g_frames, idx, dist, mx, mz, hy);
            }
            s->pedal_m = 0xFF; /* Bremse */
            return;
        }
        g_drive_reached = 0;
        if (g_drive_man) {
            /* Ausweichmanoever laeuft */
            u32 k = (u32)(g_drive_try > 4 ? 4 : g_drive_try);
            u32 el = g_frames - g_drive_manf;
            if (g_drive_man == 1 && el < 90u * k) {
                s->gear = 7; /* R */
                s->pedal_r = 0xFF;
                return;
            }
            if (g_drive_man == 1) {
                g_drive_man = 2;
                g_drive_manf = g_frames;
                el = 0;
            }
            if (g_drive_man == 2 && el < 120u * k) {
                s->gear = g_drive_gear;
                s->pedal_r = 0xFF;
                s->rot_lever = (int8_t)((g_drive_try & 1) ? 127 : -127);
                return;
            }
            g_drive_man = 0;
            g_drive_chkf = g_frames;
            g_drive_chkx = mx;
            g_drive_chkz = mz;
            HLOG_I("DRIVETO Manoever %d fertig f=%u pos=%.1f,%.1f", g_drive_try, g_frames, mx, mz);
        }
        rot = 300.0f * d * (float)g_drive_sign;
        if (rot > 127.0f) rot = 127.0f;
        if (rot < -127.0f) rot = -127.0f;
        rv = (int)rot;
        /* Selbstkalibrierung des Drehsinns (einmalig) */
        if (!g_drive_cal) {
            if (rv >= 40 || rv <= -40) {
                if (!g_drive_calf) {
                    g_drive_calf = g_frames;
                    g_drive_hy0 = hy;
                } else if (g_frames - g_drive_calf >= 30u) {
                    float dh = hy - g_drive_hy0;
                    while (dh > 3.14159265f) dh -= 6.2831853f;
                    while (dh < -3.14159265f) dh += 6.2831853f;
                    if (dh > 0.01f || dh < -0.01f) {
                        /* Soll: d > 0 -> hy steigt. Lief hy entgegen d, dann
                         * Vorzeichen des Hebels umdrehen. */
                        if ((dh > 0.0f) != (d > 0.0f)) {
                            g_drive_sign = -g_drive_sign;
                        }
                        g_drive_cal = 1;
                        HLOG_I("DRIVETO Kalibrierung f=%u: hebel=%d d=%.3f dhy=%.3f -> Vorzeichen %d",
                               g_frames, rv, d, dh, g_drive_sign);
                    } else {
                        g_drive_calf = 0; /* noch keine Drehung messbar */
                    }
                }
            } else {
                g_drive_calf = 0;
            }
        }
        /* Drehen wirkt nur in Fahrt (W63_c3: Hebel -127 ohne Gas, Gierung
         * 40000 Frames konstant) -> beim Eindrehen mit halbem Gas gehen. */
        gas = (d < 0.5f && d > -0.5f) ? 2 : 1;
        s->rot_lever = (int8_t)rv;
        s->gear = g_drive_gear;
        s->pedal_r = gas == 2 ? 0xFF : 0x90;
        /* Festfahr-Pruefung alle 60 Frames */
        if (!g_drive_chkf) {
            g_drive_chkf = g_frames;
            g_drive_chkx = mx;
            g_drive_chkz = mz;
        } else if (g_frames - g_drive_chkf >= 60u) {
            float mvx = mx - g_drive_chkx, mvz = mz - g_drive_chkz;
            if (mvx * mvx + mvz * mvz < 1.0f) {
                g_drive_try++;
                g_drive_man = 1;
                g_drive_manf = g_frames;
                HLOG_I("DRIVETO festgefahren f=%u pos=%.1f,%.1f ziel=%u dist=%.1f -> Manoever %d (%s)",
                       g_frames, mx, mz, idx, dist, g_drive_try, (g_drive_try & 1) ? "rechts" : "links");
            }
            g_drive_chkf = g_frames;
            g_drive_chkx = mx;
            g_drive_chkz = mz;
        }
        if (g_frames - g_drive_logf >= 100u) {
            g_drive_logf = g_frames;
            HLOG_I("DRIVETO f=%u ziel=%u dist=%.1f d=%.3f hy=%.3f pos=%.1f,%.1f,%.1f rot=%d gang=%u gas=%d",
                   g_frames, idx, dist, d, hy, mx, *(volatile float *)(uintptr_t)(ob + 0x10), mz,
                   rv, g_drive_gear, gas);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
