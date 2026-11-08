/* launcher_rules.c - W171 LAUNCHER-P2P3: Regelwerk R1-R30 (s. launcher_rules.h,
 * UX-Konzept W169 Abschnitt 5.2). Reine Logik, keine Fenster. Texte ueber
 * sb_lang (Schluessel gui.rule_*), Quelltext ASCII. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "launcher_rules.h"
#include "launcher_app.h"
#include "sb_lang.h"
#include "sb_edition.h"

#define printf cli_printf

typedef struct {
    int pattern;
    int locks;        /* 1 = greift -> Ziel gesperrt (A/D/E) */
    int always;       /* 1 = greift immer (feste Info/Hinweis) */
    const char *name; /* ASCII-Kurzbeschreibung */
} lr_def_t;

/* Index = Regelnummer (Konzept 5.2). */
static const lr_def_t kDef[LR_N] = {
    { 0, 0, 0, "" },
    { LR_PAT_A, 1, 0, "R1 Ordner-Modus nur ohne Disc-Abbild" },
    { LR_PAT_B, 0, 0, "R2 Programmdatei im Spielordner gefunden" },
    { LR_PAT_E, 1, 0, "R3 Start nur mit gueltigen Spieldateien" },
    { LR_PAT_E, 1, 0, "R4 Start nur mit Client (sb_ldr.exe)" },
    { LR_PAT_A, 1, 0, "R5 VR nur mit OpenXR-Runtime" },
    { LR_PAT_B, 0, 1, "R6 VR-Vorschau gilt nur beim VR-Start" },
    { LR_PAT_A, 1, 0, "R7 Aufloesung nur im Fenster" },
    { LR_PAT_A, 1, 0, "R8 Bildwiederholrate nur exklusiv" },
    { LR_PAT_B, 0, 0, "R9 Exklusiv: Bildrate folgt Hz" },
    { LR_PAT_B, 0, 0, "R10 120 fps auf <120 Hz ruckelt" },
    { LR_PAT_A, 1, 0, "R11 Raender nur bei Nicht-4:3" },
    { LR_PAT_A, 1, 0, "R12 Verlauf nur bei Ausblenden" },
    { LR_PAT_I, 0, 1, "R13 Seitenverhaeltnis fest (nie strecken)" },
    { LR_PAT_C, 0, 0, "R14 Voreinstellung ueber Benutzerdefiniert" },
    { LR_PAT_A, 1, 0, "R15 Schatten/FXAA Welt brauchen Weltbeleuchtung" }, /* W173: Launcher-Elemente */
    { LR_PAT_A, 1, 0, "R16 Weiche Schatten brauchen Schatten" },
    { LR_PAT_B, 0, 0, "R17 HD-Texturen ohne Textur-Pack" },
    { LR_PAT_A, 1, 0, "R18 Texturdetail nur mit Textur-Pack" },
    { LR_PAT_A, 1, 0, "R19 Kategorie-Grenzen nur mit 2x/4x" },
    { LR_PAT_D, 1, 0, "R20 Effekte vom Effekt-Pack gesetzt" },
    { LR_PAT_A, 1, 0, "R21 Abgelehntes Pack nicht einschaltbar" },
    { LR_PAT_B, 0, 0, "R22 Pruefung ohne Spielversion" },
    { LR_PAT_A, 1, 0, "R23 Maus-aus nur mit Original-Controller" },
    { LR_PAT_A, 1, 0, "R24 Maus-Zielen aus bei Controller + Maus aus" },
    { LR_PAT_B, 0, 0, "R25 Controller gewaehlt, Geraet fehlt" },
    { LR_PAT_B, 0, 0, "R26 Starthilfe nur Standardbelegung + Tastatur" },
    { LR_PAT_A, 1, 0, "R27 Live-Ton nur mit Ton" },
    { LR_PAT_B, 0, 0, "R28 Gespeicherte Einstellung beim Start" },
    { LR_PAT_B, 0, 0, "R29 Keine Packs (Pack Creator)" },
    { LR_PAT_B, 0, 0, "R30 Remaster ohne Textur-Pack" },
    { LR_PAT_A, 1, 0, "R31 Pack-Grenze erreicht (32)" },        /* W172 */
    { LR_PAT_B, 0, 0, "R32 Pack Creator nicht neben dem Launcher" }, /* W172 */
    { LR_PAT_B, 0, 0, "R33 Fotoordner nicht nutzbar" },              /* W195 */
    { LR_PAT_B, 0, 1, "R34 Cockpit-Perspektive nur Desktop (nicht in VR)" }, /* W191 */
};

