/* sb_version.h - W301 BETA-VERSION: Phase einer Release-Version (Client + Launcher).
 *
 * Zentrale Versionsquelle ist die Datei poc-native/VERSION (eine Zeile, z. B.
 * 0.2.0). make_alpha.py liest sie als Standard fuer --version und setzt
 * -DSB_VERSION fuer Client und Launcher (Makro SB_VERSION_STR). Entwickler-
 * Builds ohne -DSB_VERSION bleiben "dev" (kein Wasserzeichen-Versionstext,
 * kein Update-Check).
 *
 * Nutzerentscheid 15.11.: 0.2.0 ist die erste Beta, danach 0.2.x/0.3.0 ... bis
 * 1.0.0, ohne -beta-Suffix. Daher gilt: jede 0.x-Version ohne Vorab-Suffix ist
 * BETA; alte Vorab-Fassungen (0.1.0-alpha.N) bleiben ALPHA. Header-only, damit
 * Client (sb_watermark.c) und Launcher (Badge) dieselbe Regel nutzen. */
#ifndef SB_VERSION_H
#define SB_VERSION_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "ALPHA" | "BETA" | "RC" | "" (Vollversion >= 1.0.0, Entwickler-Build oder
 * unlesbar). Ein fuehrendes v wird ignoriert. */
static __inline const char *sb_release_stage(const char *v)
{
    const char *pre;
    unsigned long major = 0;
    int d = 0;
    if (!v || !v[0]) {
        return "";
    }
    if (*v == 'v' || *v == 'V') {
        v++;
    }
    while (*v >= '0' && *v <= '9' && d < 9) {
        major = major * 10 + (unsigned long)(*v - '0');
        v++;
        d++;
    }
    if (d == 0 || *v != '.') {
        return "";
    }
    pre = strchr(v, '-');
    if (pre) {
        const char *plus = strchr(v, '+');
        if (plus && plus < pre) {
            pre = NULL; /* '-' steht erst in den Build-Metadaten */
        }
    }
    if (pre) {
        if (strstr(pre, "alpha")) { return "ALPHA"; }
        if (strstr(pre, "beta")) { return "BETA"; }
        if (strstr(pre, "rc")) { return "RC"; }
    }
    return major == 0 ? "BETA" : "";
}

/* Anzeigetext wie im Wasserzeichen (UTF-8): "Steelbound 0.2.0 . BETA" (Mittelpunkt U+00B7),
 * Vollversion "Steelbound 1.0.0", ohne Version "Steelbound dev". */
static __inline void sb_version_label(char *out, size_t cap, const char *v)
{
    const char *st = sb_release_stage(v);
    if (!out || cap == 0) {
        return;
    }
    if (!v || !v[0]) {
        _snprintf_s(out, cap, _TRUNCATE, "Steelbound dev");
    } else if (st[0]) {
        _snprintf_s(out, cap, _TRUNCATE, "Steelbound %s \xC2\xB7 %s", v, st);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "Steelbound %s", v);
    }
}

/* W420/W421 steelnet-Versionen (Nutzerentscheid 22.11.: online nur mit der neuesten Version). Eigene Version ist
 * poc-native/VERSION, zur Build-Zeit als SB_NET_VERSION eingebaut (Client und Launcher, auch Entwickler-Builds).
 * sb_netver_cmp: numerischer Vergleich je Punkt-Teil ("0.3.10" > "0.3.2", fehlende Teile = 0, Nicht-Ziffern beenden
 * den Teil) - derselbe Vergleich wie das Tunnel-HELLO (loc_tun.c, W420) und parse_cver des Dienstes (protocol.py,
 * Tupel mit Nullen aufgefuellt). > 0 = a neuer, < 0 = a aelter. */
static __inline int sb_netver_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        char *ea, *eb;
        unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
        if (x != y) {
            return x > y ? 1 : -1;
        }
        a = ea;
        b = eb;
        while (*a && *a != '.') a++;
        while (*b && *b != '.') b++;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* W421 LOC-VERSION-GATE: strikter Parser fuer min_client_version aus GET /v1/info - genau X.Y.Z, je 1-4 Ziffern,
 * hoechstens SB_NETVER_MAX Zeichen (Teilmenge von parse_cver des Dienstes). 1 = gueltig. */
#define SB_NETVER_MAX 14 /* "9999.9999.9999" */
static __inline int sb_netver_strict(const char *v)
{
    int part = 0, d = 0, n = 0;
    if (!v) {
        return 0;
    }
    for (; *v; v++) {
        if (++n > SB_NETVER_MAX) {
            return 0;
        }
        if (*v >= '0' && *v <= '9') {
            if (++d > 4) {
                return 0;
            }
        } else if (*v == '.' && d > 0 && part < 2) {
            part++;
            d = 0;
        } else {
            return 0;
        }
    }
    return part == 2 && d > 0;
}

/* W421: Ist die eigene Version aelter als die Mindestversion des Dienstes? Nur wenn beide strikt gueltig sind;
 * leer/kaputt = keine Sperre (alte Server, kaputte Antwort). */
static __inline int sb_netver_outdated(const char *own, const char *min_ver)
{
    return sb_netver_strict(own) && sb_netver_strict(min_ver) && sb_netver_cmp(own, min_ver) < 0;
}

#endif /* SB_VERSION_H */
