/* sb_origview.c - W278 ORIG-FLAG (Client). Beschreibung s. sb_origview.h,
 * Einordnung s. sb_origflag.h. Quelltext ASCII. */
#include "render_int.h"
#include "sb_overlay.h"
#include "sb_photo.h"
#include "sb_lang.h"
#include "sb_pack.h"
#include "sb_remaster.h"
#include "sb_origflag.h"
#include "sb_origview.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- cfg-Felder je Schluessel ---------------------------------------- */
typedef struct { const char *key; int *p; } ov_field_t;
static const ov_field_t kOvField[] = {
    { "shading", &g_cfg.shading }, { "world_fog", &g_cfg.world_fog },
    { "world_contrast", &g_cfg.world_contrast }, { "world_gamma", &g_cfg.world_gamma },
    { "bloom_strength", &g_cfg.bloom_strength }, { "cockpit_glow", &g_cfg.cockpit_glow },
    { "cockpit_spill", &g_cfg.cockpit_spill }, { "cockpit_light", &g_cfg.cockpit_light },
    { "world_exposure", &g_cfg.world_exposure }, { "tonemap", &g_cfg.tonemap },
    { "bloom_threshold", &g_cfg.bloom_threshold }, { "light_world", &g_cfg.light_world },
    { "light_cockpit", &g_cfg.light_cockpit }, { "world_focus", &g_cfg.world_focus },
    { "shade_shadow", &g_cfg.shade_shadow }, { "shadow_soft", &g_cfg.shadow_soft },
    { "world_sharpen", &g_cfg.world_sharpen }, { "aa_mode", &g_cfg.aa_mode }, { "ssao", &g_cfg.ssao },
    { "color_sat", &g_cfg.color_sat }, { "color_temp", &g_cfg.color_temp },
    { "color_lift", &g_cfg.color_lift }, { "color_gamma", &g_cfg.color_gamma },
    { "color_gain", &g_cfg.color_gain },
    { "fx_soft", &g_cfg.fx_soft }, { "fx_embers", &g_cfg.fx_embers }, { "fx_light", &g_cfg.fx_light },
    { "fx_light_cockpit", &g_cfg.fx_light_cockpit }, { "fx_interp", &g_cfg.fx_interp },
    { "fx_wireframe", &g_cfg.fx_wireframe }, { "fx_vaporwave", &g_cfg.fx_vaporwave },
    { "monitor_fx", &g_cfg.monitor_fx }, { "monitor_grain", &g_cfg.monitor_grain },
    { "monitor_glitch", &g_cfg.monitor_glitch },
    { "hd_textures", &g_cfg.hd_textures },
    { "edge_mode", &g_cfg.edge_mode }, { "edge_fade_width", &g_cfg.edge_fade_width },
    { "edge_fade_strength", &g_cfg.edge_fade_strength },
    { "cockpit_persp", &g_cfg.cockpit_persp }, { "cockpit_panini", &g_cfg.cockpit_panini },
    { "cockpit_width", &g_cfg.cockpit_width }, { "cockpit_hide", &g_cfg.cockpit_hide },
    { "monitor_view", &g_cfg.monitor_view }, /* W319 */
    { "hud_label_avoid", &g_cfg.hud_label_avoid },
};
#define OV_NFIELD ((int)(sizeof kOvField / sizeof kOvField[0]))
typedef char ov_nfield_chk[(OV_NFIELD == SB_OF_NKEY) ? 1 : -1];

static int *g_fp[SB_OF_NKEY]; /* Feld je Tabellenindex */
static int g_init;
static int g_ihd = -1;        /* Index hd_textures */

static void ov_init(void)
{
    int i, j;
    if (g_init) { return; }
    g_init = 1;
    for (i = 0; i < SB_OF_NKEY; i++) {
        for (j = 0; j < OV_NFIELD; j++) {
            if (strcmp(kOvField[j].key, kSbOrigFlag[i].key) == 0) { g_fp[i] = kOvField[j].p; break; }
        }
        if (!g_fp[i]) { HLOG_E("ORIGFLAG: Schluessel %s ohne cfg-Feld", kSbOrigFlag[i].key); }
    }
    g_ihd = sb_of_find("hd_textures");
}