int lrules_pattern(int rule)
{
    return (rule >= LR_FIRST && rule <= LR_LAST) ? kDef[rule].pattern : 0;
}

int lrules_locks(int rule)
{
    return (rule >= LR_FIRST && rule <= LR_LAST) ? kDef[rule].locks : 0;
}

int lrules_always(int rule)
{
    return (rule >= LR_FIRST && rule <= LR_LAST) ? kDef[rule].always : 0;
}

const char *lrules_name(int rule)
{
    return (rule >= LR_FIRST && rule <= LR_LAST) ? kDef[rule].name : "";
}

void lrules_env_default(lr_env_t *e, const launcher_settings_t *s)
{
    memset(e, 0, sizeof *e);
    e->s = s;
    e->data_ok = 1;
    e->game_for_check = 1;
    e->has_ldr = 1;
    e->vr_runtime = 1;
    e->sbc_found = -1;
    e->display_hz = 144;
    e->mon_w = 2560;
    e->mon_h = 1440;
    e->n_packs = 1;
    e->tex_packs = 1;
    e->pk_level = 4;
    e->pk_sel = 0;
    e->pk_sel_enabled = 1;
    e->has_creator = 1;
    e->photo_dir_ok = 1; /* W195 */
}

/* Regel greift: Text aus Schluessel (printf-Format mit einem %s/%d-Argument
 * oder ohne). */
static void hit(lr_res_t *r, int rule, int level)
{
    r->active = 1;
    r->disable = kDef[rule].locks;
    r->level = level;
}

static void eff_size(const lr_env_t *e, unsigned *w, unsigned *h)
{
    if (e->s->window_mode == 1 || e->s->window_mode == 2) {
        *w = e->mon_w;
        *h = e->mon_h;
    } else {
        *w = e->s->width;
        *h = e->s->height;
    }
}

