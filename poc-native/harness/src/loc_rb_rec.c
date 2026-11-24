/* loc_rb_rec.c - W464 NET-CORE: aus loc_rollback.c verschoben (W325/W330/W346/W375/W381/W413), unveraendert.
 * Aufzeichnung/Wiedergabe der Spiel-Aufrufe beim Neurechnen: Winsock (select/recv/send...), XInput, XACT-Ton mit
 * Nebentabelle und Ton-Abgleich (loc_rb_sndloop.h), Akkumulator-Kette, HLE-Zensus je Phase. */
#include "loc_rb_int.h"

/* ================= Aufzeichnung / Wiedergabe der Spiel-Aufrufe ================= */
const char *const k_evname[] = {"-", "select", "recv", "recvfrom", "send", "sendto", "ioctlsocket",
                                       "WSAGetLastError", "XInputGetState", "Akkumulator",
                                       "XACT GetNotification", "XACT PlayEx", "XACT PrepareEx", "XACT Stop",
                                       "XACT SoundSource_GetStatus", "XACT StopSoundCues", "XACTEngineDoWork",
                                       "NtAllocateVirtualMemory", "NtAllocateVirtualMemory(Commit)"};
uint8_t *s_rec;
u32 s_rec_len, s_rec_cur;
u32 s_rec_events;
volatile int s_phase;
/* ---- W390 LOC-OWN-PREDICT: spekulativer Vorlauf nur fuer die Anzeige ----
 * Nach einem Live-Tick mit Sichern (Schatten = aktueller Stand) rechnet der Spiel-Thread D + 1 Ticks weiter (eigene
 * schon erzeugte Rahmen + im Vorlauf neu erzeugter Rahmen, fremde vorhergesagt), merkt die eigene VT-Lage und setzt den
 * Speicher mit rbr_revert auf den gesicherten Stand zurueck. Phase dabei PH_REPLAY (Render-/Ton-Pfade wie beim
 * Neurechnen), zusaetzlich s_spec = 1: alle Haken ohne Wirkung nach aussen (kein Senden, kein Empfang, kein Ton, keine
 * Aufzeichnung, kein Hash, keine Freigabe). */
volatile int s_spec;
rb_spec_ev_t s_spec_ev; /* abgefangene Nebenwirkungen (gesamt) */
int s_spec_pd; /* W393: dieser Vorlauf haelt den Present-Takt an (loc_pd_spec=0) */
struct rb_spec_xc_s s_spec_xc[RB_SPEC_XC]; /* XInput im Vorlauf live -> naechster Live-Tick */
u32 s_spec_xcn, s_spec_xc_hit, s_spec_xc_unused;
uint8_t s_spec_own_new[22]; /* RBI_FR: im Vorlauf erzeugter eigener Rahmen (Kopf + Satz) */
int s_spec_own_have;
int s_spec_paused_n, s_spec_off_mission; /* Kosten-Rueckfall je Mission */
u32 s_rb_esp0, s_rb_caller, s_spec_esp0, s_spec_caller, s_spec_ctx_bad; /* Aufrufer/Stapel des Feld-Wartens */
u32 s_spec_degr;
int s_spec_level; /* Rueckfall-Stufe: 0 = D + 1 Ticks, 1 = D Ticks (danach Pause) */
DWORD s_spec_pause_until;
u32 s_spec_win_n;
u32 s_nt_rec, s_nt_rep; /* Ticks je Phase (Zensus-Normierung) */
u32 s_rep_div;          /* Abweichungen der Aufruffolge im aktuellen Neurechnen */
u32 s_rep_div_fn, s_rep_div_at;
u32 s_cnt_ev[EV_N][2];  /* je Ereignisart: [0] aufgezeichnet, [1] wiedergegeben (gesamt) */

u32 fnv(u32 h, const void *p, u32 n)
{
    const uint8_t *b = (const uint8_t *)p;
    u32 i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}
int rb_mine(void) { return s_phase != PH_IDLE && GetCurrentThreadId() == s_game_tid; }

void rb_rec_put(int fn, u32 ret, u32 ih, const void *data, u32 n)
{
    rb_ev_t e;
    if (!s_rec || s_rec_len + sizeof e + n > RB_REC_CAP) {
        s_rep_div++; /* Aufzeichnung voll: Zyklus ungueltig */
        return;
    }
    e.fn = (uint8_t)fn;
    e.pad = 0;
    e.n = (uint16_t)n;
    e.ret = ret;
    e.ih = ih;
    memcpy(s_rec + s_rec_len, &e, sizeof e);
    if (n) memcpy(s_rec + s_rec_len + sizeof e, data, n);
    s_rec_len += (u32)sizeof e + n;
    s_rec_events++;
    s_cnt_ev[fn][0]++;
}

/* Naechstes Ereignis der Wiedergabe; NULL bei Abweichung (Art oder Eingangsargumente). */
int s_rbi_ep;      /* W338 (vorgezogen) */
u32 s_rep_kc[EV_N]; /* W346 R3: Wiedergabe-Zeiger je Ereignisart im laufenden Neurechen-Tick */
u32 s_rep_end;      /* W338 (vorgezogen) */
u32 s_rbi_send_live; /* W346 R3: beim Neurechnen live gesendet (nur die korrigierte Zeitlinie sendet) */
const rb_ev_t *rb_rep_get(int fn, u32 ih, const uint8_t **data)
{
    const rb_ev_t *e;
    if (s_rbi_ep) { /* W346 R3: je Ereignisart in Reihenfolge (eine andere Verschraenkung der Arten ist kein Fehler) */
        u32 c = s_rep_kc[fn];
        while (c + sizeof(rb_ev_t) <= s_rep_end) {
            e = (const rb_ev_t *)(s_rec + c);
            if (e->fn == (uint8_t)fn) {
                if (e->ih != ih) break;
                s_rep_kc[fn] = c + (u32)sizeof *e + e->n;
                s_cnt_ev[fn][1]++;
                if (data) *data = (const uint8_t *)(e + 1);
                return e;
            }
            c += (u32)sizeof *e + e->n;
        }
        if (!s_rep_div) { s_rep_div_fn = (u32)fn; s_rep_div_at = c; }
        s_rep_div++;
        return NULL;
    }
    if (s_rec_cur + sizeof(rb_ev_t) > rb_rep_limit()) {
        if (!s_rep_div) { s_rep_div_fn = (u32)fn; s_rep_div_at = s_rec_events; }
        s_rep_div++;
        return NULL;
    }
    e = (const rb_ev_t *)(s_rec + s_rec_cur);
    if (e->fn != fn || e->ih != ih) {
        if (!s_rep_div) { s_rep_div_fn = (u32)fn | ((u32)e->fn << 8) | (e->fn == fn ? 0x10000u : 0u); s_rep_div_at = s_rec_cur; }
        s_rep_div++;
        return NULL;
    }
    s_rec_cur += (u32)sizeof *e + e->n;
    s_cnt_ev[fn][1]++;
    if (data) *data = (const uint8_t *)(e + 1);
    return e;
}

