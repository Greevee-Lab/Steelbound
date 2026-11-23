/* loc_vpf.c - W394 LOC-VIEW-PERFRAME (siehe loc_vpf.h). */
#include "loc_vpf.h"
#include "hcfg.h"
#include "hlog.h"
#include "hooks_int.h"
#include "input_map.h"
#include "input_mouse.h"
#include "sb_overlay.h"
#include "loc_chatnet.h" /* W437 */
#include "sb_profile.h"
#include "loc_aim.h"
#include "loc_hud_math.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef struct {
    uint32_t poll;     /* laufende Nummer */
    long long t;       /* QPC am Ende des Polls */
    imap_state_t st;   /* Akku-Stand nach dem Poll */
    imap_snap_t snap;  /* Snapshot des Polls (Schalter/Gatter; Zeiger werden nicht benutzt) */
    uint8_t leg[4];    /* Block am Ende der Abbildung: aiming_x, aiming_y, sight_x, sight_y */
    uint8_t fin[4];    /* endgueltiger Block (nach sbc_merge) */
    int legacy;
    int keys_live;     /* echte Tasten aus Fenster-Nachrichten (kein Replay, input_source=1) */
} lvpf_pub_t;

static volatile LONG s_seq;      /* gerade = stabil, ungerade = Poll im Gang */
static lvpf_pub_t s_pub;         /* nur unter s_seq ungerade geschrieben */
static uint32_t s_poll;          /* Spiel-Thread */
static volatile LONG s_poll_pub; /* letzte vollstaendige Nummer */
static volatile LONGLONG s_poll_t;
static int s_have_mapped;

static int lvpf_on(void)
{
    return g_sba_loc && g_cfg.loc_local_aim && (g_cfg.loc_view_perframe || (g_cfg.loc_hud_diag & 6));
}

void loc_vpf_begin(void)
{
    if (!lvpf_on()) return;
    if ((s_seq & 1) == 0) InterlockedIncrement(&s_seq);
    s_have_mapped = 0;
}

void loc_vpf_mapped(const void *st, const void *snap, const void *blk, int legacy)
{
    const sbc_in_t *b = (const sbc_in_t *)blk;
    if (!lvpf_on() || (s_seq & 1) == 0) return;
    s_pub.st = *(const imap_state_t *)st;
    s_pub.snap = *(const imap_snap_t *)snap;
    s_pub.leg[0] = b->aiming_x;
    s_pub.leg[1] = b->aiming_y;
    s_pub.leg[2] = (uint8_t)b->sight_x;
    s_pub.leg[3] = (uint8_t)b->sight_y;
    s_pub.legacy = legacy;
    s_pub.keys_live = !s_pub.snap.replay_active && g_cfg.input_source == 1;
    s_have_mapped = 1;
}

void loc_vpf_end(const void *blk)
{
    const sbc_in_t *b = (const sbc_in_t *)blk;
    LARGE_INTEGER q;
    if ((s_seq & 1) == 0) return;
    if (s_have_mapped) {
        s_pub.fin[0] = b->aiming_x;
        s_pub.fin[1] = b->aiming_y;
        s_pub.fin[2] = (uint8_t)b->sight_x;
        s_pub.fin[3] = (uint8_t)b->sight_y;
        QueryPerformanceCounter(&q);
        s_pub.t = q.QuadPart;
        s_pub.poll = ++s_poll;
        InterlockedExchange64(&s_poll_t, q.QuadPart);
        InterlockedExchange(&s_poll_pub, (LONG)s_pub.poll);
    }
    InterlockedIncrement(&s_seq); /* wieder gerade */
}

/* DIAG(W394, Spiel-Thread): Block-Werte des Polls poll (0 = nicht mehr vorhanden). */
int loc_vpf_fin(uint32_t poll, uint8_t fin[4])
{
    if (!poll || s_pub.poll != poll || (s_seq & 1)) return 0;
    memcpy(fin, s_pub.fin, 4);
    return 1;
}

uint32_t loc_vpf_poll_seq(void)
{
    return (uint32_t)s_poll_pub;
}

long long loc_vpf_poll_time(void)
{
    return (long long)s_poll_t;
}