void lrules_eval(const lr_env_t *e, lr_res_t out[LR_N])
{
    const launcher_settings_t *s = e->s;
    int tex_any = e->tex_packs + e->legacy_tex;
    int aa = s->shx[LSHX_AA];
    memset(out, 0, sizeof(lr_res_t) * LR_N);

    /* R1: Ordner-Modus (Advanced) nur ohne Disc-Abbild */
    if (s->game_image[0]) {
        hit(&out[1], 1, LR_LVL_INFO);
        strcpy_s(out[1].text, sizeof out[1].text, LT("gui.rule_r1"));
    }
    /* R2: Programmdatei aus dem Spielordner (Hinweis) */
    if (!s->game_image[0] && e->xbe_auto) {
        hit(&out[2], 2, LR_LVL_INFO);
        strcpy_s(out[2].text, sizeof out[2].text, LT("gui.rule_r2"));
    }
    /* R3: Start nur mit gueltigen Spieldateien */
    if (!e->data_ok) {
        hit(&out[3], 3, LR_LVL_ERR);
        if (!s->game_image[0] && !s->xbe_path[0] && !s->media_root[0]) {
            strcpy_s(out[3].text, sizeof out[3].text, LT("gui.rule_r3"));
        } else if (e->data_err[0]) {
            strcpy_s(out[3].text, sizeof out[3].text, e->data_err);
        } else {
            strcpy_s(out[3].text, sizeof out[3].text, LT("gui.rule_r3"));
        }
    }
    /* R4: Start nur mit Client */
    if (!e->has_ldr) {
        hit(&out[4], 4, LR_LVL_ERR);
        strcpy_s(out[4].text, sizeof out[4].text, LT("gui.rule_r4"));
    }
    /* R5: VR nur mit OpenXR-Runtime */
    if (!e->vr_runtime) {
        hit(&out[5], 5, LR_LVL_INFO);
        strcpy_s(out[5].text, sizeof out[5].text, LT("gui.rule_r5"));
    }
    /* R6: Vorschau wirkt nur beim VR-Start (immer Hinweis) */
    hit(&out[6], 6, LR_LVL_INFO);
    strcpy_s(out[6].text, sizeof out[6].text, LT("gui.rule_r6"));
    /* R7: Aufloesung nur im Fenster */
    if (s->window_mode == 1 || s->window_mode == 2) {
        char res[48];
        hit(&out[7], 7, LR_LVL_INFO);
        if (e->mon_w && e->mon_h) {
            _snprintf_s(res, sizeof res, _TRUNCATE, "%u \xC3\x97 %u", e->mon_w,
                        e->mon_h);
        } else {
            strcpy_s(res, sizeof res, "?");
        }
        _snprintf_s(out[7].text, sizeof out[7].text, _TRUNCATE,
                    LT("gui.rule_r7"), res);
    }
    /* R8: Bildwiederholrate nur exklusiv */
    if (s->window_mode != 2) {
        hit(&out[8], 8, LR_LVL_INFO);
        strcpy_s(out[8].text, sizeof out[8].text, LT("gui.rule_r8"));
    }
    /* R9: exklusiv -> Bildrate folgt der Bildwiederholrate */
    if (s->window_mode == 2) {
        hit(&out[9], 9, LR_LVL_INFO);
        strcpy_s(out[9].text, sizeof out[9].text, LT("gui.rule_r9"));
    }
    /* R10: 120 fps auf Fenster/randlos mit < 120 Hz */
    if (s->interp_fps == 120 && s->window_mode != 2 && e->display_hz > 0 &&
        e->display_hz < 120) {
        hit(&out[10], 10, LR_LVL_WARN);
        _snprintf_s(out[10].text, sizeof out[10].text, _TRUNCATE,
                    LT("gui.rule_r10"), e->display_hz);
    }
    /* R11: Breitbild-Raender nur, wenn das Bild nicht 4:3 ist */
    {
        unsigned w, h;
        eff_size(e, &w, &h);
        if (w > 0 && h > 0 && (unsigned long long)w * 3 ==
                                  (unsigned long long)h * 4) {
            hit(&out[11], 11, LR_LVL_INFO);
            strcpy_s(out[11].text, sizeof out[11].text, LT("gui.rule_r11"));
        }
    }
    /* R12: Verlaufsbreite/-staerke nur bei "Ausblenden" */
    if (!out[11].active && s->edge_mode != 2) {
        hit(&out[12], 12, LR_LVL_INFO);
        _snprintf_s(out[12].text, sizeof out[12].text, _TRUNCATE,
                    LT("gui.rule_r12"), LT("gui.edge_fade"));
    }
    /* R13: Seitenverhaeltnis fest (S3.10) */
    hit(&out[13], 13, LR_LVL_INFO);
    strcpy_s(out[13].text, sizeof out[13].text, LT("gui.rule_r13"));
    /* R14: Voreinstellung waehlen ersetzt Benutzerdefiniert (Rueckfrage);
     * W173: ein Preset, Schluessel des Effect Packs zaehlen nicht */
    if (lcfg_gfx_preset(s, e->gfx_lock) == LGP_CUSTOM) {
        hit(&out[14], 14, LR_LVL_INFO);
        strcpy_s(out[14].text, sizeof out[14].text, LT("gui.rule_r14"));
    }
    /* R15 (W173): Schatten, weiche Schatten und FXAA "Welt" nur mit
     * Weltbeleuchtung (Schatten-Elemente grau; FXAA-Auswahl: Zusatz) */
    if (s->shx[LSHX_SHADING] != 1) {
        hit(&out[15], 15, LR_LVL_INFO);
        strcpy_s(out[15].text, sizeof out[15].text,
                 LT((aa == 2 || aa == 3) ? "gui.rule_r15_aa" : "gui.rule_r15"));
    }
    /* R16: weiche Schatten nur mit Schatten (W173: Element im Launcher) */
    if (!s->shx[LSHX_SHADOW] && !out[15].active) {
        hit(&out[16], 16, LR_LVL_INFO);
        strcpy_s(out[16].text, sizeof out[16].text, LT("gui.rule_r16"));
    }
    /* R17: HD-Texturen an, aber keine Texturquelle */
    if (s->remaster && s->hd_textures && tex_any == 0) {
        hit(&out[17], 17, LR_LVL_INFO);
        strcpy_s(out[17].text, sizeof out[17].text, LT("gui.rule_r17"));
    }
    /* R18: Texturdetail nur mit aktivem Textur-Pack */
    if (e->tex_packs == 0) {
        hit(&out[18], 18, LR_LVL_INFO);
        strcpy_s(out[18].text, sizeof out[18].text, LT("gui.rule_r18"));
    }
    /* R19: Kategorie-Grenzen nur bei Texturdetail 2x/4x */
    if (e->pk_level == 0) {
        hit(&out[19], 19, LR_LVL_INFO);
        strcpy_s(out[19].text, sizeof out[19].text, LT("gui.rule_r19"));
    }
    /* R20: Effekt-Pack setzt Effekte/Farben (Pack gewinnt beim Start) */
    if (e->fx_pack[0]) {
        hit(&out[20], 20, LR_LVL_LOCK);
        _snprintf_s(out[20].text, sizeof out[20].text, _TRUNCATE,
                    LT("gui.rule_r20"), e->fx_pack);
        _snprintf_s(out[20].text2, sizeof out[20].text2, _TRUNCATE,
                    LT("gui.rule_r20_preset"), e->fx_pack);
    }
    /* R21: abgelehntes Pack nicht einschaltbar (ausschalten bleibt erlaubt) */
    if (e->pk_sel >= 0 && e->pk_sel_rejected) {
        hit(&out[21], 21, LR_LVL_ERR);
        out[21].disable = !e->pk_sel_enabled;
        _snprintf_s(out[21].text, sizeof out[21].text, _TRUNCATE,
                    LT("gui.rule_r21"),
                    e->pk_sel_reason[0] ? e->pk_sel_reason : "?");
    }
    /* R22: Pruefung ohne Spielversion (kein Abbild/keine XBE) */
    if (!e->game_for_check) {
        hit(&out[22], 22, LR_LVL_INFO);
        strcpy_s(out[22].text, sizeof out[22].text, LT("gui.rule_r22"));
    }
    /* R23: Maus-aus-Schalter nur mit Original-Controller */
    if (!s->sbc_usb) {
        hit(&out[23], 23, LR_LVL_INFO);
        strcpy_s(out[23].text, sizeof out[23].text, LT("gui.rule_r23"));
    }
    /* R24: Maus-Zielen wirkungslos bei Controller + Maus aus */
    if (s->sbc_usb && s->sbc_mouse_off) {
        hit(&out[24], 24, LR_LVL_INFO);
        strcpy_s(out[24].text, sizeof out[24].text, LT("gui.rule_r24"));
    }
    /* R25: Controller gewaehlt, Geraet nicht gefunden */
    if (s->sbc_usb && e->sbc_found == 0) {
        hit(&out[25], 25, LR_LVL_WARN);
        strcpy_s(out[25].text, sizeof out[25].text, LT("gui.rule_r25"));
    }
    /* R26: Tastatur-Starthilfe nur mit Standardbelegung ohne Controller */
    if (s->sbc_usb || s->input_profile[0]) {
        hit(&out[26], 26, LR_LVL_INFO);
        strcpy_s(out[26].text, sizeof out[26].text,
                 LT(s->sbc_usb ? "gui.rule_r26_pad" : "gui.rule_r26_layout"));
    }
    /* R27: Live-Ton nur mit Ton */
    if (!s->audio) {
        hit(&out[27], 27, LR_LVL_INFO);
        strcpy_s(out[27].text, sizeof out[27].text, LT("gui.rule_r27"));
    }
    /* R28: gespeicherte Einstellung wird beim Launcher-Start geladen */
    if (e->default_setup[0]) {
        hit(&out[28], 28, LR_LVL_INFO);
        _snprintf_s(out[28].text, sizeof out[28].text, _TRUNCATE,
                    LT("gui.rule_r28"), e->default_setup);
    }
    /* R29: noch keine Packs */
    if (e->n_packs == 0) {
        hit(&out[29], 29, LR_LVL_INFO);
        strcpy_s(out[29].text, sizeof out[29].text, LT(SB_EDITION_COMMUNITY ? "gui.rule_r29_community" : "gui.rule_r29"));
    }
    /* R30: Remaster ohne Texturquelle -> nur Licht/Effekte */
    if (s->remaster && tex_any == 0) {
        hit(&out[30], 30, LR_LVL_INFO);
        strcpy_s(out[30].text, sizeof out[30].text, LT("gui.rule_r30"));
    }
    /* R31 (W172): Pack-Grenze erreicht -> Hinzufuegen gesperrt */
    if (e->n_packs >= 32) {
        hit(&out[31], 31, LR_LVL_INFO);
        strcpy_s(out[31].text, sizeof out[31].text, LT("gui.rule_r31"));
    }
    /* R34 (W191 UW-PERSP): Cockpit-Perspektive nur am Monitor (immer Hinweis
     * wie R6: VR ist ein Startweg, kein Schalter; beim VR-Start schaltet der
     * Client die Option ab, die VR-Projektion bleibt physikalisch korrekt) */
    hit(&out[34], 34, LR_LVL_INFO);
    strcpy_s(out[34].text, sizeof out[34].text, LT("gui.rule_r34"));
    /* R32 (W172): Pack Creator liegt nicht neben dem Launcher -> Erklaerung
     * statt Knopf (PO-Entscheidung 6) */
    if (!SB_EDITION_COMMUNITY && !e->has_creator) {
        hit(&out[32], 32, LR_LVL_INFO);
        strcpy_s(out[32].text, sizeof out[32].text, LT("gui.rule_r32"));
    }
    /* R33 (W195 PHOTO-MODE): eingestellter Fotoordner nicht nutzbar (nicht absolut bzw.
     * weder vorhanden noch anlegbar) -> Hinweis; der Client faellt dann nicht still zurueck,
     * sondern meldet die fehlgeschlagene Aufnahme. Leer = Standardordner (immer ok). */
    if (s->photo_dir[0] && !e->photo_dir_ok) {
        hit(&out[33], 33, LR_LVL_WARN);
        _snprintf_s(out[33].text, sizeof out[33].text, _TRUNCATE, LT("gui.rule_r33"), s->photo_dir);
    }
}

