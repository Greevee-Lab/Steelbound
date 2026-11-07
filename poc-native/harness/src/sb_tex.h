#pragma once
/* sb_tex.h - Xbox-Textur-Cache (D3DResource -> D3D11-SRV, M2b).
 * Nur Stage 0 (Titel/Menue). Formate: DXT1/3/5 (linear angenommen),
 * LIN_A8R8G8B8 nur mit USIZE/VSIZE (sonst SKIP+WARN). */
#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

void sb_tex_init(struct ID3D11Device *dev);
void sb_tex_set(unsigned stage, unsigned xptr);
/* Aktuelle SRV (NULL = keine/unbekannt -> PS nutzt nur Vertex-Farbe). */
struct ID3D11ShaderResourceView *sb_tex_get(void);
void sb_tex_dims(unsigned *w, unsigned *h); /* M2c: Stage0-Dims */
unsigned sb_tex_xptr(void); /* Stage0-xptr (Diagnose) */
/* TEX3D-Diagnose: NULL-Grund (0=SRV ok), Stage-Belegung,
 * RT-Treffer fuer Stage0. */
unsigned sb_tex_why(void);
void sb_tex_stages(unsigned xp[4], unsigned n[4]);
int sb_tex_rtt_hit(void);
/* DIAG(MOVIE): Xbox-FORMAT-DWORD + Linear-Klasse der Stage0-Textur. */
void sb_tex_fmt(unsigned *fmt, int *is_linear);
/* Spiel hat die Textur gesperrt (LockRect) -> beim naechsten Get neu
 * hochladen (Bink schreibt jeden Movie-Frame per Lock). */
void sb_tex_mark_dirty(unsigned xptr);
/* W118 REM-OVERLAY: alle Eintraege neu hochladen (HD live umgeschaltet). */
void sb_tex_relive_all(void);
/* W193 PAUSE-LIVE: relive-Eintraege in der Pause neu laden (max je Aufruf; Rueckgabe offen),
 * alte -> neue SRV fuer die Wiedergabe, Tabelle am Pausenende leeren. */
int sb_tex_pause_relive(int max, unsigned *nup, unsigned *nfail);
struct ID3D11ShaderResourceView *sb_tex_pause_map(struct ID3D11ShaderResourceView *s);
int sb_tex_pause_nmap(void);
void sb_tex_pause_clear(void);
/* TEXLIFE: Eintrag entfernen (Release rc=0 -> Speicher frei, kein Re-Upload). */
void sb_tex_evict(unsigned xptr);
void sb_tex_lockwatch_note(unsigned xptr);   /* W98: LockRect-Textur beobachten */
void sb_tex_lockwatch_forget(unsigned xptr); /* W98: Release rc=0 */
/* W2B-FIX: Linear-Dims direkt aus Header (Norm-Fallback ohne Cache). */
int sb_tex_hdr_dims(unsigned xptr, unsigned *w, unsigned *h);
void sb_tex_hdr_info(unsigned xptr, unsigned *c0, unsigned *data, unsigned *fmt, unsigned *size);

#ifdef __cplusplus
}
#endif
/* RENDER-QA (W77): SRV/Masse/Linear-Klasse der Stufe 0..3 (Shading-Pfad). */
struct ID3D11ShaderResourceView *sb_tex_get_stage(unsigned stage, unsigned *w, unsigned *h, int *lin);
int sb_tex_lockbits(unsigned xptr, unsigned *pitch, unsigned *bits, unsigned flags);
