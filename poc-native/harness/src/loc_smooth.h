/* loc_smooth.h - W385 LOC-SMOOTH: Rollback-Korrekturen weich darstellen (nur LoC-Netzspiel mit Eingabeschicht R2).
 *
 * Spiel-Thread (loc_rollback.c):
 *  - loc_smooth_rb_pre vor rbr_load: Lage aller VTs (vorhergesagter Stand des laufenden Spielticks) merken.
 *  - loc_smooth_rb_post nach rbi_replay_finish: korrigierter Stand desselben Ticks -> Sprung je VT messen (Zaehler,
 *    LOCSMOOTH-Zeile) und mit loc_rb_smooth=1 den Versatz E = vorhergesagt - korrigiert abklingend vormerken.
 *  - loc_smooth_tick vor dem Feld-Warten (live): Lage + Versatz dieses Ticks fuer die Liste veroeffentlichen, die der
 *    Render-Thread waehrend des Wartens zeichnet (wie W376).
 * Render-Thread (render_interp.c, am Swap): loc_smooth_get -> Welt-Draws je VT bzw. die Welt fuer die eigene Kamera
 * verschieben. Die Simulation bleibt unberuehrt (Hash gleich). Keine Eingaben im Log, nur Lagen-Statistik. */
#pragma once
#include <stdint.h>

#define LS_MAXV 10 /* VT-Objekte = Spielerplaetze */

typedef struct {
    uint32_t seq;   /* je Live-Tick (0 = nie) */
    uint32_t gtick; /* Spieltick */
    int own;        /* eigener Spielerindex (-1 unbekannt) */
    int any;        /* irgendein Versatz != 0 */
    float radius;   /* Zuordnung Draw -> VT (Spieleinheiten) */
    int opp_mode;   /* W387: loc_smooth_opp (0 aus, 1 auto, 2 fest) */
    float opp_ms;   /* W387: angezeigte Verzoegerung anderer VTs (ms, 0 = keine) */
    struct {
        int ok;        /* VT gelesen */
        float p[3];    /* simulierte (korrigierte) Lage dieses Ticks */
        float yaw;
        int on;        /* Versatz != 0 */
        float off[4];  /* dx, dy, dz, dGier (W385 vorhergesagt - korrigiert, gewichtet; W387 + Lage(T-d) - Lage(T)) */
    } v[LS_MAXV];
} loc_smooth_pub_t;

void loc_smooth_rb_pre(uint32_t gtick);
void loc_smooth_rb_post(uint32_t gtick, uint32_t cause, uint32_t ticks);
void loc_smooth_tick(uint32_t gtick);
/* W387: Ende eines Neurechen-Ticks (Lagen-Historie auf den korrigierten Stand). */
void loc_smooth_replay_tick(uint32_t gtick);
/* W387: F3-Netz-Overlay: Modus (0 aus, 1 auto, 2 fest) und aktuelle Anzeige-Verzoegerung in ms. 1 = Wert vorhanden. */
int loc_smooth_opp_hud(int *mode, int *ms);
void loc_smooth_reset(void); /* Missionsbeginn/-ende: Versaetze weg */
/* W390 LOC-OWN-PREDICT (Spiel-Thread): eigene VT-Lage {x, y, z, Gier} lesen (1 = gueltig); Ergebnis eines spekulativen
 * Vorlaufs uebergeben: Lagen p[k] der Ticks gt[k] (k < n), letzte = Anzeige fuer den Live-Tick gt_disp. Mit
 * loc_own_predict = 1 zeigt loc_smooth_tick(gt_disp) die eigene Lage (Kamera) von dort; Korrekturen zwischen zwei
 * Vorlaeufen klingen wie W385 ab. */
int loc_smooth_own_read(float out[4]);
void loc_smooth_own_pred(uint32_t gt_disp, int n, const uint32_t *gt, const float (*p)[4], const int *ok);
void loc_smooth_log(int t_s); /* LOCSMOOTH-Zeile (alle 10 s mit LOCRBI-TEL) */
/* W386: groesster Sprung (anderer oder eigener VT, Spieleinheiten) seit dem letzten Aufruf (Spiel-Thread). */
double loc_smooth_window_jump(void);

/* Render-Seite: juengsten Stand kopieren. 1 = loc_rb_smooth an und ein Versatz vorhanden. */
int loc_smooth_get(loc_smooth_pub_t *out);
/* DIAG (loc_rb_smooth_diag Bit 2): juengsten Stand unabhaengig vom Schalter (0 = nichts veroeffentlicht). */
int loc_smooth_peek(loc_smooth_pub_t *out);
/* Render-Seite: Zaehler je Liste (verschobene Draws je VT, Kamera-Draws, eigene). */
void loc_smooth_render_note(unsigned obj_draws, unsigned cam_draws, unsigned own_draws, unsigned vts);
/* W387: Render-Thread: Dauer von li_smooth_apply einer Liste mit Versatz (Mikrosekunden). */
void loc_smooth_render_cost(double us);
/* W390: Render-Thread: groesste Abweichung einer Zielmarkierung (simulierter Stand) vom gezeichneten anderen VT in dieser
 * Liste (Pixel bei 640x480): gesamt bzw. nur durch die eigene Vorhersage (Kamera). */
void loc_smooth_render_mark(double px_all, double px_own);
/* W390: dasselbe fuer gedachte Ziele geradeaus in 30 m bzw. 100 m (nur eigene Vorhersage). */
void loc_smooth_render_mark_virt(double px30, double px100);
/* DIAG (Bit 8): Ruck (zweite Differenz) je Liste gegen die zuletzt gezeigten (cam 0 = anderer VT, Anker-Draw naechst
 * seinem Mittelpunkt; 1 = eigenes Auge), roh = ohne Versatz, angezeigt = mit; win = Versatz aktiv (Fenster nach Korrektur). */
void loc_smooth_render_step(int cam, double raw, double disp, int win);