/* ------------------------------------------------------------------ */
/* Selbsttest: je Regel Fixture "greift" (v = 1) und "greift nicht"   */
/* (v = 0) -> erwartet active/disable/Text.                            */
/* ------------------------------------------------------------------ */
void lrules_fixture(int rule, int v, launcher_settings_t *s, lr_env_t *e)
{
    switch (rule) {
    case 1: strcpy_s(s->game_image, sizeof s->game_image, v ? "C:/sb.xiso" : ""); break;
    case 2: e->xbe_auto = v; break;
    case 3: e->data_ok = !v;
            if (v) { s->xbe_path[0] = s->media_root[0] = s->game_image[0] = '\0'; }
            break;
    case 4: e->has_ldr = !v; break;
    case 5: e->vr_runtime = !v; break;
    case 6: break;
    case 7: s->window_mode = v ? 1 : 0; break;
    case 8: s->window_mode = v ? 0 : 2; break;
    case 9: s->window_mode = v ? 2 : 0; break;
    case 10: s->interp_fps = 120; s->window_mode = 0; e->display_hz = v ? 60 : 144; break;
    case 11: s->window_mode = 0; s->width = v ? 1600 : 1920; s->height = v ? 1200 : 1080; break;
    case 12: s->window_mode = 0; s->width = 1920; s->height = 1080;
             s->edge_mode = v ? 1 : 2; break;
    case 13: break;
    case 14: lcfg_gfx_apply(s, LGP_ORIGINAL);
             if (v) { s->shx[LSHX_SAT] = 77; } break;
    case 15: s->shx[LSHX_AA] = 2; s->shx[LSHX_SHADING] = v ? 0 : 1;
             s->shx[LSHX_SHADOW] = 1; break; /* W173: R16 greift nicht */
    case 16: s->shx[LSHX_SHADING] = 1; s->shx[LSHX_SHADOW] = v ? 0 : 1; break;
    case 17: s->remaster = 1; s->hd_textures = 1; e->tex_packs = v ? 0 : 1;
             e->legacy_tex = 0; break;
    case 18: e->tex_packs = v ? 0 : 2; break;
    case 19: e->pk_level = v ? 0 : 2; break;
    case 20: strcpy_s(e->fx_pack, sizeof e->fx_pack, v ? "Embers+" : ""); break;
    case 21: e->pk_sel = 0; e->pk_sel_rejected = v; e->pk_sel_enabled = 0;
             strcpy_s(e->pk_sel_reason, sizeof e->pk_sel_reason, "game_mismatch"); break;
    case 22: e->game_for_check = !v; break;
    case 23: s->sbc_usb = !v; break;
    case 24: s->sbc_usb = 1; s->sbc_mouse_off = v; break;
    case 25: s->sbc_usb = 1; e->sbc_found = v ? 0 : 1; break;
    case 26: s->sbc_usb = v; s->input_profile[0] = '\0'; break;
    case 27: s->audio = !v; break;
    case 28: strcpy_s(e->default_setup, sizeof e->default_setup, v ? "my_4k" : ""); break;
    case 29: e->n_packs = v ? 0 : 1; break;
    case 30: s->remaster = s->hd_textures = v; /* W173: remaster = HD-Texturen */
             e->tex_packs = 0; e->legacy_tex = 0; break;
    case 31: e->n_packs = v ? 32 : 31; break;
    case 32: e->has_creator = !v; break;
    case 33: strcpy_s(s->photo_dir, sizeof s->photo_dir, "Q:\\gibt_es_nicht\\Fotos"); e->photo_dir_ok = !v; break; /* W195 */
    case 34: break; /* W191: feste Info */
    }
}

