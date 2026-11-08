/* xdk_hooks.c - JMP-Patches auf XDK-Funktionen.
 * Adressen aus functions.tsv (XbSymbolDatabase-Erkennung, MIT); per
 * OOVPA-Methode identifiziert, per Disasm verifiziert.
 * M1: loggen + plausible Rueckgaben (kein echtes Rendering).
 * M2 verankert hier: Aufloesung (CreateDevice), Aspect (Viewport/Transform),
 * Fog/Sichtweite (RenderStates), Present (Swap).
 * W68 SPLIT: hier Patch-Helfer, Fake-Objekte und hooks_install; Hooks in
 * hooks_d3d/_timing/_audio/_xapi/_input/_bots.c (gemeinsam: hooks_int.h). */
#include "hooks_int.h"

float u2f(u32 v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Xbox-SetLastError: schreibt [Block+4] des XAPI-Blocks (vgl. XDK-Code:
 * GetLastError @0x18CE5F: EAX=tls[idx]; EAX=[EAX+4]; SetLastError @0x18CEA5:
 * [EAX+4]=val). slots[idx] selbst ist der BLOCK-ZEIGER (Prefill idx=-5) und
 * darf NICHT ueberschrieben werden! Win32-SetLastError allein sieht das
 * Spiel NICHT (eigene TLS-Emulation) — das war der 997-Retry-Spin
 * (capcom/BGM/0xC7E10: JNZ @0xC7E9D nimmt Free+Alloc-Retry bei GLE!=997). */
static int g_gle_idx_logged;
void xbox_set_last_error(u32 code)
{
    u32 *slots = fs_emu_tls_slots();
    if (slots) {
        int idx = *(volatile int *)(uintptr_t)0x6E8A80u;
        if (!g_gle_idx_logged) {
            g_gle_idx_logged = 1;
            HLOG_I("GLE: xdk-idx=[0x6E8A80]=%d", idx);
        }
        if (idx >= -1024 && idx < 2048) {
            u32 blk = slots[idx];
            if (!blk) {
                /* Thread ohne Prefill (Worker): XAPI-Block nachtraeglich. */
                blk = (u32)(uintptr_t)calloc(1, 64);
                if (blk)
                    slots[idx] = blk;
            }
            if (blk)
                ((u32 *)(uintptr_t)blk)[1] = code; /* [Block+4] */
        }
    }
    SetLastError(code); /* Win32-Seite parallel pflegen */
}

unsigned g_frames;
uint64_t g_qpf, g_fps_start;
u32 g_last_frame_ms;

/* Fake-Objekte: 1 KB (256 reichte nicht: Spiel schreibt z.B.
 * [obj+0x104/+0x108/+0x10C] (State-Cache @0x106850) und lief in den
 * Nachbar-Block (g_fake_d3d-Limit!) ueber -> Crash 0x21185C). */
uint8_t g_fake_device[1024];
uint8_t g_fake_surface[1024];
uint8_t g_fake_stream[1024];
uint8_t g_fake_obj[1024];
uint8_t g_fake_d3d[0x3000]; /* D3D-Device-Struktur ([0x21A8A0]) */
static u32 g_fake_pushbuf[16];  /* Fake-Pushbuffer-Zelle (get==put = idle) */

/* ---------- Hook-Helfer ---------- */
static void patch_rel(u32 va, void *handler, const char *name, uint8_t op)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    uint8_t orig[5];
    intptr_t rel;
    memcpy(orig, p, 5);
    rel = (uint8_t *)handler - (p + 5);
    p[0] = op;
    memcpy(p + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    HLOG_I("HOOK %s @0x%X (orig %02X %02X %02X %02X %02X)", name, va, orig[0],
           orig[1], orig[2], orig[3], orig[4]);
}

void patch_jmp(u32 va, void *handler, const char *name)
{
    patch_rel(va, handler, name, 0xE9);
}

/* FIX(SHADE-FX,W121) vis_query_live: Die W117-Hooks auf Begin/EndVisibility-
 * Test wurden nur beim Start installiert (shading=1). Schaltet der Spieler
 * Shading erst im ESC-Menue ein, galt die Sonne wieder fuer jeden Index als
 * sichtbar (QueryStatus = 1) -> Dauerblendung, Umgebungslicht +0,257 ->
 * Hangar/Welt ueberbelichtet (W121_h2). Hier nachinstallieren, sobald der
 * Test aktiv wird. Aufruf am Present-Anfang im D3D-Thread des Spiels (nie
 * mitten in Begin/End). Start mit shading=0 und ohne Umschalten: unveraendert. */
void xdk_visq_live(void)
{
    extern int sb_visq_active(void);
    extern unsigned g_present_no;
    if (g_visq_on || !g_cfg.vis_query_live || !sb_visq_active()) { return; }
    patch_jmp(0x20EA80u, hook_D3D_BeginVisTest, "D3D-BeginVisibilityTest(query, live)");
    patch_jmp(0x20EB20u, hook_D3D_EndVisTest, "D3D-EndVisibilityTest(query, live)");
    g_visq_on = 1;
    HLOG_I("VISQ live: Sichtbarkeitstest nachinstalliert (Shading zur Laufzeit an, f=%u)", g_present_no);
}

/* NOP-Sled: reinen HW-Write-Block stilllegen (Spiellogik davor/dahinter
 * laeuft weiter). Nur fuer verifiziert reine Write-Sequenzen! Verifiziert
 * erstes Byte + Folgeteil (kein Blind-NOP ueber Codegrenzen). */
static void patch_nop(u32 va, u32 len, const char *name, uint8_t first,
                      uint8_t after)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    if (p[0] != first || p[len] != after) {
        HLOG_E("NOP %s @0x%X: Byte-Mismatch (0x%02X/0x%02X) - ABBRUCH", name,
               va, p[0], p[len]);
        hlog_close();
        ExitProcess(7);
    }
    memset(p, 0x90, len);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    HLOG_I("NOP %s @0x%X len=%u", name, va, len);
}

/* Byte-Patch: ersetzt len Bytes an va durch data (z.B. Jcc->JMP). Verifiziert
 * alle Originalbytes vorher (kein Blind-Patch ueber Codegrenzen). */