/* Originalziele der verketteten Sprungstellen */
fn5_t o_select;
fn4_t o_recv, o_send;
fn6_t o_recvfrom, o_sendto;
fn3_t o_ioctl;
fn0_t o_wsaerr;
fn2_t o_xin;

static u32 s_fake_err;

static u32 fdset_hash(u32 h, u32 p)
{
    if (!p) return fnv(h, "n", 1);
    __try {
        u32 c = *(const u32 *)(uintptr_t)p;
        if (c > 64) c = 64;
        h = fnv(h, (const void *)(uintptr_t)p, 4 + c * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return h;
}
static u32 fdset_len(u32 p)
{
    u32 c = p ? *(const u32 *)(uintptr_t)p : 0;
    return 4 + (c > 64 ? 64 : c) * 4;
}

u32 __stdcall rb_select(u32 nfds, u32 r, u32 w, u32 x, u32 tv)
{
    static uint8_t buf[3 * 260]; /* nur Spiel-Thread */
    u32 ih, n = 0, ret;
    if (r) LOC_RB_PREWRITE(r, 260);
    if (w) LOC_RB_PREWRITE(w, 260);
    if (x) LOC_RB_PREWRITE(x, 260);
    if (!rb_mine()) return o_select(nfds, r, w, x, tv);
    if (s_spec) { /* W390: Vorlauf - nichts lesbar (die echte Zeitlinie holt es ab) */
        s_spec_ev.sel++;
        if (r) *(u32 *)(uintptr_t)r = 0;
        if (w) *(u32 *)(uintptr_t)w = 0;
        if (x) *(u32 *)(uintptr_t)x = 0;
        return 0;
    }
    ih = fdset_hash(fdset_hash(fdset_hash(2166136261u, r), w), x);
    if (s_phase == PH_RECORD) {
        ret = o_select(nfds, r, w, x, tv);
        if (r) { u32 l = fdset_len(r); memcpy(buf + n, (void *)(uintptr_t)r, l); n += l; }
        if (w) { u32 l = fdset_len(w); memcpy(buf + n, (void *)(uintptr_t)w, l); n += l; }
        if (x) { u32 l = fdset_len(x); memcpy(buf + n, (void *)(uintptr_t)x, l); n += l; }
        rb_rec_put(EV_SELECT, ret, ih, buf, n);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_SELECT, ih, &d);
        if (!e) {
            if (r) *(u32 *)(uintptr_t)r = 0;
            if (w) *(u32 *)(uintptr_t)w = 0;
            if (x) *(u32 *)(uintptr_t)x = 0;
            return 0;
        }
        if (r) { u32 l = 4 + (*(const u32 *)(d + n) > 64 ? 64 : *(const u32 *)(d + n)) * 4; memcpy((void *)(uintptr_t)r, d + n, l); n += l; }
        if (w) { u32 l = 4 + (*(const u32 *)(d + n) > 64 ? 64 : *(const u32 *)(d + n)) * 4; memcpy((void *)(uintptr_t)w, d + n, l); n += l; }
        if (x) { u32 l = 4 + (*(const u32 *)(d + n) > 64 ? 64 : *(const u32 *)(d + n)) * 4; memcpy((void *)(uintptr_t)x, d + n, l); n += l; }
        return e->ret;
    }
}

u32 __stdcall rb_recv(u32 s, u32 buf, u32 len, u32 flags)
{
    u32 ih, ret;
    LOC_RB_PREWRITE(buf, len); /* W330: Kernel schreibt in den Gast-Puffer */
    if (!rb_mine()) return o_recv(s, buf, len, flags);
    if (s_spec) { s_spec_ev.recv++; s_fake_err = WSAEWOULDBLOCK_X; return 0xFFFFFFFFu; } /* W390 */
    ih = s * 31u + len * 7u + flags;
    if (s_phase == PH_RECORD) {
        ret = o_recv(s, buf, len, flags);
        rb_rec_put(EV_RECV, ret, ih, (const void *)(uintptr_t)buf, ((int)ret > 0 && ret <= len) ? ret : 0);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_RECV, ih, &d);
        if (!e) { s_fake_err = WSAEWOULDBLOCK_X; return 0xFFFFFFFFu; }
        if (e->n) memcpy((void *)(uintptr_t)buf, d, e->n);
        return e->ret;
    }
}

u32 __stdcall rb_recvfrom(u32 s, u32 buf, u32 len, u32 flags, u32 from, u32 pfl)
{
    static uint8_t tmp[0x1100 + 24]; /* nur Spiel-Thread */
    u32 ih, ret;
    LOC_RB_PREWRITE(buf, len); /* W330 */
    if (from) LOC_RB_PREWRITE(from, 16);
    if (pfl) LOC_RB_PREWRITE(pfl, 4);
    if (!rb_mine()) return o_recvfrom(s, buf, len, flags, from, pfl);
    if (s_spec) { s_spec_ev.recv++; s_fake_err = WSAEWOULDBLOCK_X; return 0xFFFFFFFFu; } /* W390 */
    ih = s * 31u + len * 7u + flags;
    if (s_phase == PH_RECORD) {
        u32 n = 0, fl;
        ret = o_recvfrom(s, buf, len, flags, from, pfl);
        fl = pfl ? *(const u32 *)(uintptr_t)pfl : 0;
        if (fl > 16) fl = 16;
        memcpy(tmp, &fl, 4);
        if (from && fl) memcpy(tmp + 4, (const void *)(uintptr_t)from, fl);
        n = 20;
        if ((int)ret > 0 && ret <= len && ret <= 0x1100) { memcpy(tmp + n, (const void *)(uintptr_t)buf, ret); n += ret; }
        rb_rec_put(EV_RECVFROM, ret, ih, tmp, n);
        return ret;
    } else {
        const uint8_t *d;
        u32 fl;
        const rb_ev_t *e = rb_rep_get(EV_RECVFROM, ih, &d);
        if (!e) { s_fake_err = WSAEWOULDBLOCK_X; return 0xFFFFFFFFu; }
        memcpy(&fl, d, 4);
        if (pfl) *(u32 *)(uintptr_t)pfl = fl;
        if (from && fl) memcpy((void *)(uintptr_t)from, d + 4, fl);
        if (e->n > 20) memcpy((void *)(uintptr_t)buf, d + 20, e->n - 20u);
        return e->ret;
    }
}

/* W355: Beim Neurechnen (R2) ein Senden gleicher Laenge auf demselben Socket im selben Tick suchen. Der TCP-Strom
 * hat die Nachricht der ersten Rechnung schon - eine zweite Kopie (nur der Inhalt, z. B. das Sync-Byte des
 * 0x34-Herzschlags, ist anders) waere eine doppelte Nachricht beim Mitspieler. W346 schickte bei jedem
 * ueberlappenden Rollback jede solche Nachricht erneut live (Nutzerlauf: 612 Kopien in 5 s). */
