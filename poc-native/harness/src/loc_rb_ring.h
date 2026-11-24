/* loc_rb_ring.h - W330 LOC-ROLLBACK-R1: inkrementeller Zustandsring fuer Rollback (nur LoC-Profil).
 *
 * Verfolgt Schreibzugriffe auf registrierte Speicherbereiche und haelt je Tick die Seiten, die sich
 * geaendert haben, mit ihrem Inhalt VOR dem Tick in einem Ring (Undo-Protokoll). Laden von k Ticks =
 * diese Seiten rueckwaerts zurueckschreiben. Zwei Verfahren je Bereich:
 *   RBR_WW  Write-Watch (Bereich wurde mit MEM_WRITE_WATCH reserviert): GetWriteWatch je Tick.
 *   RBR_WP  Schreibschutz-Verfolgung: Seiten schreibgeschuetzt, erster Schreibzugriff je Tick loest
 *           eine Zugriffsverletzung aus, der eigene VEH-Handler hebt den Schutz auf und merkt die Seite.
 *           Spiegel-Ansichten (gleicher Speicher an anderer VA, LoC-Contig 0x80000000|P, 0xF0000000|P)
 *           werden mitgeschuetzt.
 * Eine Schattenkopie S haelt den Stand des letzten Ticks; Sichern = nur gemeldete Seiten vergleichen,
 * geaenderte alte Inhalte in den Ring, S nachziehen.
 *
 * Reines Modul ohne Logging/Konfiguration (Selbsttest tests/loc_rb_selftest.c mit synthetischem
 * Speicher). Nicht thread-sicher gegen sich selbst: rbr_* ausser rbr_prewrite nur aus einem Thread
 * (Spiel-Thread am Ruhepunkt). Schreibzugriffe anderer Threads werden verfolgt (VEH). */
#pragma once
#include <stdint.h>

enum { RBR_WP = 0, RBR_WW = 1 };

typedef struct {
    uint32_t dirty;     /* gemeldete Seiten (Verfolgung) */
    uint32_t saved;     /* davon wirklich geaendert -> Ring */
    uint32_t ww, wp;    /* gemeldete Seiten je Verfahren */
    uint32_t us_total;  /* Sichern gesamt */
    uint32_t us_ww;     /* GetWriteWatch */
    uint32_t us_prot;   /* Schutz neu setzen */
    uint32_t us_copy;   /* Vergleich + Kopie */
    uint32_t dropped;   /* aelteste Ticks verworfen (Ring voll) */
    uint32_t overflow;  /* Tick passte nicht in den Ring (Ring danach leer) */
    uint32_t hot;       /* davon heisse Seiten (offen, nur verglichen) */
    uint32_t hot_total; /* heisse Seiten insgesamt */
    uint32_t cooled;    /* in diesem Tick abgekuehlt (wieder geschuetzt) */
} rbr_commit_t;

typedef struct {
    uint32_t faults;        /* Schreibschutz-Fehler gesamt */
    uint32_t faults_foreign;/* davon andere Threads als der Eigner (rbr_set_owner) */
    uint32_t faults_alias;  /* davon ueber eine Spiegel-Ansicht */
    uint32_t prewrites;     /* Seiten per rbr_prewrite freigegeben */
    uint64_t fault_qpc;     /* Summe der Handlerzeit (QPC-Einheiten, nur Handler selbst) */
    uint32_t prot_fail;     /* VirtualProtect fehlgeschlagen (Bereich ueber Belegungsgrenzen o. ae.) */
} rbr_stat_t;

/* Fremde Schreiber (Threads ungleich Eigner), fuer die Diagnose */
typedef struct {
    uint32_t tid, n, first_eip, first_va, last_va;
} rbr_writer_t;

/* depth = hoechstens so viele Ticks im Ring, pool_pages = Seitenplaetze fuer alte Inhalte */
int rbr_init(unsigned depth, unsigned pool_pages);
/* Bereich anmelden (vor rbr_start). want_ww = 1: Write-Watch versuchen (sonst/Fehlschlag WP).
 * alias = weitere VAs desselben Speichers (je len Bytes), nalias <= 2. kind frei (Statistik).
 * Rueckgabe: Verfahren (RBR_WW/RBR_WP) oder -1. Bereiche duerfen sich nicht ueberlappen. */
int rbr_add(uint32_t va, uint32_t len, int want_ww, const uint32_t *alias, int nalias, uint32_t kind);
/* Heisse Seiten: nach n aufeinanderfolgenden beschriebenen Ticks offen lassen (0 = aus), nach cool unveraenderten
 * Ticks wieder schuetzen (Standard 2/64). Vor rbr_start. */
void rbr_set_hot(unsigned n, unsigned cool);
/* W469: Ring-Puffer und Schatten vorab anlegen und einmal beschreiben (vor dem ersten rbr_init/rbr_start, z. B. beim
 * Einschalten); rbr_init nutzt den Puffer, wenn pool_pages gleich ist. 0 = ok. */
