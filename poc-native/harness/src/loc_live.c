/* loc_live.c - W479 LIVE-LOGON: Kern des Xbox-Live-Modus (siehe loc_live.h, docs/loc/LOC_LIVE.md). */
#include "loc_live.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <windows.h> /* W487: Pilotordner umbenennen */

uint32_t loclive_service_hr(uint32_t svc)
{
    switch (svc) {
    case LOCLIVE_SVC_MATCH:
    case LOCLIVE_SVC_FEEDBACK:
    case LOCLIVE_SVC_ACCOUNT:
        return LOCLIVE_S_OK;
    default: /* Kampagne 0x43430009 (bis M4) und unbekannte Dienste */
        return LOCLIVE_E_SVC_UNAVAILABLE;
    }
}

void loclive_fill_user(uint8_t u[LOCLIVE_USER_SIZE], uint64_t xuid, const char *tag, uint32_t hr)
{
    size_t n = 0;
    memset(u, 0, LOCLIVE_USER_SIZE);
    memcpy(u + LOCLIVE_USER_OFF_XUID, &xuid, 8);
    if (tag) {
        n = strlen(tag);
        if (n > LOCLIVE_TAG_MAX) n = LOCLIVE_TAG_MAX;
        memcpy(u + LOCLIVE_USER_OFF_TAG, tag, n);
    }
    memcpy(u + LOCLIVE_USER_OFF_HR, &hr, 4);
}

uint64_t loclive_xuid(uint32_t account_id)
{
    if (!account_id) return 0;
    return ((uint64_t)LOCLIVE_XUID_PREFIX << 32) | account_id;
}

int loclive_xuid_is_pseudo(uint64_t xuid)
{
    return (uint32_t)(xuid >> 32) == LOCLIVE_XUID_PREFIX && (uint32_t)xuid != 0;
}

uint32_t loclive_xuid_account(uint64_t xuid)
{
    return loclive_xuid_is_pseudo(xuid) ? (uint32_t)xuid : 0;
}