u32 s_rbi_send_same; /* W355: beim Neurechnen als schon gesendet gewertet (gleiche Laenge, anderer Inhalt) */
u32 s_rbi_sendto_rep, s_rbi_sendto_same; /* W355: Datagramme beim Neurechnen */
u32 s_rbi_send_rep, s_rbi_send_exact, s_rbi_send_nosock, s_rbi_send_otherlen, s_rbi_send_logged; /* W355 DIAG */
static const rb_ev_t *rb_rep_get_sl(int fn, u32 s, u32 len, u32 toh)
{
    u32 c = s_rep_kc[fn], nk = fn == EV_SENDTO ? 12u : 8u;
    while (c + sizeof(rb_ev_t) <= s_rep_end) {
        const rb_ev_t *e = (const rb_ev_t *)(s_rec + c);
        u32 nx = c + (u32)sizeof *e + e->n;
        if (e->fn == (uint8_t)fn && e->n == nk) {
            const u32 *d = (const u32 *)(e + 1);
            if (d[0] == s && d[1] == len && (nk == 8u || d[2] == toh)) {
                s_rep_kc[fn] = nx;
                s_cnt_ev[fn][1]++;
                return e;
            }
        }
        c = nx;
    }
    return NULL;
}

u32 __stdcall rb_send(u32 s, u32 buf, u32 len, u32 flags)
{
    u32 ih;
    if (!rb_mine()) return o_send(s, buf, len, flags);
    if (s_spec) { s_spec_ev.send++; return len; } /* W390: Vorlauf sendet nie */
    ih = fnv(s * 31u + len, (const void *)(uintptr_t)buf, len > 0x2000 ? 0x2000 : len); /* Inhalt nur als Hash */
    if (s_phase == PH_RECORD) {
        u32 ret = o_send(s, buf, len, flags), sl[2];
        sl[0] = s; /* W355: Socket und Laenge (kein Inhalt) fuer die Zuordnung beim Neurechnen */
        sl[1] = len;
        rb_rec_put(EV_SEND, ret, ih, sl, 8u);
        return ret;
    } else {
        const rb_ev_t *e = rb_rep_get(EV_SEND, ih, NULL); /* unterdrueckt: nicht senden */
        if (s_rbi_ep) s_rbi_send_rep++;
        if (e && s_rbi_ep) s_rbi_send_exact++;
        if (!e && s_rbi_ep) {
            e = rb_rep_get_sl(EV_SEND, s, len, 0);
            if (e) { s_rbi_send_same++; return e->ret; }
            { /* W355 DIAG: was lag in diesem Tick an Sendungen vor (nur Socket/Laengen, kein Inhalt) */
                u32 c = s_rbi_toff[rbi_gtick(s_rbc) & (RBI_GH - 1u)], nsock = 0, nother = 0, l0 = 0, n = 0;
                while (c + sizeof(rb_ev_t) <= s_rep_end) {
                    const rb_ev_t *x = (const rb_ev_t *)(s_rec + c);
                    if (x->fn == EV_SEND && x->n == 8u) {
                        const u32 *dd = (const u32 *)(x + 1);
                        n++;
                        if (dd[0] == s) { nsock++; if (dd[1] != len) { nother++; if (!l0) l0 = dd[1]; } }
                    }
                    c += (u32)sizeof *x + x->n;
                }
                if (!nsock) s_rbi_send_nosock++;
                else s_rbi_send_otherlen++;
                if (s_rbi_send_logged < 12) {
                    s_rbi_send_logged++;
                    HLOG_I("LOCRBI: DIAG Neurechnen Senden live: Tick %u Laenge %u | Aufzeichnung des Ticks: %u Sendungen, %u auf "
                           "dem Socket, %u mit anderer Laenge (erste %u), Fenster %u..%u", rbi_gtick(s_rbc), len, n, nsock, nother,
                           l0, s_rbi_toff[rbi_gtick(s_rbc) & (RBI_GH - 1u)], s_rep_end);
                }
            }
            s_rbi_send_live++;
            return o_send(s, buf, len, flags); /* W346: nur die korrigierte Zeitlinie sendet */
        }
        return e ? e->ret : len;
    }
}

u32 __stdcall rb_sendto(u32 s, u32 buf, u32 len, u32 flags, u32 to, u32 tolen)
{
    u32 ih;
    if (!rb_mine()) return o_sendto(s, buf, len, flags, to, tolen);
    if (s_spec) { s_spec_ev.send++; return len; } /* W390 */
    ih = fnv(s * 31u + len, (const void *)(uintptr_t)buf, len > 0x2000 ? 0x2000 : len);
    if (s_phase == PH_RECORD) {
        u32 ret = o_sendto(s, buf, len, flags, to, tolen), sl[3];
        sl[0] = s; /* W355: Socket, Laenge, Ziel (Hash, kein Inhalt) fuer die Zuordnung beim Neurechnen */
        sl[1] = len;
        sl[2] = fnv(2166136261u, (const void *)(uintptr_t)to, to && tolen <= 32u ? tolen : 0u);
        rb_rec_put(EV_SENDTO, ret, ih, sl, 12u);
        return ret;
    } else {
        const rb_ev_t *e = rb_rep_get(EV_SENDTO, ih, NULL);
        if (s_rbi_ep) s_rbi_sendto_rep++;
        if (!e && s_rbi_ep) { /* W355: gleiches Datagramm (Socket/Laenge/Ziel) der ersten Rechnung = schon gesendet */
            e = rb_rep_get_sl(EV_SENDTO, s, len, fnv(2166136261u, (const void *)(uintptr_t)to, to && tolen <= 32u ? tolen : 0u));
            if (e) { s_rbi_sendto_same++; return e->ret; }
            s_rbi_send_live++;
            return o_sendto(s, buf, len, flags, to, tolen); /* W346: nur die korrigierte Zeitlinie sendet */
        }
        return e ? e->ret : len;
    }
}

u32 __stdcall rb_ioctl(u32 s, u32 cmd, u32 argp)
{
    u32 ih;
    if (argp) LOC_RB_PREWRITE(argp, 4); /* W330 */
    if (!rb_mine()) return o_ioctl(s, cmd, argp);
    if (s_spec) { s_spec_ev.ioctl++; if (argp) *(u32 *)(uintptr_t)argp = 0; return 0; } /* W390: 0 Bytes bereit */
    ih = s * 31u + cmd;
    if (s_phase == PH_RECORD) {
        u32 ret = o_ioctl(s, cmd, argp), v = argp ? *(const u32 *)(uintptr_t)argp : 0;
        rb_rec_put(EV_IOCTL, ret, ih, &v, 4);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_IOCTL, ih, &d);
        if (!e) return 0;
        if (argp) memcpy((void *)(uintptr_t)argp, d, 4);
        return e->ret;
    }
}

u32 __stdcall rb_wsaerr(void)
{
    if (!rb_mine()) return o_wsaerr();
    if (s_spec) { u32 f = s_fake_err; s_spec_ev.wsa++; s_fake_err = 0; return f ? f : WSAEWOULDBLOCK_X; } /* W390 */
    if (s_phase == PH_RECORD) {
        u32 ret = o_wsaerr();
        rb_rec_put(EV_WSAERR, ret, 0, NULL, 0);
        return ret;
    } else {
        const rb_ev_t *e = rb_rep_get(EV_WSAERR, 0, NULL);
        if (!e) { u32 f = s_fake_err; s_fake_err = 0; return f ? f : WSAEWOULDBLOCK_X; }
        return e->ret;
    }
}

