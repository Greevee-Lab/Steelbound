#pragma once
/* render_post.h - W463 RCORE: Nachbearbeitungskette des gezeigten Bildes (Post-FX) an EINER Stelle.
 *
 * 1. Pass-Tabelle (render_post.c, kPOST): Reihenfolge und Aktiv-Bedingung aller Bild-Passes, die
 *    nach dem fertigen Spielbild (bzw. nach Blit/Verify der Zwischenbild-Wiedergabe) auf den
 *    Backbuffer wirken. Beide Present-Wege in d3d11_render.c rufen sb_post_chain():
 *      - SB_POST_SITE_GAME: Spiel-Present ohne FPS-INTERP (Ziel g_rtv),
 *      - SB_POST_SITE_SWAP: gezeigtes Bild mit FPS-INTERP (sb_render_blit, Ziel Swapchain-BB),
 *        mit den GPU-COST-Marken (W146) zwischen den Passes.
 *    Ein neuer Pass (z. B. LoC-Bloom/-Glow im SB-Stil, W459) = eine Zeile in kPOST plus Modul mit
 *    on()/apply(); Doku docs/RENDER_ARCH.md.
 * 2. Gemeinsamer Vollbild-Kopierpass (sb_post_fs_t): Kopie des Ziels als Textur t0, ein Dreieck mit
 *    eigenem VS/PS, Konstanten b0, Sampler s0 (linear, CLAMP), ohne Tiefe/Blend, Zustand gesichert und
 *    wiederhergestellt. Genutzt von Vaporwave (render_xfx.c) und Panini/Breite (render_cpanini.c);
 *    vorher zwei identische Kopien (W463). */
#include <d3d11.h>

enum { SB_POST_SITE_GAME = 0, SB_POST_SITE_SWAP = 1 };

/* Ziel eines Passes (unter ctx_lock geholt): rtv + Ressource dahinter. */
typedef struct {
    ID3D11RenderTargetView *rtv;
    ID3D11Resource *res;
} sb_post_tgt_t;
typedef int (*sb_post_tgt_get_fn)(sb_post_tgt_t *t);   /* 1 = Ziel gueltig (rtv darf trotzdem NULL sein) */
typedef void (*sb_post_tgt_put_fn)(sb_post_tgt_t *t);  /* Gegenstueck (Release), darf NULL sein */

/* Kette ausfuehren. gate = 0: kein Pass laeuft (Marken trotzdem, wie bisher je Stelle), marks = 1:
 * GPU-COST-Marken nach den Passes laut Tabelle. Jeder aktive Pass: ctx_lock, Ziel holen, anwenden, freigeben. */
void sb_post_chain(int site, int gate, int marks, sb_post_tgt_get_fn get, sb_post_tgt_put_fn put);

/* ---------- gemeinsamer Vollbild-Kopierpass ---------- */
typedef struct {
    const char *vs_src, *ps_src; /* HLSL (vs_4_0/ps_4_0) */
    unsigned cb_bytes;           /* Konstantenpuffer b0 (Vielfaches von 16) */
    const char *fail_log;        /* Logzeile, wenn die Anlage scheitert */
    int state;                   /* 0 nie versucht, 1 bereit, -1 gescheitert */
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *cb;
    ID3D11SamplerState *smp;
    ID3D11RasterizerState *rs;
    ID3D11DepthStencilState *ds;
    ID3D11Texture2D *tex;        /* Kopie des Ziels (t0) */
    ID3D11ShaderResourceView *srv;
    unsigned tw, th;
    DXGI_FORMAT fmt;
} sb_post_fs_t;

int sb_post_fs_init(sb_post_fs_t *p);                       /* einmalig; 1 = bereit */
int sb_post_fs_ensure_copy(sb_post_fs_t *p, ID3D11Resource *res); /* Kopie-Textur passend zu res */
/* Unter ctx_lock nach Map/Unmap von p->cb: Zustand sichern, Dreieck in rtv (Viewport vpw x vph), Zustand zurueck. */
void sb_post_fs_draw(sb_post_fs_t *p, ID3D11RenderTargetView *rtv, float vpw, float vph);