/* HD-Texturen ueberhaupt vorhanden (Paket bzw. Ordner)? Ab Bild 60 gemerkt. */
static int ov_hd_avail(void)
{
    static int av = -1;
    int v;
    if (av >= 0) { return av; }
    v = sb_rem_live_avail() != 0;
    if (g_present_no >= 60) { av = v; }
    return v;
}

/* wirksamer Wert (HD: remaster && hd_textures und ein Paket ist da) */
static int ov_cur(int i)
{
    if (i == g_ihd) { return (g_cfg.remaster && g_cfg.hd_textures && ov_hd_avail()) ? 1 : 0; }
    return g_fp[i] ? *g_fp[i] : kSbOrigFlag[i].orig;
}

static int ov_fx_pack(void)
{
    int k;
    for (k = 0; k < SB_FXN; k++) {
        if (kSbFxParams[k].key && sb_rem_fx_locked(kSbFxParams[k].key)) { return 1; }
    }
    return 0;
}

unsigned sb_ov_mask(void)
{
    unsigned m = 0;
    int i;
    ov_init();
    for (i = 0; i < SB_OF_NKEY; i++) {
        if (sb_of_dev(i, ov_cur(i))) { m |= 1u << kSbOrigFlag[i].grp; }
    }
    if (ov_fx_pack()) { m |= 1u << SB_OFG_PACKS; }
    return m;
}

int sb_ov_key_state(const char *key)
{
    int i = key ? sb_of_find(key) : -1;
    ov_init();
    if (i < 0) { return 0; }
    if (sb_of_dev(i, ov_cur(i))) { return 2; }
    if (!kSbOrigFlag[i].det) { /* Rand-Breite/-Staerke: folgen edge_mode */
        int e = sb_of_find("edge_mode");
        return sb_of_dev(e, ov_cur(e)) ? 2 : 1;
    }
    return 1;
}

int *sb_ov_field(const char *key) /* W291 */
{
    int i = key ? sb_of_find(key) : -1;
    ov_init();
    return i >= 0 ? g_fp[i] : NULL;
}

int sb_ov_group_state(unsigned groups)
{
    return (sb_ov_mask() & groups) ? 2 : 1;
}

/* ---- Live-Umschalten ------------------------------------------------- */
static int g_cmp;               /* 1 = Original-Vergleich aktiv */
static int g_snap[SB_OF_NKEY];  /* eigene Werte */
static int g_snap_rem, g_snap_hd;
static volatile LONG g_req;

int sb_ov_compare_on(void) { return g_cmp; }

static void ov_hd_live(int on)
{
    int scale = g_cfg.hd_scale == 2 ? 2 : 4;
    if (sb_rem_live_set(on, scale) < 0) {
        HLOG_W("ORIGFLAG: HD-Texturen %s live nicht moeglich (kein Paket)", on ? "an" : "aus");
    } else {
        sb_tex_relive_all();
    }
}

static void ov_set(int orig, const char *why)
{
    int i, hd_was = (g_cfg.remaster && g_cfg.hd_textures) ? 1 : 0, hd_want;
    if (orig == g_cmp) { return; }
    if (orig) {
        for (i = 0; i < SB_OF_NKEY; i++) {
            if (g_fp[i]) { g_snap[i] = *g_fp[i]; }
        }
        g_snap_rem = g_cfg.remaster;
        g_snap_hd = g_cfg.hd_textures;
        for (i = 0; i < SB_OF_NKEY; i++) {
            if (g_fp[i] && i != g_ihd) { *g_fp[i] = kSbOrigFlag[i].orig; }
        }
        hd_want = 0;
    } else {
        for (i = 0; i < SB_OF_NKEY; i++) {
            if (g_fp[i] && i != g_ihd) { *g_fp[i] = g_snap[i]; }
        }
        hd_want = (g_snap_rem && g_snap_hd) ? 1 : 0;
    }
    if (hd_want != hd_was && ov_hd_avail()) { ov_hd_live(hd_want); } /* nur mit Paket (wie ESC: g_ov_avail) */
    if (orig) {
        g_cfg.remaster = 0; /* HD aus (auch ohne Paket) */
        g_cfg.hd_textures = 0;
    } else {
        g_cfg.remaster = g_snap_rem;
        g_cfg.hd_textures = g_snap_hd;
    }
    g_cmp = orig;
    sb_render_pause_dirty();
    HLOG_I("ORIGFLAG: %s (%s, f=%u) shading=%d edge_mode=%d monitor_fx=%d hd=%d cockpit_hide=%d", orig ? "Original-Ansicht" : "eigene Einstellungen",
           why, g_present_no, g_cfg.shading, g_cfg.edge_mode, g_cfg.monitor_fx, g_cfg.remaster && g_cfg.hd_textures,
           g_cfg.cockpit_hide);
}