long long s_lat_xin_q; /* W386: Zeitpunkt der letzten SBC-Abfrage des Spiel-Threads (live, QPC) */
u32 s_lat_xin_n;        /* W386: SBC-Abfragen live */
u32 __stdcall rb_xin(u32 h, u32 pState)
{
    if (pState) LOC_RB_PREWRITE(pState, 32); /* W330 */
    if (!rb_mine() || !pState) return o_xin(h, pState);
    if (s_spec) { /* W390: Vorlauf fragt das Geraet live ab (genau einmal je Handle); der folgende Live-Tick bekommt
                   * denselben Stand (keine doppelte Abfrage, kein verlorener Mausweg). Inhalt bleibt im Speicher. */
        u32 k, n = loc_pad_is_handle(h) ? 22u : RB_XIN_LEN, ret;
        for (k = 0; k < s_spec_xcn; k++) {
            if (s_spec_xc[k].h == h) {
                memcpy((void *)(uintptr_t)pState, s_spec_xc[k].d, s_spec_xc[k].n);
                s_spec_ev.xin_rep++;
                return s_spec_xc[k].ret;
            }
        }
        ret = o_xin(h, pState);
        s_spec_ev.xin_live++;
        if (s_spec_xcn < RB_SPEC_XC && n <= sizeof s_spec_xc[0].d) {
            s_spec_xc[s_spec_xcn].h = h;
            s_spec_xc[s_spec_xcn].ret = ret;
            s_spec_xc[s_spec_xcn].n = n;
            s_spec_xc[s_spec_xcn].used = 0;
            memcpy(s_spec_xc[s_spec_xcn].d, (const void *)(uintptr_t)pState, n);
            s_spec_xcn++;
        }
        return ret;
    }
    if (s_phase == PH_RECORD) {
        u32 ret, k;
        int cached = 0;
        if (!loc_pad_is_handle(h)) { /* W386: nur Zeitpunkt, keine Eingabe */
            s_lat_xin_q = rb_now();
            s_lat_xin_n++;
        }
        for (k = 0; k < s_spec_xcn; k++) { /* W390: im Vorlauf schon abgefragt -> denselben Stand verwenden */
            if (s_spec_xc[k].h == h && !s_spec_xc[k].used) {
                s_spec_xc[k].used = 1;
                memcpy((void *)(uintptr_t)pState, s_spec_xc[k].d, s_spec_xc[k].n);
                ret = s_spec_xc[k].ret;
                cached = 1;
                s_spec_xc_hit++;
                break;
            }
        }
        if (!cached) ret = o_xin(h, pState);
        /* Laenge je Geraet: virtuelles Gamepad 4+18 Byte, SBC 4+24 Byte (nur im Speicher) */
        rb_rec_put(EV_XIN, ret, h, (const void *)(uintptr_t)pState, loc_pad_is_handle(h) ? 22u : RB_XIN_LEN);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_XIN, h, &d);
        if (!e) return o_xin(h, pState);
        memcpy((void *)(uintptr_t)pState, d, e->n);
        if (s_ctl && e->n >= RB_XIN_LEN && !loc_pad_is_handle(h)) { /* Gegenprobe (nur SBC): Hebel, Gas, Zielen */
            sbc_in_t *si = (sbc_in_t *)(uintptr_t)(pState + 4);
            si->aiming_x ^= 0x80u;
            si->aiming_y ^= 0x80u;
            si->rot_lever = 127;
            si->pedal_r = 255;
            si->pedal_m = 0;
        }
        return e->ret;
    }
}

/* ---- W330 xact=1: XACT-Aufrufe des Spiel-Threads aufzeichnen; beim Neurechnen aus der Aufzeichnung bedienen,
 * nichts starten/stoppen (kein doppelter Ton, gleiche Cue-Handles im Gast-Speicher). Vorlage fuer R3. ---- */