static int ll_isalnum(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* W487 LIVE-GAMERTAG: Gamertag-Regel (steelcore names::check_syntax, steelnet gamertag.syntax) */
int loclive_tag_valid(const char *tag)
{
    size_t n, i;
    if (!tag) return 0;
    n = strlen(tag);
    if (n < 1 || n > LOCLIVE_TAG_MAX) return 0;
    if (!((tag[0] >= 'A' && tag[0] <= 'Z') || (tag[0] >= 'a' && tag[0] <= 'z')) || tag[n - 1] == ' ') return 0;
    for (i = 0; i < n; i++) {
        if (tag[i] == ' ') {
            if (tag[i + 1] == ' ') return 0;
        } else if (!ll_isalnum((unsigned char)tag[i])) {
            return 0;
        }
    }
    return 1;
}

/* Ordner-Tag einer Merkzeile: Gamertag oder geparkter Name "_a<Konto-ID>" (A-Z a-z 0-9 Leerzeichen _). */
static int ll_folder_tag_ok(const char *t)
{
    size_t n = strlen(t), i;
    if (n < 1 || n > LOCLIVE_TAG_MAX || t[0] == ' ' || t[n - 1] == ' ') return 0;
    for (i = 0; i < n; i++)
        if (!ll_isalnum((unsigned char)t[i]) && t[i] != ' ' && t[i] != '_') return 0;
    return 1;
}

void loclive_park_tag(uint32_t account_id, char out[16])
{
    _snprintf_s(out, 16, _TRUNCATE, "_a%u", account_id);
}

/* Merkdatei-Text zeilenweise: cb(acct, tag, line_start, line_end, ctx); Rueckgabe != 0 bricht ab. */
typedef int (*ll_line_cb)(uint32_t acct, const char *tag, const char *ls, const char *le, void *ctx);
static void ll_pin_lines(const char *text, ll_line_cb cb, void *ctx)
{
    const char *p = text ? text : "";
    while (*p) {
        const char *ls = p, *le;
        char line[96], tag[24];
        size_t n;
        unsigned a = 0;
        int off = 0;
        while (*p && *p != '\n') p++;
        le = p;
        if (*p) p++;
        n = (size_t)(le - ls);
        if (n >= sizeof line) continue;
        memcpy(line, ls, n);
        line[n] = 0;
        while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
        if (sscanf_s(line, "a%u %n", &a, &off) != 1 || off <= 0 || !a) continue;
        if (strlen(line + off) >= sizeof tag) continue;
        strcpy_s(tag, sizeof tag, line + off);
        if (!ll_folder_tag_ok(tag)) continue;
        if (cb(a, tag, ls, p, ctx)) return;
    }
}

typedef struct {
    uint32_t acct, except;
    const char *tag;
    char out[16];
    int found;
} ll_find_ctx;
static int ll_find_acct(uint32_t a, const char *tag, const char *ls, const char *le, void *c)
{
    ll_find_ctx *x = (ll_find_ctx *)c;
    (void)ls; (void)le;
    if (a != x->acct) return 0;
    strcpy_s(x->out, sizeof x->out, tag);
    x->found = 1;
    return 1;
}
static int ll_find_tag(uint32_t a, const char *tag, const char *ls, const char *le, void *c)
{
    ll_find_ctx *x = (ll_find_ctx *)c;
    (void)ls; (void)le;
    if (a == x->except || _stricmp(tag, x->tag) != 0) return 0;
    x->acct = a;
    x->found = 1;
    return 1;
}

int loclive_pin_get(const char *text, uint32_t account_id, char out[16])
{
    ll_find_ctx x;
    memset(&x, 0, sizeof x);
    x.acct = account_id;
    if (!account_id) return 0;
    ll_pin_lines(text, ll_find_acct, &x);
    if (x.found) strcpy_s(out, 16, x.out);
    return x.found;
}

uint32_t loclive_pin_owner(const char *text, const char *tag, uint32_t except_acct)
{
    ll_find_ctx x;
    memset(&x, 0, sizeof x);
    x.tag = tag;
    x.except = except_acct;
    if (!tag || !tag[0]) return 0;
    ll_pin_lines(text, ll_find_tag, &x);
    return x.found ? x.acct : 0;
}

typedef struct {
    uint32_t acct;
    char *out;
    size_t cap, n;
    int err;
} ll_put_ctx;
static int ll_put_keep(uint32_t a, const char *tag, const char *ls, const char *le, void *c)
{
    ll_put_ctx *x = (ll_put_ctx *)c;
    int w;
    (void)ls; (void)le;
    if (a == x->acct) return 0; /* wird ersetzt (auch doppelte Zeilen) */
    w = _snprintf_s(x->out + x->n, x->cap - x->n, _TRUNCATE, "a%u %s\r\n", a, tag);
    if (w < 0) {
        x->err = 1;
        return 1;
    }
    x->n += (size_t)w;
    return 0;
}

int loclive_pin_put(const char *text, uint32_t account_id, const char *tag, char *out, size_t cap)
{
    ll_put_ctx x;
    int w;
    if (!out || cap < 2 || !account_id || !tag || !ll_folder_tag_ok(tag)) return -1;
    memset(&x, 0, sizeof x);
    x.acct = account_id;
    x.out = out;
    x.cap = cap;
    out[0] = 0;
    ll_pin_lines(text, ll_put_keep, &x);
    if (x.err) return -1;
    w = _snprintf_s(out + x.n, cap - x.n, _TRUNCATE, "a%u %s\r\n", account_id, tag);
    return w < 0 ? -1 : 0;
}

int loclive_tag_plan(const char *text, uint32_t account_id, const char *tag, loclive_tag_plan_t *p)
{
    char old[16] = "";
    int n = 0;
    if (!p) return -1;
    memset(p, 0, sizeof *p);
    if (!account_id || !loclive_tag_valid(tag)) return -1;
    p->park_acct = loclive_pin_owner(text, tag, account_id);
    if (p->park_acct) { /* veralteter Eintrag eines anderen Kontos mit diesem Tag: dessen Ordner parken */
        loclive_pin_get(text, p->park_acct, p->park_from);
        loclive_park_tag(p->park_acct, p->park_to);
        n++;
    }
    if (loclive_pin_get(text, account_id, old) && strcmp(old, tag) != 0) {
        strcpy_s(p->own_from, sizeof p->own_from, old);
        n++;
    }
    strcpy_s(p->own_to, sizeof p->own_to, tag);
    return n;
}

int loclive_move_folders(const char *tdir, const char *from, const char *to)
{
    static const char *const suf[2] = {"CAMP", "FREE"};
    char src[2][760], dst[2][760], base[720];
    size_t bl;
    int i, any = 0;
    if (!tdir || !from || !to || !from[0] || !to[0] || !ll_folder_tag_ok(from) || !ll_folder_tag_ok(to)) return -1;
    if (strcmp(from, to) == 0) return 1;
    strcpy_s(base, sizeof base, tdir);
    bl = strlen(base);
    while (bl && (base[bl - 1] == '\\' || base[bl - 1] == '/')) base[--bl] = 0;
    for (i = 0; i < 2; i++) {
        DWORD as, ad;
        _snprintf_s(src[i], sizeof src[i], _TRUNCATE, "%s\\%s%s", base, from, suf[i]);
        _snprintf_s(dst[i], sizeof dst[i], _TRUNCATE, "%s\\%s%s", base, to, suf[i]);
        as = GetFileAttributesA(src[i]);
        ad = GetFileAttributesA(dst[i]);
        if (as == INVALID_FILE_ATTRIBUTES) {
            src[i][0] = 0; /* nichts zu verschieben */
            continue;
        }
        if (ad != INVALID_FILE_ATTRIBUTES && _stricmp(from, to) != 0) return 0; /* Ziel belegt: nichts anfassen */
        any = 1;
    }
    for (i = 0; any && i < 2; i++) {
        if (src[i][0] && !MoveFileExA(src[i], dst[i], 0)) {
            if (i == 1 && src[0][0]) MoveFileExA(dst[0], src[0], 0); /* halb verschoben -> zurueck */
            return -1;
        }
    }
    return 1;
}

int loclive_pin_lookup(const char *path, uint32_t account_id, char out[16])
{
    FILE *f = NULL;
    char buf[LOCLIVE_PIN_MAX + 1];
    size_t n;
    if (!account_id || !path || !path[0] || fopen_s(&f, path, "rb") != 0 || !f) return 0;
    n = fread(buf, 1, LOCLIVE_PIN_MAX, f);
    fclose(f);
    buf[n] = 0;
    return loclive_pin_get(buf, account_id, out);
}

uint32_t loclive_task_step(loclive_task *t, int auth, uint32_t now_ms, uint32_t timeout_ms)
{
    if (!t || t->magic != LOCLIVE_TASK_MAGIC) return LOCLIVE_E_INVALIDARG;
    t->steps++;
    switch (t->kind) {
    case LLT_LOGON:
        if (!t->result) {
            if (auth == 2) {
                t->result = LOCLIVE_LOGON_OK;
            } else if (auth == 1 && (uint32_t)(now_ms - t->start_ms) < timeout_ms) {
                return LOCLIVE_TASK_RUNNING;
            } else {
                t->result = LOCLIVE_E_NO_SERVICE; /* kein Konto, abgelehnt oder Zeit um */
            }
        }
        return t->result;
    case LLT_FRIENDS:
    case LLT_MUTE:
        return LOCLIVE_TASK_RUNNING;
    case LLT_FRIENDS_ENUM:
        if (t->finished) return LOCLIVE_TASK_SUCCESS;
        return t->steps == 1 ? LOCLIVE_TASK_RESULTS : LOCLIVE_TASK_RUNNING;
    case LLT_DONE:
        return LOCLIVE_TASK_SUCCESS;
    case LLT_NET: /* W482: Ergebnis setzt die HLE, sobald steelnet geantwortet hat (bzw. Zeit um) */
        return t->result ? t->result : LOCLIVE_TASK_RUNNING;
    default:
        return LOCLIVE_E_INVALIDARG;
    }
}

/* ==================================================================== W482 LIVE-MATCH */
/* LOC_RE1 §2.3 (Match, 19) und §2.4 (Chatraum, 13): Reihenfolge = Create = Suchparameter */
static const uint32_t k_match_ids[19] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                                         0x0B, 0x0C, 0x0D, 0x0E, 0x0010000Fu, 0x10, 0x11, 0x18, 0x0010001Fu};
static const uint32_t k_chat_ids[13] = {0x0010000Fu, 0x12, 0x19, 0x13, 0x14, 0x1A, 0x1B,
                                        0x1C, 0x1E, 0x09, 0x0A, 0x0B, 0x0010001Fu};

