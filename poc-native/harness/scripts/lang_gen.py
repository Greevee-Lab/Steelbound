# -*- coding: utf-8 -*-
"""W142 L10N-EN: erzeugt sb_lang_en.inc (eingebautes Englisch) aus en.lang.

Aufruf (CMake, Configure-Zeit):
  python lang_gen.py <poc-native>/lang/en.lang <out>/sb_lang_en.inc
Exit 0 = ok. Fehler: doppelte/leere Schluessel, leere Datei.
Nicht-ASCII-Bytes werden als \\xNN ausgegeben (compilerfest).
"""
import re
import sys

KEY_RE = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$")


def parse(path):
    ents = {}
    with open(path, encoding="utf-8-sig") as f:
        for ln, line in enumerate(f, 1):
            t = line.strip("\r\n")
            s = t.strip()
            if not s or s.startswith("#"):
                continue
            if "=" not in s:
                print(f"lang_gen: {path}:{ln}: Zeile ohne '=' wird ignoriert", file=sys.stderr)
                continue
            key, _, val = s.partition("=")
            key = key.strip()
            # Wert: fuehrende Leer bleiben (eingerueckte Zeilen)! Nur das eine
            # Trenn-'=' abziehen: val beginnt direkt danach im Original.
            raw = t.split("=", 1)[1]
            if not key:
                print(f"lang_gen: {path}:{ln}: leerer Schluessel", file=sys.stderr)
                return None
            if not KEY_RE.match(key):
                print(f"lang_gen: {path}:{ln}: Schluessel '{key}' verletzt [a-z0-9_.]", file=sys.stderr)
                return None
            if key in ents:
                print(f"lang_gen: {path}:{ln}: doppelter Schluessel '{key}'", file=sys.stderr)
                return None
            ents[key] = raw
    return ents


def c_escape(raw):
    # Escapes der .lang-Datei (\\n \\t \\r \\\\) in C-Escapes uebersetzen,
    # Nicht-ASCII als \\xNN (UTF-8-Bytes).
    out = []
    i = 0
    while i < len(raw):
        c = raw[i]
        if c == "\\" and i + 1 < len(raw) and raw[i + 1] in "ntr\\":
            n = raw[i + 1]
            out.append({"n": "\\n", "t": "\\t", "r": "\\r", "\\": "\\\\"}[n])
            i += 2
        elif c == '"':
            out.append('\\"')
            i += 1
        elif c == "\\":
            out.append("\\\\")
            i += 1
        elif ord(c) < 32 or ord(c) > 126:
            # Oktal (3-stellig): \\xNN wuerde mit folgender Hex-Ziffer
            # verschmelzen (MSVC liest \\x gierig).
            for b in c.encode("utf-8"):
                out.append(f"\\{b:03o}")
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main():
    if len(sys.argv) != 3:
        print("Aufruf: lang_gen.py en.lang sb_lang_en.inc", file=sys.stderr)
        return 2
    ents = parse(sys.argv[1])
    if ents is None:
        return 1
    if not ents:
        print("lang_gen: keine Schluessel gefunden", file=sys.stderr)
        return 1
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Erzeugt aus en.lang (lang_gen.py) - nicht von Hand editieren. */\n")
        f.write("typedef struct { const char *key; const char *val; } sb_lang_ent_t;\n")
        f.write("static const sb_lang_ent_t kSbLangEn[] = {\n")
        for key in sorted(ents):
            f.write(f'    {{ "{key}", "{c_escape(ents[key])}" }},\n')
        f.write("};\n")
        f.write(f"static const unsigned kSbLangEnN = {len(ents)}u;\n")
    print(f"lang_gen: {len(ents)} Schluessel -> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