int s_xact;
u32 s_xc_dup, s_xc_new, s_xc_orphan; /* W346 R3: Ton beim Neurechnen doppelt / nachgeholt / verwaist (Zaehler) */
u32 s_xc_orphan_stop; /* W375: davon als Schleife gestoppt */
int s_xkey_legacy;    /* W381 DIAG loc_rb_opts xkey=0: Schluessel wie bis W380 (Gegenprobe #61) */
u32 s_xc_stop_live;   /* W381: Stopps, die beim Neurechnen live liefen (nur die korrigierte Zeitlinie stoppt) */
fn3_t o_xgn, o_xplay, o_xprep;
fn4_t o_xstop;
fn2_t o_xstat;
fn1_t o_xstopsrc;
fn0_t o_xwork;
static u32 rb_rd_u32(u32 p)
{
    u32 v = 0;
    if (p) __try { v = *(const u32 *)(uintptr_t)p; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return v;
}
u32 __stdcall rb_xgn(u32 e, u32 pdesc, u32 pout)
{
    if (!rb_mine()) return o_xgn(e, pdesc, pout);
    if (s_spec) { s_spec_ev.xgn++; return 0x80004005u; } /* W390: keine Meldung im Vorlauf */
    if (s_phase == PH_RECORD) {
        u32 ret = o_xgn(e, pdesc, pout);
        rb_rec_put(EV_XGN, ret, 0, (const void *)(uintptr_t)pout, (ret == 0 && pout) ? 24u : 0u);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *ev = rb_rep_get(EV_XGN, 0, &d);
        if (!ev) return 0x80004005u; /* keine Meldung */
        if (ev->n && pout) { LOC_RB_PREWRITE(pout, 24); memcpy((void *)(uintptr_t)pout, d, ev->n); }
        return ev->ret;
    }
}
/* W346 R3: Cue-Starts beim Neurechnen. Erste Rechnung (Aufzeichnung) und bereits beim Neurechnen live gestartete Cues
 * (Nebentabelle) gelten im ganzen Neurechen-Fenster: gleicher Cue (Parameter-Hash) = wiederverwenden (kein zweiter
 * Start, gleicher Handle). Nur was die korrigierte Zeitlinie zusaetzlich startet, startet live (nachgeholt). */
rb_xside_t s_xside[RB_XSIDE];
u32 s_xside_n;
u32 s_xwin0, s_xwin1, s_xwin_g0, s_xwin_g1; /* Fenster der laufenden Neurechnung (Aufzeichnung, Spielticks) */

/* ---- W413 LOC-SOUND-LOOP (#65): Ton nach dem Neurechnen mit der korrigierten Zeitlinie abgleichen (loc_rb_sndloop.h).
 * loc_sndloop_fix (Standard 1) stoppt, loc_sndloop_diag zaehlt/loggt auch ohne Fix (Gegenprobe: was liefe weiter). ---- */
rbs_net_t s_rbs_net[RBS_NET];
u32 s_rbs_net_n, s_rbs_net_drop, s_rbs_net_stop, s_rbs_net_would, s_rbs_net_umiss;
u32 s_rbs_rec_loop, s_rbs_rec_dead;          /* verwaist aus der Aufzeichnung: davon Schleife / als tot markiert */
u32 s_rbs_side_orphan, s_rbs_side_loop, s_rbs_side_stop; /* nachgeholte Waisen (Nebentabelle) */
u32 s_rbs_gap, s_rbs_gap_loop, s_rbs_gap_stop;  /* erledigter Index-/Quellen-Stopp: Instanz spielte noch */
volatile LONG s_rbs_log_n;
static void rbs_net_add(u32 serial, u32 h, u32 pp, int touched, int origin)
{
    u32 i;
    if (!rbs_on() || g_cfg.loc_sndloop_grace <= 0 || !serial || !h || !pp) return;
    for (i = 0; i < s_rbs_net_n; i++)
        if (s_rbs_net[i].serial == serial) {
            s_rbs_net[i].touched |= (uint8_t)(touched != 0);
            return;
        }
    if (s_rbs_net_n >= RBS_NET) {
        s_rbs_net_drop++;
        return;
    }
    s_rbs_net[s_rbs_net_n].serial = serial;
    s_rbs_net[s_rbs_net_n].h = h;
    s_rbs_net[s_rbs_net_n].pp = pp;
    s_rbs_net[s_rbs_net_n].miss = 0;
    s_rbs_net[s_rbs_net_n].touched = (uint8_t)(touched != 0);
    s_rbs_net[s_rbs_net_n].origin = (uint8_t)origin;
    s_rbs_net_n++;
}
void rbs_net_touch(u32 serial)
{
    u32 i;
    for (i = 0; i < s_rbs_net_n; i++)
        if (s_rbs_net[i].serial == serial) s_rbs_net[i].touched = 1;
}
static u32 rb_rd_u32(u32 p);
/* Sicherheitsnetz je Live-Tick (nach dem Neurechnen, Spiel-Thread): Schleife, die eine Neurechnung beruehrt hat und deren
 * Handle-Ablage im Spiel sie grace Ticks lang nicht mehr enthaelt, endet. Unberuehrte Schleifen werden nur gezaehlt. */
void rbs_net_tick(void)
{
    u32 i, w = 0;
    for (i = 0; i < s_rbs_net_n; i++) {
        rbs_net_t *x = &s_rbs_net[i];
        int cue, loops, st;
        char nm[40];
        st = loc_audio_cue_info(x->serial, &cue, &loops, NULL, nm, (int)sizeof nm);
        if (st == 0) continue; /* beendet: Eintrag weg */
        if (st == 2 && loops) {
            u32 owner = rb_rd_u32(x->pp);
            if (!x->touched) {
                if (owner != x->h) s_rbs_net_umiss++; /* Gegenprobe: Ablage leer ohne Neurechnung (Netz greift dort nie) */
            } else if (lrs_net_step(owner, x->h, &x->miss, (u32)g_cfg.loc_sndloop_grace)) {
                if (rbs_act() && loc_audio_stop_serial(x->serial, 1)) {
                    s_rbs_net_stop++;
                    RBS_LOG("LOCSNDLOOP Netz: Cue %d '%s' (%s) ohne Besitzer seit %u Ticks -> gestoppt", cue, nm,
                            x->origin ? "nachgeholt" : "Aufzeichnung", (u32)x->miss);
                    continue;
                }
                if (!rbs_act() && x->miss == (uint16_t)g_cfg.loc_sndloop_grace) {
                    s_rbs_net_would++;
                    RBS_LOG("LOCSNDLOOP Netz: Cue %d '%s' (%s) ohne Besitzer seit %u Ticks -> spielt weiter (Fix aus)", cue,
                            nm, x->origin ? "nachgeholt" : "Aufzeichnung", (u32)x->miss);
                }
            }
        }
        s_rbs_net[w++] = *x;
    }
    s_rbs_net_n = w;
}
/* Erledigter Stopp nach Index (cue >= 0) bzw. StopSoundCues (cue < 0, Quelle src) am Neurechen-Tick: Instanzen, die die
 * korrigierte Zeitlinie zu diesem Zeitpunkt kennt (Nebentabelle, in dieser Neurechnung verwendete Aufzeichnung) und die
 * noch spielen, hat der damalige Live-Stopp nicht getroffen -> live stoppen. */
static void rbs_inst_cover(u32 serial, int32_t cue, u32 src, const char *from)
{
    int icue, loops, st;
    u32 isrc;
    char nm[40];
    if (!serial) return;
    st = loc_audio_cue_info(serial, &icue, &loops, &isrc, nm, (int)sizeof nm);
    if (st == 0 || !lrs_stop_covers(cue, src, icue, isrc)) return;
    s_rbs_gap++;
    if (loops) s_rbs_gap_loop++;
    if (rbs_act() && loc_audio_stop_serial(serial, 0)) s_rbs_gap_stop++;
    RBS_LOG("LOCSNDLOOP Stopp-Luecke: %s %d erledigt (erste Rechnung), Instanz Cue %d '%s' Schleife=%s (%s) spielte noch -> %s",
            cue >= 0 ? "Index" : "Quelle", cue >= 0 ? (int)cue : (int)(src & 0xFFFFu), icue, nm, loops ? "ja" : "nein",
            from, rbs_act() ? "gestoppt" : "spielt weiter (Fix aus)");
}
static void rbs_stop_cover(int32_t cue, u32 src)
{
    u32 c, i, t = rb_rbi_gtick();
    if (!rbs_on() || !s_rbi_ep) return;
    for (i = 0; i < s_xside_n; i++) {
        const rb_xside_t *x = &s_xside[i];
        if (lrs_side_in_timeline(x->g, x->used, s_xwin_g0, t)) rbs_inst_cover(x->serial, cue, src, "nachgeholt");
    }
    for (c = s_xwin0; c + sizeof(rb_ev_t) <= s_xwin1 && s_xwin1 <= s_rec_len;) {
        const rb_ev_t *e = (const rb_ev_t *)(s_rec + c);
        if ((e->fn == EV_XPLAY || e->fn == EV_XPREP) && (e->pad & LRS_PAD_USED) && e->n >= 8u)
            rbs_inst_cover(((const u32 *)(e + 1))[1], cue, src, "erste Rechnung, spaeter im Fenster");
        c += (u32)sizeof *e + e->n;
    }
}
static const rb_ev_t *rb_rep_xcue(int fn, u32 ih, const uint8_t **d, u32 *side_h, int *side_hit)
{
    u32 c, pass, cur0 = s_rec_cur;
    *side_hit = 0;
    for (pass = 0; pass < 2; pass++) { /* zuerst ab dem laufenden Tick, dann vom Fensterbeginn */
        u32 a = pass ? s_xwin0 : cur0, b = pass ? cur0 : s_xwin1;
        for (c = a; c + sizeof(rb_ev_t) <= b;) {
            rb_ev_t *e = (rb_ev_t *)(s_rec + c);
            if (e->fn == (uint8_t)fn && e->ih == ih && !(e->pad & (LRS_PAD_USED | LRS_PAD_DEAD))) { /* W413: tote nie */
                e->pad |= 1;
                s_cnt_ev[fn][1]++;
                *d = (const uint8_t *)(e + 1);
                return e;
            }
            c += (u32)sizeof *e + e->n;
        }
    }
    for (c = 0; c < s_xside_n; c++) {
        rb_xside_t *x = &s_xside[c];
        if (x->fn == (uint8_t)fn && x->ih == ih && !x->used && x->g >= s_xwin_g0 && x->g < s_xwin_g1) {
            x->used = 1;
            *side_h = x->h;
            *side_hit = 1;
            return NULL;
        }
    }
    return NULL;
}
static int rb_xcue_seen(int fn, u32 ih) /* gleicher Cue im Fenster schon verwendet (Zaehler doppelt) */
{
    u32 c;
    for (c = s_xwin0; c + sizeof(rb_ev_t) <= s_xwin1;) {
        const rb_ev_t *e = (const rb_ev_t *)(s_rec + c);
        if (e->fn == (uint8_t)fn && e->ih == ih) return 1;
        c += (u32)sizeof *e + e->n;
    }
    return 0;
}
static void rb_xside_add(int fn, u32 ih, u32 ret, u32 h, u32 serial)
{
    u32 g = rb_rbi_gtick(), i, w = 0;
    for (i = 0; i < s_xside_n; i++) /* alte Eintraege verwerfen */
        if (s_xside[i].g + RBI_GH / 2u > g) s_xside[w++] = s_xside[i];
    s_xside_n = w;
    if (s_xside_n >= RB_XSIDE) return;
    s_xside[s_xside_n].g = g;
    s_xside[s_xside_n].ih = ih;
    s_xside[s_xside_n].ret = ret;
    s_xside[s_xside_n].h = h;
    s_xside[s_xside_n].serial = serial;
    s_xside[s_xside_n].fn = (uint8_t)fn;
    s_xside[s_xside_n].used = 1;
    s_xside[s_xside_n].live = 1;
    s_xside_n++;
}
static u32 rb_xcue_call(int fn, fn3_t o, u32 sb, u32 params, u32 pp)
{
    u32 ih;
    if (s_spec) { s_spec_ev.xcue++; return 0; } /* W390: kein Ton aus dem Vorlauf (S_OK wie stumm, Handle unberuehrt) */
    ih = fnv(2166136261u, (const void *)(uintptr_t)params, params ? 12u : 0u) * 31u + (pp != 0);
    if (s_rbi_ep && s_phase == PH_REPLAY) {
        const uint8_t *d = NULL;
        u32 sh = 0, ret, h;
        int side = 0;
        const rb_ev_t *ev = rb_rep_xcue(fn, ih, &d, &sh, &side);
        if (ev) {
            if (ev->n && pp) { LOC_RB_PREWRITE(pp, 4); memcpy((void *)(uintptr_t)pp, d, 4); }
            return ev->ret;
        }
        if (side) {
            if (pp) { LOC_RB_PREWRITE(pp, 4); *(u32 *)(uintptr_t)pp = sh; }
            return 0;
        }
        if (rb_xcue_seen(fn, ih)) s_xc_dup++; /* korrigierte Zeitlinie startet denselben Cue oefter als die erste */
        s_xc_new++;
        if (pp) LOC_RB_PREWRITE(pp, 4);
        {
            u32 s0 = loc_audio_last_serial(), ser;
            ret = o(sb, params, pp); /* nachgeholt: nur die korrigierte Zeitlinie startet ihn */
            h = rb_rd_u32(pp);
            ser = loc_audio_last_serial(); /* W413: laufende Nummer (Waise der naechsten Neurechnung, Netz) */
            if (ser == s0) ser = 0;
            rb_xside_add(fn, ih, ret, h, ser);
            if (ser && rbs_on()) {
                int lp = 0, st = loc_audio_cue_info(ser, NULL, &lp, NULL, NULL, 0);
                if (fn == EV_XPREP || (st == 2 && lp)) rbs_net_add(ser, h, pp, 1, 1);
            }
        }
        return ret;
    }
    if (s_phase == PH_RECORD) {
        /* W375 (#51): neben dem Handle die laufende Cue-Nummer mitschreiben (0 = kein neuer Cue), damit eine Schleife,
         * die nur diese Zeitlinie startet, nach einem Rollback gezielt gestoppt werden kann (rbi_replay_finish). */
        u32 s0 = loc_audio_last_serial(), hs[2], ret = o(sb, params, pp);
        hs[0] = rb_rd_u32(pp);
        hs[1] = loc_audio_last_serial();
        if (hs[1] == s0) hs[1] = 0;
        rb_rec_put(fn, ret, ih, hs, 8u);
        if (s_rbi_ep && hs[1] && rbs_on()) { /* W413: Schleifen fuer das Sicherheitsnetz merken (beruehrt erst im Fenster) */
            int lp = 0, st = loc_audio_cue_info(hs[1], NULL, &lp, NULL, NULL, 0);
            if (fn == EV_XPREP || (st == 2 && lp)) rbs_net_add(hs[1], hs[0], pp, 0, 0);
        }
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *ev = rb_rep_get(fn, ih, &d);
        if (!ev) return 0;
        if (ev->n && pp) { LOC_RB_PREWRITE(pp, 4); memcpy((void *)(uintptr_t)pp, d, 4); }
        return ev->ret;
    }
}
u32 __stdcall rb_xplay(u32 sb, u32 params, u32 pp)
{
    if (!rb_mine()) return o_xplay(sb, params, pp);
    return rb_xcue_call(EV_XPLAY, o_xplay, sb, params, pp);
}
u32 __stdcall rb_xprep(u32 sb, u32 params, u32 pp)
{
    if (!rb_mine()) return o_xprep(sb, params, pp);
    return rb_xcue_call(EV_XPREP, o_xprep, sb, params, pp);
}
u32 __stdcall rb_xstop(u32 sb, u32 cue, u32 flags, u32 pcue)
{
    if (!rb_mine()) return o_xstop(sb, cue, flags, pcue);
    if (s_spec) { s_spec_ev.xstop++; return 0; } /* W390 */
    u32 key = loc_rb_xkey_stop(cue, flags, pcue, s_xkey_legacy); /* W381 (#61): Handle im Schluessel */
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstop(sb, cue, flags, pcue);
        rb_rec_put(EV_XSTOP, ret, key, NULL, 0);
        return ret;
    } else {
        const rb_ev_t *ev = rb_rep_get(EV_XSTOP, key, NULL);
        if (!ev && s_rbi_ep) { s_xc_stop_live++; return o_xstop(sb, cue, flags, pcue); } /* W346: nur die korrigierte Zeitlinie stoppt */
        if (ev && s_rbi_ep && (int)cue >= 0) rbs_stop_cover((int32_t)cue, 0); /* W413: Index-Stopp erledigt - Luecken */
        return ev ? ev->ret : 0;
    }
}
u32 __stdcall rb_xstat(u32 sp, u32 pst)
{
    if (!rb_mine()) return o_xstat(sp, pst);
    if (s_spec) return o_xstat(sp, pst); /* W390: nur lesend */
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstat(sp, pst), v = rb_rd_u32(pst);
        rb_rec_put(EV_XSTAT, ret, loc_rb_xkey_src(sp, s_xkey_legacy), &v, pst ? 4u : 0u); /* W381: Quelle im Schluessel */
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *ev = rb_rep_get(EV_XSTAT, loc_rb_xkey_src(sp, s_xkey_legacy), &d);
        if (!ev) return o_xstat(sp, pst);
        if (ev->n && pst) { LOC_RB_PREWRITE(pst, 4); memcpy((void *)(uintptr_t)pst, d, 4); }
        return ev->ret;
    }
}
u32 __stdcall rb_xstopsrc(u32 sp)
{
    if (!rb_mine()) return o_xstopsrc(sp);
    if (s_spec) { s_spec_ev.xstop++; return 0; } /* W390 */
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstopsrc(sp);
        rb_rec_put(EV_XSTOPSRC, ret, loc_rb_xkey_src(sp, s_xkey_legacy), NULL, 0); /* W381: Quelle im Schluessel */
        return ret;
    } else {
        const rb_ev_t *ev = rb_rep_get(EV_XSTOPSRC, loc_rb_xkey_src(sp, s_xkey_legacy), NULL);
        if (!ev && s_rbi_ep) { s_xc_stop_live++; return o_xstopsrc(sp); } /* W346 */
        if (ev && s_rbi_ep && sp) rbs_stop_cover(-1, sp); /* W413: StopSoundCues erledigt - Luecken */
        return ev ? ev->ret : 0;
    }
}
u32 __stdcall rb_xwork(void)
{
    if (!rb_mine()) return o_xwork();
    if (s_spec) return 0; /* W390: kein Ton-Fortschritt im Vorlauf */
    if (s_phase == PH_RECORD) return o_xwork(); /* nicht aufzeichnen: auch aus dem Feld-Warten (entfaellt beim Neurechnen) */
    s_cnt_ev[EV_XWORK][1]++;                     /* beim Neurechnen zurueckgehalten (kein Ton-Fortschritt) */
    return 0;
}