/* ---- Taste ----------------------------------------------------------- */
/* W283 ORIG-KEY-BIND: bis zu 4 Tasten (Taste + Zusatztasten). Quelle ist die
 * Profil-Aktion orig_view (sb_ov_set_keys, aus dem Hook beim Profil-Laden)
 * oder als Rueckfall [overlay] orig_key (ov_key_parse). */
#define OV_KEYS 4
static unsigned g_kvk;      /* erste Taste, 0 = aus */
static unsigned g_kvks[OV_KEYS];
static int g_kmods[OV_KEYS]; /* Bit 0 Shift, 1 Ctrl, 2 Alt */
static volatile LONG g_kn;   /* Anzahl gueltiger Eintraege */
static char g_kname[32];
static volatile LONG g_kparsed;

static void ov_key_fmt(unsigned vk, int mod, char *out, size_t cap)
{
    char kn[16];
    if (vk >= VK_F1 && vk <= VK_F24) { _snprintf_s(kn, sizeof kn, _TRUNCATE, "F%u", vk - VK_F1 + 1); }
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { _snprintf_s(kn, sizeof kn, _TRUNCATE, "%c", (char)vk); }
    else { _snprintf_s(kn, sizeof kn, _TRUNCATE, "VK%02X", vk); }
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s%s", (mod & 2) ? "Ctrl+" : "", (mod & 4) ? "Alt+" : "",
                (mod & 1) ? "Shift+" : "", kn);
}

static int ov_vk_reserved(unsigned vk)
{
    return vk == VK_F10 || vk == VK_ESCAPE || vk == 0 || vk > 0xFE; /* Maus frei / Menue */
}

static void ov_key_parse(void)
{
    char buf[32], *t, *ctx = NULL;
    int mod = 0;
    unsigned vk = 0;
    g_kvk = 0;
    g_kname[0] = 0;
    strncpy_s(buf, sizeof buf, g_cfg.orig_key, _TRUNCATE);
    for (t = strtok_s(buf, "+ ", &ctx); t; t = strtok_s(NULL, "+ ", &ctx)) {
        if (_stricmp(t, "SHIFT") == 0) { mod |= 1; }
        else if (_stricmp(t, "CTRL") == 0) { mod |= 2; }
        else if (_stricmp(t, "ALT") == 0) { mod |= 4; }
        else if ((t[0] == 'F' || t[0] == 'f') && t[1] >= '1' && t[1] <= '9') {
            int n = atoi(t + 1);
            if (n >= 1 && n <= 24) { vk = (unsigned)(VK_F1 + n - 1); }
        } else if ((t[0] == 'V' || t[0] == 'v') && (t[1] == 'K' || t[1] == 'k') && t[2]) {
            vk = (unsigned)strtoul(t + 2, NULL, 16);
        } else if (!t[1] && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z') ||
                             (t[0] >= '0' && t[0] <= '9'))) {
            vk = (unsigned)(t[0] >= 'a' ? t[0] - 32 : t[0]);
        } else {
            vk = 0; /* OFF, leer, unbekannt */
            break;
        }
    }
    if (ov_vk_reserved(vk)) { vk = 0; }
    g_kvks[0] = vk;
    g_kmods[0] = mod;
    g_kvk = vk;
    if (vk) { ov_key_fmt(vk, mod, g_kname, sizeof g_kname); }
    InterlockedExchange(&g_kn, vk ? 1 : 0);
    InterlockedExchange(&g_kparsed, 1);
    HLOG_I("ORIGFLAG: Umschalttaste %s (orig_key=\"%s\")", vk ? g_kname : "aus", g_cfg.orig_key);
}