static void patch_bytes(u32 va, const uint8_t *expect, const uint8_t *data,
                        u32 len, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    u32 i;
    for (i = 0; i < len; i++) {
        if (p[i] != expect[i]) {
            HLOG_E("BYTES %s @0x%X+%u: Byte-Mismatch (0x%02X statt 0x%02X)"
                   " - ABBRUCH",
                   name, va, i, p[i], expect[i]);
            hlog_close();
            ExitProcess(7);
        }
    }
    memcpy(p, data, len);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    HLOG_I("BYTES %s @0x%X len=%u", name, va, len);
}

/* Call-Umlenkung: ruft stattdessen handler (gleiche Signatur), der das
 * Original selbst aufrufen kann (Wrapper-Diagnose). */
static void patch_call(u32 va, void *handler, const char *name)
{
    patch_rel(va, handler, name, 0xE8);
}

/* W153 HUD-LABEL-OPT: Ziel-Etikett weicht RELOAD/MAIN aus (hud_label_avoid=1).
 * Ort (Disasm, s. BERICHT_M2 W153): FUN_0002A500 projiziert je markiertem
 * Gegner den Anker (X,Y) und zeichnet 3 Textzeilen via FUN_0002A140
 * (str,x,y,color,a5,a6; cdecl): Distanz (X+66,Y-46), VT-Name (X+59,Y-65),
 * Kenn-Buchstabe (X+41,Y-67). Glyphen 16x16, Vorschub je Zeichen aus der
 * Spiel-Tabelle (a5=1: Wort [0x2A46E0+([0x354F3C]*0xC7+Zeichen)*2]+1).
 * Der Wrapper haengt an den 3 call-Stellen (Original: E8 + Ziel 0x2A140,
 * geprueft) und ruft das Original direkt (Funktion selbst ungepatcht).
 * Mit hud_label_avoid=0 (und label_diag=0) wird NICHTS gepatcht: der
 * Original-Code bleibt bitgleich. Entscheidung je (Bild, Anker) einmal aus
 * der exakten Union (eigene Breite frisch, Rest aus dem Vorbild-Cache):
 * erst Original (kein Schnitt -> unveraendert), sonst links spiegeln, sonst
 * unter den Anker. DIAG(HUD-LABEL): label_diag=1 loggt LABEL-Zeilen in
 * Screenshot-Bildern (f = Shot-Nr.). */
typedef void(__cdecl *label_text_fn)(const char *s, float x, float y, u32 color,
                                     u32 a5, u32 a6);
static label_text_fn g_label_orig = (label_text_fn)(uintptr_t)0x2A140u;
enum { LBL_DIST = 0, LBL_NAME = 1, LBL_CHAR = 2 };
static const u32 kLblCall[3] = { 0x2AA0Bu, 0x2AA4Du, 0x2AA9Eu };
static const u32 kLblRA[3] = { 0x2AA10u, 0x2AA52u, 0x2AAA3u };
static const float kLblDX[3] = { 66.0f, 59.0f, 41.0f };
static const float kLblDY[3] = { -46.0f, -65.0f, -67.0f };
static const float kLblMaxW[3] = { 80.0f, 128.0f, 16.0f }; /* 5/8/1 Zeichen a 16 */
#define LBL_GLYPH 16.0f
typedef struct { float x0, y0, x1, y1; } lbl_rect_t;
/* Feste HUD-Texte im Etikett-Raum (Farbrechtecke, aus Shots vermessen:
 * Abbildung Shot-px = 1.748 * HUD + (278.7, 28.4), s. BERICHT_M2 W153).
 * RELOAD px 932-1052/294-318, MAIN px 932-995/326-350 (4:3-Shot 1440x1080). */
static const lbl_rect_t kLblFixed[] = {
    { 373.7f, 151.9f, 442.4f, 165.7f }, /* RELOAD */
    { 373.7f, 170.3f, 409.8f, 184.0f }, /* MAIN */
};
static int lbl_hit(float ax0, float ay0, float ax1, float ay1,
                   float bx0, float by0, float bx1, float by1)
{
    return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}