/* Akkumulator (Zustandssumme je Lockstep-Rahmen) vor dem Lesen+Loeschen abgreifen. Im Neurechnen
 * direkt zur Spielfunktion (loc_sync zaehlt dann nicht mit: seine Rahmen/Hashes bleiben die der
 * Zeitlinie, die der Mitspieler sieht). */
int s_rbi_acc_chained;  /* W338: Akkumulator-Kette schon bei der Installation */
u32 s_acc_next = RB_ACC_FN; /* bisheriges Ziel (loc_sync-Thunk oder Spielfunktion) */
static u32 s_acc_game = RB_ACC_FN;
static u32 s_acc_tgt;
u32 s_acc_rec[RB_ACC_MAX], s_acc_rep[RB_ACC_MAX];
u32 s_nacc_rec, s_nacc_rep;
static void __cdecl rb_acc_note(void)
{
    u32 v = rd32(RB_ACC) ^ (rd32(RB_TICK) * 0x9E3779B1u);
    s_acc_tgt = s_acc_next;
    if (s_spec && GetCurrentThreadId() == s_game_tid) { /* W390: Vorlauf - kein Hash, kein loc_sync (direkt zur Spielfunktion) */
        s_spec_ev.acc++;
        s_acc_tgt = s_acc_game;
        return;
    }
    if (rb_r2_acc()) { /* W338: vor der Thread-Pruefung (Spiel-Thread ist vor dem Ringstart noch unbekannt) */ /* W338: Akkumulator je Slot in die Eingabeschicht; im Neurechnen loc_sync auslassen */
        if (s_phase == PH_REPLAY) s_acc_tgt = s_acc_game;
        return;
    }
    if (GetCurrentThreadId() != s_game_tid) return;
    if (s_phase == PH_RECORD) {
        if (s_nacc_rec < RB_ACC_MAX) s_acc_rec[s_nacc_rec] = v;
        s_nacc_rec++;
    } else if (s_phase == PH_REPLAY) {
        if (s_nacc_rep < RB_ACC_MAX) s_acc_rep[s_nacc_rep] = v;
        s_nacc_rep++;
        s_acc_tgt = s_acc_game;
    }
}
__declspec(naked) void rb_acc_thunk(void)
{
    __asm {
        pushad
        pushfd
        call rb_acc_note
        popfd
        popad
        jmp dword ptr [s_acc_tgt]
    }
}