/* Zielhebel: Block-Wert (uint8) -> Byte im Eingabesatz. Belegt W394 (DIAG-Paare Block/Satz, Lauf C2/C3): Stufen wie in SB
 * (W87_rel), 0x7F und 0x80 = 0 (Totzone), darueber v - 0x80, darunter v - 0x7F; Zielhebel y mit umgekehrtem Vorzeichen.
 * Sicht x unveraendert, Sicht y bitweise invertiert (Ruhewert -1, wie SB "~sight_y"). */
int loc_vpf_conv_aim(int v)
{
    return lh_conv_aim(v);
}

int loc_vpf_preview(const uint8_t base[14], uint8_t out[14], uint32_t *poll)
{
    static lvpf_pub_t p; /* nur Render-Thread (loc_aim/ip_la_prep halten die Reihenfolge) */
    static SRWLOCK lk = SRWLOCK_INIT;
    imap_state_t st2;
    LONG q1, q2;
    int k, dx = 0, dy = 0, ok = 0, ax, ay, sx, sy, have[4];
    memcpy(out, base, 14);
    if (!lvpf_on()) return 0;
    AcquireSRWLockExclusive(&lk); /* Wiedergaben koennen aus zwei Threads kommen (Swap/Takt) */
    for (k = 0; k < 3 && !ok; k++) {
        q1 = s_seq;
        if ((q1 & 1) || !s_poll_pub) continue;
        MemoryBarrier();
        p = s_pub;
        (void)imouse_raw_peek(&dx, &dy);
        if (loc_chat_typing()) dx = dy = 0; /* W437: Simulation bekommt beim Tippen keine Deltas -> Vorschau auch nicht */
        MemoryBarrier();
        q2 = s_seq;
        ok = q1 == q2;
    }
    if (!ok || !p.legacy) {
        ReleaseSRWLockExclusive(&lk);
        return 0;
    }
    *poll = p.poll;
    imap_mouse_preview(&p.st, &p.snap, dx, dy, &st2);
    /* Achsen, die der letzte Poll aus der Legacy-Belegung bekam und die danach unveraendert blieben */
    for (k = 0; k < 4; k++) have[k] = p.leg[k] == p.fin[k];
    ax = p.fin[0]; ay = p.fin[1]; sx = (int8_t)p.fin[2]; sy = (int8_t)p.fin[3];
    if (p.keys_live && !(g_cfg.input_focus_only && !imouse_has_focus()) && !(g_cfg.overlay && sb_ovl_open()) &&
        !loc_chat_typing()) { /* W437: offene Chatzeile wie offenes Menue */
        /* Spielerbetrieb: Pfeiltasten jetzt (wie imap_legacy_apply), Maus auf dem Rest */
        int L = imsg_key_peek(VK_LEFT), R = imsg_key_peek(VK_RIGHT), U = imsg_key_peek(VK_UP), D = imsg_key_peek(VK_DOWN);
        int sh = imsg_key_peek(VK_SHIFT);
        int arx = L || R, ary = U || D;
        sx = L ? -127 : (R ? 127 : 0);
        sy = U ? -127 : (D ? 127 : 0);
        ax = (!sh && arx) ? (L ? 0x00 : 0xFF) : 0x7F;
        ay = (!sh && ary) ? (U ? 0x00 : 0xFF) : 0x7F;
        if (p.snap.mouse_aim) {
            if (p.snap.mouse_sight) {
                if (!arx) sx = (int8_t)st2.m_sx;
                if (!ary) sy = (int8_t)st2.m_sy;
            } else {
                if (!arx) ax = st2.m_ax;
                if (!ary) ay = st2.m_ay;
            }
        }
    } else if (p.snap.mouse_aim) {
        /* Testfolge bzw. Tasten nicht live: nur Achsen, die im letzten Poll aus der Maus kamen */
        if (p.snap.mouse_sight) {
            if (have[2] && (int8_t)p.leg[2] == (int8_t)p.st.m_sx) sx = (int8_t)st2.m_sx;
            if (have[3] && (int8_t)p.leg[3] == (int8_t)p.st.m_sy) sy = (int8_t)st2.m_sy;
        } else {
            if (have[0] && (p.st.m_aim_src & 1u) && p.leg[0] == (uint8_t)p.st.m_ax) ax = st2.m_ax;
            if (have[1] && (p.st.m_aim_src & 2u) && p.leg[1] == (uint8_t)p.st.m_ay) ay = st2.m_ay;
        }
    }
    lh_frame(base, p.fin, ax, ay, sx, sy, have, out); /* Satz = Satz des Polls + Aenderung der Achsen */
    ReleaseSRWLockExclusive(&lk);
    return 1;
}

