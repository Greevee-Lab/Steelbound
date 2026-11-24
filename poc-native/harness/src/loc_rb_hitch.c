/* loc_rb_hitch.c - W469 LOC-RB-HITCH: Haenger-Zuordnung der Rollback-Anbindung (DIAG, immer an, nur Zaehler).
 *
 * Je Lockstep-Rahmen (ls_record, Spiel-Thread) wird der Abstand zum vorigen Rahmen gemessen. Dazwischen sammeln die
 * Phasen der Rollback-Anbindung ihre Zeit (rbh_add, QPC). Ist der Abstand groesser als RBH_LOG_MS, schreibt rbh_frame
 * eine Zeile LOCHITCH mit der Aufteilung:
 *   Sim    = Spiel-Thread im Spielcode (vom Ende des Ruhepunkts bis vor das Feld-Warten, ohne Vorlauf/Neurechnen)
 *   Feld   = Feld-Warten des Spiels (Taktgeber, inkl. W372-Verlaengerung)
 *   RB     = rb_after gesamt, davon die Teilphasen (Ruhepunkt-Warten, Sichern, Neuaufbau, Aufbau, ...)
 *   Vorlauf/Neurechnen = Spieltakte des eigenen Vorlaufs (W390) bzw. des Neurechnens (Zeit im Spielcode)
 * Dazu die Zahl der Felder (Ruhepunkte) im Abstand und die Stauframes des Spiels (Warten auf fremde Eingaben).
 * Keine Eingaben, keine Inhalte. Nur Ganzzahlen (ls_record laeuft im Thunk; der x87-Stapel des Spiels bleibt unberuehrt). */
#include "loc_rb_int.h"

#define RBH_LOG_MS 75u   /* Abstand ab dem eine LOCHITCH-Zeile entsteht (Soll 50 ms) */
#define RBH_LOG_MAX 400u /* hoechstens so viele Zeilen je Prozess */

static const char *const k_rbh_name[RBH_N] = {"Ruhepunkt-Warten", "Sichern",   "Neuaufbau",   "Aufbau",
                                               "WP-Mikromessung",  "Stacks",    "Log-Leeren",  "Laden",
                                               "Aushandlung",      "Regler",    "Eingabeschicht", "Ketten"};

static long long s_rbh_acc[RBH_N];
static long long s_rbh_sim, s_rbh_field, s_rbh_rb, s_rbh_spec, s_rbh_resim;
static long long s_rbh_out;     /* Ende des letzten rb_after */
static long long s_rbh_bef;     /* letzter rb_before */
static long long s_rbh_in;      /* Eintritt des laufenden rb_after */
static long long s_rbh_frame;   /* letzter Lockstep-Rahmen */
static int s_rbh_out_kind;      /* Art des Abschnitts nach dem letzten rb_after: 0 Spiel, 1 Vorlauf, 2 Neurechnen */
static u32 s_rbh_fields, s_rbh_logged, s_rbh_n, s_rbh_stall0;
static u32 s_rbh_max_ms, s_rbh_win_max, s_rbh_h75, s_rbh_h100;
u32 s_rbh_events;               /* RBH_EV_*: Ereignisse im laufenden Abstand (von den Modulen gesetzt) */

void rbh_add(int ph, long long d)
{
    if (ph >= 0 && ph < RBH_N && d > 0) s_rbh_acc[ph] += d;
}

static int rbh_kind_now(void)
{
    if (s_spec) return 1;
    if (s_phase == PH_REPLAY || (s_rbi_ep && s_rbc && rbi_replaying(s_rbc))) return 2;
    return 0;
}

void rbh_before(void)
{
    long long t = rb_now();
    if (s_rbh_out && s_rbh_out_kind == 0) s_rbh_sim += t - s_rbh_out;
    s_rbh_bef = t;
}

void rbh_after_in(void)
{
    long long t = rb_now();
    s_rbh_in = t;
    if (!s_rbh_out) return;
    if (s_rbh_out_kind == 1) s_rbh_spec += t - s_rbh_out;
    else if (s_rbh_out_kind == 2) s_rbh_resim += t - s_rbh_out;
    else if (s_rbh_bef > s_rbh_out) s_rbh_field += t - s_rbh_bef;
    else s_rbh_sim += t - s_rbh_out; /* Feld-Warten uebersprungen (s_skipwait) */
}