int loclive_proc_ids(uint32_t proc, const uint32_t **ids)
{
    if (proc == LOCLIVE_PROC_MATCH) {
        *ids = k_match_ids;
        return 19;
    }
    if (proc == LOCLIVE_PROC_CHAT) {
        *ids = k_chat_ids;
        return 13;
    }
    *ids = NULL;
    return 0;
}

int loclive_kind(const loclive_attr *a, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i].id == 0x13 || a[i].id == 0x1A) return 2;
    }
    return 1;
}

static int ll_app(char *out, int cap, int *o, const char *s)
{
    int l = (int)strlen(s);
    if (*o + l >= cap) return 0;
    memcpy(out + *o, s, (size_t)l);
    *o += l;
    out[*o] = 0;
    return 1;
}

/* WCHAR (BMP, ohne Surrogate - wie steelnet) -> UTF-8 mit JSON-Maskierung */
static int ll_wjson(const uint16_t *w, char *out, int cap, int *o)
{
    int i;
    char b[8];
    if (!ll_app(out, cap, o, "\"")) return 0;
    for (i = 0; i < LOCLIVE_STR_MAX && w[i]; i++) {
        unsigned c = w[i];
        if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c <= 0xDFFF)) c = '?';
        if (c == '"' || c == '\\') {
            b[0] = '\\';
            b[1] = (char)c;
            b[2] = 0;
        } else if (c < 0x80) {
            b[0] = (char)c;
            b[1] = 0;
        } else if (c < 0x800) {
            b[0] = (char)(0xC0 | (c >> 6));
            b[1] = (char)(0x80 | (c & 0x3F));
            b[2] = 0;
        } else {
            b[0] = (char)(0xE0 | (c >> 12));
            b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[2] = (char)(0x80 | (c & 0x3F));
            b[3] = 0;
        }
        if (!ll_app(out, cap, o, b)) return 0;
    }
    return ll_app(out, cap, o, "\"");
}

