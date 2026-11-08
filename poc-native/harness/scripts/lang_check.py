# -*- coding: utf-8 -*-
"""W142 L10N-EN: Konsistenzpruefung der Sprachdateien.

Prueft:
  1. Jeder im Code benutzte Schluessel existiert in en.lang.
  2. de.lang ist vollstaendig (gleiche Menge wie en.lang).
  3. Keine verwaisten Schluessel (in en.lang, aber unbenutzt).
  4. Platzhalter (%d, %s, ...) stimmen je Schluessel zwischen en und de ueberein.
  5. Format: key=Wert (kein Leer um den Schluessel, Wert darf mit Leer
     beginnen; [a-z0-9_.], keine Duplikate, kein Leer am Wertende).

Aufruf: python lang_check.py [poc-native]
Exit 0 = gruen. Laeuft auch im Build (CMake-Target lang_check).
"""
import os
import re
import sys

PREFIXES = ("gui.", "ovl.", "bind.", "fx.", "vr.", "app.", "cfg.", "msg.", "as.")

SRC = [
    "launcher/src/launcher_gui.c",
    "launcher/src/launcher_ctrl.c",
    "launcher/src/launcher_cfg.c",
    "launcher/src/launcher_assist.c",
    "launcher/src/launcher.c",
    "launcher/src/launcher_saves.c",  # W199 SAVE-IMPORT
    "launcher/src/launcher_rules.c",  # W171 LAUNCHER-P2P3 (gui.rule_*)
    "harness/src/sb_overlay.c",
    "harness/src/render_fx_rem.c",
    "harness/src/vr.c",
    "creator/src/creator.c",  # W155 PACK-CREATOR
    "creator/src/creator_gui.c",
    "harness/src/emu_main.c",  # W150: Startfehler (Abbild/Version)
]

# W180 EXPORT: Quellen, die im Community-Export (noch) fehlen duerfen (Pack
# Creator erst mit Phase 2, s. inventory.tsv). Fehlt eine, werden ihre
# Schluessel (Praefix) nicht als verwaist gemeldet; privat ist alles da.
OPTIONAL_SRC = {
    "creator/src/creator.c": "gui.crt_",
    "creator/src/creator_gui.c": "gui.crt_",
}
missing_prefixes = []

KEY_RE = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$")
# LT("k"), sb_lang("k"), sb_lang_w("k" + jedes Literal mit Sprach-Praefix
# (deckt Tabellen mit Schluesseln ab, z. B. kCtrlElems, Overlay-Labels).
USE_RE = re.compile(r'(?:\bLT|\bsb_lang(?:_w)?)\s*\(\s*"([^"]+)"')
LIT_RE = re.compile(r'"((?:gui|ovl|bind|fx|vr|app|cfg|msg|as)\.[A-Za-z0-9_.]+)"')
PH_RE = re.compile(r"%(?:\d+\$)?[+#\-0 ]*(?:\d+|\*)?(?:\.(?:\d+|\*))?"
                   r"(?:hh|h|ll|l|j|z|t|L|I64|I32|w)?[diuoxXfFeEgGaAcspS%]")

fails = []
warns = []


def fail(msg):
    fails.append(msg)


def parse_lang(path):
    """-> dict key: (value, line). Fehler -> fail()."""
    ents = {}
    with open(path, encoding="utf-8-sig") as f:
        for ln, line in enumerate(f, 1):
            t = line.rstrip("\r\n")
            s = t.strip()
            if not s or s.startswith("#"):
                continue
            if "=" not in t:
                fail(f"{path}:{ln}: Zeile ohne '=': {t[:60]!r}")
                continue
            left, _, val = t.partition("=")
            key = left.strip()
            if not key:
                fail(f"{path}:{ln}: leerer Schluessel")
                continue
            if not KEY_RE.match(key):
                fail(f"{path}:{ln}: Schluessel '{key}' verletzt [a-z0-9_.]")
                continue
            if left != key:
                fail(f"{path}:{ln}: Leer um den Schluessel '{key}' (Format: key=Wert)")
                continue
            if val != val.rstrip(" \t"):
                fail(f"{path}:{ln}: Wert endet mit Leerzeichen ('{key}')")
                continue
            # Wert darf mit Leer beginnen (Overlay-Einzug, angehaengte Saetze).
            if key in ents:
                fail(f"{path}:{ln}: doppelter Schluessel '{key}'")
                continue
            ents[key] = (val, ln)
    return ents