static int lbl_hit_fixed(float x0, float y0, float x1, float y1)
{
    unsigned i;
    for (i = 0; i < sizeof kLblFixed / sizeof kLblFixed[0]; i++) {
        if (kLblFixed[i].x0 < kLblFixed[i].x1 &&
            kLblFixed[i].y0 < kLblFixed[i].y1 &&
            lbl_hit(x0, y0, x1, y1, kLblFixed[i].x0, kLblFixed[i].y0,
                    kLblFixed[i].x1, kLblFixed[i].y1)) {
            return 1;
        }
    }
    return 0;
}
/* Exakte Zeilenbreite nach Spiel-Regel (FUN_0002A140, a5=1). */
static float lbl_text_width(const char *s, u32 a6)
{
    float w = 0.0f;
    unsigned n = 0;
    __try {
        u32 base = *(volatile u32 *)(uintptr_t)0x354F3Cu * 0xC7u;
        unsigned char off = (unsigned char)a6;
        if (!s) { return 0.0f; }
        while (n < 64 && s[n]) {
            unsigned ci = base + (unsigned)(unsigned char)(s[n] + off);
            w += (float)*(volatile uint16_t *)(uintptr_t)(0x2A46E0u + ci * 2u) + 1.0f;
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return (float)(n * 12);
    }
    return w;
}
static unsigned g_lbl_diag_n;
static unsigned g_lbl_f;
static float g_lbl_ax, g_lbl_ay, g_lbl_w[3];
static int g_lbl_have, g_lbl_mode;
extern unsigned g_present_no; /* d3d11_render.c */
static void __cdecl hook_label_text(const char *s, float x, float y, u32 color,
                                    u32 a5, u32 a6)
{
    u32 ra = (u32)(uintptr_t)_ReturnAddress();
    int line = ra == kLblRA[0] ? LBL_DIST : ra == kLblRA[1] ? LBL_NAME : ra == kLblRA[2] ? LBL_CHAR : -1;
    float ax, ay, w;
    int mode = 0; /* 0 = Original, 1 = links, 2 = unten */
    if (line < 0) { /* fremder Aufrufer (sollte nicht vorkommen): durchreichen */
        g_label_orig(s, x, y, color, a5, a6);
        return;
    }
    ax = x - kLblDX[line];
    ay = y - kLblDY[line];
    w = a5 ? lbl_text_width(s, a6) : 136.0f;
    if (g_cfg.hud_label_avoid) {
        /* Exakte Union: eigene Breite frisch, Rest aus dem Cache (Vorbild;
         * max. Breite nur im allerersten Bild). Entscheidung je (Bild, Anker)
         * einmal faellen und einrasten (kein Zeilen-Split, kein Flackern). */
        int i;
        float uw[3];
        if (g_frames != g_lbl_f || ax < g_lbl_ax - 0.5f || ax > g_lbl_ax + 0.5f ||
            ay < g_lbl_ay - 0.5f || ay > g_lbl_ay + 0.5f) {
            int over = 0, over_m = 0, over_b = 0;
            g_lbl_f = g_frames;
            g_lbl_ax = ax;
            g_lbl_ay = ay;
            g_lbl_w[line] = w;
            g_lbl_have |= (1 << line);
            for (i = 0; i < 3; i++) {
                uw[i] = (g_lbl_have & (1 << i)) ? g_lbl_w[i] : kLblMaxW[i];
                if (lbl_hit_fixed(ax + kLblDX[i], ay + kLblDY[i],
                                  ax + kLblDX[i] + uw[i], ay + kLblDY[i] + LBL_GLYPH)) {
                    over = 1;
                }
            }
            g_lbl_mode = 0;
            if (over) {
                for (i = 0; i < 3; i++) { /* links spiegeln? */
                    if (lbl_hit_fixed(ax - kLblDX[i] - uw[i], ay + kLblDY[i],
                                      ax - kLblDX[i], ay + kLblDY[i] + LBL_GLYPH)) {
                        over_m = 1;
                        break;
                    }
                }
                if (!over_m) {
                    g_lbl_mode = 1;
                } else {
                    for (i = 0; i < 3; i++) { /* darunter? */
                        if (lbl_hit_fixed(ax + kLblDX[i], ay - kLblDY[i] - LBL_GLYPH,
                                          ax + kLblDX[i] + uw[i], ay - kLblDY[i])) {
                            over_b = 1;
                            break;
                        }
                    }
                    if (!over_b) {
                        g_lbl_mode = 2;
                    }
                }
            }
        } else {
            g_lbl_w[line] = w;
            g_lbl_have |= (1 << line);
        }
        mode = g_lbl_mode;
        if (mode == 1) {
            x = ax - kLblDX[line] - w;
        } else if (mode == 2) {
            y = ay - kLblDY[line] - LBL_GLYPH;
        }
    }
    if (g_cfg.label_diag && g_lbl_diag_n < 4000u) {
        unsigned p = g_present_no + 1u, sf = (unsigned)g_cfg.screenshot_frame,
                 ev = (unsigned)g_cfg.screenshot_every;
        if ((int)g_cfg.screenshot_frame > 0 && ev > 0 && p >= sf && (p - sf) % ev == 0u) {
            char buf[20];
            unsigned n = 0;
            __try {
                while (n < sizeof buf - 1 && s && s[n]) { buf[n] = s[n]; n++; }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
            buf[n] = 0;
            HLOG_W("LABEL f=%u g=%u line=%d str=\"%s\" x=%.1f y=%.1f w=%.1f ax=%.1f ay=%.1f mode=%d",
                   p, g_frames, line, buf, x, y, w, ax, ay, mode);
            g_lbl_diag_n++;
        }
    }
    g_label_orig(s, x, y, color, a5, a6);
}
static void label_avoid_install(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)kLblCall[i];
        int32_t rel;
        u32 tgt;
        memcpy(&rel, p + 1, 4);
        tgt = kLblCall[i] + 5u + (u32)rel;
        if (p[0] != 0xE8 || tgt != 0x2A140u) { /* Original-Pruefbytes */
            HLOG_E("LABEL: @0x%X Byte-Mismatch (0x%02X -> 0x%X statt E8 -> 0x2A140) - ABBRUCH",
                   kLblCall[i], p[0], tgt);
            hlog_close();
            ExitProcess(7);
        }
    }
    for (i = 0; i < 3; i++) {
        patch_call(kLblCall[i], hook_label_text, i == 0 ? "Etikett-Distanz(W153)" : i == 1 ? "Etikett-Name(W153)" : "Etikett-Zeichen(W153)");
    }
}

/* Pro-State-Drossel fuer RenderStates (neue States immer loggen) */
#define SEEN_MAX 128
static struct {
    u32 state;
    u32 count;
} g_seen_state[SEEN_MAX];

int seen_state(u32 state)
{
    unsigned i;
    for (i = 0; i < SEEN_MAX; i++) {
        if (g_seen_state[i].count && g_seen_state[i].state == state) {
            g_seen_state[i].count++;
            return (g_seen_state[i].count % 5000) == 0;
        }
    }
    for (i = 0; i < SEEN_MAX; i++) {
        if (!g_seen_state[i].count) {
            g_seen_state[i].state = state;
            g_seen_state[i].count = 1;
            return 1;
        }
    }
    return 0;
}