void lrules_selftest(int *pass, int *fail)
{
    int rule, v, bad = 0;
    char why[200] = "";
    for (rule = LR_FIRST; rule <= LR_LAST; rule++) {
        for (v = 1; v >= 0; v--) {
            launcher_settings_t s;
            lr_env_t e;
            lr_res_t r[LR_N];
            int want_act, want_dis;
            if (v == 0 && kDef[rule].always) {
                continue; /* feste Info: greift immer */
            }
            lcfg_defaults(&s);
            lrules_env_default(&e, &s);
            lrules_fixture(rule, v, &s, &e);
            lrules_eval(&e, r);
            want_act = (SB_EDITION_COMMUNITY && rule == 32) ? 0 : v;
            want_dis = v && kDef[rule].locks;
            if (r[rule].active != want_act || r[rule].disable != want_dis ||
                (want_act && !r[rule].text[0]) ||
                (r[rule].active && lrules_pattern(rule) == 0)) {
                if (!bad) {
                    _snprintf_s(why, sizeof why, _TRUNCATE,
                                "R%d v=%d: active=%d disable=%d text='%.60s'",
                                rule, v, r[rule].active, r[rule].disable,
                                r[rule].text);
                }
                bad++;
            }
        }
    }
    /* Zusatz: R20 hat Zweittext, R21 laesst Ausschalten zu, R11 im Vollbild
     * nach Monitor, R12 verschwindet bei R11 (keine doppelte Begruendung). */
    {
        launcher_settings_t s;
        lr_env_t e;
        lr_res_t r[LR_N];
        lcfg_defaults(&s);
        lrules_env_default(&e, &s);
        strcpy_s(e.fx_pack, sizeof e.fx_pack, "X");
        e.pk_sel_rejected = 1;
        e.pk_sel_enabled = 1;
        s.window_mode = 1;
        s.width = 1600;
        s.height = 1200; /* Fenstermass 4:3, Monitor 16:9 -> Raender da */
        e.mon_w = 1024;
        e.mon_h = 768; /* Monitor 4:3 -> keine Raender */
        lrules_eval(&e, r);
        { /* W173: R14 - Schluessel des Effect Packs zaehlen nicht */
            launcher_settings_t t;
            lr_env_t e2;
            lr_res_t r2[LR_N];
            lcfg_defaults(&t);
            lcfg_gfx_apply(&t, LGP_SUBTLE);
            t.shx[LSHX_SAT] = 140;
            lrules_env_default(&e2, &t);
            lrules_eval(&e2, r2);
            if (!r2[14].active) { bad++; }
            lcfg_gfx_lock_key(&e2.gfx_lock, "color_sat");
            lrules_eval(&e2, r2);
            if (r2[14].active) { bad++; }
            if (bad && !why[0]) {
                strcpy_s(why, sizeof why, "Zusatz: R14 mit Effect-Pack-Sperre");
            }
        }
        if (!r[20].text2[0] || !r[21].active || r[21].disable ||
            !r[11].active || r[12].active || !r[7].active) {
            if (!bad) {
                _snprintf_s(why, sizeof why, _TRUNCATE,
                            "Zusatz: R20.text2=%d R21=%d/%d R11=%d R12=%d R7=%d",
                            r[20].text2[0] != 0, r[21].active, r[21].disable,
                            r[11].active, r[12].active, r[7].active);
            }
            bad++;
        }
    }
    if (bad == 0) {
        printf("PASS 42 rules-logic (R1-R%d, %d Faelle)\n", LR_LAST,
               2 * LR_LAST - 2 + 1); /* R6/R13 nur "greift" */
        (*pass)++;
    } else {
        printf("FAIL 42 rules-logic: %d Abweichung(en), erste: %s\n", bad, why);
        (*fail)++;
    }
}
