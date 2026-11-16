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

#endif /* SB_VERSION_H */