int loclive_attrs_json(const loclive_attr *a, int n, char *out, int cap)
{
    int i, o = 0;
    char b[48];
    if (!out || cap < 3) return -1;
    out[0] = 0;
    if (!ll_app(out, cap, &o, "[")) return -1;
    for (i = 0; i < n && i < LOCLIVE_MAX_ATTR; i++) {
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s[%u,", i ? "," : "", a[i].id);
        if (!ll_app(out, cap, &o, b)) return -1;
        if (a[i].is_null) {
            if (!ll_app(out, cap, &o, "null")) return -1;
        } else if (a[i].is_str) {
            if (!ll_wjson(a[i].w, out, cap, &o)) return -1;
        } else {
            _snprintf_s(b, sizeof b, _TRUNCATE, "%lld", (long long)a[i].v);
            if (!ll_app(out, cap, &o, b)) return -1;
        }
        if (!ll_app(out, cap, &o, "]")) return -1;
    }
    if (!ll_app(out, cap, &o, "]")) return -1;
    return o;
}

void loclive_key_placeholder(const uint8_t sid[8], uint8_t key[16])
{
    memcpy(key, "SBLIVEK", 7);
    key[7] = 0;
    memcpy(key + 8, sid, 8);
}

static void ll_put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}
static uint32_t ll_get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

#define LL_STR_SLOT 32u /* WCHAR[16] je String im Ergebnisblock */