/* rel32 an p+1 schreiben; atomar, wenn die 4 Bytes in einem ausgerichteten Quadwort liegen (andere
 * Threads koennen die Stelle gleichzeitig ausfuehren). */
static void rb_put_rel(uint8_t *p, intptr_t rel)
{
    DWORD old;
    uintptr_t a = (uintptr_t)(p + 1);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    if ((a & 7u) <= 4u) {
        volatile LONGLONG *q = (volatile LONGLONG *)(a & ~(uintptr_t)7u);
        LONGLONG cur, nw;
        do {
            cur = *q;
            nw = cur;
            memcpy((uint8_t *)&nw + (a & 7u), &rel, 4);
        } while (InterlockedCompareExchange64(q, nw, cur) != cur);
    } else {
        memcpy(p + 1, &rel, 4);
    }
    VirtualProtect(p, 5, old, &old);
}

/* ---- Sprungstellen verketten: E9 rel32 an va -> bisheriges Ziel merken, auf mine umlenken ---- */
int rb_chain_jmp(u32 va, void *mine, void **orig, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    intptr_t rel;
    DWORD old;
    if (p[0] != 0xE9) {
        HLOG_W("LOCRB: %s @0x%X kein JMP (0x%02X) - nicht verkettet", name, va, p[0]);
        return 0;
    }
    *orig = (void *)(uintptr_t)(va + 5 + *(int32_t *)(p + 1));
    rel = (uint8_t *)mine - (p + 5);
    (void)old;
    rb_put_rel(p, rel);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}
