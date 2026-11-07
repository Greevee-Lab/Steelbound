#pragma once
/* sb_pack.h - .sbpack-Paketlader (W140, PACK_FORMAT.md v1).
 *
 * Start: packs.json + Manifeste lesen, Index Hash -> (Paket, Datei/Offset)
 * bauen, Shadowing einmal aufloesen. Spielpfad: nur Hash-Lookup + Lesen
 * (kein JSON, keine Pruefsummen, keine Verzeichnissuche). Aufruf nur aus
 * sb_remaster.c (dort haelt der Aufrufer g_rcs).
 */
#ifdef __cplusplus
extern "C" {
#endif

/* packs_json: Pfad (relativ = zu rdir). Rueckgabe: 1 = ok (auch 0 Pakete),
 * 0 = fatal (pack_loader faellt auf Original zurueck). Loggt je Paket eine
 * Zeile plus Summenzeile (PACK ...). */
int sb_pack_init(const char *packs_json, const char *rdir);
/* 1 = mindestens ein aktives Paket mit Eintraegen. */
int sb_pack_active(void);
/* Bitmaske vorhandener Stufen (1 = x2, 2 = x4). */
int sb_pack_avail(void);
/* Fundort eines Hash auf Stufe (2/4, hd_scale): 1 = Treffer (loc gesetzt),
 * 0 = Original. loc ist nur bis zum naechsten Aufruf gueltig. */
typedef struct {
    int zip;              /* 0 = Ordner-Paket (path), 1 = ZIP (off/size) */
    char path[600];       /* Anzeige + Ordner-Pfad */
    unsigned long long off;  /* ZIP: Datenoffset */
    unsigned long long size; /* ZIP: Bytezahl */
    int slot;             /* 2 oder 4 */
} sb_pack_loc_t;
int sb_pack_find(unsigned long long hv, int level, sb_pack_loc_t *loc);
/* Bytes zum Fundort lesen (malloc, free() durch Aufrufer): 1 = ok. */
int sb_pack_read(const sb_pack_loc_t *loc, unsigned char **out,
                 unsigned long *nout);
void sb_pack_shutdown(void);

/* W151 EFFECT-PACK: Positivliste der Effekt-Parameter (PACK_FORMAT.md
 * Abschnitt 9). Einzige Quelle im Client; sbpack.py spiegelt sie. */
typedef struct { const char *key; int min, max; } sb_fxparam_t;
#define SB_FXN 11
extern const sb_fxparam_t kSbFxParams[SB_FXN + 1]; /* letztes key = NULL */
/* Effekt-Parameter aller aktiven Effect-Packs lesen (nur Startpfad, kein
 * Spielzustand): je Parameter gewinnt das hoechste aktive Paket mit diesem
 * Schluessel; Werte geklemmt, unbekannte Schluessel mit Warnung ignoriert.
 * hit[i] = 1 + out[i] = Wert bei Treffer, sonst hit[i] = 0; src[i] =
 * Paket-Id des Gewinners (darf NULL sein). Rueckgabe 1 = ok, auch ohne
 * Effect-Pack (dann alle hit = 0). */
int sb_pack_fxparams(const char *packs_json, int *out, int *hit,
                     char (*src)[128]);

/* W148 PACK-LAUNCHER: Launcher-Nutzung (liest nur, kein Spielzustand).
 * Nutzt denselben JSON-/ZIP-/Inflate-Parser wie der Lader oben. */

/* Ein Reihenfolge-Eintrag aus packs.json (+ Manifest-Kurzinfo). */
typedef struct {
    char src[600];      /* "file" aus packs.json (relativ/absolut) */
    char id[128];       /* Manifest (oder packs.json-id, falls unlesbar) */
    char name[128];
    char version[32];
    char author[128];
    char license[128];
    char type[16];      /* texture | effect */
    int enabled;        /* 1 = aktiv (enabled fehlt = an) */
    int is_zip;         /* 1 = .sbpack, 0 = Ordner, -1 = fehlt/unlesbar */
    int nentries;       /* Manifest-Eintraege (-1 = Manifest unlesbar) */
    int nparams;        /* W151: bekannte params.json-Schluessel (-1 = kein params.json) */
    char paramsum[256]; /* W151: "fx_soft=60, fx_embers=50" (gekuerzt, "" = keine) */
    int status;         /* Schnellstatus: 2 gruen, 1 gelb, 0 rot, -1 unbekannt */
    char reason[64];    /* stabiler Grund ("" = ok) */
} sb_pack_scan_t;
/* packs.json lesen: out[cap] fuellen. Rueckgabe: Order-Eintraege (cap
 * deckelt), -1 = Datei unlesbar/kein JSON. */
int sb_pack_scan(const char *packs_json, sb_pack_scan_t *out, int cap);

/* Vollpruefung V1-V13 (PACK_FORMAT.md §7) eines Pakets (Ordner oder
 * .sbpack): 2 = gruen, 1 = gelb, 0 = rot. reason = stabiler Code der
 * ersten roten (sonst gelben) Regel, detail = naehere Angabe (beide
 * NUL-terminiert, ggf. leer). xbe_sha256 = 64 Hex oder NULL/leer
 * (V2 entfaellt dann). Laeuft ohne Python, auch grosse Pakete. */
int sb_pack_check(const char *pkg_path, const char *xbe_sha256,
                  char *reason, unsigned rcap, char *detail, unsigned dcap);

/* Verdeckungszahlen je Order-Eintrag (Reihenfolge aus packs.json):
 * nentries[i] = Eintraege, shadowed[i] = davon verdeckt (-1 =
 * uebersprungen: aus/fehlend/unlesbar). Rueckgabe: Order-Eintraege
 * (cap deckelt), -1 = packs.json unlesbar. */
int sb_pack_shadow(const char *packs_json, int *nentries, int *shadowed,
                   int cap);

/* W172 LAUNCHER-P567: Abdeckung je Kategorie + paarweise Verdeckung (nur
 * Launcher, liest nur; gleiche Aufloesung wie sb_pack_shadow).
 * cov[i].cat[c] = Eintraege von Order-Eintrag i in Kategorie c (alle -1 =
 * aus/fehlend/unlesbar), cat2/cat4 = davon mit Variante x2/x4.
 * covby[i * cap + w] = Eintraege von i, die Paket w gewinnt (w != i, also
 * "verdeckt durch w"); covby darf NULL sein, sonst cap * cap ints.
 * Rueckgabe: Order-Eintraege (cap deckelt), -1 = packs.json unlesbar. */
#define SB_PACK_NCAT 9
extern const char *const kSbPackCat[SB_PACK_NCAT]; /* "effect" = "effects" */
typedef struct {
    int cat[SB_PACK_NCAT];
    int cat2[SB_PACK_NCAT];
    int cat4[SB_PACK_NCAT];
} sb_pack_cov_t;
int sb_pack_coverage(const char *packs_json, sb_pack_cov_t *cov, int *covby,
                     int cap);

/* packs.json lesen/schreiben (Launcher-Seite "Packs"). */
typedef struct { char id[128], file[600]; int enabled; } sb_pack_order_t;
typedef struct { char name[64]; int cap; } sb_pack_cap_t; /* cap 0/2/4 */
typedef struct { char hash[17]; int cap; } sb_pack_over_t;
/* Rueckgabe: Order-Eintraege (-1 = unlesbar). level = "x2"/"x4"/"orig"
 * (Default "x4"); caps/overs nur so weit gefuellt wie cap > 0
 * (Rueckgabe zaehlt nur order). */
int sb_pack_read_cfg(const char *packs_json, sb_pack_order_t *ord, int ocap,
                     char *level, unsigned lcap, sb_pack_cap_t *caps, int ccap,
                     sb_pack_over_t *overs, int vcap);
/* Schreibt version/order/level/category_level/overrides. 1 = ok. */
int sb_pack_write_cfg(const char *packs_json, const sb_pack_order_t *ord,
                      int n, const char *level, const sb_pack_cap_t *caps,
                      int nc, const sb_pack_over_t *overs, int nv);

#ifdef __cplusplus
}
#endif