void sb_ov_set_keys(const int *vks, const int *mods, int n, int pad)
{
    int i, k = 0;
    char all[96];
    InterlockedExchange(&g_kn, 0);
    for (i = 0; i < n && k < OV_KEYS; i++) {
        unsigned v = (unsigned)vks[i];
        if (ov_vk_reserved(v)) { continue; }
        /* Fenster meldet Zusatztasten ohne links/rechts (VK_SHIFT/CONTROL/MENU) */
        if (v == VK_LSHIFT || v == VK_RSHIFT) { v = VK_SHIFT; }
        else if (v == VK_LCONTROL || v == VK_RCONTROL) { v = VK_CONTROL; }
        else if (v == VK_LMENU || v == VK_RMENU) { v = VK_MENU; }
        g_kvks[k] = v;
        g_kmods[k] = mods[i] & 7;
        k++;
    }
    g_kvk = k ? g_kvks[0] : 0;
    g_kname[0] = 0;
    all[0] = 0;
    for (i = 0; i < k; i++) {
        char one[32];
        ov_key_fmt(g_kvks[i], g_kmods[i], one, sizeof one);
        if (i == 0) { strcpy_s(g_kname, sizeof g_kname, one); }
        if (i) { strncat_s(all, sizeof all, ", ", _TRUNCATE); }
        strncat_s(all, sizeof all, one, _TRUNCATE);
    }
    InterlockedExchange(&g_kn, k);
    InterlockedExchange(&g_kparsed, 1);
    HLOG_I("ORIGFLAG: Umschalttaste %s (Profil orig_view%s)", k ? all : "aus",
           pad ? ", dazu Pad/Controller" : "");
}

void sb_ov_request(void)
{
    InterlockedExchange(&g_req, 2);
}

const char *sb_ov_key_name(void)
{
    if (!g_kparsed) { ov_key_parse(); }
    return g_kname;
}

int sb_ov_key(unsigned vk, int repeat)
{
    int mod, i, n;
    if (!g_kparsed) { ov_key_parse(); }
    n = (int)g_kn;
    if (!n || !vk) { return 0; }
    mod = ((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) |
          ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
    /* W283: Zusatztaste selbst belegt (KEY:SHIFT): ihr eigenes Bit zaehlt nicht */
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) { mod &= ~1; }
    else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) { mod &= ~2; }
    else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) { mod &= ~4; }
    for (i = 0; i < n; i++) {
        if (vk == g_kvks[i] && mod == g_kmods[i]) {
            if (!repeat) { InterlockedExchange(&g_req, 1); }
            return 1;
        }
    }
    return 0;
}

/* ---- Einblendung (Band oben in der 4:3-Box) ---------------------------- */
#define TO_TITLE_R 255
#define TO_TITLE_G 196
#define TO_TITLE_B 92
#define TO_TEXT_R 200
#define TO_TEXT_G 236
#define TO_TEXT_B 205
#define TO_BG_A 0.84f

static wchar_t g_t1[160], g_t2[200];
static ULONGLONG g_until;
static int g_tdirty = 1;
static HDC to_dc;
static HBITMAP to_bmp, to_bmp_old;
static unsigned *to_bits, *to_pm;
static unsigned to_w, to_h, to_tw, to_th;
static float to_f;
static int to_dirty_tex = 1;
static ID3D11Texture2D *to_tex;
static ID3D11ShaderResourceView *to_srv;
static sb_ovl_save_t to_save;

static void ov_toast(const wchar_t *t1, const wchar_t *t2, unsigned ms)
{
    wcsncpy_s(g_t1, 160, t1 ? t1 : L"", _TRUNCATE);
    wcsncpy_s(g_t2, 200, t2 ? t2 : L"", _TRUNCATE);
    g_until = GetTickCount64() + ms;
    g_tdirty = 1;
}

/* Gruppenliste "HD textures, widescreen, lighting" (Reihenfolge kSbOrigGroupOrder). */
static void ov_group_list(unsigned m, wchar_t *out, int cap)
{
    int k, n = 0;
    out[0] = 0;
    for (k = 0; k < SB_OFG_N; k++) {
        int g = kSbOrigGroupOrder[k];
        wchar_t w[64];
        if (!(m & (1u << g))) { continue; }
        sb_lang_w(kSbOrigGroupKey[g], w, 64);
        if (n++) { wcsncat_s(out, cap, L", ", _TRUNCATE); }
        wcsncat_s(out, cap, w, _TRUNCATE);
    }
}