void rbh_after_out(void)
{
    long long t = rb_now();
    if (s_rbh_in) s_rbh_rb += t - s_rbh_in;
    s_rbh_fields++;
    s_rbh_out = t;
    s_rbh_out_kind = rbh_kind_now();
}

static u32 rbh_ms(long long d) { return rb_us(d) / 1000u; }

void rbh_frame(u32 stallf)
{
    long long t;
    u32 gap;
    int i;
    if (s_qpf <= 1) return; /* Rollback-Anbindung nicht installiert */
    t = rb_now();
    gap = s_rbh_frame ? rb_us(t - s_rbh_frame) / 1000u : 0u;
    if (s_rbh_frame && !loc_rb_resim()) {
        s_rbh_n++;
        if (gap > s_rbh_max_ms) s_rbh_max_ms = gap;
        if (gap > s_rbh_win_max) s_rbh_win_max = gap;
        if (gap > RBH_LOG_MS) s_rbh_h75++;
        if (gap > 100u) s_rbh_h100++;
        if (gap > RBH_LOG_MS && s_rbh_logged < RBH_LOG_MAX) {
            char b[512];
            int n = 0;
            s_rbh_logged++;
            for (i = 0; i < RBH_N && n < (int)sizeof b - 48; i++) {
                u32 us = rb_us(s_rbh_acc[i]);
                if (us >= 1000u) n += wsprintfA(b + n, " %s=%u", k_rbh_name[i], us / 1000u);
            }
            b[n] = 0;
            HLOG_I("LOCHITCH: Abstand=%u ms Tick=%u Felder=%u Stauframes=+%u Ring=%d bereit=%d Ereignisse=0x%X | Sim=%u Feld=%u "
                   "RB=%u Vorlauf=%u Neurechnen=%u | RB-Teile:%s",
                   gap, rd32(RB_TICK), s_rbh_fields, stallf - s_rbh_stall0, s_ring && rbr_active(), s_ready,
                   s_rbh_events, rbh_ms(s_rbh_sim), rbh_ms(s_rbh_field), rbh_ms(s_rbh_rb), rbh_ms(s_rbh_spec),
                   rbh_ms(s_rbh_resim), n ? b : " -");
        }
    }
    if (!loc_rb_resim()) {
        s_rbh_frame = t;
        s_rbh_stall0 = stallf;
        memset(s_rbh_acc, 0, sizeof s_rbh_acc);
        s_rbh_sim = s_rbh_field = s_rbh_rb = s_rbh_spec = s_rbh_resim = 0;
        s_rbh_fields = 0;
        s_rbh_events = 0;
    }
}

/* Statuszeile (alle 10 s mit dem Rollback-Log): Rahmen, Haenger > 75/100 ms, groesster Abstand im Fenster und gesamt,
 * Sichern ohne freien Render-Thread (Wartegrenze qwait). Nur wenn sich etwas getan hat. */
void rbh_log(int waited_s)
{
    static u32 n0, h75_0, h100_0, short0;
    u32 n = s_rbh_n, mx = s_rbh_win_max;
    if (n == n0) return;
    s_rbh_win_max = 0;
    HLOG_I("LOCHITCH-SUM: t=%ds Rahmen=%u (+%u) >75ms=%u (+%u) >100ms=%u (+%u) max Fenster/gesamt=%u/%u ms | Sichern ohne "
           "freien Render-Thread=%u (+%u, Grenze %d ms) | Aushandlung fein %d ms",
           waited_s, n, n - n0, s_rbh_h75, s_rbh_h75 - h75_0, s_rbh_h100, s_rbh_h100 - h100_0, mx, s_rbh_max_ms, s_q_short,
           s_q_short - short0, s_qwait, s_negpoll);
    n0 = n;
    h75_0 = s_rbh_h75;
    h100_0 = s_rbh_h100;
    short0 = s_q_short;
}

/* oeffentlich (loc_rollback.h): aus ls_record je Lockstep-Rahmen */
void loc_rb_hitch_frame(unsigned stallf) { rbh_frame(stallf); }
