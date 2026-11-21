/* sb_tex_budget.h - W383 LOC-GH-DATA (#60): Zulassung zur Upload-Drossel (STALLFIX boot484, 12 Uploads je Bild).
 *
 * Befund (Tester-Logs #60/#62, LoC, zweite Runde): 17 swizzled V8U8-Bump-Karten (Format 0x28, Kopf 0x301070 ff.)
 * sind ohne loc_bumpenv>=2 nie ladbar. tex_upload zaehlte den Platz in der Drossel aber VOR der Formatpruefung:
 * jeder aussichtslose Versuch verbrauchte einen der 12 Plaetze. Sobald einmal gedrosselt wurde, galten die
 * Bump-Eintraege als "an der Drossel gescheitert" (W307 tex_throttle_retry) und versuchten es bei jedem Get neu -
 * das Gelaende wird vor dem Cockpit gezeichnet, also verbrauchten sie das Budget zu Beginn jedes Bilds. Die in
 * Runde 2 neu belegten Cockpit-Texturen kamen nie mehr an die Reihe (Cockpit mit Inhalt aus Runde 1 bzw. ohne
 * Textur, flache Farben). Tester-Log: 1,6 Mio. Drossel-Neuversuche in Runde 2, < 1024 in Runde 1.
 *
 * Regel: Ein Versuch, der ohnehin am Format scheitert, belegt keinen Platz (fmt_first=1). fmt_first=0 = alt.
 *
 * Nur Header (render_selftest spielt das Modell aus Bump- und Cockpit-Eintraegen durch). */
#ifndef SB_TEX_BUDGET_H
#define SB_TEX_BUDGET_H

/* Liefert 1 = Platz belegt (Upload darf laufen), 0 = gedrosselt bzw. kein Platz noetig.
 * fmt_ok: Format ladbar (tex_decode_fmt != 0); *count: Uploads in diesem Bild. *fail: 2 = Drossel,
 * 4 = Format (ohne Platz), 0 = zugelassen. */
static __inline int tex_budget_admit(int fmt_ok, int fmt_first, unsigned *count, unsigned budget, int paused,
                                     unsigned *fail)
{
    if (fmt_first && !fmt_ok) { *fail = 4u; return 0; } /* scheitert ohnehin: kein Platz */
    if (*count >= budget && !paused) { *fail = 2u; return 0; }
    (*count)++;
    *fail = fmt_ok ? 0u : 4u; /* alt: Platz belegt, danach Formatfehler */
    return fmt_ok ? 1 : 0;
}

#endif
