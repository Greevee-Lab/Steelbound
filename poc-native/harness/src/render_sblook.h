/* render_sblook.h - W459 LOC-FX-LIGHT: "SB look (Experimental)" fuer Line of Contact - reine Regeln (ohne D3D,
 * ohne Speicherzugriff), Selbsttest render_selftest Fall SBLOOK.
 *
 * Befund (Ghidra, LoC-VAs, nur gelesen): Die Bildfunktion FUN_00029320 rechnet je Bild die Glow-Kette
 * (FUN_0003D3C0: 256x256-Verkleinerung + Weichzeichner + Alpha-Maske) und danach
 *     0x2970B  CMP [0x3067B8],1      ; Bloom-Schalter des Spiels
 *     0x29712  JZ  0x29723           ; -> CALL 0x34270 = Hellpass + zeitliche Mischung in das 14x14-Bloom-Ziel
 *     0x29714  CMP [0x306BA8],ESI    ; Replay/Aussenkamera (Welt im Backbuffer)
 *     0x2971A  JNZ 0x29723
 *     0x2971C  CALL 0x34AA0          ; sonst: 14x14-Ziel nur loeschen (schwarz) -> Komposition ohne Bloom
 * Die Komposition FUN_00034B00 addiert das 14x14-Ziel (Stufe 2, Texel-UV 0..14) auf das Hauptbild. Im
 * Missionslauf ist [0x3067B8]=1 (Zensus: Abkling- und Additiv-Draw ins 14x14-Ziel je Bild) - der Bloom laeuft
 * im Cockpit also schon im Original; Bit 1 greift nur, wo das Spiel ihn abschaltet. Abklingrate/Staerke setzt
 * das Spiel je Zustand selbst (FUN_00020FA0: [0x307044] 0x14/0x23/0x2D, [0x306C18] 0/0,55).
 * Zweite Ansicht (Lock-On-Kamera): 0x298B5 CMP [0x306F30],EBP / 0x298BB JNZ -> CALL 0x34AD0 (loeschen)
 * statt CALL 0x34680 (Bloom).
 *
 * SB look Bit 1 (Bloom Hauptbild): JZ (0x74) bei 0x29712 -> JMP (0xEB): der Original-Bloom-Pfad laeuft
 * immer. Bit 2 (Bloom Lock-On-Kamera): JNZ (75 07) bei 0x298BB -> 2x NOP (sichtbar: die Kamera ist im
 * Missionsbild zeitweise ohne Bloom). Bit 4 (Bloom-Verstaerkung, kein Patch): der SB-Bloom-Regler
 * (render_shfx.c, bloom_strength-Regel "kleines RT <= 32^2 in Ziel >= 128") mit loc_sblook_bloom % auf die
 * LoC-Bloom-RTs (14^2 Hauptbild, 9^2 Kamera) - der SB-eigene Parameter auf dem spieleigenen Pfad. Reine Darstellung: die Pfade
 * zeichnen nur in die Bloom-Ziele (keine Schreibzugriffe auf Spielzustand, der Sync-Akkumulator [0x6D0C58]
 * bleibt unberuehrt). Vor dem Patch werden die Originalbytes verglichen; weichen sie ab (anderes Abbild),
 * bleibt der Code unveraendert. */
#ifndef RENDER_SBLOOK_H
#define RENDER_SBLOOK_H

#include <string.h>

#define SBLOOK_BLOOM_MAIN 1u /* Bloom im Hauptbild (Cockpit-Sicht) */
#define SBLOOK_BLOOM_CAM 2u  /* Bloom in der Lock-On-Kamera */
#define SBLOOK_BLOOM_BOOST 4u /* SB-Bloom-Regler auf den LoC-Bloom (render_shfx.c sfx_bloom_pct, loc_sblook_bloom) */
#define SBLOOK_ALL 7u

typedef struct {
    unsigned va;                  /* Gast-Adresse */
    unsigned len;                 /* Laenge des Vergleichs */
    unsigned char expect[8];      /* Originalbytes */
    unsigned plen;                /* Laenge des Patches */
    unsigned char patch[2];       /* neue Bytes ab va */
    unsigned bit;                 /* SBLOOK_* */
    const char *name;
} sblook_site_t;

static const sblook_site_t kSBLOOK_SITES[2] = {
    { 0x29712u, 8u, { 0x74, 0x0F, 0x39, 0x35, 0xA8, 0x6B, 0x30, 0x00 }, 1u, { 0xEB, 0x00 }, SBLOOK_BLOOM_MAIN,
      "Bloom Hauptbild (0x29712 JZ->JMP)" },
    { 0x298BBu, 3u, { 0x75, 0x07, 0xE8, 0, 0, 0, 0, 0 }, 2u, { 0x90, 0x90 }, SBLOOK_BLOOM_CAM,
      "Bloom Lock-On-Kamera (0x298BB JNZ->NOP)" },
};

/* Wirksame Bits aus den Schaltern: loc_sblook (0/1) und loc_sblook_mask (Unterschalter, Standard alle). */
static __inline unsigned sblook_bits(int on, int mask)
{
    if (!on) { return 0u; }
    return (unsigned)mask & SBLOOK_ALL;
}

/* Zustand einer Stelle: 0 = Original (Bytes wie erwartet), 1 = bereits gepatcht, -1 = fremde Bytes. */
static __inline int sblook_site_state(const sblook_site_t *s, const unsigned char *cur)
{
    if (memcmp(cur, s->expect, s->len) == 0) { return 0; }
    if (memcmp(cur, s->patch, s->plen) == 0 && memcmp(cur + s->plen, s->expect + s->plen, s->len - s->plen) == 0) {
        return 1;
    }
    return -1;
}

/* Was ist an der Stelle zu tun? 1 = patchen, 2 = Original wiederherstellen, 0 = nichts, -1 = verweigert. */
static __inline int sblook_site_action(const sblook_site_t *s, const unsigned char *cur, unsigned bits)
{
    int st = sblook_site_state(s, cur);
    int want = (bits & s->bit) != 0u;
    if (st < 0) { return -1; }
    if (want && st == 0) { return 1; }
    if (!want && st == 1) { return 2; }
    return 0;
}

#endif
