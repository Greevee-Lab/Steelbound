/* sb_texhash.h - Inhalts-Hash "sbt1" der Remaster-Texturen und Null-Pruefung.
 *
 * W103 REM-FRAME: Hash (64 Bit, FNV-1a) ueber Kopf "SBT1" + Xbox-Formatcode
 * + Breite + Hoehe + Palettenlaenge (0) + Mip-Stufe 0 NACH dem Deswizzle bzw.
 * dicht gepackt (DXT-Bloecke, lineare Zeilen ohne Pitch-Polster).
 * Referenz in Python: tools/pack/sbpack.py sbt1_hash (C == Python, W140).
 *
 * W288 PACK-ZERO-TEX: tex_mip0_zero prueft genau den gehashten Bereich auf
 * lauter Null-Bytes (Abbruch beim ersten Nicht-Null-Byte). Eine solche Textur
 * ist in der Regel ein noch nicht geladener bzw. geleerter Speicherblock;
 * Pack-Eintraege auf ihren Hash wuerden jede frisch angelegte Textur gleicher
 * Groesse treffen. tex_hd_gate entscheidet, ob die Pack-Suche laufen darf.
 *
 * Nur statische Funktionen: von sb_remaster.c und render_selftest.c benutzt
 * (der Selbsttest prueft damit genau den Client-Code). */
#ifndef SB_TEXHASH_H
#define SB_TEXHASH_H

#include <windows.h>
#include <stddef.h>

#define FNV64_INIT 0xcbf29ce484222325ull
#define FNV64_PRIME 0x100000001b3ull

static unsigned long long fnv64(unsigned long long h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= FNV64_PRIME;
    }
    return h;
}

static unsigned lin_bpp(unsigned code) { return code == 0x1Cu ? 2u : 4u; }

/* Bytes je Zeile (dicht) und Zeilenzahl der Mip-0-Daten.
 * kind: 1 = DXT, 2 = linear, 3 = 32 Bit ent-swizzelt. */
static void mip0_rows(int kind, unsigned code, unsigned w, unsigned h,
                      unsigned *rowbytes, unsigned *rows)
{
    if (kind == 1) {
        unsigned blk = (code == 0x0Cu) ? 8u : 16u;
        *rowbytes = (w / 4u) * blk; /* wie tex_upload */
        *rows = h / 4u;
    } else if (kind == 3) {
        *rowbytes = w * 4u;
        *rows = h;
    } else {
        *rowbytes = w * lin_bpp(code);
        *rows = h;
    }
}

static unsigned long long tex_content_hash(int kind, unsigned code, unsigned w, unsigned h,
                                           const void *pix, unsigned pitch, int *ok)
{
    unsigned hdr[5], rb, rows, y;
    unsigned long long hv = FNV64_INIT;
    *ok = 0;
    hdr[0] = 0x31544253u; /* "SBT1" */
    hdr[1] = code;
    hdr[2] = w;
    hdr[3] = h;
    hdr[4] = 0; /* Palettenlaenge (P8 nicht im Spiel) */
    hv = fnv64(hv, hdr, sizeof hdr);
    mip0_rows(kind, code, w, h, &rb, &rows);
    if (!pix || !rb || !rows) {
        return 0;
    }
    __try {
        for (y = 0; y < rows; y++) {
            hv = fnv64(hv, (const unsigned char *)pix + (size_t)y * pitch, rb);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    *ok = 1;
    return hv ? hv : 1ull;
}

/* W288: 1 = der gehashte Bereich (Mip 0, dichte Zeilen, ohne Pitch-Polster)
 * besteht nur aus Null-Bytes. 0 = mindestens ein Byte ungleich 0, Bereich
 * leer oder nicht lesbar. Bricht beim ersten Nicht-Null-Byte ab: bei echten
 * Texturen meist nach wenigen Bytes. */
static int tex_mip0_zero(int kind, unsigned code, unsigned w, unsigned h,
                         const void *pix, unsigned pitch)
{
    unsigned rb, rows, y, x;
    mip0_rows(kind, code, w, h, &rb, &rows);
    if (!pix || !rb || !rows) {
        return 0;
    }
    __try {
        for (y = 0; y < rows; y++) {
            const unsigned char *r = (const unsigned char *)pix + (size_t)y * pitch;
            for (x = 0; x < rb; x++) {
                if (r[x]) { return 0; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

/* W288: Darf fuer diese Textur ein HD-Ersatz gesucht werden?
 * cpu = per CPU beschrieben (nie ersetzen), zero = tex_mip0_zero,
 * zero_skip = [remaster] hd_zero_skip (0 = alt: auch Null-Texturen suchen). */
static int tex_hd_gate(int zero_skip, int cpu, int zero)
{
    if (cpu) { return 0; }
    if (zero_skip && zero) { return 0; }
    return 1;
}

#endif