int rb_chain_call(u32 va, void *mine, u32 *orig, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    intptr_t rel;
    DWORD old;
    if (p[0] != 0xE8) {
        HLOG_W("LOCRB: %s @0x%X kein CALL (0x%02X) - nicht verkettet", name, va, p[0]);
        return 0;
    }
    *orig = va + 5 + *(int32_t *)(p + 1);
    rel = (uint8_t *)mine - (p + 5);
    (void)old;
    rb_put_rel(p, rel);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

/* ================= HLE-Zensus: welche Kernel-/XDK-HLE-Aufrufe macht der Spiel-Thread je Phase ========= */
static uint8_t *s_hc_pool;
static u32 s_hc_site[RB_HC_MAX];   /* Kernel: Thunk-Index | 0x80000000, XDK: Gast-VA */
static u32 s_hc[RB_HC_MAX][3];     /* [Phase] Aufrufe */
static unsigned s_nhc;
static void __cdecl rb_hcount(u32 idx)
{
    if (idx < RB_HC_MAX && s_phase != PH_IDLE && GetCurrentThreadId() == s_game_tid) s_hc[idx][s_phase]++;
}
static void *rb_hc_slot(u32 idx, void *target)
{
    uint8_t *t = s_hc_pool + idx * 32u;
    u32 cell = (u32)(uintptr_t)(t + 28);
    u32 fnp = (u32)(uintptr_t)&rb_hcount;
    t[0] = 0x60;                 /* pushad */
    t[1] = 0x9C;                 /* pushfd */
    t[2] = 0x68; memcpy(t + 3, &idx, 4);        /* push idx */
    t[7] = 0xB8; memcpy(t + 8, &fnp, 4);        /* mov eax, rb_hcount */
    t[12] = 0xFF; t[13] = 0xD0;  /* call eax */
    t[14] = 0x83; t[15] = 0xC4; t[16] = 0x04;   /* add esp,4 */
    t[17] = 0x9D;                /* popfd */
    t[18] = 0x61;                /* popad */
    t[19] = 0xFF; t[20] = 0x25; memcpy(t + 21, &cell, 4); /* jmp [cell] */
    memcpy(t + 28, &target, 4);
    return t;
}

static int rb_is_code(u32 a)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void rb_census_install(void)
{
    xbe_image_t *img = rb_img();
    HMODULE me = NULL;
    u32 dll_lo = 0, dll_hi = 0, nk = 0, nx = 0;
    unsigned i;
    if (!img) return;
    s_hc_pool = (uint8_t *)VirtualAlloc(NULL, RB_HC_MAX * 32u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s_hc_pool) return;
    memset(s_hc_pool, 0xCC, RB_HC_MAX * 32u);
    /* Kernel-Thunks: Funktionsziele (keine Variablen); Ordinal aus der Programmdatei */
    {
        u32 *kt = (u32 *)(uintptr_t)img->kt_addr;
        static u32 raw[512];
        FILE *f = NULL;
        unsigned si;
        memset(raw, 0, sizeof raw);
        if (g_cfg.xbe_path[0] && fopen_s(&f, g_cfg.xbe_path, "rb") == 0 && f) {
            for (si = 0; si < img->n_sections; si++) {
                const section_info_t *s = &img->sections[si];
                if (img->kt_addr >= s->vaddr && img->kt_addr < s->vaddr + s->rawsize) {
                    if (fseek(f, (long)(s->rawaddr + img->kt_addr - s->vaddr), SEEK_SET) == 0) {
                        size_t got = fread(raw, 4, 512, f);
                        (void)got;
                    }
                    break;
                }
            }
            fclose(f);
        }
        DWORD kold;
        VirtualProtect(kt, 512 * 4, PAGE_EXECUTE_READWRITE, &kold);
        for (i = 0; i < 512 && kt[i] && s_nhc < RB_HC_MAX; i++) {
            if (!rb_is_code(kt[i])) continue;
            s_hc_site[s_nhc] = 0x80000000u | (i << 16) | (raw[i] & 0x1FFu);
            InterlockedExchange((volatile LONG *)&kt[i], (LONG)(uintptr_t)rb_hc_slot(s_nhc, (void *)(uintptr_t)kt[i]));
            s_nhc++;
            nk++;
        }
        VirtualProtect(kt, 512 * 4, kold, &kold);
    }
    /* XDK-Hooks: E9-Spruenge im Abbild, die von der Programmdatei abweichen und in sb_emu.dll zeigen */
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (const char *)(void *)rb_census_install, &me);
    if (me) {
        const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)me;
        const IMAGE_NT_HEADERS *nh = (const IMAGE_NT_HEADERS *)((const uint8_t *)me + dh->e_lfanew);
        dll_lo = (u32)(uintptr_t)me;
        dll_hi = dll_lo + nh->OptionalHeader.SizeOfImage;
    }
    if (dll_lo && g_cfg.xbe_path[0]) {
        FILE *f = NULL;
        if (fopen_s(&f, g_cfg.xbe_path, "rb") == 0 && f) {
            unsigned si;
            for (si = 0; si < img->n_sections; si++) {
                const section_info_t *s = &img->sections[si];
                uint8_t *orig;
                u32 j;
                if (!s->rawsize || !(s->flags & 4u)) continue; /* nur ausfuehrbare Abschnitte */
                orig = (uint8_t *)malloc(s->rawsize);
                if (!orig) continue;
                if (fseek(f, (long)s->rawaddr, SEEK_SET) != 0 || fread(orig, 1, s->rawsize, f) != s->rawsize) {
                    free(orig);
                    continue;
                }
                for (j = 0; j + 5 <= s->rawsize && s_nhc < RB_HC_MAX; j++) {
                    uint8_t *p = (uint8_t *)(uintptr_t)(s->vaddr + j);
                    u32 tgt, va = s->vaddr + j;
                    if (p[0] != 0xE9 || memcmp(p, orig + j, 5) == 0) continue;
                    tgt = va + 5 + *(int32_t *)(p + 1);
                    if (tgt < dll_lo || tgt >= dll_hi) continue;
                    if (va == RB_SELECT || va == RB_RECV || va == RB_RECVFROM || va == RB_SEND || va == RB_SENDTO ||
                        va == RB_IOCTL || va == RB_WSAERR || va == RB_XIN_VA) continue; /* eigene Kette */
                    {
                        void *slot = rb_hc_slot(s_nhc, (void *)(uintptr_t)tgt);
                        rb_put_rel(p, (uint8_t *)slot - (p + 5));
                        s_hc_site[s_nhc] = va;
                        s_nhc++;
                        nx++;
                    }
                    j += 4;
                }
                free(orig);
            }
            fclose(f);
            FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        }
    }
    HLOG_I("LOCRB: HLE-Zensus aktiv: %u Kernel-Thunks, %u XDK-Sprungstellen (sb_emu 0x%X-0x%X)", nk, nx, dll_lo,
           dll_hi);
}

void rb_census_log(void)
{
    unsigned i, nl = 0;
    HLOG_I("LOCRB: ZENSUS Ticks Vorlauf=%u Neurechnen=%u", s_nt_rec, s_nt_rep);
    for (i = 0; i < s_nhc; i++) {
        if (!s_hc[i][PH_RECORD] && !s_hc[i][PH_REPLAY]) continue;
        if (s_hc_site[i] & 0x80000000u) {
            u32 ord = s_hc_site[i] & 0x1FFu;
            HLOG_I("LOCRB: ZENSUS Kernel %u %s Vorlauf=%u Neurechnen=%u", ord, ord ? krnl_name(ord) : "?",
                   s_hc[i][PH_RECORD], s_hc[i][PH_REPLAY]);
        } else {
            HLOG_I("LOCRB: ZENSUS XDK @0x%X Vorlauf=%u Neurechnen=%u", s_hc_site[i], s_hc[i][PH_RECORD],
                   s_hc[i][PH_REPLAY]);
        }
        if (++nl >= 200) break;
    }
}