void hooks_install(void)
{
    /* Adressen: analyse/functions.tsv */
    patch_jmp(0x210B20u, hook_Direct3D_CreateDevice, "Direct3D_CreateDevice");
    patch_jmp(0x20DDE0u, hook_D3DDevice_Swap, "D3DDevice_Swap");
    patch_jmp(0x20DF80u, hook_D3DDevice_Clear, "D3DDevice_Clear");
    patch_jmp(0x210AA0u, hook_D3DDevice_Begin, "D3DDevice_Begin");
    patch_jmp(0x210AE0u, hook_D3DDevice_End, "D3DDevice_End");
    patch_jmp(0x20E830u, hook_D3DDevice_SetTexture, "D3DDevice_SetTexture");
    patch_jmp(0x20FB00u, hook_D3DDevice_SetVertexShader, "D3DDevice_SetVertexShader");
    patch_jmp(0x210040u, hook_D3DDevice_SetPixelShader, "D3DDevice_SetPixelShader");
    if (g_cfg.rq_diag >= 2) { /* DIAG(DLGTEXT,W77): Aufrufstellen der Text-Komposition 0x4EF50 */
        extern void hook_comp_13536(void); extern void hook_comp_1383B(void); extern void hook_comp_CD715(void);
        extern void hook_comp_E1ECE(void); extern void hook_comp_100226(void); extern void hook_comp_102649(void);
        extern void hook_comp_1050AB(void);
        patch_call(0x13536u, hook_comp_13536, "Komposition@13536(DIAG)");
        patch_call(0x1383Bu, hook_comp_1383B, "Komposition@1383B(DIAG)");
        patch_jmp(0xCD715u, hook_comp_CD715, "Komposition@CD715(DIAG)");
        patch_call(0xE1ECEu, hook_comp_E1ECE, "Komposition@E1ECE(DIAG)");
        patch_call(0x100226u, hook_comp_100226, "Komposition@100226(DIAG)");
        patch_call(0x102649u, hook_comp_102649, "Komposition@102649(DIAG)");
        patch_call(0x1050ABu, hook_comp_1050AB, "Komposition@1050AB(DIAG)");
    }
    if (g_cfg.hud_label_avoid || g_cfg.label_diag) { /* W153 HUD-LABEL-OPT: Etikett-Hook (nur dann gepatcht, sonst bitgleich) */
        label_avoid_install();
    }
    if (g_cfg.dlg_text_upload) { /* FIX(DLGTEXT,W77): CPU-Text (TextOut) in RT-Texturen hochladen */
        extern unsigned __stdcall hook_txt_lockrect(unsigned, unsigned, unsigned, unsigned);
        extern unsigned __stdcall hook_txt_render(unsigned, unsigned, unsigned, unsigned, unsigned,
                                                  unsigned, unsigned, unsigned, unsigned, unsigned);
        patch_call(0x194D42u, hook_txt_lockrect, "TextOut-LockRect(DLGTEXT)");
        patch_call(0x194D65u, hook_txt_render, "TextOut-Render(DLGTEXT)");
    }
    /* RENDER-QA (W77): PS-Konstanten mitschreiben - FIX(PSCONST,W84): nicht mehr
     * hier patchen (wurde vom spaeteren hook_D3D_PSConst @0x210220 ueberschrieben);
     * hook_D3D_PSConst reicht bei rq_diag/shading an den Spiegel weiter. */
    patch_jmp(0x20E640u, hook_D3DDevice_GetBackBuffer2, "D3DDevice_GetBackBuffer2");
    patch_jmp(0x20E370u, hook_D3DDevice_SetRenderTarget, "D3DDevice_SetRenderTarget");
    patch_jmp(0x20BAD0u, hook_D3DDevice_SetRenderState_Simple,
              "D3DDevice_SetRenderState_Simple(fastcall)");
    patch_jmp(0x2109B0u, hook_D3DDevice_SetVertexData2f, "D3DDevice_SetVertexData2f");
    patch_jmp(0x2109F0u, hook_D3DDevice_SetVertexData4f, "D3DDevice_SetVertexData4f");
    patch_jmp(0x210A50u, hook_D3DDevice_SetVertexDataColor,
              "D3DDevice_SetVertexDataColor");
    patch_jmp(0x20E690u, hook_D3DDevice_GetRenderTarget2, "D3DDevice_GetRenderTarget2");
    patch_jmp(0x20E6D0u, hook_D3DDevice_SetViewport, "D3DDevice_SetViewport");
    if (g_cfg.getstate_fix) { /* FIX(COCKPIT,W60) */
        patch_jmp(0x20F4C0u, hook_D3DDevice_GetVertexShader, "D3DDevice_GetVertexShader");
        patch_jmp(0x20FCB0u, hook_D3DDevice_GetPixelShader, "D3DDevice_GetPixelShader");
        patch_jmp(0x20F710u, hook_D3DDevice_GetVertexShaderConstant,
                  "D3DDevice_GetVertexShaderConstant");
    }
    patch_jmp(0x20E390u, hook_D3DDevice_SetTransform, "D3DDevice_SetTransform");
    patch_jmp(0x210F70u, hook_D3DResource_Release, "D3DResource_Release");
    patch_jmp(0x20BB00u, hook_D3D_SetRenderStateNotInline,
              "SetRenderStateNotInline");
    patch_jmp(0x20BE30u, hook_D3D_SetRenderState_EdgeAntiAlias,
              "SetRenderState_EdgeAntiAlias");
    patch_jmp(0x20BE70u, hook_D3D_SetRenderState_ShadowFunc,
              "SetRenderState_ShadowFunc");
    patch_jmp(0x20BEB0u, hook_D3D_SetRenderState_FogColor,
              "SetRenderState_FogColor");
    patch_jmp(0x20BF00u, hook_D3D_SetRenderState_CullMode,
              "SetRenderState_CullMode");
    patch_jmp(0x20BFF0u, hook_D3D_SetRenderState_TextureFactor,
              "SetRenderState_TextureFactor");
    patch_jmp(0x20C050u, hook_D3D_SetRenderState_LineWidth,
              "SetRenderState_LineWidth");
    patch_jmp(0x20C0C0u, hook_D3D_SetRenderState_Dxt1NoiseEnable,
              "SetRenderState_Dxt1NoiseEnable");
    patch_jmp(0x20C390u, hook_D3D_SetTextureState_TexCoordIndex,
              "SetTextureState_TexCoordIndex");
    patch_jmp(0x20C4A0u, hook_D3D_SetTextureState_BumpEnv,
              "SetTextureState_BumpEnv");
    patch_jmp(0x20C510u, hook_D3D_SetTextureState_BorderColor,
              "SetTextureState_BorderColor");
    patch_jmp(0x20C550u, hook_D3D_SetTextureState_ColorKeyColor,
              "SetTextureState_ColorKeyColor");
    patch_jmp(0x20C590u, hook_D3D_SetTextureStageStateNotInline,
              "SetTextureStageStateNotInline");
    patch_jmp(0x20CF40u, hook_D3D_SetRenderState_ZEnable,
              "SetRenderState_ZEnable");
    patch_jmp(0x20CFD0u, hook_D3D_SetRenderState_StencilEnable,
              "SetRenderState_StencilEnable");
    patch_jmp(0x20D060u, hook_D3D_SetRenderState_StencilFail,
              "SetRenderState_StencilFail");
    patch_jmp(0x20D100u, hook_D3D_SetRenderState_OcclusionCullEnable,
              "SetRenderState_OcclusionCullEnable");
    patch_jmp(0x229AB9u, hook_GPU_229AB9, "GPU-Bringup(idle)");
    patch_jmp(0x229CC4u, hook_GPU_readywait, "GPU-ReadyWait(ready)");
    patch_jmp(0x229C97u, hook_GPU_readywait2, "GPU-ReadyWait2(ready)");
    if (g_cfg.audio) {
        ds_audio_install(); /* A7: HLE statt Fake/Stumm */
    } else {
        patch_jmp(0x221658u, hook_DS_CreateSoundStream,
                  "DS-CreateSoundStream(Fake)");
        patch_jmp(0x21E7C5u, hook_DS_BufferStop, "DS-BufferStop(S_OK)");
        patch_jmp(0x21E86Du, hook_DS_BufferGetStatus,
                  "DS-BufferGetStatus(S_OK)");
    }
    patch_jmp(0x106E30u, hook_DS_Wrapper106E30, "DS-Wrapper106E30(S_OK)");
    patch_jmp(0x1066C0u, hook_DS_Wrapper1066C0, "DS-Wrapper1066C0(S_OK)");
    patch_jmp(0x106E00u, hook_DS_Wrapper106E00, "DS-Wrapper106E00(S_OK)");
    patch_jmp(0x106DE0u, hook_DS_Wrapper106DE0, "DS-Wrapper106DE0(S_OK)");
    patch_jmp(0x220EAEu, hook_DS_SetI3DL2Listener, "DS-SetI3DL2Listener(S_OK)");
    patch_jmp(0x21E58Cu, hook_DS_21E58C, "DS-21E58C(Null-Guard)");
    patch_jmp(0x18CFC0u, hook_XapiSelectCachePartition,
              "XAPI-SelectCachePartition(Cache=1)");
    patch_jmp(0x192363u, hook_XLaunchNewImageA, "XLAUNCH-NewImageA(IGNORE)");
    patch_jmp(0x18D1EBu, hook_XMountUtilityDrive, "XAPI-MountUtilityDrive");
    patch_jmp(0x18D97Fu, hook_ReadFile, "XAPI-ReadFile(sync)");
    patch_jmp(0x191250u, hook_PerfFrequency, "XAPI-PerfFreq(TSC)");
    if (g_cfg.bink_pause) { patch_jmp(0x19123Fu, hook_PerfCounter, "XAPI-PerfCounter(Pause)"); }
    patch_jmp(0x108720u, hook_108720, "108720(NULL-Check)");
    patch_jmp(0x1086C0u, hook_1086C0, "1086C0(No-Op)");
    patch_jmp(0x108740u, hook_108740, "108740(No-Op)");
    patch_jmp(0x107E30u, hook_107E30, "107E30(Slot-Fake)");
    patch_jmp(0x109770u, hook_109770, "109770(Null-Guard)");
    patch_jmp(0x106560u, hook_106560, "106560(No-Op)");
    patch_jmp(0x19273Fu, hook_XapiMapLetterToDirectory,
              "XAPI-MapLetter(No-Op)");
    /* Zweit-Einstieg hinter Prolog (Loop umgeht 0x19273F): gleiche No-Op. */
    patch_jmp(0x192744u, hook_XapiMapLetterToDirectory,
              "XAPI-MapLetter2(No-Op)");
    /* FUN_000C7C90: JNZ->JMP @0xC7CC0 (Retry-Loop umgehen, Handle gueltig). */
    {
        static const uint8_t jnz_exp[2] = { 0x75, 0x07 };
        static const uint8_t jmp_new[2] = { 0xEB, 0x07 };
        patch_bytes(0xC7CC0u, jnz_exp, jmp_new, 2, "C7C90-NoRetry");
    }
    /* FUN_000C7C90: JNZ->NOP @0xC7D43 (ReadFile-Fehler umgehen, immer PENDING). */
    {
        static const uint8_t jnz2_exp[2] = { 0x75, 0x40 };
        static const uint8_t nop_new[2] = { 0x90, 0x90 };
        patch_bytes(0xC7D43u, jnz2_exp, nop_new, 2, "C7C90-NoReadErr");
    }
    /* FUN_00013D70: JZ->NOP @0x13E37 (BGM-Retry umgehen, immer weiter). */
    {
        static const uint8_t jz_exp[2] = { 0x74, 0xC7 };
        static const uint8_t nop2_new[2] = { 0x90, 0x90 };
        patch_bytes(0x13E37u, jz_exp, nop2_new, 2, "13D70-NoRetry");
    }
    /* FUN_000C7E10: MOV EDI,0x80004005 -> XOR EDI,EDI @0xC7ECF (E_FAIL-
     * Poisoning: EDI bleibt ueber Retries negativ, der PENDING-Erfolgspfad
     * (JGE @0xC7EAB) wird nie genommen -> 100K/s Alloc/Free-Spin. HLE-
     * Reads gelingen synchron, E_FAIL ist hier bedeutungslos). */
    {
        static const uint8_t efail_exp[5] = { 0xBF, 0x05, 0x40, 0x00, 0x80 };
        static const uint8_t edi0_new[5] = { 0x33, 0xFF, 0x90, 0x90, 0x90 };
        patch_bytes(0xC7ECFu, efail_exp, edi0_new, 5, "C7E10-NoFail");
    }
    /* 0x2050C0/0x2050D4/0x2050E5: FS-Stellen der unerkannten Funktion liegen
     * in fs_emu.c (g_sites), nicht hier (Doppel-Patch wuerde abbrechen). */

    /* Globale Audio-Struktur (this=0x6C1740): Feld +0x184 (0x6C18C4) wird nie
     * initialisiert (kein echtes Audio) -> Fake-Objekt (NULLEN), damit Caller
     * ([ptr+0x64]==0 -> "nicht gefunden") statt zu faulten weiterlaufen. */
    *(u32 *)(uintptr_t)0x6C18C4u = (u32)(uintptr_t)g_fake_obj;
    HLOG_I("INIT [0x6C18C4] = Fake-Objekt (Audio-Struktur)");
    /* NULLCALL (W13): HRTF-Funktionszeiger vorbelegen (wie
     * DirectSoundUseFullHRTF -> Setter 0x21E067). DirectSoundCreate scheitert
     * ohne MCPX-HW -> Spiel ueberspringt UseFullHRTF (jl @0x10898F) ->
     * [0x23F698] bleibt 0 -> Voice-Commit (call [0x23F698] @0x2258F7)
     * springt nach 0 (~200x/Mission). Ziele = echte XDK-Funktionen,
     * ABI-vermessen (ret 4 / ret 8, wie die Aufrufstellen). */
    if (g_cfg.hrtf_ptr) {
        *(u32 *)(uintptr_t)0x23F698u = 0x222FAAu; /* CFullHrtfSource_GetHrtfFilterPair */
        *(u32 *)(uintptr_t)0x23F694u = 0x22324Fu; /* HRTF-Commit (FUN_002230e1+0x6E) */
        HLOG_I("INIT HRTF-Zeiger: [23F698]=0x222FAA [23F694]=0x22324F");
    }
    /* D3D-Device-Struktur ([0x21A8A0], min. 0x255C Bytes): CreateDevice ist
     * gehookt (kein echtes D3D) -> Fake-Struktur (NULLEN), damit
     * BlockUntilVerticalBlank ([EAX+0x2558]) nicht faultet. */
    *(u32 *)(uintptr_t)0x21A8A0u = (u32)(uintptr_t)g_fake_d3d;
    /* Pushbuffer-Zeiger (+0x30=put-Wert, +0x34->get-Zelle): get==put meldet
     * "keine GPU-Arbeit", D3D_BlockOnTime nimmt den Idle-Frühpfad (JNC). */
    g_fake_pushbuf[0] = 0x1000u;
    *(u32 *)(g_fake_d3d + 0x30) = 0x1000u;
    *(u32 *)(g_fake_d3d + 0x34) = (u32)(uintptr_t)&g_fake_pushbuf[0];
    /* Command-Puffer (+0=put, +4=limit): echter Puffer (8 MB). Der D3D-Code
     * prueft put<limit ("Platz?") und schreibt Kommandos an put. Mit 0/0
     * naehme er den Nachforderungs-Pfad (CALL 0x211990 -> Device+0x23BC =
     * NULL -> Crash 0x21185C). Daten ungenutzt (M1 rendert nicht). */
    *(u32 *)(g_fake_d3d + 0x0) = (u32)(uintptr_t)g_d3d_pool;
    *(u32 *)(g_fake_d3d + 0x4) =
        (u32)(uintptr_t)(g_d3d_pool + sizeof g_d3d_pool);
    /* Unter-Objekte (NULL -> Fake-Block, 0-Init = "nicht vorhanden"-Pfad):
     * +0x380 (0x20FA03: Flag-Test [EAX+4]&2 -> JZ-Alternative). */
    *(u32 *)(g_fake_d3d + 0x380) = (u32)(uintptr_t)g_fake_obj;
    HLOG_I("INIT [0x21A8A0] = Fake-D3D (0x3000 Bytes)");
    HLOG_I("INIT g_fake_d3d=0x%X g_d3d_pool=0x%X",
           (u32)(uintptr_t)g_fake_d3d, (u32)(uintptr_t)g_d3d_pool);
    HLOG_I("INIT fakes: dev=0x%X surf=0x%X stream=0x%X obj=0x%X",
           (u32)(uintptr_t)g_fake_device, (u32)(uintptr_t)g_fake_surface,
           (u32)(uintptr_t)g_fake_stream, (u32)(uintptr_t)g_fake_obj);
    patch_jmp(0x21E14Fu, hook_GPU_regread0, "GPU-RegRead(0)");
    patch_jmp(0x223E2Du, hook_GPU_223E2D, "GPU-Config(0)");
    /* FUN_002258xx: bewachter GPU-Block ([ESI+0x64]>0 -> Poll 0xFE820010 +
     * 5+ GPU-Writes, bis 0x2258EA). M1: JBE->JMP (Block immer ueberspringen,
     * kein NV2A-MMIO). JBE @0x225818: 0F 86 CC 00 00 00 -> E9 CD 00 00 00 90. */
    {
        static const uint8_t jbe_exp[6] = { 0x0F, 0x86, 0xCC, 0x00, 0x00,
                                            0x00 };
        static const uint8_t jmp_new[6] = { 0xE9, 0xCD, 0x00, 0x00, 0x00,
                                            0x90 };
        patch_bytes(0x225818u, jbe_exp, jmp_new, 6, "GPU-Block-Skip(225818)");
    }
    /* GPU-Poll-Loops (0xFE820010-Reads, 14 Kopien im XBE, per Muster
     * 8B 15 10 00 82 FE gefunden + Kontext verifiziert): Poll sofort
     * erfolgreich (kein NV2A-MMIO). ECX-Typ (CMP EDX,ECX): LEA
     * EDX,[ECX*4]; EAX-Typ (CMP EDX,EAX): MOV EDX,EAX + SHL 2.
     * Typ B (0x2250C4, AND/CMP-Kontext) NICHT gepatcht (unbekannt). */
    {
        static const u32 poll_ecx[] = { 0x224816u, 0x224F22u, 0x2255F7u,
                                        0x225956u, 0x225ABDu, 0x225C41u,
                                        0x225E6Cu, 0x226AFCu, 0x227126u,
                                        0x2271D9u };
        static const u32 poll_eax[] = { 0x225237u, 0x225A03u, 0x225FC4u,
                                        0x226306u };
        static const uint8_t p_exp[6] = { 0x8B, 0x15, 0x10, 0x00, 0x82,
                                          0xFE };
        static const uint8_t p_ecx[6] = { 0x8D, 0x14, 0x89, 0x90, 0x90,
                                          0x90 };
        static const uint8_t p_eax[6] = { 0x8B, 0xD0, 0xC1, 0xE2, 0x02,
                                          0x90 };
        u32 i;
        for (i = 0; i < sizeof poll_ecx / sizeof poll_ecx[0]; i++) {
            patch_bytes(poll_ecx[i], p_exp, p_ecx, 6, "GPU-Poll-ECX");
        }
        for (i = 0; i < sizeof poll_eax / sizeof poll_eax[0]; i++) {
            patch_bytes(poll_eax[i], p_exp, p_eax, 6, "GPU-Poll-EAX");
        }
    }
#include "gpu_polls.inc" /* Generierte A1-Poll-Fixes (s.o.) */
    /* FUN_00224CB8: GPU-Status-Reads ([reg+0xFE800000], Werte nur in
     * Locals gesichert, keine Poll-Verzweigung). M1: -> 0 (XOR+NOPs). */
    {
        static const uint8_t r1_exp[6] = { 0x8B, 0xBA, 0x00, 0x00, 0x80,
                                           0xFE };
        static const uint8_t r1_new[6] = { 0x33, 0xFF, 0x90, 0x90, 0x90,
                                           0x90 };
        static const uint8_t r2_exp[6] = { 0x8B, 0xBF, 0x00, 0x00, 0x80,
                                           0xFE };
        static const uint8_t r3_exp[6] = { 0x8B, 0x80, 0x00, 0x00, 0x80,
                                           0xFE };
        static const uint8_t r3_new[6] = { 0x33, 0xC0, 0x90, 0x90, 0x90,
                                           0x90 };
        patch_bytes(0x224CCBu, r1_exp, r1_new, 6, "GPU-Rd0(224CCB)");
        patch_bytes(0x224CE0u, r2_exp, r1_new, 6, "GPU-Rd0(224CE0)");
        patch_bytes(0x224CE6u, r3_exp, r3_new, 6, "GPU-Rd0(224CE6)");
    }
    /* FUN_00195Fx: MMIO-Burst-Fill (STOSD.REP nach 0xFE2B65B8, Framebuffer/
     * GPU-Apertur). M1: NOP (kein NV2A, EDI wird pro Loop neu berechnet). */
    /* FIX(DLGTEXT3,W85): 0x196033 ist die Glyphen-Fuellung des TextOut-
     * Rasterers 0x195E1C (einzige Kette 0x4F121 -> 0x194D25 -> 0x194D65 ->
     * 0x1963D2). Der Zielzeiger war nur deshalb eine GPU-Apertur, weil Lock
     * 0xF0000000|Data lieferte; mit dlg_text_upload=1 ersetzt
     * hook_txt_lockrect ihn, hook_txt_render ueberspringt ungueltige Ziele. */
    if (!(g_cfg.dlg_text_upload && g_cfg.dlg_text_raster)) {
        patch_nop(0x196033u, 0x02u, "MMIO-STOSD(196033)", 0xF3, 0x8B);
    } else {
        HLOG_I("DLGTEXT3: rep stosd @0x196033 bleibt (Glyphen-Fuellung, dlg_text_raster=1)");
    }
    /* GPU-Config-Writes (reine MMIO, keine Ruecklesung, per Muster
     * gefunden: MOV [0xFE8202F8/0x304/0x37C],EAX/EDX). M1: NOP (kein NV2A). */
    patch_nop(0x224831u, 0x05u, "GPU-Write(224831)", 0xA3, 0x8B);
    patch_nop(0x224840u, 0x05u, "GPU-Write(224840)", 0xA3, 0x0F);
    patch_nop(0x22515Fu, 0x05u, "GPU-Write(22515F)", 0xA3, 0x8B);
    patch_nop(0x225612u, 0x05u, "GPU-Write(225612)", 0xA3, 0x8B);
    patch_nop(0x225632u, 0x05u, "GPU-Write(225632)", 0xA3, 0x8B);
    patch_nop(0x225971u, 0x05u, "GPU-Write(225971)", 0xA3, 0x8B);
    patch_nop(0x225991u, 0x05u, "GPU-Write(225991)", 0xA3, 0x8B);
    patch_nop(0x225A1Cu, 0x06u, "GPU-Write(225A1C)", 0x89, 0x8B);
    patch_nop(0x225AD6u, 0x06u, "GPU-Write(225AD6)", 0x89, 0x8B);
    patch_nop(0x225ADFu, 0x06u, "GPU-Write(225ADF)", 0x89, 0x0F);
    patch_nop(0x225E87u, 0x05u, "GPU-Write(225E87)", 0xA3, 0x8B);
    patch_nop(0x225EA7u, 0x05u, "GPU-Write(225EA7)", 0xA3, 0x8B);
    patch_nop(0x225FF5u, 0x06u, "GPU-Write(225FF5)", 0x89, 0xC7);
    patch_nop(0x226B15u, 0x06u, "GPU-Write(226B15)", 0x89, 0xC7);
    patch_nop(0x22721Fu, 0x05u, "GPU-Write(22721F)", 0xA3, 0x75);
#include "gpu_nops.inc" /* 128 generierte GPU-Write-NOPs (s.o.) */

    /* FUN_00229D28: indizierter GPU-Write (0x229D31-42, EAX=1 bleibt!). */
    patch_nop(0x229D31u, 0x11u, "GPU-Write(229D28)", 0x66, 0xC2);
    /* FUN_0022A3F7: 2er-Loop mit GPU-Byte-Write ([ECX+0xFEC0010B]=2),
     * 0x22A428-3F (reine HW-Sequenz, ECX-Tabellen-Reads ohne Wirkung). */
    patch_nop(0x22A428u, 0x17u, "GPU-Loop(22A3F7)", 0x33, 0xF6);
    patch_jmp(0x2294F2u, hook_GPU_rmw, "GPU-RMW(FE801004)");
    patch_jmp(0x229529u, hook_GPU_rmw, "GPU-RMW(FE801510)");
    patch_jmp(0x229560u, hook_GPU_rmw, "GPU-RMW(FE801100)");
    patch_jmp(0x2295BBu, hook_GPU_rmw, "GPU-RMW(FE802000)");
    patch_jmp(0x20EB80u, hook_GetDisplayFieldStatus,
              "D3D-GetDisplayFieldStatus(counter)");
    patch_jmp(0x20E520u, hook_BlockUntilVerticalBlank,
              "D3D-BlockUntilVerticalBlank(vblank_hz)");
    patch_jmp(0x20EBC0u, hook_D3D_SetTileNoWait, "D3D-SetTileNoWait(no-op)");
    patch_jmp(0x2111C0u, hook_D3D_VBLock2, "D3D-VBLock2([this+4])");
    patch_jmp(0x212820u, hook_Get2DSurfaceDesc, "D3D-Get2DSurfaceDesc(640x480)");
    /* XInput (M2b): SBC-Controller -> Tastatur. */
    patch_jmp(0x256784u, hook_XInputOpen, "XIN-Open(fake)");
    patch_jmp(0x2567DAu, hook_XInputClose, "XIN-Close(no-op)");
    patch_jmp(0x2569C3u, hook_XInputGetState, "XIN-GetState(kb)");
    patch_jmp(0x256A32u, hook_XInputSetState, "XIN-SetState(no-op)");
    patch_jmp(0x25649Eu, hook_XGetDeviceChanges, "XIN-GetDeviceChanges");
    if (g_cfg.poll_census) {
        poll_census_install(); /* W59 DIAG: 48CF0-Aufrufer-Zensus */
    }
    patch_jmp(0x190D21u, krnl_GetExitCodeThread, "XAPI-GetExitCodeThread");
    /* Shader-Konstanten DEAKTIVIERT (Diagnose: Stack-Corruption?). */
    patch_jmp(0x20F4E0u, hook_D3D_VSConst1, "D3D-VSConst1(cdecl)");
    patch_jmp(0x20F540u, hook_D3D_VSConst4, "D3D-VSConst4");
    patch_jmp(0x20F5F0u, hook_D3D_VSConstFast, "D3D-VSConstFast(ret4)");
    patch_jmp(0x210220u, hook_D3D_PSConst, "D3D-PSConst(cdecl)");
    patch_jmp(0x211810u, hook_D3D_MakeSpace, "D3D-MakeSpace");
    patch_jmp(0x211130u, hook_D3D_CreateIB2, "D3D-CreateIB2");
    patch_jmp(0x211170u, hook_D3D_CreateVB2, "D3D-CreateVB2");
    patch_jmp(0x210540u, hook_D3D_DrawUP, "D3D-DrawUP");
    patch_jmp(0x20E4C0u, hook_D3D_QueryStatus, "D3D-QueryStatus(ready)");
    {   /* FIX(SHADE-M2,W117) vis_query: echte Sichtbarkeitstests (Sonnenblendung) */
        extern int sb_visq_active(void);
        if (sb_visq_active()) {
            patch_jmp(0x20EA80u, hook_D3D_BeginVisTest, "D3D-BeginVisibilityTest(query)");
            patch_jmp(0x20EB20u, hook_D3D_EndVisTest, "D3D-EndVisibilityTest(query)");
            g_visq_on = 1;
        }
    }
    patch_jmp(0x20F750u, hook_D3D_SetStream, "D3D-SetStream");
    patch_jmp(0x20E9B0u, hook_D3D_SetIndices, "D3D-SetIndices");
    patch_jmp(0x210740u, hook_D3D_DrawIdx, "D3D-DrawIdx");
    patch_jmp(0x2106A0u, hook_D3D_DrawVertices, "D3D-DrawVertices");
    patch_jmp(0x211530u, hook_D3D_KickOff, "D3D-KickOff(no-op)");
    patch_jmp(0x210C20u, hook_D3DTexture_LockRect, "D3D-TexLockRect");
    patch_jmp(0x2116D0u, hook_D3D_BlockOnTime, "D3D-BlockOnTime(no-op)");
    /* FUN_00229BB5-Tail: 11x NV2A-Write (0xFE802054-74, 0xFE83FFFC,
     * 0xFE85FFFC), verifiziert reine Write-Sequenz 0x229BED-0x229C33
     * (B8.. / danach 5E=POP ESI). */
    patch_nop(0x229BEDu, 0x46u, "GPU-Writes(229BB5-Tail)", 0xB8, 0x5E);
    /* FUN_0022986E: 5 NV2A-Write-Bloecke; XOR/PUSH/MOV-Logik + CALLs bleiben.
     * (PUSH EDX @0x22990F = CALL-Arg! / MOV ECX,ESI = THIS! bleiben!) */
    patch_nop(0x22986Eu, 0x14u, "GPU-Writes(22986E-B1)", 0xC7, 0x33);
    patch_nop(0x229884u, 0x16u, "GPU-Writes(22986E-B2)", 0x89, 0x56);
    patch_nop(0x22989Bu, 0x10u, "GPU-Writes(22986E-B3)", 0xC7, 0x8B);
    patch_nop(0x2298B2u, 0x5Du, "GPU-Writes(22986E-B4a)", 0xC7, 0x52);
    patch_nop(0x229912u, 0x05u, "GPU-Writes(22986E-B4b)", 0xA3, 0xE8);
    /* FUN_00229710: Flag-Logik (0x229710-2C) bleibt, 12 NV2A-Writes
     * (0x229731-89, inkl. toter ECX/EAX-Rechnung) stilllegen. */
    patch_nop(0x229731u, 0x58u, "GPU-Writes(229710)", 0xA3, 0xC3);
    /* FUN_002290BC: Poll (0x2290BC-C9) + 14 NV2A-Writes (0x2290ED-8B)
     * stilllegen; Game-Mem-Loop (0x2290C9-ED) laeuft weiter. */
    patch_nop(0x2290BCu, 0x0Du, "GPU-Poll(2290BC)", 0xA1, 0x33);
    patch_nop(0x2290EDu, 0x9Eu, "GPU-Writes(2290BC)", 0xA1, 0x5E);
    patch_jmp(0x192B2Du, hook_XapiMediaCheck, "XapiMediaCheck(Bypass)");
    patch_call(0x190E2Fu, hook_XapiNotifyWrap, "XapiNotifyWrap(prefill)");
    HLOG_I("HOOKS: 42 installiert + 11 NOP");
}

unsigned hooks_frame_count(void)
{
    return g_frames;
}