uint32_t loclive_result_size(const loclive_sess *s)
{
    uint32_t n = LOCLIVE_SR_SIZE;
    int i, na = s->nattr < 0 ? 0 : (s->nattr > LOCLIVE_MAX_ATTR ? LOCLIVE_MAX_ATTR : s->nattr);
    n += (uint32_t)na * LOCLIVE_ATTR_SIZE;
    for (i = 0; i < na; i++) {
        if (s->attr[i].is_str) n += LL_STR_SLOT;
    }
    return n;
}

void loclive_result_build(uint8_t *dst, uint32_t va, const loclive_sess *s, const uint8_t xnaddr[LOCLIVE_XNADDR_SIZE],
                          const uint8_t key[16])
{
    uint32_t size = loclive_result_size(s), str_off;
    int i, na = s->nattr < 0 ? 0 : (s->nattr > LOCLIVE_MAX_ATTR ? LOCLIVE_MAX_ATTR : s->nattr);
    memset(dst, 0, size);
    ll_put32(dst + 0x00, size);           /* dwReserved: Blockgroesse (nur fuer unser Parse) */
    memcpy(dst + 0x04, s->sid, 8);        /* SessionID (XNKID) */
    memcpy(dst + 0x0C, xnaddr, LOCLIVE_XNADDR_SIZE); /* HostAddress */
    memcpy(dst + 0x30, key, 16);          /* KeyExchangeKey */
    ll_put32(dst + 0x40, s->slots[0]);    /* dwPublicOpen */
    ll_put32(dst + 0x44, s->slots[2]);    /* dwPrivateOpen */
    ll_put32(dst + 0x48, s->slots[1]);    /* dwPublicFilled */
    ll_put32(dst + 0x4C, s->slots[3]);    /* dwPrivateFilled */
    ll_put32(dst + 0x50, (uint32_t)na);   /* dwNumAttributes */
    str_off = LOCLIVE_SR_SIZE + (uint32_t)na * LOCLIVE_ATTR_SIZE;
    for (i = 0; i < na; i++) {
        uint8_t *e = dst + LOCLIVE_SR_SIZE + (uint32_t)i * LOCLIVE_ATTR_SIZE;
        const loclive_attr *a = &s->attr[i];
        ll_put32(e, a->id);
        if (a->is_str) {
            int k;
            for (k = 0; k < LOCLIVE_STR_MAX && a->w[k]; k++) memcpy(dst + str_off + 2u * (uint32_t)k, &a->w[k], 2);
            ll_put32(e + 8, va + str_off);
            str_off += LL_STR_SLOT;
        } else {
            memcpy(e + 8, &a->v, 8);
        }
    }
}

uint32_t loclive_parse(const uint8_t *res, uint32_t va, uint32_t len, uint32_t nspec, const uint32_t *spec,
                       uint8_t *out, uint32_t cap, uint32_t *used)
{
    uint32_t i, na, size, o = 0;
    if (used) *used = 0;
    if (!res || !spec || !out || len < LOCLIVE_SR_SIZE || nspec > LOCLIVE_MAX_ATTR) return LOCLIVE_E_FAIL;
    size = ll_get32(res);
    na = ll_get32(res + 0x50);
    if (size > len || size < LOCLIVE_SR_SIZE || na > LOCLIVE_MAX_ATTR ||
        LOCLIVE_SR_SIZE + na * LOCLIVE_ATTR_SIZE > size || nspec > na)
        return LOCLIVE_E_FAIL;
    for (i = 0; i < nspec; i++) {
        uint32_t typ = spec[2 * i], sl = spec[2 * i + 1];
        const uint8_t *e = res + LOCLIVE_SR_SIZE + i * LOCLIVE_ATTR_SIZE;
        uint32_t id = ll_get32(e);
        if ((typ & LOCLIVE_ATTR_TYPEMASK) == LOCLIVE_ATTR_STRING) {
            uint32_t p = ll_get32(e + 8), off, k;
            if (!(id & LOCLIVE_ATTR_STRING) || sl < 2 || sl > 256 || (sl & 1) || o + sl > cap) return LOCLIVE_E_FAIL;
            off = p - va;
            if (p < va || off < LOCLIVE_SR_SIZE || off + LL_STR_SLOT > size) return LOCLIVE_E_FAIL;
            memset(out + o, 0, sl);
            for (k = 0; k + 1 < sl / 2 && k < LOCLIVE_STR_MAX; k++) {
                uint16_t ch;
                memcpy(&ch, res + off + 2 * k, 2);
                if (!ch) break;
                memcpy(out + o + 2 * k, &ch, 2);
            }
            o += sl;
        } else if ((typ & LOCLIVE_ATTR_TYPEMASK) == 0) {
            if ((id & LOCLIVE_ATTR_TYPEMASK) != 0 || o + 8 > cap) return LOCLIVE_E_FAIL;
            memcpy(out + o, e + 8, 8);
            o += 8;
        } else {
            return LOCLIVE_E_FAIL;
        }
    }
    if (used) *used = o;
    return LOCLIVE_S_OK;
}

