/* launcher_assist.c - W143 CTRL-ASSIST: Engine des Controller-Assistenten.
 *
 * Quelle (USB-Geraet per input_sbc_usb oder Fixture-Datei per sbc_fixture),
 * gefuehrte Pruefung/Kalibrierung ueber den gemeinsamen Kern sbc_test auf
 * einem Arbeiter-Thread, Lampensteuerung (Geraet sendet, Fixture zeigt Mock)
 * und chirurgisches Schreiben der eigenen user.cfg-Zeilen.
 * ASCII only (Umlaute kommen aus den Sprachdateien der Oberflaeche).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_assist.h"
#include "sbc_fixture.h"

/* Eigene user.cfg-Keys (nur diese schreibt/entfernt der Assistent). */
static const char *const kOwn[] = {
    "sbc_cal_aim_x", "sbc_cal_aim_y", "sbc_cal_rot",
    "sbc_cal_sight_x", "sbc_cal_sight_y",
    "sbc_cal_pedal_l", "sbc_cal_pedal_m", "sbc_cal_pedal_r",
    "sbc_dead_rot", "sbc_dead_sight_x", "sbc_dead_sight_y",
    "sbc_dead_pedal_l", "sbc_dead_pedal_m", "sbc_dead_pedal_r",
    "pad_dz_stick", "pad_dz_trig",
    NULL
};

