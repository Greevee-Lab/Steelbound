#pragma once
/* iso_vfs.h - W150 ISO-DIRECT: Spiel-Abbild (XDVDFS) nur lesend als D:.
 *
 * Gemeinsamer Baustein fuer Client (krnl_file.c, xbe_loader.c) und Launcher
 * (Abbild pruefen). Keine Abhaengigkeit ausser Win32 (kein hlog): Fehler
 * kommen als Text zurueck, der Aufrufer loggt.
 *
 * Format (oeffentlich dokumentiert, xboxdevwiki "XDVDFS"; nur Wissen, kein
 * fremder Code): Sektor 2048 B; Volume-Deskriptor in Sektor 32 relativ zum
 * Partitionsbeginn (Kennung "MICROSOFT*XBOX*MEDIA", Wurzel-Sektor, -Groesse,
 * FILETIME); jedes Verzeichnis ist eine Tabelle mit binaerem Suchbaum
 * (Eintrag: links/rechts in DWORDs, Startsektor, Groesse, Attribut,
 * Namenslaenge, Name; 4-Byte-ausgerichtet, nicht ueber Sektorgrenzen).
 * Partitionsbeginn 0 (XISO) oder 0x18300000 (Redump-Vollabbild mit
 * Videopartition).
 *
 * Thread-Sicherheit: Die Pfadtabelle ist nach iso_vfs_open unveraenderlich;
 * Lesen per ReadFile mit OVERLAPPED-Offset (positionell) auf einem Handle,
 * keine eigene Sperre (kein Lesen unter ctx_lock noetig). */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISO_VFS_ATTR_DIR 0x10u

typedef struct iso_vfs iso_vfs_t;

typedef struct {
    uint32_t size;      /* Dateigroesse (Verzeichnis: Tabellengroesse) */
    uint8_t attr;       /* XDVDFS-Attribut (Bits wie FILE_ATTRIBUTE_*) */
    int is_dir;
} iso_vfs_stat_t;

/* Abbild oeffnen + Pfadtabelle aufbauen. NULL bei Fehler (err gefuellt). */
iso_vfs_t *iso_vfs_open(const char *path, char *err, size_t errcap);
void iso_vfs_close(iso_vfs_t *v);

/* Kennzahlen (Log/Launcher). */
uint64_t iso_vfs_part_base(const iso_vfs_t *v);
unsigned iso_vfs_count(const iso_vfs_t *v);
unsigned iso_vfs_dir_count(const iso_vfs_t *v);
void iso_vfs_volume_time(const iso_vfs_t *v, uint32_t *lo, uint32_t *hi);

/* Pfad (relativ zur Wurzel, '\\' oder '/', Gross/Klein egal; "" = Wurzel)
 * -> Knoten-Index >= 0, -1 = nicht gefunden. parent_ok (optional): 1 wenn
 * das Elternverzeichnis existiert (fuer FILE_NOT_FOUND vs PATH_NOT_FOUND). */
int iso_vfs_lookup(const iso_vfs_t *v, const char *rel, int *parent_ok);
int iso_vfs_stat(const iso_vfs_t *v, int node, iso_vfs_stat_t *st);
const char *iso_vfs_name(const iso_vfs_t *v, int node);

/* Verzeichnis: k-tes Kind (0-basiert, sortiert wie die Xbox), -1 = Ende. */
int iso_vfs_child(const iso_vfs_t *v, int dir, unsigned k);

/* Positionelles Lesen (thread-sicher). Liest hoechstens bis Dateiende.
 * Rueckgabe 0 = ok (*got kann 0 sein = EOF), sonst Win32-Fehler. */
unsigned long iso_vfs_read(const iso_vfs_t *v, int node, uint64_t pos,
                           void *buf, uint32_t len, uint32_t *got);

/* Ganze Datei lesen (malloc, Aufrufer gibt frei). 0 = ok. */
int iso_vfs_read_all(const iso_vfs_t *v, const char *rel, uint8_t **out,
                     uint32_t *len);

/* Selbsttest ohne Spieldaten: baut ein winziges XDVDFS-Abbild im
 * Temp-Ordner (beide Partitions-Offsets), prueft Erkennen, Suche,
 * Gross/Klein, Lesen, EOF, falsches Abbild. 0 = ok, sonst Fehlernummer. */
int iso_vfs_selftest(char *msg, size_t cap);

/* Test: winziges XISO mit nur einer default.xbe (Inhalt vom Aufrufer,
 * z. B. synthetischer Header einer fremden Fassung). 0 = ok. */
int iso_vfs_make_test_image(const char *path, const void *xbe, uint32_t n);

#ifdef __cplusplus
}
#endif
