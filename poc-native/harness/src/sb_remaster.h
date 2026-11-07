#pragma once
/* sb_remaster.h - REMASTER-Geruest (W103): Inhalts-Hash je Textur,
 * Textur-Dump (PNG + index.json, DIAG tex_dump=1) und Ersatz-Lader
 * (remaster=1 + hd_textures=1: remaster/textures/<hash>.dds statt des
 * Originals). Alles hinter Schaltern; mit tex_dump=0 und remaster=0 ist
 * jeder Einstieg ein sofortiges return (Original bitgenau). */
#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11Device;
struct ID3D11ShaderResourceView;

void sb_rem_init(struct ID3D11Device *dev);
/* W151 EFFECT-PACK: Effekt-Parameter aktiver Effect-Packs auf die cfg-Werte
 * legen (Paket gewinnt ueber cfg, PACK_FORMAT.md Abschnitt 9.3). Startpfad,
 * einmal nach hcfg_load; ohne Effect-Pack keine Aenderung (bitgleich). */
void sb_rem_fx_start(void);
/* W173 LAUNCHER-P4: 1 = Schluessel wird von einem aktiven Effect Pack gesetzt
 * (Effect-Pack-Ausnahme der Preset-Erkennung, PACK_FORMAT 9.3). */
int sb_rem_fx_locked(const char *key);
/* 1 = Dump oder Ersatz aktiv (sonst kostet nichts). */
int sb_rem_active(void);
/* Aus tex_upload nach erfolgreichem Erzeugen der Original-SRV.
 * kind: 1 = DXT (pix = Bloecke, pitch = Bytes je Blockzeile), 2 = linear
 * (pix = Zeilen mit pitch), 3 = 0x06 ent-swizzelt (pitch = w*4).
 * fmt = Xbox-FORMAT-DWORD. Liefert die zu bindende SRV: das Original oder
 * (Ersatz gefunden) die HD-SRV; das Original wird dann freigegeben. */
struct ID3D11ShaderResourceView *sb_rem_upload(unsigned xptr, unsigned fmt, int kind,
                                               unsigned w, unsigned h, const void *pix,
                                               unsigned pitch, unsigned nlev,
                                               struct ID3D11ShaderResourceView *orig);
void sb_rem_note_cpu(unsigned xptr);  /* Schreib-LockRect: per CPU beschrieben */
void sb_rem_forget(unsigned xptr);    /* Release rc=0: xptr frei */
void sb_rem_note_draw(int rhw, unsigned xptr); /* je Draw (nur Dump-Statistik) */
void sb_rem_note_read(unsigned buf, unsigned len, const char *path); /* NtReadFile (Quelle) */
void sb_rem_present(unsigned frame);  /* je Present: index.json + VRAM-Zeile */
void sb_rem_shutdown(void);           /* Lauf-Ende: index.json schreiben */
/* W118 REM-OVERLAY: Paket-Stufen (1 = x2, 2 = x4) und HD live schalten
 * (1 = ok, -1 = kein Paket); danach sb_tex_relive_all(). */
int sb_rem_live_avail(void);
int sb_rem_live_set(int on, int scale);

#ifdef __cplusplus
}
#endif