/* ---- TEST: Mausschritte zu zufaelliger Phase (Messung Eingabe-zu-Bild) ---- */
static volatile LONG s_t_on, s_t_id;
static HANDLE s_t_thr;
static FILE *s_t_fp;

static void lvpf_wait_until(LONGLONG t)
{
    LARGE_INTEGER q;
    for (;;) {
        QueryPerformanceCounter(&q);
        if (q.QuadPart >= t) return;
        Sleep(0);
    }
}

static DWORD WINAPI lvpf_test_thread(void *arg)
{
    LARGE_INTEGER f, q;
    LONG seen = 0;
    uint32_t rnd = 0x9E3779B9u;
    (void)arg;
    QueryPerformanceFrequency(&f);
    for (;;) {
        LONG id = s_t_id;
        LONGLONG t0, step = f.QuadPart / 200; /* 5 ms */
        int n = 0, dir, axis;
        if (id == seen || !s_t_on) {
            Sleep(1);
            continue;
        }
        seen = id;
        rnd = rnd * 1664525u + 1013904223u;
        QueryPerformanceCounter(&q);
        t0 = q.QuadPart + (LONGLONG)((rnd >> 8) % 50000u) * f.QuadPart / 1000000; /* 0..50 ms nach der Flanke */
        dir = (id & 1) ? 1 : -1;
        axis = (id >> 1) & 1; /* +x, -x, +y, -y, ... */
        lvpf_wait_until(t0);
        while (s_t_on && s_t_id == id && n < 80) {
            QueryPerformanceCounter(&q);
            if (!n && s_t_fp) { /* Beginn des Schritts (Zeit, Richtung, Achse) - Testfolge, keine echte Eingabe */
                struct { LONGLONG q; int dir, id, axis; } r;
                r.q = q.QuadPart; r.dir = dir; r.id = (int)id; r.axis = axis;
                fwrite(&r, sizeof r, 1, s_t_fp);
                fflush(s_t_fp);
            }
            (void)imouse_test_post(IMOUSE_MSG_TEST_MOVE, (uintptr_t)(intptr_t)(axis ? 0 : dir * g_cfg.loc_vpf_test),
                                   (intptr_t)(axis ? dir * g_cfg.loc_vpf_test : 0));
            n++;
            lvpf_wait_until(t0 + step * n);
        }
    }
}

int loc_vpf_test_off(void)
{
    return g_cfg.loc_vpf_test_alt && s_t_id > 0 && ((s_t_id - 1) >> 2 & 1) == 0;
}

void loc_vpf_test_drive(void)
{
    static int prev;
    int k;
    if (!g_sba_loc || g_cfg.loc_vpf_test <= 0 || !g_cfg.input_test_hook || !g_cfg.loc_mouse_test) return;
    if (!g_cfg.loc_replay_tick[0] && !g_cfg.loc_steps[0]) return;
    k = key_down(337) != 0;
    if (k && !prev) {
        if (!s_t_thr) {
            s_t_fp = (FILE *)loc_aim_diag_file("locvpf_onset.bin");
            s_t_thr = CreateThread(NULL, 0, lvpf_test_thread, NULL, 0, NULL);
            HLOG_I("LOCVPF: Testthread (Mausschritte %d Counts je 5 ms ab zufaelliger Phase)", g_cfg.loc_vpf_test);
        }
        InterlockedExchange(&s_t_on, 1);
        InterlockedIncrement(&s_t_id);
    } else if (!k && prev) {
        InterlockedExchange(&s_t_on, 0);
    }
    prev = k;
}
