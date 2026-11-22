/* sb_ally_core.h - W424 SB-ALLIED-SQUADS: reine Entscheidungslogik (ohne
 * Spielspeicher; auch input_selftest).
 *
 * Idee nach den oeffentlichen "SB1 Patch Dev Notes" eines Community-Patches
 * (LGPL 2.1): Verbuendete Einheiten auch fern vom Spieler simulieren. Eigene
 * Umsetzung, kein fremder Code.
 *
 * Original (EU 0x5BF80, einmal je Einheit und Spielbild): Trupp-Mitglieder
 * folgen dem Anfuehrer, jede andere Einheit ist nur aktiv, wenn ihre
 * Kartenzelle im Rechteck um den Spieler liegt (Spielerzelle +-10).
 * W426 (Nutzerentscheid 22.11.): Eine verbuendete Einheit gilt als "gesehen",
 * sobald das Original sie einmal aktiv schaltet (im Rechteck um den Spieler,
 * Trupp-Mitglieder ueber ihren Anfuehrer). Ab dann bleibt sie fuer den Rest
 * der Mission aktiv. Vorher bleibt sie wie im Original eingefroren.
 * Zusatz (nur wenn das Original "eingefroren" sagt):
 *   - verbuendete Einheit (Flag 0x80000)   -> aktiv, wenn gesehen
 *   - Trupp-Mitglied                       -> wie Original (folgt Anfuehrer)
 *   - andere Einheit innerhalb +-range Zellen (Chebyshev) um eine lebende,
 *     gesehene verbuendete Einheit         -> aktiv
 * Merker "gesehen" je Slot mit Schluessel (Einheiten-Index),
 * geloescht bei Missionsanfang, Tod, leerem Slot oder anderem Schluessel
 * (Slot neu belegt). Alles reine Funktionen des Spielzustands (Bildzaehler der
 * Einheitenschleife, Flags, Positionen), keine Host-Zeit. */
#ifndef SB_ALLY_CORE_H
#define SB_ALLY_CORE_H

#define SB_ALLY_RANGE 7      /* 2/3 der Spielerzone (+-10 Zellen), gerundet */
#define SB_ALLY_MAXN 256     /* Verbuendete je Bild (Rest ohne Feind-Naehe) */
#define SB_ALLY_SLOTS 1024   /* Merker "gesehen" je Slot-Index (Rest nie gesehen) */

typedef struct {
    short x, z; /* Kartenzelle (Einheit +0x3D4/+0x3D6) */
} sb_ally_pt_t;

/* Schluessel einer Slot-Belegung (nie 0): Einheiten-Index (Slot+0x34) + 1.
 * Slot-Wort 0 eignet sich nicht (wird 0 beim Tod, kann sich aendern). Ein neu
 * belegter Slot wird ausserdem durch den Abgleich je Bild (Tod/leer) frei. */
static __inline unsigned sb_ally_key(unsigned short uidx)
{
    return (unsigned)uidx + 1u;
}

/* 1 = Slot idx mit diesem Schluessel ist gesehen. */
static __inline int sb_ally_seen(const unsigned *tab, int cap, int idx, unsigned key)
{
    return idx >= 0 && idx < cap && tab[idx] == key;
}

/* Original schaltet die verbuendete Einheit aktiv -> merken. Bild 0 (erster
 * Durchlauf nach dem Missionsanfang, Rechteck und Anfuehrer-Bits noch vom
 * Ladezustand) zaehlt nicht. Rueckgabe 1 = neu gemerkt. */
static __inline int sb_ally_mark(unsigned *tab, int cap, int idx, unsigned key, unsigned long frame)
{
    if (frame == 0 || idx < 0 || idx >= cap || tab[idx] == key) { return 0; }
    tab[idx] = key;
    return 1;
}

/* Abgleich je Bild: tot, leer oder anders belegt -> Merker loeschen.
 * Rueckgabe 1 = noch gesehen. */
static __inline int sb_ally_keep(unsigned *tab, int cap, int idx, unsigned key, int alive)
{
    if (idx < 0 || idx >= cap) { return 0; }
    if (!alive || tab[idx] != key) {
        tab[idx] = 0;
        return 0;
    }
    return 1;
}

/* Zusatzregel fuer eine Einheit, die das Original einfriert.
 * member = Trupp-Mitglied (folgt dem Anfuehrer), ally = verbuendet,
 * seen = verbuendete Einheit wurde in dieser Mission schon einmal vom Original
 * aktiv geschaltet. al/n = lebende, gesehene Verbuendete. */
static __inline int sb_ally_extra(int member, int ally, int seen, short x, short z, const sb_ally_pt_t *al, int n,
                                  int range)
{
    int i;
    if (member) { return 0; }
    if (ally) { return seen ? 1 : 0; }
    for (i = 0; i < n; i++) {
        int dx = (int)x - (int)al[i].x, dz = (int)z - (int)al[i].z;
        if (dx >= -range && dx <= range && dz >= -range && dz <= range) { return 1; }
    }
    return 0;
}

/* FNV-1a 64 ueber die Replay-Daten ohne die 20-Byte-Signatur am Ende
 * (ZCACHE- und UDATA-Kopie desselben Replays haben gleiche Daten). */
static __inline unsigned long long sb_ally_fnv64(const unsigned char *p, unsigned long n)
{
    unsigned long long h = 14695981039346656037ull;
    unsigned long i;
    for (i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

#endif