def strip_comments(src):
    out = []
    i, n = 0, len(src)
    instr = False
    while i < n:
        c = src[i]
        if instr:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == '"':
                instr = False
            i += 1
        else:
            if c == '"':
                instr = True
                out.append(c)
                i += 1
            elif c == "/" and i + 1 < n and src[i + 1] == "/":
                while i < n and src[i] != "\n":
                    i += 1
            elif c == "/" and i + 1 < n and src[i + 1] == "*":
                i += 2
                while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                    out.append("\n" if src[i] == "\n" else " ")
                    i += 1
                i += 2
            else:
                out.append(c)
                i += 1
    return "".join(out)


def collect_used(root):
    used = {}  # key -> [ Fundstellen ]
    for rel in SRC:
        p = os.path.join(root, rel)
        if rel in OPTIONAL_SRC and not os.path.isfile(p):
            missing_prefixes.append(OPTIONAL_SRC[rel])
            warns.append(f"{rel} fehlt (Export ohne Pack Creator) - "
                         f"Schluessel {OPTIONAL_SRC[rel]}* nicht auf Verwaisung geprueft")
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            clean = strip_comments(f.read())
        lines = clean.split("\n")
        for ln, line in enumerate(lines, 1):
            if line.lstrip().startswith("#"):
                continue  # #include "vr.h" u. a.: kein Sprachschluessel
            for m in USE_RE.finditer(line):
                used.setdefault(m.group(1), []).append(f"{rel}:{ln}")
            for m in LIT_RE.finditer(line):
                used.setdefault(m.group(1), []).append(f"{rel}:{ln}")
    return used


def placeholders(val):
    ph = []
    for m in PH_RE.finditer(val):
        if m.group(0) == "%%":
            continue
        ph.append(m.group(0))
    return sorted(ph)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..")
    root = os.path.abspath(root)
    en_p = os.path.join(root, "lang", "en.lang")
    de_p = os.path.join(root, "lang", "de.lang")
    if not os.path.isfile(en_p) or not os.path.isfile(de_p):
        print(f"lang_check: {en_p} oder {de_p} fehlt", file=sys.stderr)
        return 2
    en = parse_lang(en_p)
    de = parse_lang(de_p)
    used = collect_used(root)

    for key, at in sorted(used.items()):
        if key not in en:
            fail(f"Schluessel '{key}' benutzt ({at[0]}{', ...' if len(at) > 1 else ''}), fehlt in en.lang")
    for key in sorted(set(en) - set(de)):
        ln = en[key][1]
        fail(f"Schluessel '{key}' (en.lang:{ln}) fehlt in de.lang")
    for key in sorted(set(de) - set(en)):
        ln = de[key][1]
        fail(f"Schluessel '{key}' (de.lang:{ln}) fehlt in en.lang")
    for key in sorted(set(en) - set(used)):
        if missing_prefixes and key.startswith(tuple(missing_prefixes)):
            continue
        ln = en[key][1]
        fail(f"Schluessel '{key}' (en.lang:{ln}) ist verwaist (unbenutzt)")
    for key in sorted(set(en) & set(de)):
        pe, pd = placeholders(en[key][0]), placeholders(de[key][0])
        if pe != pd:
            fail(f"Schluessel '{key}': Platzhalter en={pe} de={pd}")
    # Harte Faelle: Key-Literal ohne LT() ausserhalb bekannter Tabellen?
    # (Hinweis, kein Fehler: Tabellen halten Schluessel ohne LT.)
    for w in warns:
        print(f"Warnung: {w}")
    for e in fails:
        print(f"FEHLER: {e}")
    n = len(en)
    print(f"lang_check: {n} Schluessel, {len(used)} benutzt, "
          f"{len(fails)} Fehler")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