/* W290 QUICK-SHOT: Einblendung von aussen (Schnell-Screenshot "Screenshot saved"), gleiche Zeichnung. */
void sb_ov_toast_msg(const wchar_t *t1, const wchar_t *t2, unsigned ms)
{
    ov_toast(t1, t2, ms);
}

static void ov_toast_key(const char *k1, const char *k2, const wchar_t *arg, unsigned ms)
{
    wchar_t a[160], b[200], fmt[200];
    sb_lang_w(k1, a, 160);
    b[0] = 0;
    if (k2) {
        sb_lang_w(k2, fmt, 200);
        if (arg && wcsstr(fmt, L"%s")) { _snwprintf_s(b, 200, _TRUNCATE, fmt, arg); }
        else { wcsncpy_s(b, 200, fmt, _TRUNCATE); }
    }
    ov_toast(a, b, ms);
}

static void ov_toast_switch(int orig)
{
    wchar_t kn[32];
    MultiByteToWideChar(CP_UTF8, 0, sb_ov_key_name(), -1, kn, 32);
    ov_toast_key(orig ? "ovl.of_t_orig" : "ovl.of_t_user", orig ? "ovl.of_t_orig2" : "ovl.of_t_user2", kn, 3500);
}

static int g_hint_done;

void sb_ov_tick(unsigned n)
{
    LONG req;
    ov_init();
    if (!g_kparsed) { ov_key_parse(); }
    if (g_cmp && sb_ovl_open()) { /* nie den Vergleichszustand im Menue (Speichern) */
        ov_set(0, "ESC-Menue");
        ov_toast_switch(0);
    }
    if ((req = InterlockedExchange(&g_req, 0)) != 0) {
        if (g_cfg.vr_enable) {
            HLOG_I("ORIGFLAG: Umschalttaste im VR-Betrieb ohne Wirkung");
        } else if (sb_ovl_open()) {
            HLOG_I("ORIGFLAG: Umschalttaste bei offenem Menue ignoriert");
        } else if (!g_cmp && !sb_ov_mask()) {
            ov_toast_key("ovl.of_t_same", "ovl.of_t_same2", NULL, 3000);
            HLOG_I("ORIGFLAG: schon Original (f=%u)", g_present_no);
        } else {
            ov_set(!g_cmp, req == 2 ? "Belegung" : "Taste"); /* W283: 2 = Pad/Controller (orig_view) */
            ov_toast_switch(g_cmp);
        }
    }
    if (!g_hint_done && n >= 150u && !sb_ovl_open()) {
        unsigned m = sb_ov_mask();
        g_hint_done = 1;
        if (g_cfg.orig_once) {
            ov_toast_key("ovl.of_t_once", "ovl.of_t_once2", NULL, 9000);
            HLOG_I("ORIGFLAG: Einmal-Start im Original-Modus (Maske 0x%X, f=%u)", m, n);
        } else if (g_cfg.orig_hint && m) {
            wchar_t list[200], line[260], fmt[120], kn[32];
            sb_lang_w("ovl.of_t_hint2", fmt, 120);
            ov_group_list(m, list, 200);
            _snwprintf_s(line, 260, _TRUNCATE, fmt, list);
            if (g_kvk) {
                wchar_t kf[120], kl[120];
                MultiByteToWideChar(CP_UTF8, 0, g_kname, -1, kn, 32);
                sb_lang_w("ovl.of_t_hint3", kf, 120);
                _snwprintf_s(kl, 120, _TRUNCATE, kf, kn);
                wcsncat_s(line, 260, L"  \x00B7  ", _TRUNCATE);
                wcsncat_s(line, 260, kl, _TRUNCATE);
            }
            {
                wchar_t t1[160];
                sb_lang_w("ovl.of_t_hint", t1, 160);
                ov_toast(t1, line, 10000);
            }
            HLOG_I("ORIGFLAG: Start-Hinweis experimentell (Maske 0x%X, f=%u)", m, n);
        } else {
            HLOG_I("ORIGFLAG: Start ohne Hinweis (Maske 0x%X, orig_hint=%d, f=%u)", m, g_cfg.orig_hint, n);
        }
    }
}