int rbr_prealloc(unsigned pool_pages, unsigned shadow_pages);
int rbr_start(void);          /* Schutz setzen, Write-Watch zuruecksetzen, Schattenkopie; 0 = ok */
void rbr_stop(void);          /* Schutz zurueck, VEH weg, Speicher frei (rbr_init noetig fuer neuen Start) */
int rbr_active(void);
void rbr_set_owner(uint32_t tid); /* Eigner-Thread (Spiel-Thread) fuer die Fremd-Statistik */

int rbr_commit(rbr_commit_t *st); /* Tick-Ende: inkrementell sichern; 0 = ok */
/* k Ticks zurueck (k <= rbr_depth()). Danach ist der Ring um k Ticks kuerzer und S = Stand T-k.
 * pages = zurueckgeschriebene Seiten, us = Zeit. 0 = ok, -1 = zu wenig Ticks. */
int rbr_load(unsigned k, uint32_t *pages, uint32_t *us);
/* W390: seit dem letzten rbr_commit beschriebene Seiten auf den gesicherten Stand zuruecksetzen; Ring unveraendert.
 * pages = zurueckgeschrieben, checked = verglichen, us = Zeit. 0 = ok, -1 = nicht aktiv. */
int rbr_revert(uint32_t *pages, uint32_t *checked, uint32_t *us);
unsigned rbr_depth(void);     /* Ticks im Ring (ladbar) */
void rbr_stop_times(uint32_t out[4]); /* W372 DIAG: letzter rbr_stop: Schutz zurueck, Warten auf Handler, Freigeben (us), Warteschleifen */
void rbr_reset_ring(void);    /* Ring leeren (Schatten bleibt) */

/* Vor Kernel-Schreibzugriffen in Gast-Puffer (ReadFile/recv): Schutz aufheben + Seite merken.
 * Ohne aktiven Ring ein Vergleich. Thread-sicher. */
void rbr_prewrite(uintptr_t a, uint32_t n);

void rbr_stats(rbr_stat_t *st);
unsigned rbr_writers(rbr_writer_t *w, unsigned cap);
uint32_t rbr_pages(void);     /* angemeldete Seiten */
uint32_t rbr_pages_method(int method);
uint64_t rbr_qpf(void);

/* Diagnose: Vollkopie aller Bereiche nach dst (rbr_pages()*4096 Bytes) bzw. Vergleich mit src.
 * rbr_full_diff liefert die Zahl abweichender Seiten, bis cap Adressen (erstes abweichendes Wort). */
void rbr_full_copy(uint8_t *dst);
uint32_t rbr_full_diff(const uint8_t *src, uint32_t *addr, uint32_t cap);
/* Schatten gegen Live vergleichen (prueft die Verfolgung: S muss nach rbr_commit = Live sein). */
uint32_t rbr_shadow_diff(uint32_t *addr, uint32_t cap);
/* Schatten (= Stand des letzten Sicherns) nach dst kopieren (rbr_pages()*4096 Bytes) */
uint32_t rbr_shadow_copy(uint8_t *dst);
/* LOC-DESYNC-REPAIR: Stand k Ticks zurueck (k <= rbr_depth()) nach dst (rbr_pages()*4096 Bytes), Live/Schatten/Ring
 * unveraendert. Rueckgabe Seiten, 0 = nicht aktiv bzw. k zu gross. */
uint32_t rbr_snapshot_at(unsigned k, uint8_t *dst);
/* Bereich i: Lage, Art, Verfahren, heisse Seiten. 0 = kein Bereich i */
int rbr_region_info(int i, uint32_t *va, uint32_t *len, uint32_t *kind, int *method, uint32_t *hot);

/* W362 LOC-RING-INIT-EXIT: Stack-Belegungen ALLER Threads des Prozesses (TEB: StackBase/StackLimit/DeallocationStack;
 * unter WoW64 auch der 64-Bit-Stack, sofern unter 4 GB). Ein Schreibschutz auf einem Stack beendet den Prozess still
 * (Ausnahmerahmen nicht zustellbar), solche Seiten darf der Ring nie schuetzen. Belegungen ueber 4 MB werden auf
 * 1 MB um den Stack-Zeiger begrenzt. Rueckgabe: Anzahl Eintraege; nthr = Threads des Prozesses, noteb = ohne TEB. */
typedef struct { uint32_t a, b, tid; int bits; } rbr_stack_t;
int rbr_thread_stacks(rbr_stack_t *out, int cap, unsigned *nthr, unsigned *noteb);
/* Ganze Belegung (AllocationBase bis Ende), in der x liegt; 0 = frei/ungueltig */
int rbr_alloc_span(uint32_t x, uint32_t *pa, uint32_t *pb);
