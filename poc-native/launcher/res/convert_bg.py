"""W82 LAUNCHER-UI2: Hintergrundmotiv einmalig konvertieren (W178 COMM-BG).

Quelle:  res/bg_nutzer.webp  (1672x941, SW-Motiv: Mech-Silhouette vor der
         Sonne; Eigentum des Projektinhabers, Lizenz wie die Projekt-Assets.
         Das fruehere Motiv ist ersetzt und per editions/clean_deny.tsv fuer
         jeden Export gesperrt.)
Ziel:    res/bg_nutzer.png   (8-bit-Graustufen-PNG, gleiche Groesse;
         byte-identisch reproduzierbar: convert_bg.py aus dem WebP)

Der Launcher laedt das PNG seit W535 AVVT als Datei res/launcher_bg.png neben
sb_launcher.exe (Build: CMake-POST_BUILD, Release: make_alpha.py; vorher
Ressource IDR_BG) und dekodiert es zur Laufzeit per WIC (Windows-Bordmittel, kein WebP-Codec
noetig). Ausbleichen/Abdunkeln und Zuschnitt (fuellen ohne Strecken) macht
der Launcher selbst (launcher_gui.c: bg_scaled), das PNG bleibt unveraendert
hell, damit der Schalter "Hintergrundbild" und spaetere Stufen frei bleiben.

Aufruf (aus poc-native/launcher):  python res/convert_bg.py [ZIEL.png]
Braucht Pillow mit WebP-Unterstuetzung (hier vorhanden: Pillow 12.2).
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "bg_nutzer.webp")
DST = os.path.join(HERE, "bg_nutzer.png")


def main(argv):
    dst = argv[1] if len(argv) > 1 else DST
    im = Image.open(SRC)
    if im.size != (1672, 941):
        print("Hinweis: unerwartete Groesse", im.size)
    # W535 AVVT: wieder normal komprimiert (optimize, byte-identisch zum LFS-PNG 08df9bad...). Das PNG liegt
    # seit W535 nicht mehr als Ressource in sb_launcher.exe, sondern als Datei res/launcher_bg.png neben der EXE
    # (CMake-POST_BUILD, make_alpha.py). W534 hatte es unkomprimiert eingebettet (.rsrc-Entropie 7,99 -> 2,04).
    im.convert("L").save(dst, optimize=True)
    print("ok:", dst, os.path.getsize(dst), "Bytes", im.size)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