/* ---------- W485 LIVE-QOS ---------- */
uint32_t loclive_qos_bps(int src)
{
    uint32_t b;
    switch (src) {
    case LOCLIVE_QOS_SRC_DIRECT: b = LOCLIVE_QOS_BPS_DIRECT; break;
    case LOCLIVE_QOS_SRC_RELAY: b = LOCLIVE_QOS_BPS_RELAY; break;
    case LOCLIVE_QOS_SRC_SERVICE: b = LOCLIVE_QOS_BPS_SERVICE; break;
    default: b = LOCLIVE_QOS_BPS_EST; break;
    }
    return b < LOCLIVE_QOS_MIN_BPS ? LOCLIVE_QOS_MIN_BPS : b;
}

static int qos_clamp(int v)
{
    return v < 1 ? 1 : v > 4999 ? 4999 : v;
}

void loclive_qos_entry(uint8_t e[LOCLIVE_QOS_INFO_SIZE], unsigned probes, const loclive_qos_val *v)
{
    int med = qos_clamp(v ? v->rtt_med : LOCLIVE_QOS_RTT_DEFAULT);
    int mn = qos_clamp(v && v->rtt_min > 0 ? v->rtt_min : med);
    uint16_t p = (uint16_t)(probes == 0 ? 1 : probes > 0xFFFF ? 0xFFFF : probes);
    uint16_t a = (uint16_t)(mn > med ? med : mn), m = (uint16_t)med;
    uint32_t bps = loclive_qos_bps(v ? v->src : LOCLIVE_QOS_SRC_NONE);
    memset(e, 0, LOCLIVE_QOS_INFO_SIZE);
    e[0] = (uint8_t)(LOCLIVE_QOS_F_COMPLETE | LOCLIVE_QOS_F_CONTACTED);
    memcpy(e + 2, &p, 2);
    memcpy(e + 4, &p, 2);
    memcpy(e + 12, &a, 2);
    memcpy(e + 14, &m, 2);
    memcpy(e + 16, &bps, 4);
    memcpy(e + 20, &bps, 4);
}

uint32_t loclive_qos_block(uint8_t *q, uint32_t cap, unsigned probes, const loclive_qos_val *v, unsigned n)
{
    uint32_t i, need;
    if (n == 0) n = 1;
    if (n > LOCLIVE_QOS_MAX_TARGETS) n = LOCLIVE_QOS_MAX_TARGETS;
    need = 8u + LOCLIVE_QOS_INFO_SIZE * n;
    if (!q || cap < need) return 0;
    memset(q, 0, need);
    memcpy(q, &n, 4); /* cxnqos; cxnqosPending = 0: alles fertig */
    for (i = 0; i < n; i++) loclive_qos_entry(q + 8 + LOCLIVE_QOS_INFO_SIZE * i, probes, v ? &v[i] : NULL);
    return need;
}

int loclive_qos_class(const uint8_t e[LOCLIVE_QOS_INFO_SIZE])
{
    uint16_t med;
    int p;
    if (!(e[0] & LOCLIVE_QOS_F_COMPLETE)) return 0;
    memcpy(&med, e + 14, 2);
    p = med / 2;
    return p < 100 ? 4 : p < 200 ? 3 : p < 300 ? 2 : 1;
}