int sb_ov_toast_on(void)
{
    return g_until && GetTickCount64() < g_until && !g_cfg.vr_enable && !sb_photo_active() && !sb_ovl_open();
}

static void to_box(unsigned w, unsigned h, unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh)
{
    float ga = 640.0f / 480.0f, ba = (float)w / (float)(h ? h : 1);
    if (ga >= ba) {
        *bw = w; *bh = (unsigned)((float)w / ga + 0.5f); if (*bh > h) { *bh = h; }
        *bx = 0; *by = (h - *bh) / 2;
    } else {
        *bh = h; *bw = (unsigned)((float)h * ga + 0.5f); if (*bw > w) { *bw = w; }
        *bx = (w - *bw) / 2; *by = 0;
    }
}

static int S(float f, float x)
{
    int v = (int)(x * f + 0.5f);
    return v < 1 ? 1 : v;
}

/* Band bei Faktor f, hoechstens maxw breit, in to_pm rendern. 1 = ok. */
static int to_render(float f, unsigned maxw)
{
    BITMAPINFO bi;
    HFONT f1, f2, fold;
    SIZE s1, s2;
    RECT r;
    int p1 = S(f, 10.0f), p2 = S(f, 7.5f), pad = S(f, 5.0f), gap = S(f, 2.0f), bd = S(f, 1.0f);
    unsigned w, h, i, x, y;
    int n1 = (int)wcslen(g_t1), n2 = (int)wcslen(g_t2);
    if (to_pm && f == to_f && !g_tdirty) { return 1; }
    if (!to_dc) { to_dc = CreateCompatibleDC(NULL); if (!to_dc) { return 0; } }
    f1 = CreateFontW(-p1, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                     ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    f2 = CreateFontW(-p2, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                     ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!f1 || !f2) { if (f1) { DeleteObject(f1); } if (f2) { DeleteObject(f2); } return 0; }
    fold = (HFONT)SelectObject(to_dc, f1);
    if (!GetTextExtentPoint32W(to_dc, g_t1, n1, &s1)) { s1.cx = p1 * n1 / 2; s1.cy = p1; }
    SelectObject(to_dc, f2);
    if (!n2 || !GetTextExtentPoint32W(to_dc, g_t2, n2, &s2)) { s2.cx = 0; s2.cy = n2 ? p2 : 0; }
    w = (unsigned)((s1.cx > s2.cx ? s1.cx : s2.cx) + 2 * pad + 2 * bd);
    if (w > maxw) { w = maxw; }
    h = (unsigned)(s1.cy + (n2 ? gap + s2.cy : 0) + 2 * pad);
    if (to_bmp) { SelectObject(to_dc, to_bmp_old); DeleteObject(to_bmp); to_bmp = NULL; to_bits = NULL; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    to_bmp = CreateDIBSection(to_dc, &bi, DIB_RGB_COLORS, (void **)&to_bits, NULL, 0);
    if (!to_bmp || !to_bits) {
        SelectObject(to_dc, fold); DeleteObject(f1); DeleteObject(f2);
        return 0;
    }
    to_bmp_old = (HBITMAP)SelectObject(to_dc, to_bmp);
    memset(to_bits, 0, (size_t)w * h * 4u);
    SetBkMode(to_dc, TRANSPARENT);
    /* Abdeckung: Titel im Rotkanal, Zeile 2 im Gruenkanal (Graustufen-AA) */
    SelectObject(to_dc, f1);
    SetTextColor(to_dc, RGB(255, 0, 0));
    r.left = pad + bd; r.top = pad; r.right = (LONG)w - pad - bd; r.bottom = pad + s1.cy;
    DrawTextW(to_dc, g_t1, n1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (n2) {
        SelectObject(to_dc, f2);
        SetTextColor(to_dc, RGB(0, 255, 0));
        r.top = pad + s1.cy + gap; r.bottom = r.top + s2.cy;
        DrawTextW(to_dc, g_t2, n2, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    GdiFlush();
    free(to_pm);
    to_pm = (unsigned *)malloc((size_t)w * h * 4u);
    if (!to_pm) { SelectObject(to_dc, fold); DeleteObject(f1); DeleteObject(f2); return 0; }
    for (y = 0, i = 0; y < h; y++) {
        for (x = 0; x < w; x++, i++) {
            float c1 = (float)((to_bits[i] >> 16) & 255u) / 255.0f, c2 = (float)((to_bits[i] >> 8) & 255u) / 255.0f;
            int edge = x < (unsigned)bd || y < (unsigned)bd || x >= w - (unsigned)bd || y >= h - (unsigned)bd;
            float a = TO_BG_A, rr = 4.0f * a, gg = 22.0f * a, bb = 11.0f * a;
            if (edge) { rr = TO_TITLE_R * a; gg = TO_TITLE_G * a; bb = TO_TITLE_B * a; }
            rr = TO_TITLE_R * c1 + rr * (1.0f - c1); gg = TO_TITLE_G * c1 + gg * (1.0f - c1);
            bb = TO_TITLE_B * c1 + bb * (1.0f - c1); a = c1 + a * (1.0f - c1);
            rr = TO_TEXT_R * c2 + rr * (1.0f - c2); gg = TO_TEXT_G * c2 + gg * (1.0f - c2);
            bb = TO_TEXT_B * c2 + bb * (1.0f - c2); a = c2 + a * (1.0f - c2);
            to_pm[i] = ((unsigned)(a * 255.0f + 0.5f) << 24) | ((unsigned)(rr + 0.5f) << 16) |
                       ((unsigned)(gg + 0.5f) << 8) | (unsigned)(bb + 0.5f);
        }
    }
    SelectObject(to_dc, fold);
    DeleteObject(f1);
    DeleteObject(f2);
    to_w = w;
    to_h = h;
    to_f = f;
    g_tdirty = 0;
    to_dirty_tex = 1;
    return 1;
}

static int to_place(unsigned w, unsigned h, unsigned *x0, unsigned *y0)
{
    unsigned bx, by, bw, bh, m;
    float f;
    to_box(w, h, &bx, &by, &bw, &bh);
    f = (float)bh / 540.0f;
    if (f < 1.0f) { f = 1.0f; }
    m = (unsigned)S(f, 10.0f);
    if (bw <= 4u * m) { return 0; }
    if (!to_render(f, bw - 2u * m)) { return 0; }
    if (to_w + 2u * m > bw || to_h + 2u * m > bh) { return 0; }
    *x0 = bx + (bw - to_w) / 2;
    *y0 = by + m;
    return 1;
}

static int to_upload(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!to_tex || to_tw != to_w || to_th != to_h) {
        D3D11_TEXTURE2D_DESC td;
        if (to_srv) { ID3D11ShaderResourceView_Release(to_srv); to_srv = NULL; }
        if (to_tex) { ID3D11Texture2D_Release(to_tex); to_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = to_w;
        td.Height = to_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &to_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)to_tex, NULL, &to_srv))) {
            HLOG_W("ORIGFLAG: Textur %ux%u gescheitert", to_w, to_h);
            return 0;
        }
        to_tw = to_w;
        to_th = to_h;
        to_dirty_tex = 1;
    }
    if (!to_dirty_tex) { return 1; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)to_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return 0;
    }
    for (y = 0; y < to_h; y++) {
        memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, to_pm + (size_t)y * to_w, to_w * 4u);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)to_tex, 0);
    to_dirty_tex = 0;
    return 1;
}

void sb_ov_toast_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned x0, y0;
    if (!sb_ov_toast_on() || !rtv || !g_ctx || !g_dev) { return; }
    if (!to_place(g_bb_w, g_bb_h, &x0, &y0)) { return; }
    if (!to_upload()) { return; }
    if (save) { sb_ovl_save_take(&to_save, rtv, x0, y0, to_w, to_h); }
    sb_ovl_quad(rtv, to_srv, x0, y0, to_w, to_h);
}

void sb_ov_toast_restore(void)
{
    if (!to_save.saved) { return; }
    sb_ctx_enter();
    sb_ovl_save_put(&to_save);
    sb_ctx_leave();
}