int assist_cfg_owns(const char *key)
{
    int i;
    if (!key) {
        return 0;
    }
    for (i = 0; kOwn[i]; i++) {
        if (strcmp(key, kOwn[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Zeile = "key =..." (Toleranz wie lcfg line_is_key)? */
static int line_is_key(const char *line, const char *key)
{
    size_t n;
    line += strspn(line, " \t");
    n = strlen(key);
    if (strncmp(line, key, n) != 0) {
        return 0;
    }
    line += n + strspn(line + n, " \t");
    return *line == '=';
}

/* Key einer "key=..."-Zeile nach out (Toleranz wie oben). 1 = ok. */
static int line_key(const char *line, char *out, size_t cap)
{
    size_t n;
    const char *e;
    line += strspn(line, " \t");
    e = line;
    while (*e && *e != '=' && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') {
        e++;
    }
    n = (size_t)(e - line);
    if (!n || n + 1 > cap) {
        return 0;
    }
    {
        const char *p = e + strspn(e, " \t");
        if (*p != '=') {
            return 0;
        }
    }
    memcpy(out, line, n);
    out[n] = '\0';
    return 1;
}

/* Datei lesen (binaer, bis 1 MB). 0 = ok (*buf malloc'd, *len gesetzt). */
static int read_all(const char *path, char **buf, size_t *len)
{
    FILE *f = NULL;
    long n;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1024 * 1024) {
        fclose(f);
        return 1;
    }
    *buf = (char *)malloc((size_t)n + 1);
    if (!*buf) {
        fclose(f);
        return 1;
    }
    *len = fread(*buf, 1, (size_t)n, f);
    (*buf)[*len] = '\0';
    fclose(f);
    return 0;
}

/* Eigene Zeilen setzen (lines NULL/n 0 = nur entfernen). Datei bleibt sonst
 * bytegleich (Zeilenenden je Zeile erhalten; neue Zeilen im Stil der Datei,
 * Default \n). */
static int cfg_rewrite(const char *user_cfg, const char lines[][96], int n)
{
    char *old_txt = NULL, *out = NULL, key[96];
    size_t old_len = 0, out_cap, out_len = 0;
    const char *eol = "\n";
    int have_input = 0, in_input = 0, i;
    /* Zeilen mit Terminatoren zerlegen (Offsets, NUL-sicher bis old_len). */
    typedef struct { size_t off, len, tlen; } span_t;
    span_t *sp = NULL;
    size_t nsp = 0, capsp = 0, p = 0;
    FILE *f = NULL;

    if (!user_cfg || !user_cfg[0]) {
        return 1;
    }
    if (read_all(user_cfg, &old_txt, &old_len) != 0) {
        /* Neu anlegen (nur wenn Zeilen zu setzen sind). */
        if (n <= 0) {
            return 0;
        }
        if (fopen_s(&f, user_cfg, "w") != 0 || !f) {
            return 1;
        }
        fputs("# user.cfg - vom Launcher geschrieben, ueberschreibt sb_native.cfg\n"
              "[input]\n", f);
        for (i = 0; i < n; i++) {
            fprintf(f, "%s\n", lines[i]);
        }
        fclose(f);
        return 0;
    }
    if (strstr(old_txt, "\r\n") != NULL) {
        eol = "\r\n";
    }
    /* Zerlegen */
    while (p < old_len) {
        size_t q = p;
        while (q < old_len && old_txt[q] != '\r' && old_txt[q] != '\n') {
            q++;
        }
        {
            size_t t = q;
            if (t < old_len && old_txt[t] == '\r') {
                t++;
            }
            if (t < old_len && old_txt[t] == '\n') {
                t++;
            }
            if (nsp >= capsp) {
                size_t nc = capsp ? capsp * 2 : 256;
                void *np = realloc(sp, nc * sizeof *sp);
                if (!np) {
                    free(sp);
                    free(old_txt);
                    return 1;
                }
                sp = (span_t *)np;
                capsp = nc;
            }
            sp[nsp].off = p;
            sp[nsp].len = q - p;
            sp[nsp].tlen = t - q;
            nsp++;
            p = t ? t : q + 1;
            if (!t) {
                break;
            }
        }
    }
    out_cap = old_len + (size_t)(n > 0 ? n : 0) * 128 + 64;
    out = (char *)malloc(out_cap);
    if (!out) {
        free(sp);
        free(old_txt);
        return 1;
    }
    for (i = 0; i < (int)nsp; i++) {
        char lb[4096];
        size_t L = sp[i].len < sizeof lb - 1 ? sp[i].len : sizeof lb - 1;
        memcpy(lb, old_txt + sp[i].off, L);
        lb[L] = '\0';
        {
            const char *t = lb + strspn(lb, " \t");
            if (*t == '[') {
                const char *e = strchr(t, ']');
                char sec[32];
                size_t sl = (e && (size_t)(e - t - 1) < sizeof sec - 1)
                                ? (size_t)(e - t - 1)
                                : 0;
                memcpy(sec, t + 1, sl);
                sec[sl] = '\0';
                in_input = _stricmp(sec, "input") == 0;
                if (in_input) {
                    have_input = 1;
                }
            }
        }
        /* Eigene Keys in [input] entfernen (Ersatz folgt unten gebuendelt). */
        if (in_input && line_key(lb, key, sizeof key) && assist_cfg_owns(key)) {
            continue;
        }
        if (out_len + sp[i].len + sp[i].tlen + 1 > out_cap) {
            break;
        }
        memcpy(out + out_len, old_txt + sp[i].off, sp[i].len + sp[i].tlen);
        out_len += sp[i].len + sp[i].tlen;
    }
    if (n > 0) {
        if (!have_input) {
            /* [input] anhaengen (vorher Zeilenende sichern). */
            if (out_len && out[out_len - 1] != '\n') {
                memcpy(out + out_len, eol, strlen(eol));
                out_len += strlen(eol);
            }
            memcpy(out + out_len, "[input]", 7);
            out_len += 7;
            memcpy(out + out_len, eol, strlen(eol));
            out_len += strlen(eol);
        } else {
            /* Ans Ende der [input]-Section: hinter die letzte Zeile vor der
             * naechsten Section - vereinfacht: ans Dateiende, falls [input]
             * die letzte Section ist, sonst vor die naechste Section. */
            size_t ins = out_len;
            char *tmp = (char *)malloc(out_len + 1);
            if (!tmp) {
                free(out);
                free(sp);
                free(old_txt);
                return 1;
            }
            memcpy(tmp, out, out_len);
            tmp[out_len] = '\0';
            {
                char *sec = tmp;
                int found = 0;
                while ((sec = strstr(sec, "[input]")) != NULL) {
                    /* Section-Kopf? (Zeilenanfang, "[input]" = 7 Zeichen) */
                    if ((sec == tmp || sec[-1] == '\n') && sec[6] == ']') {
                        found = 1;
                        break;
                    }
                    sec += 7;
                }
                if (found) {
                    char *nl = strchr(sec, '\n');
                    char *nx;
                    sec = nl ? nl + 1 : tmp + out_len;
                    nx = sec;
                    /* naechsten Section-Kopf suchen */
                    while ((nx = strstr(nx, "\n[")) != NULL) {
                        char *ce = strchr(nx + 2, ']');
                        char *le = strchr(nx + 1, '\n');
                        if (ce && (!le || ce < le)) {
                            break;
                        }
                        nx += 2;
                    }
                    ins = nx ? (size_t)(nx + 1 - tmp) : out_len;
                }
            }
            free(tmp);
            if (ins < out_len) {
                /* Einfuegen vor der naechsten Section (out ist gross genug:
                 * +n*128 reserviert). */
                {
                    size_t need = 0;
                    int k;
                    for (k = 0; k < n; k++) {
                        need += strlen(lines[k]) + strlen(eol);
                    }
                    memmove(out + ins + need, out + ins, out_len - ins);
                    for (k = 0; k < n; k++) {
                        size_t L = strlen(lines[k]);
                        memcpy(out + ins, lines[k], L);
                        ins += L;
                        memcpy(out + ins, eol, strlen(eol));
                        ins += strlen(eol);
                    }
                    out_len += need;
                }
            } else {
                int k;
                if (out_len && out[out_len - 1] != '\n') {
                    memcpy(out + out_len, eol, strlen(eol));
                    out_len += strlen(eol);
                }
                for (k = 0; k < n; k++) {
                    size_t L = strlen(lines[k]);
                    memcpy(out + out_len, lines[k], L);
                    out_len += L;
                    memcpy(out + out_len, eol, strlen(eol));
                    out_len += strlen(eol);
                }
            }
        }
    }
    if (fopen_s(&f, user_cfg, "wb") != 0 || !f) {
        free(out);
        free(sp);
        free(old_txt);
        return 1;
    }
    fwrite(out, 1, out_len, f);
    fclose(f);
    free(out);
    free(sp);
    free(old_txt);
    return 0;
}

int assist_cfg_apply(const char *user_cfg, const char lines[][96], int n)
{
    int i;
    if (n < 0) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        char k[96];
        if (!line_key(lines[i], k, sizeof k) || !assist_cfg_owns(k)) {
            return 1;
        }
    }
    return cfg_rewrite(user_cfg, lines, n);
}

int assist_cfg_reset(const char *user_cfg)
{
    return cfg_rewrite(user_cfg, NULL, 0);
}

int assist_cfg_read(const char *user_cfg, char lines[][96], int cap)
{
    char *txt = NULL, key[96];
    size_t len = 0, p = 0;
    int in_input = 0, n = 0;
    if (!user_cfg || !user_cfg[0] || cap <= 0) {
        return 0;
    }
    if (read_all(user_cfg, &txt, &len) != 0) {
        return 0;
    }
    while (p < len && n < cap) {
        size_t q = p;
        char lb[512];
        size_t L;
        const char *t;
        while (q < len && txt[q] != '\r' && txt[q] != '\n') {
            q++;
        }
        L = q - p < sizeof lb - 1 ? q - p : sizeof lb - 1;
        memcpy(lb, txt + p, L);
        lb[L] = '\0';
        p = q;
        while (p < len && (txt[p] == '\r' || txt[p] == '\n')) {
            p++;
        }
        t = lb + strspn(lb, " \t");
        if (*t == '[') {
            const char *e = strchr(t, ']');
            char sec[32];
            size_t sl = (e && (size_t)(e - t - 1) < sizeof sec - 1)
                            ? (size_t)(e - t - 1)
                            : 0;
            memcpy(sec, t + 1, sl);
            sec[sl] = '\0';
            in_input = _stricmp(sec, "input") == 0;
            continue;
        }
        if (in_input && line_key(lb, key, sizeof key) &&
            assist_cfg_owns(key)) {
            /* Wert normalisieren ("key=value", ohne Kommentar). */
            const char *eq = strchr(lb, '=');
            const char *v;
            char val[64];
            size_t vl;
            v = eq ? eq + 1 + strspn(eq + 1, " \t") : "";
            vl = strcspn(v, " \t#;\r\n");
            if (vl > sizeof val - 1) {
                vl = sizeof val - 1;
            }
            memcpy(val, v, vl);
            val[vl] = '\0';
            _snprintf_s(lines[n], 96, _TRUNCATE, "%s=%s", key, val);
            n++;
        }
    }
    free(txt);
    return n;
}

/* ======================================================================
 * Engine
 * ====================================================================== */

typedef struct {
    uint64_t t; /* ms */
    uint8_t raw[SBC_USB_IN_LEN];
} fix_ev_t;

enum { JOB_NONE = 0, JOB_TEST_RUN, JOB_TEST_STEP, JOB_CAL_AXIS, JOB_CAL_EXTRA, JOB_LED_SEQ };

struct assist {
    assist_cb_t cb;
    CRITICAL_SECTION cs;
    /* Quelle */
    int src; /* ASSIST_SRC_* */
    int src_err;
    sbc_usb_dev_t *dev;
    sbc_fixture_t fx;
    int fx_loaded;
    int want_fixture; /* 1 = Fixture gewaehlt (kein Auto-Geraet) */
    char fx_name[260];
    fix_ev_t *ev;
    unsigned nev, iev;
    ULONGLONG real_t0; /* ms-Takt (Echtzeit) */
    int fast;          /* x20 (nur Fixture) */
    /* letzter Report */
    int have;
    uint8_t raw[SBC_USB_IN_LEN];
    sbc_state_t st;
    ULONGLONG last_open_try;
    /* Arbeiter */
    HANDLE thread;
    HANDLE key_ev;
    int keys[16];
    unsigned nkey;
    volatile LONG busy;
    volatile LONG stop;
    int job, job_arg, job_from;
    /* Testzustand */
    sbc_test_io_t io;
    sbc_test_ctx_t tctx;
    int tctx_valid;
    sbc_test_step_t steps[SBC_TEST_N_STEPS];
    sbc_test_res_t res[SBC_TEST_N_STEPS];
    uint8_t tun_seen[16], gear_seen[256];
    int cur_step;
    sbc_test_sum_t sum;
    /* Kalibrierung */
    int cal_what;
    sbc_test_res_t cal_res;
    int cal_valid;
    /* LEDs */
    uint8_t led_level[SBC_LED_COUNT];
    uint8_t (*outs)[SBC_USB_OUT_LEN]; /* Mock-Aufzeichnung (Fixture) */
    unsigned nout, capout;
    int err;
};

static void a_emit(assist_t *as, int ev, int a, int b, const char *text)
{
    if (as->cb.event) {
        as->cb.event(as->cb.ctx, ev, a, b, text ? text : "");
    }
}

/* ---- Uhr ---- */

static uint64_t a_now(assist_t *as)
{
    uint64_t d = (uint64_t)(GetTickCount64() - as->real_t0);
    return as->fast ? d * 20 : d;
}

/* ---- Fixture laden (ms-Ereignisse, stabil sortiert; fN = N/60 s) ---- */

static void a_fx_unload(assist_t *as)
{
    if (as->fx_loaded) {
        sbc_fixture_free(&as->fx);
        as->fx_loaded = 0;
    }
    free(as->ev);
    as->ev = NULL;
    as->nev = as->iev = 0;
}

static int a_fx_load(assist_t *as, const char *path)
{
    unsigned i, j;
    a_fx_unload(as);
    if (sbc_fixture_load(path, &as->fx) != 0) {
        return 1;
    }
    as->fx_loaded = 1;
    if (!as->fx.n) {
        return 0;
    }
    as->ev = (fix_ev_t *)calloc(as->fx.n, sizeof *as->ev);
    if (!as->ev) {
        return 1;
    }
    for (i = 0; i < as->fx.n; i++) {
        as->ev[i].t = as->fx.e[i].is_ms ? as->fx.e[i].when
                                        : as->fx.e[i].when * 1000 / 60;
        memcpy(as->ev[i].raw, as->fx.e[i].raw, SBC_USB_IN_LEN);
    }
    as->nev = as->fx.n;
    for (i = 1; i < as->nev; i++) {
        fix_ev_t tmp = as->ev[i];
        j = i;
        while (j > 0 && as->ev[j - 1].t > tmp.t) {
            as->ev[j] = as->ev[j - 1];
            j--;
        }
        as->ev[j] = tmp;
    }
    {
        const char *b = strrchr(path, '\\');
        const char *b2 = strrchr(path, '/');
        if (b2 && (!b || b2 > b)) {
            b = b2;
        }
        strcpy_s(as->fx_name, sizeof as->fx_name, b ? b + 1 : path);
    }
    return 0;
}

/* ---- Quelle ---- */

assist_t *assist_create(const assist_cb_t *cb)
{
    assist_t *as = (assist_t *)calloc(1, sizeof *as);
    if (!as) {
        return NULL;
    }
    if (cb) {
        as->cb = *cb;
    }
    InitializeCriticalSection(&as->cs);
    as->key_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    as->real_t0 = GetTickCount64();
    as->cur_step = -1;
    {
        int i;
        sbc_test_build_steps(as->steps);
        for (i = 0; i < SBC_TEST_N_STEPS; i++) {
            as->res[i].status = SBC_TR_NOTRUN;
        }
    }
    return as;
}

static void a_close_src(assist_t *as)
{
    if (as->dev) {
        uint8_t out[SBC_USB_OUT_LEN];
        /* Lampen aus (Geraet ruhig zuruecklassen). */
        memset(out, 0, sizeof out);
        out[0] = 0x00;
        out[1] = SBC_USB_OUT_LEN;
        sbc_usb_send_out_raw(as->dev, out);
        sbc_usb_close(as->dev);
        as->dev = NULL;
    }
    a_fx_unload(as);
    as->src = ASSIST_SRC_NONE;
}

void assist_destroy(assist_t *as)
{
    if (!as) {
        return;
    }
    assist_test_stop(as);
    if (as->thread) {
        WaitForSingleObject(as->thread, 5000);
        CloseHandle(as->thread);
    }
    a_close_src(as);
    free(as->outs);
    if (as->key_ev) {
        CloseHandle(as->key_ev);
    }
    DeleteCriticalSection(&as->cs);
    free(as);
}

int assist_open(assist_t *as, const char *fixture)
{
    int src = ASSIST_SRC_NONE;
    EnterCriticalSection(&as->cs);
    a_close_src(as);
    as->real_t0 = GetTickCount64();
    as->iev = 0;
    as->have = 0;
    as->tctx_valid = 0;
    as->nout = 0;
    as->src_err = SBC_OK;
    memset(as->led_level, 0, sizeof as->led_level);
    as->want_fixture = (fixture && fixture[0]) ? 1 : 0;
    if (fixture && fixture[0]) {
        if (a_fx_load(as, fixture) == 0 && as->nev > 0) {
            src = ASSIST_SRC_FIXTURE;
        } else {
            as->src_err = SBC_ERR_ARG;
        }
    } else {
        int rc = sbc_usb_open(&as->dev);
        if (rc == SBC_OK) {
            src = ASSIST_SRC_DEVICE;
        } else {
            as->src_err = rc;
            as->dev = NULL;
        }
    }
    as->src = src;
    LeaveCriticalSection(&as->cs);
    a_emit(as, ASSIST_EV_SOURCE, src, 0, NULL);
    return src;
}

void assist_close(assist_t *as)
{
    EnterCriticalSection(&as->cs);
    a_close_src(as);
    LeaveCriticalSection(&as->cs);
    a_emit(as, ASSIST_EV_SOURCE, ASSIST_SRC_NONE, 0, NULL);
}

int assist_source(assist_t *as)
{
    int s;
    EnterCriticalSection(&as->cs);
    s = as->src;
    LeaveCriticalSection(&as->cs);
    return s;
}

int assist_source_err(assist_t *as)
{
    return as->src_err;
}

void assist_set_fast(assist_t *as, int on)
{
    EnterCriticalSection(&as->cs);
    as->fast = on ? 1 : 0;
    LeaveCriticalSection(&as->cs);
}

/* Einen Report uebernehmen (Lock halten). 1 = neu. */
static int a_got_report(assist_t *as, const uint8_t *raw, unsigned len)
{
    if (len < SBC_USB_IN_LEN) {
        return 0;
    }
    if (as->have && memcmp(as->raw, raw, SBC_USB_IN_LEN) == 0) {
        return 0;
    }
    memcpy(as->raw, raw, SBC_USB_IN_LEN);
    as->have = 1;
    if (sbc_usb_parse_report(raw, &as->st) != SBC_OK) {
        memset(&as->st, 0, sizeof as->st);
    }
    return 1;
}

/* Timer-Poll (nur wenn der Arbeiter idle ist - sonst liefert der Arbeiter). */
void assist_poll(assist_t *as)
{
    int fire_state = 0, fire_src = 0, src = ASSIST_SRC_NONE;
    if (InterlockedCompareExchange(&as->busy, 0, 0)) {
        return;
    }
    EnterCriticalSection(&as->cs);
    if (as->src == ASSIST_SRC_DEVICE && as->dev) {
        uint8_t raw[SBC_USB_RAW_MAX];
        unsigned len = 0;
        int rc = sbc_usb_poll_raw(as->dev, raw, &len);
        if (rc == SBC_OK) {
            fire_state = a_got_report(as, raw, len);
        } else if (rc != SBC_ERR_TIMEOUT) {
            /* Geraet verloren (gezogen?) - sauber trennen, Hinweis. */
            a_close_src(as);
            as->src_err = rc;
            fire_src = 1;
        }
    } else if (as->src == ASSIST_SRC_FIXTURE && as->fx_loaded) {
        uint64_t now = a_now(as);
        while (as->iev < as->nev && as->ev[as->iev].t <= now) {
            fire_state |= a_got_report(as, as->ev[as->iev].raw, SBC_USB_IN_LEN);
            as->iev++;
        }
        /* Am Ende stehenbleiben (letzter Report bleibt gueltig). */
    } else if (as->src == ASSIST_SRC_NONE && !as->want_fixture) {
        /* Auto-Wiederholung (1/s): Geraet angesteckt? (Nur wenn kein
         * Fixture gewaehlt ist - sonst Hinweis lassen.) */
        ULONGLONG now = GetTickCount64();
        if (!as->last_open_try || now - as->last_open_try >= 1000) {
            sbc_usb_dev_t *dev = NULL;
            as->last_open_try = now;
            if (sbc_usb_open(&dev) == SBC_OK) {
                as->dev = dev;
                as->src = ASSIST_SRC_DEVICE;
                as->src_err = SBC_OK;
                as->have = 0;
                fire_src = 1;
            }
        }
    }
    src = as->src;
    LeaveCriticalSection(&as->cs);
    if (fire_src) {
        a_emit(as, ASSIST_EV_SOURCE, src, 0, NULL);
    }
    if (fire_state) {
        a_emit(as, ASSIST_EV_STATE, 0, 0, NULL);
    }
}

int assist_has_report(assist_t *as)
{
    int h;
    EnterCriticalSection(&as->cs);
    h = as->have;
    LeaveCriticalSection(&as->cs);
    return h;
}

void assist_raw(assist_t *as, uint8_t raw[SBC_USB_IN_LEN])
{
    EnterCriticalSection(&as->cs);
    memcpy(raw, as->raw, SBC_USB_IN_LEN);
    LeaveCriticalSection(&as->cs);
}

void assist_state(assist_t *as, sbc_state_t *st)
{
    EnterCriticalSection(&as->cs);
    *st = as->st;
    LeaveCriticalSection(&as->cs);
}

/* ======================================================================
 * sbc_test_io_t ueber die Engine (Arbeiter-Thread)
 * ====================================================================== */

static uint64_t aio_now(void *v)
{
    return a_now((assist_t *)v);
}

/* Taste aus der Warteschlange (Lock innen). 1 = key gueltig. */
static int a_pop_key(assist_t *as, int *key)
{
    int got = 0;
    unsigned i;
    EnterCriticalSection(&as->cs);
    if (as->nkey) {
        *key = as->keys[0];
        for (i = 1; i < as->nkey; i++) {
            as->keys[i - 1] = as->keys[i];
        }
        as->nkey--;
        got = 1;
    }
    LeaveCriticalSection(&as->cs);
    return got;
}

static int aio_next(void *v, uint64_t deadline, sbc_test_ev_t *out)
{
    assist_t *as = (assist_t *)v;
    for (;;) {
        int key;
        if (a_pop_key(as, &key)) {
            out->t = a_now(as);
            out->key = key;
            out->len = 0;
            return SBC_TEV_KEY;
        }
        if (InterlockedCompareExchange(&as->stop, 0, 0)) {
            as->err = SBC_ERR_IO;
            return SBC_TEV_ERR;
        }
        if (a_now(as) >= deadline) {
            return SBC_TEV_TIMEOUT;
        }
        if (as->src == ASSIST_SRC_FIXTURE) {
            /* Naechstes faelliges Fixture-Ereignis liefern (jedes genau
             * einmal, wie die Konsole). */
            uint64_t now = a_now(as);
            int have = 0;
            EnterCriticalSection(&as->cs);
            if (as->iev < as->nev && as->ev[as->iev].t <= now) {
                out->t = as->ev[as->iev].t;
                out->key = 0;
                out->len = SBC_USB_IN_LEN;
                memcpy(out->raw, as->ev[as->iev].raw, SBC_USB_IN_LEN);
                as->iev++;
                a_got_report(as, out->raw, out->len);
                have = 1;
            }
            LeaveCriticalSection(&as->cs);
            if (have) {
                a_emit(as, ASSIST_EV_STATE, 0, 0, NULL);
                return SBC_TEV_REPORT;
            }
            {
                uint64_t w = deadline > now ? deadline - now : 0;
                if (w > 5) {
                    w = 5;
                }
                WaitForSingleObject(as->key_ev, (DWORD)w);
            }
            continue;
        }
        if (as->src == ASSIST_SRC_DEVICE && as->dev) {
            uint8_t raw[SBC_USB_RAW_MAX];
            unsigned len = 0;
            int rc = sbc_usb_poll_raw(as->dev, raw, &len);
            if (rc == SBC_OK) {
                int is_new;
                out->t = a_now(as);
                out->key = 0;
                out->len = len > SBC_USB_RAW_MAX ? SBC_USB_RAW_MAX : len;
                memcpy(out->raw, raw, out->len);
                EnterCriticalSection(&as->cs);
                is_new = a_got_report(as, raw, len);
                LeaveCriticalSection(&as->cs);
                if (is_new) {
                    a_emit(as, ASSIST_EV_STATE, 0, 0, NULL);
                }
                return SBC_TEV_REPORT;
            }
            if (rc == SBC_ERR_TIMEOUT) {
                if (a_now(as) >= deadline) {
                    return SBC_TEV_TIMEOUT;
                }
                continue;
            }
            EnterCriticalSection(&as->cs);
            as->err = rc;
            /* Geraet verloren: sauber trennen. */
            a_close_src(as);
            as->src_err = rc;
            LeaveCriticalSection(&as->cs);
            a_emit(as, ASSIST_EV_SOURCE, ASSIST_SRC_NONE, 0, NULL);
            return SBC_TEV_ERR;
        }
        /* Keine Quelle: auf Taste/Ende warten. */
        {
            uint64_t now = a_now(as);
            uint64_t w = deadline > now ? deadline - now : 0;
            if (w > 50) {
                w = 50;
            }
            WaitForSingleObject(as->key_ev, (DWORD)w);
        }
    }
}

static void aio_sleep(void *v, unsigned ms)
{
    assist_t *as = (assist_t *)v;
    if (as->fast) {
        ms /= 20;
        if (!ms) {
            ms = 1;
        }
    }
    Sleep(ms);
}

static int aio_send_out(void *v, const uint8_t out[SBC_USB_OUT_LEN])
{
    assist_t *as = (assist_t *)v;
    int i;
    /* Pegel merken (Anzeige, beide Quellen). */
    EnterCriticalSection(&as->cs);
    for (i = 0; i < SBC_LED_COUNT; i++) {
        int nib = i < 30 ? i : i + 1;
        int by = 2 + nib / 2;
        as->led_level[i] = (uint8_t)(nib & 1 ? (out[by] >> 4) & 0xF
                                             : out[by] & 0xF);
    }
    LeaveCriticalSection(&as->cs);
    if (as->src == ASSIST_SRC_DEVICE && as->dev) {
        return sbc_usb_send_out_raw(as->dev, out);
    }
    /* Fixture/keine Quelle: aufzeichnen (Mock). */
    EnterCriticalSection(&as->cs);
    if (as->nout >= as->capout) {
        unsigned nc = as->capout ? as->capout * 2 : 64;
        void *p = realloc(as->outs, (size_t)nc * SBC_USB_OUT_LEN);
        if (!p) {
            LeaveCriticalSection(&as->cs);
            return SBC_ERR_ARG;
        }
        as->outs = (uint8_t (*)[SBC_USB_OUT_LEN])p;
        as->capout = nc;
    }
    memcpy(as->outs[as->nout++], out, SBC_USB_OUT_LEN);
    LeaveCriticalSection(&as->cs);
    return SBC_OK;
}

static unsigned aio_nout(void *v)
{
    assist_t *as = (assist_t *)v;
    return (as->src == ASSIST_SRC_DEVICE) ? 0 : as->nout;
}

static const uint8_t *aio_out_at(void *v, unsigned i)
{
    assist_t *as = (assist_t *)v;
    if (as->src == ASSIST_SRC_DEVICE || i >= as->nout) {
        return NULL;
    }
    return as->outs[i];
}

static int aio_is_live(void *v)
{
    return ((assist_t *)v)->src == ASSIST_SRC_DEVICE;
}

static int aio_has_ended(void *v)
{
    assist_t *as = (assist_t *)v;
    if (as->src != ASSIST_SRC_FIXTURE) {
        return 0;
    }
    return as->iev >= as->nev;
}

static int aio_err(void *v)
{
    return ((assist_t *)v)->err;
}

static int aio_aborted(void *v)
{
    assist_t *as = (assist_t *)v;
    return InterlockedCompareExchange(&as->stop, 0, 0) != 0;
}

static void aio_emit(void *v, const char *text)
{
    assist_t *as = (assist_t *)v;
    a_emit(as, ASSIST_EV_TEST_LINE, 0, 0, text);
}

static void aio_progress(void *v, int ev, int a, int b)
{
    assist_t *as = (assist_t *)v;
    if (ev == SBC_TP_STEP_START) {
        EnterCriticalSection(&as->cs);
        as->cur_step = a;
        LeaveCriticalSection(&as->cs);
        a_emit(as, ASSIST_EV_TEST_START, a, b, NULL);
    } else if (ev == SBC_TP_LED) {
        a_emit(as, ASSIST_EV_LED, a, b, NULL);
    }
}

static void aio_discard_keys(void *v)
{
    assist_t *as = (assist_t *)v;
    int k;
    while (a_pop_key(as, &k)) {
    }
}

static int aio_get_report(void *v, uint8_t raw[SBC_USB_RAW_MAX], unsigned *len)
{
    assist_t *as = (assist_t *)v;
    if (as->src != ASSIST_SRC_DEVICE || !as->dev) {
        return 0;
    }
    *len = 0;
    return sbc_usb_get_report(as->dev, raw, len) == SBC_OK;
}

/* ---- Arbeiter ---- */

static void a_make_io(assist_t *as)
{
    as->io.ctx = as;
    as->io.now = aio_now;
    as->io.next = aio_next;
    as->io.sleep_ms = aio_sleep;
    as->io.send_out = aio_send_out;
    as->io.nout = aio_nout;
    as->io.out_at = aio_out_at;
    as->io.is_live = aio_is_live;
    as->io.has_ended = aio_has_ended;
    as->io.err = aio_err;
    as->io.aborted = aio_aborted;
    as->io.emit = aio_emit;
    as->io.progress = aio_progress;
    as->io.discard_keys = aio_discard_keys;
    as->io.get_report = aio_get_report;
}

/* Testkontext gueltig machen (erster Report). 1 = ok. */
static int a_test_ctx(assist_t *as)
{
    if (as->tctx_valid) {
        return 1;
    }
    a_make_io(as);
    sbc_test_ctx_init(&as->tctx, &as->io);
    if (!sbc_test_first_report(&as->tctx)) {
        a_emit(as, ASSIST_EV_TEST_LINE, 0, 0,
               "Kein Report vom Controller (Geraet/Fixture pruefen).\n");
        return 0;
    }
    sbc_test_unk_add(&as->tctx.unk, as->tctx.cur, as->tctx.len);
    as->tctx.t_first = a_now(as);
    as->tctx_valid = 1;
    return 1;
}

static DWORD WINAPI a_worker(LPVOID v)
{
    assist_t *as = (assist_t *)v;
    int job = as->job, arg = as->job_arg, from = as->job_from;
    if (job == JOB_TEST_RUN || job == JOB_TEST_STEP) {
        int last = job == JOB_TEST_STEP ? arg : SBC_TEST_N_STEPS - 1;
        int i, aborted = 0;
        a_make_io(as);
        if (from == 0 || !as->tctx_valid) {
            as->tctx_valid = 0;
            memset(as->tun_seen, 0, sizeof as->tun_seen);
            memset(as->gear_seen, 0, sizeof as->gear_seen);
        }
        if (!a_test_ctx(as)) {
            InterlockedExchange(&as->busy, 0);
            a_emit(as, ASSIST_EV_TEST_END, -1, 1, NULL);
            return 0;
        }
        for (i = from; i <= last; i++) {
            sbc_test_res_t *r = &as->res[i];
            memset(r, 0, sizeof *r);
            as->tctx.skip_key = 0;
            a_emit(as, ASSIST_EV_TEST_START, i, as->steps[i].kind, NULL);
            EnterCriticalSection(&as->cs);
            as->cur_step = i;
            LeaveCriticalSection(&as->cs);
            switch (as->steps[i].kind) {
            case SBC_TK_BTN:
                sbc_test_step_button(&as->tctx, as->steps[i].idx, 0, r);
                break;
            case SBC_TK_TOG:
                sbc_test_step_button(&as->tctx, as->steps[i].idx, 1, r);
                break;
            case SBC_TK_AXIS:
                sbc_test_step_axis(&as->tctx, as->steps[i].idx, r);
                break;
            case SBC_TK_TUNER:
                sbc_test_step_tuner(&as->tctx, r, as->tun_seen);
                break;
            case SBC_TK_GEAR:
                sbc_test_step_gear(&as->tctx, r, as->gear_seen);
                break;
            default:
                sbc_test_step_led(&as->tctx, r);
                break;
            }
            if (as->tctx.aborted) {
                r->status = SBC_TR_NOTRUN;
                aborted = 1;
                EnterCriticalSection(&as->cs);
                as->cur_step = -1;
                LeaveCriticalSection(&as->cs);
                a_emit(as, ASSIST_EV_TEST_DONE, i, SBC_TR_NOTRUN, NULL);
                break;
            }
            a_emit(as, ASSIST_EV_TEST_DONE, i, r->status, NULL);
        }
        EnterCriticalSection(&as->cs);
        as->cur_step = -1;
        LeaveCriticalSection(&as->cs);
        InterlockedExchange(&as->busy, 0);
        a_emit(as, ASSIST_EV_TEST_END, last, aborted, NULL);
        return 0;
    }
    if (job == JOB_CAL_AXIS || job == JOB_CAL_EXTRA) {
        sbc_test_ctx_t c;
        sbc_test_res_t r;
        memset(&r, 0, sizeof r);
        a_make_io(as);
        sbc_test_ctx_init(&c, &as->io);
        if (!sbc_test_first_report(&c)) {
            a_emit(as, ASSIST_EV_TEST_LINE, 0, 0,
                   "Kein Report vom Controller (Geraet/Fixture pruefen).\n");
        } else if (job == JOB_CAL_AXIS) {
            sbc_test_step_axis(&c, arg, &r);
        } else if (arg == ASSIST_CAL_TUNER) {
            uint8_t seen[16];
            sbc_test_step_tuner(&c, &r, seen);
        } else {
            uint8_t seen[256];
            sbc_test_step_gear(&c, &r, seen);
        }
        EnterCriticalSection(&as->cs);
        as->cal_what = arg;
        as->cal_res = r;
        as->cal_valid = !c.aborted;
        LeaveCriticalSection(&as->cs);
        InterlockedExchange(&as->busy, 0);
        a_emit(as, ASSIST_EV_CAL_DONE, arg, r.status, NULL);
        return 0;
    }
    if (job == JOB_LED_SEQ) {
        int rc;
        a_make_io(as);
        rc = sbc_test_led_seq_run(&as->io, 600, 1);
        InterlockedExchange(&as->busy, 0);
        a_emit(as, ASSIST_EV_TEST_END, -2, rc != SBC_OK, NULL);
        return 0;
    }
    InterlockedExchange(&as->busy, 0);
    return 0;
}

/* Auftrag starten (1 = laeuft). Vorbedingung: !busy. */
static int a_start(assist_t *as, int job, int arg, int from)
{
    DWORD id;
    if (InterlockedCompareExchange(&as->busy, 1, 0)) {
        return 0;
    }
    InterlockedExchange(&as->stop, 0);
    EnterCriticalSection(&as->cs);
    as->nkey = 0;
    as->err = SBC_OK;
    LeaveCriticalSection(&as->cs);
    as->job = job;
    as->job_arg = arg;
    as->job_from = from;
    if (as->thread) {
        CloseHandle(as->thread);
        as->thread = NULL;
    }
    as->thread = CreateThread(NULL, 0, a_worker, as, 0, &id);
    if (!as->thread) {
        InterlockedExchange(&as->busy, 0);
        return 0;
    }
    return 1;
}

int assist_test_busy(assist_t *as)
{
    return InterlockedCompareExchange(&as->busy, 0, 0) != 0;
}

int assist_test_current(assist_t *as)
{
    int c;
    EnterCriticalSection(&as->cs);
    c = as->cur_step;
    LeaveCriticalSection(&as->cs);
    return c;
}

void assist_test_run(assist_t *as, int from)
{
    int i;
    if (from < 0) {
        from = 0;
    }
    if (from >= SBC_TEST_N_STEPS) {
        return;
    }
    EnterCriticalSection(&as->cs);
    for (i = from; i < SBC_TEST_N_STEPS; i++) {
        as->res[i].status = SBC_TR_NOTRUN;
        as->res[i].note[0] = '\0';
    }
    LeaveCriticalSection(&as->cs);
    a_start(as, JOB_TEST_RUN, 0, from);
}

void assist_test_step(assist_t *as, int idx)
{
    if (idx < 0 || idx >= SBC_TEST_N_STEPS) {
        return;
    }
    EnterCriticalSection(&as->cs);
    as->res[idx].status = SBC_TR_NOTRUN;
    as->res[idx].note[0] = '\0';
    LeaveCriticalSection(&as->cs);
    a_start(as, JOB_TEST_STEP, idx, idx);
}

void assist_test_key(assist_t *as, int key)
{
    EnterCriticalSection(&as->cs);
    if (as->nkey < 16) {
        as->keys[as->nkey++] = key;
    }
    LeaveCriticalSection(&as->cs);
    SetEvent(as->key_ev);
}

void assist_test_stop(assist_t *as)
{
    HANDLE t;
    InterlockedExchange(&as->stop, 1);
    SetEvent(as->key_ev);
    EnterCriticalSection(&as->cs);
    t = as->thread;
    LeaveCriticalSection(&as->cs);
    if (t) {
        WaitForSingleObject(t, 5000);
    }
}

const sbc_test_step_t *assist_steps(assist_t *as)
{
    return as->steps;
}

const sbc_test_res_t *assist_results(assist_t *as)
{
    return as->res;
}

void assist_results_clear(assist_t *as, int from)
{
    int i;
    if (from < 0) {
        from = 0;
    }
    EnterCriticalSection(&as->cs);
    for (i = from; i < SBC_TEST_N_STEPS; i++) {
        as->res[i].status = SBC_TR_NOTRUN;
        as->res[i].note[0] = '\0';
    }
    LeaveCriticalSection(&as->cs);
}

int assist_write_report(assist_t *as, const char *path)
{
    char desc[320];
    int aborted;
    EnterCriticalSection(&as->cs);
    if (as->src == ASSIST_SRC_DEVICE) {
        strcpy_s(desc, sizeof desc, "Geraet 0A7B:D000 (WinUSB)");
    } else if (as->src == ASSIST_SRC_FIXTURE) {
        _snprintf_s(desc, sizeof desc, _TRUNCATE, "Fixture %s", as->fx_name);
    } else {
        strcpy_s(desc, sizeof desc, "keine Quelle");
    }
    aborted = InterlockedCompareExchange(&as->stop, 0, 0) != 0;
    LeaveCriticalSection(&as->cs);
    if (!as->tctx_valid) {
        return 1;
    }
    return sbc_test_write_report(&as->tctx, as->steps, as->res, path, desc,
                                 "Launcher-Assistent", "W143", aborted, &as->sum);
}

void assist_cal_axis(assist_t *as, int a)
{
    if (a < 0 || a > 7) {
        return;
    }
    a_start(as, JOB_CAL_AXIS, a, 0);
}

void assist_cal_extra(assist_t *as, int what)
{
    if (what != ASSIST_CAL_TUNER && what != ASSIST_CAL_GEAR) {
        return;
    }
    a_start(as, JOB_CAL_EXTRA, what, 0);
}

int assist_cal_result(assist_t *as, int *what_out, sbc_test_res_t *out)
{
    int v;
    EnterCriticalSection(&as->cs);
    v = as->cal_valid;
    if (v) {
        if (what_out) {
            *what_out = as->cal_what;
        }
        if (out) {
            *out = as->cal_res;
        }
    }
    LeaveCriticalSection(&as->cs);
    return v;
}

/* Nennbereich (Spiel-Einheiten) je SBC_AX_*. */
static void a_nominal(int ax, int *lo, int *mid, int *hi)
{
    if (ax == SBC_AX_ROT || ax == SBC_AX_SIGHT_X || ax == SBC_AX_SIGHT_Y) {
        *lo = -128;
        *mid = 0;
        *hi = 127;
    } else if (ax >= SBC_AX_PEDAL_L) {
        *lo = 0;
        *mid = 0;
        *hi = 255;
    } else {
        *lo = 0;
        *mid = 0x80;
        *hi = 255;
    }
}

int assist_preview(assist_t *as, int a, int set, int lo, int mid, int hi,
                   int dead, int *raw_out, int *game_out)
{
    sbc_cal_t cal;
    sbc_state_t st;
    int ax, have;
    uint8_t raw[SBC_USB_IN_LEN];
    if (a < 0 || a > 7) {
        return 0;
    }
    EnterCriticalSection(&as->cs);
    have = as->have;
    memcpy(raw, as->raw, sizeof raw);
    LeaveCriticalSection(&as->cs);
    if (!have) {
        return 0;
    }
    if (sbc_usb_parse_report(raw, &st) != SBC_OK) {
        return 0;
    }
    ax = sbc_test_axes[a].cal_axis;
    memset(&cal, 0, sizeof cal);
    cal.ax[ax].set = 1;
    if (set) {
        cal.ax[ax].lo = lo;
        cal.ax[ax].mid = mid;
        cal.ax[ax].hi = hi;
    } else {
        a_nominal(ax, &cal.ax[ax].lo, &cal.ax[ax].mid, &cal.ax[ax].hi);
    }
    cal.ax[ax].dead = dead;
    sbc_usb_cal_apply(&cal, &st);
    if (raw_out) {
        *raw_out = sbc_test_raw_axis(raw, a);
    }
    if (game_out) {
        *game_out = sbc_usb_axis_get(&st, ax);
    }
    return 1;
}

/* ---- Lampen ---- */

int assist_led_set(assist_t *as, int led, int level)
{
    sbc_leds_t leds;
    uint8_t out[SBC_USB_OUT_LEN];
    int i, rc;
    if (led < -1 || led >= SBC_LED_COUNT) {
        return 1;
    }
    if (level < 0) {
        level = 0;
    }
    if (level > 0xF) {
        level = 0xF;
    }
    memset(&leds, 0, sizeof leds);
    EnterCriticalSection(&as->cs);
    for (i = 0; i < SBC_LED_COUNT; i++) {
        leds.level[i] = as->led_level[i];
    }
    LeaveCriticalSection(&as->cs);
    if (led < 0) {
        for (i = 0; i < SBC_LED_COUNT; i++) {
            leds.level[i] = (uint8_t)level;
        }
    } else {
        leds.level[led] = (uint8_t)level;
    }
    sbc_usb_build_led_report(&leds, out);
    rc = aio_send_out(as, out);
    return rc == SBC_OK ? 0 : 1;
}

int assist_led_all(assist_t *as, int level)
{
    return assist_led_set(as, -1, level);
}

int assist_led_seq_start(assist_t *as)
{
    return a_start(as, JOB_LED_SEQ, 0, 0);
}

void assist_led_seq_stop(assist_t *as)
{
    assist_test_stop(as);
}

int assist_led_level(assist_t *as, int led)
{
    int v = 0;
    if (led < 0 || led >= SBC_LED_COUNT) {
        return 0;
    }
    EnterCriticalSection(&as->cs);
    v = as->led_level[led];
    LeaveCriticalSection(&as->cs);
    return v;
}
