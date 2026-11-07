#!/usr/bin/env python3
"""inventory.py - versionierte Dateien klassifizieren + Historie (W116 EDITIONS).

  python poc-native/tools/edition/inventory.py [--check]

Schreibt editions/inventory.tsv (jede Datei aus git ls-files, inkl. LFS) mit
Klasse kern | private | infra | unklar, Grund, LFS, Groesse, Community-Export
(Positivliste) und editions/history.tsv (spielabgeleitete Dateien, die je in
der Git-Historie standen: Bild/Audio/Video/Binaer/Spieldatei + LFS).
--check: nur pruefen, dass jede Datei der Positivliste Klasse 'kern' hat und
keine Datei ohne Regel bleibt (Exit 1 sonst). Liest nur Git, schreibt nur
die beiden TSV-Dateien.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import edition_common as ec  # noqa: E402

XISO = "Steel Battalion (Europe).xiso"
EXT_MEDIA = {"png", "jpg", "jpeg", "bmp", "dds", "webp", "gif", "tga", "wav",
             "bik", "mp4", "xbx"}
# Spieldatei-Endungen aus dem Disc-Inhalt (Media/...) + Spielstaende
EXT_GAME = {"xbe", "xpr", "iso", "sav", "bin", "rot", "pat", "stg", "ema",
            "esd", "gad", "eng", "uv", "coc", "plg", "flg", "sgt", "lid",
            "bmf", "lnp", "rep", "hws", "efp", "wdt", "fls", "adv", "inc_game"}

# (regex auf Pfad, klasse, grund) - erste passende Regel gewinnt
RULES = [
    # --- Spieldaten / spielabgeleitet ---------------------------------------
    (r"^Steel Battalion \(Europe\)\.xiso(/|\.iso$)", "private",
     "Spieldaten (Capcom): Disc-Image bzw. extrahierter XISO-Inhalt (LFS)"),
    (r"^xiso_listing\.txt$", "private", "Dateiliste des Disc-Images (Namen/Groessen)"),
    (r"^screenshots/", "private", "Spiel-Screenshots/Referenztexturen"),
    # W178 COMM-BG: eigenes Hintergrundmotiv (Eigentum des Projektinhabers) -
    # steht vor der generischen Medien-Regel (erste passende Regel gewinnt).
    (r"^poc-native/launcher/res/bg_nutzer\.(png|webp)$", "kern",
     "W178 COMM-BG: Launcher-Hintergrundbild (neues Motiv, Eigentum des "
     "Projektinhabers, Positivliste + assets_allow.tsv)"),
    # Nutzerentscheidung 7.11.: README-Vorschaubild der Community-Seite (einzige
    # Screenshot-Ausnahme, verkleinert, Link aufs Video; assets_allow.tsv).
    (r"^editions/community/media/cockpit\.jpg$", "kern",
     "README-Vorschaubild (Nutzerentscheidung 7.11., assets_allow.tsv)"),
    (r"^poc-native/launcher/res/convert_bg\.py$", "kern",
     "W178 COMM-BG: Konverter WebP->PNG (reproduziert das Community-Hintergrundbild)"),
    (r"^poc-native/harness/golden/", "private",
     "Golden-Referenzen (Spielbilder, Bild-Hashes, Pruefliste)"),
    (r"^poc-native/harness/hdd_seed/", "private", "Spielstaende/Xbox-Speicherbilder"),
    (r"^poc-native/analyse/[^/]+\.(tsv|json|txt)$", "private",
     "aus der XBE abgeleitete Tabellen/Reports (Funktionen, Strings, Xrefs)"),
    (r"^model-re/[^/]+\.(txt|json)$", "private",
     "Ausgaben aus Spieldaten (Modell-Header, Kataloge, Tabellen)"),
    (r"^poc-native/docs/[^/]+\.log$", "private", "Laufprotokoll (Spieltexte/Pfade)"),
    (r"^poc-native/docs/(VT_DATEN|MISSIONEN)\.md$", "private",
     "Spieldaten-Tabellen (aus XBE/Media gelesene Werte)"),
    (r"^poc-native/harness/scripts/(remaster_text|test_remaster_text)\.py$", "private",
     "Text-Rekonstruktion (W113): lokale Windows-Schriften, nur privat"),
    (r"\.(%s)$" % "|".join(sorted(EXT_MEDIA)), "private",
     "Bild/Audio/Video aus dem Spiel bzw. Laufbeleg"),
    # --- Infrastruktur --------------------------------------------------------
    (r"^orchestra/", "infra", "Steuerzentrale: Board, Discord, Worker-Briefs, Steuer-Agent"),
    (r"^\.claude/", "infra", "Agenten-Startkonfiguration"),
    (r"^tools/get_upscaler\.ps1$", "infra", "Upscaler-Download (Netzwerk)"),
    (r"^tools/", "infra", "Cxbx-Emulator-Strang: Fensterklick/-liste, Desktop-Shot, Profil"),
    (r"^(HANDOVER_ANSATZ1|PROMPT_ANSATZ2_NATIV)\.md$", "infra", "Agenten-Prompt/Handover"),
    (r"^poc-native/docs/PROMPT_MUSE_WEITERARBEIT\.md$", "infra", "Agenten-Prompt"),
    (r"^poc-native/harness/scripts/remaster_select\.py$", "infra",
     "liest Board-Feedback (orchestra/steuer/remaster_feedback.jsonl)"),
    (r"^\.git(ignore|attributes)$", "infra",
     "Regeln des privaten Repos (Whitelist, LFS fuer Spieldaten)"),
    # W180 EXPORT: im exportierten Community-Baum (kein editions/community/)
    # ist README.md die Community-README -> COMMUNITY_RULES (s. classify).
    (r"^README\.md$", "infra", "README des privaten Repos (beschreibt LFS-Spieldaten)"),
    (r"^DOKU_TASTATUR\.md$", "infra", "Cxbx-Tastaturprofil (Emulator-Strang)"),
    # --- Kern ---------------------------------------------------------------
    (r"^poc-native/harness/src/render_selftest_data\.h$", "private",
     "VSH-Mikrocode/PS-Definitionen des Spiels (Testvektoren aus W77_cen1; clean_check-Fund)"),
    (r"^poc-native/harness/src/sb_private_paths\.h$", "private",
     "Standardpfade der Private Edition (Rechnerpfade)"),
    (r"^poc-native/harness/src/ordinal_table\.inc$", "kern",
     "Xbox-Kernel-Schnittstelle (Ordinal, Name, Argumentzahl = Fakten); "
     "neu hergeleitet aus der eigenen XBE (W157, kein GPL-Inhalt)"),
    (r"^poc-native/harness/src/", "kern", "Client-Quellcode (HLE, Renderer, Audio, Eingabe)"),
    (r"^poc-native/lang/", "kern", "Spielertexte (eigene Uebersetzung, UTF-8)"),
    (r"^poc-native/harness/scripts/lang_(gen|check)\.py$", "kern",
     "Sprach-Build/Pruefung (eigener Code, keine Spieldaten)"),
    (r"^poc-native/harness/CMakeLists\.txt$", "kern", "Client-Build"),
    (r"^poc-native/harness/sb_native\.cfg$", "kern",
     "Client-Konfiguration der Tests (Replay, Turbo, Shots; nur Private Edition)"),
    (r"^poc-native/harness/sb_player\.cfg$", "kern",
     "W152: Spieler-Konfiguration (Community-Standard; keine Test-Voreinstellungen, "
     "leere Spielpfade; Export als sb_native.cfg)"),
    (r"^poc-native/harness/profiles/", "kern", "Eingabe-Profile (eigene)"),
    (r"^poc-native/harness/fixtures/sbc/", "kern", "Controller-Testdaten (eigene Aufzeichnung, Text)"),
    (r"^poc-native/launcher/(CMakeLists\.txt|src/)", "kern", "Launcher-Quellcode"),
    (r"^poc-native/tools/edition/", "kern", "Editions-Werkzeuge (Clean-Check, Paketierung)"),
    (r"^editions/", "kern", "Editions-Positivliste/Inventar"),
    (r"^poc-native/contrib/", "kern", "eigene Doku (Controller-Belegung)"),
    # W180 EXPORT: englische Spieler-Doku (W175) und Tester-Hinweise, eigener
    # Text ohne Spielbilder/Adressen -> Export (docs/community im Alpha-Paket).
    (r"^poc-native/docs/community/[^/]+\.md$", "kern",
     "englische Spieler-Doku/Tester-Hinweise (eigener Text, ohne Spielbilder)"),
    (r"^poc-native/client/", "unklar",
     "SDL2-PoC: Hand-Port einer Spielfunktion (FUN_00049410) - abgeleitet? Lizenz pruefen"),
    (r"^poc-native/harness/tests/", "kern", "Selbsttests (synthetische Daten)"),
    # W152 COMM-PATHS: eigene Werkzeuge, Rechnerpfade per Argument/Umgebung
    # bzw. aus der Repo-Wurzel abgeleitet, ohne Spielbytes, ohne Netz.
    # Laufzeit-Eingaben (Build, Spielstand, XBE, Medien) kommen per Argument
    # oder aus abgeleiteten Pfaden, nie als Literal; je Datei begruendet.
    (r"^poc-native/harness/scripts/run_once\.ps1$", "kern",
     "W152: Testlauf-Steuerung (Lock-/HDD-/Media-Pfade abgeleitet; braucht Build und Spielstand, repo-relativ)"),
    (r"^poc-native/harness/scripts/acceptance\.py$", "kern",
     "W152: Abnahme-Auswertung (Lock-Pfad abgeleitet; liest Lauf-Logs per Argument, ruft run_once auf)"),
    (r"^poc-native/harness/scripts/realinput\.py$", "kern",
     "W152: Eingabe-Nachstellung per PostMessage ans eigene Fenster (Client-/HDD-/Lock-Pfade abgeleitet)"),
    (r"^poc-native/harness/scripts/boot_loop(2)?\.ps1$", "kern",
     "W152: Legacy-Retry (Build-Ordner skriptrelativ bzw. SB_BUILD_DIR)"),
    (r"^poc-native/harness/scripts/film_audio_check\.py$", "kern",
     "W152: Audio-Analyse (Film-Ordner und ffmpeg per --media-dir/--ffmpeg bzw. SB_MEDIA_DIR/SB_FFMPEG)"),
    (r"^poc-native/harness/scripts/oracle/bink_ref\.py$", "kern",
     "W152: Bink-Referenz via ffmpeg (--ffmpeg-bin bzw. FFMPEG_BIN/SB_FFMPEG, sonst abgeleitet)"),
    (r"^poc-native/harness/scripts/oracle/movie_compare\.py$", "kern",
     "W152: Filmvergleich (kein Pfad-Literal; reicht --ffmpeg-bin an bink_ref durch)"),
    (r"^poc-native/harness/scripts/perf_bench\.py$", "kern",
     "W152: Leistungs-Benchmark (Lock-/Remaster-Pfade abgeleitet; setzt LOCK_PERF wie bisher)"),
    (r"^poc-native/harness/scripts/remaster_upscale\.py$", "kern",
     "W152: Upscale-Pipeline (Pfade schon Argumente; nur Doku-Beispiel ohne Rechnerpfad)"),
    (r"^poc-native/harness/scripts/remaster_ingame\.py$", "kern",
     "W152: In-Game-A/B-Paare (Pfade schon Argumente; nur Doku-Beispiel ohne Rechnerpfad)"),
    (r"^poc-native/harness/scripts/fx_pairs\.py$", "kern",
     "W152: Effekt-A/B-Paare (Pfade schon Argumente; nur Doku-Beispiel ohne Rechnerpfad)"),
    (r"^poc-native/analyse/vt_tabellen\.py$", "kern",
     "W152: VT-Tabellen lesend (--xbe/--media, Defaults abgeleitet; Ausgaben bleiben privat)"),
    (r"^poc-native/analyse/missions_tabellen\.py$", "kern",
     "W152: Missions-Tabellen lesend (--xbe/--media, Defaults abgeleitet; Ausgaben bleiben privat)"),
    (r"^poc-native/harness/scripts/(golden|milestones|run_parallel|"
     r"smoke|mission_sweep|campaign_check|state_check)\.(py|ps1)$", "unklar",
     "Test-/Abnahme-Werkzeug: setzt Golden-Bilder, Spielstaende, Lauf-Ordner voraus"),
    (r"^poc-native/tools/pack/", "unklar",
     "Paket-Werkzeuge (eigener Code; lesen Remaster-Ableitungen, kein Export)"),
    # W158 README-LEGAL: Pack Creator (W155) war ohne Regel (--check rot);
    # eigener Code, Export erst mit OFFLINE-DUMP/Phase 2 (Gewichte-Lizenz
    # offen, COMMUNITY_KONZEPT §7) - bis dahin unklar mit Begruendung.
    (r"^poc-native/creator/", "unklar",
     "Pack Creator (eigener Code; Export erst mit Phase 2, Gewichte-Lizenz offen)"),
    (r"^poc-native/tools/creator/", "unklar",
     "Pack-Creator-Pipeline (eigener Code; Export erst mit Phase 2, Gewichte-Lizenz offen)"),
    (r"^poc-native/harness/scripts/", "unklar",
     "Entwickler-/Diagnose-Skript (eigener Code; Bezug auf Laufordner/Spieldaten pruefen)"),
    (r"^poc-native/analyse/", "unklar",
     "Analyse-Skripte (eigener Code, lesen die XBE; Ausgaben sind privat)"),
    (r"^model-re/", "unklar",
     "Modell-RE-Skripte/Notizen (eigener Code, Format-Wissen aus Spieldaten)"),
    (r"^poc-native/(BUILD|BERICHT|STRATEGIE)\.md$", "unklar",
     "eigener Text, aber Arbeitsprotokoll mit Worker-/Board-Bezuegen, Rechnerpfaden, "
     "Spiel-Adressen - fuer Community nur redigiert"),
    (r"^poc-native/docs/(CONTROLLER|EINGABE|BUTTONMAP|FPS120)\.md$", "unklar",
     "Nutzer-/Technik-Doku (eigener Text) mit Adressen/Lauf-Tags - redigieren"),
    (r"^poc-native/docs/", "unklar",
     "Bericht/Arbeitsnotiz (eigener Text; Spiel-Adressen, Lauf-Tags, Worker-Bezuege)"),
]
RULES_C = [(re.compile(r), k, g) for r, k, g in RULES]

# W180 EXPORT: Regeln nur fuer den exportierten Community-Baum (erkannt am
# fehlenden editions/community/, der Quelle der Wurzel-Texte). Dort liegen
# die W158-Texte an der Wurzel; Staging-Marke und Manifest von
# package_edition gehoeren nicht ins Community-Repo (infra).
COMMUNITY_RULES = [
    (r"^(README|DONATE|COMMERCIAL|CONTRIBUTING|THIRD_PARTY)\.md$|^LICENSE$", "kern",
     "Community-Text an der Repo-Wurzel (Export von editions/community/, W158)"),
    (r"^(\.sb_edition_stage|EDITION\.txt)$", "infra",
     "Staging-Marke/Manifest von package_edition (im Community-Repo nicht einchecken)"),
]
COMMUNITY_RULES_C = [(re.compile(r), k, g) for r, k, g in COMMUNITY_RULES]
_community_tree = False

HIST_KIND = [
    (re.compile(r"\.(png|jpg|jpeg|bmp|dds|webp|gif|tga|xbx)$", re.I), "bild"),
    (re.compile(r"\.(wav|mp3|ogg)$", re.I), "audio"),
    (re.compile(r"\.(bik|mp4|avi)$", re.I), "video"),
    (re.compile(r"\.(xbe|xpr|iso|sav)$", re.I), "spieldatei"),
    (re.compile(r"^Steel Battalion \(Europe\)\.xiso/"), "spieldatei"),
    (re.compile(r"\.(bin|dat|exe|dll|pdb|7z|zip)$", re.I), "binaer"),
]


def classify(path):
    for rx, k, g in (COMMUNITY_RULES_C if _community_tree else []) + RULES_C:
        if rx.search(path):
            return k, g
    return "unklar", "keine Regel (neu?)"


def git(root, *args):
    return subprocess.check_output(["git", "-C", root] + list(args)).decode("utf-8", "replace")


def lfs_files(root, all_refs=False):
    try:
        out = git(root, "lfs", "ls-files", "--name-only", *(["--all"] if all_refs else []))
        return set(x.strip() for x in out.splitlines() if x.strip())
    except (OSError, subprocess.CalledProcessError):
        return set()


def sizes(root):
    """Blob-Groessen aus HEAD (LFS: Groesse aus dem Zeiger)."""
    out = git(root, "ls-tree", "-r", "-l", "-z", "HEAD")
    res = {}
    for rec in out.split("\0"):
        if not rec:
            continue
        meta, path = rec.split("\t", 1)
        parts = meta.split()
        try:
            res[path] = int(parts[3])
        except (IndexError, ValueError):
            res[path] = 0
    return res


def lfs_size(root, path):
    try:
        with open(os.path.join(root, path), "rb") as f:
            head = f.read(300).decode("ascii", "replace")
        m = re.search(r"^size (\d+)", head, re.M)
        return int(m.group(1)) if m else None
    except OSError:
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--md", action="store_true",
                    help="zusaetzlich Markdown-Zusammenfassung ausgeben (EDITIONEN.md)")
    a = ap.parse_args(argv)
    root = ec.repo_root()
    global _community_tree
    _community_tree = not os.path.isdir(os.path.join(root, "editions", "community"))
    files = sorted(set(git(root, "ls-files", "-z").split("\0")) - {""})
    files_now = ec.git_files(root)  # inkl. neue, nicht ignorierte Dateien
    files = sorted(set(files) | set(files_now))
    lfs = lfs_files(root)
    sz = sizes(root)
    rules = ec.load_include(os.path.join(root, "editions", "community.include"))
    sel = ec.select(files, rules)
    rows, bad = [], []
    stat = collections.Counter()
    stat_b = collections.Counter()
    for p in files:
        k, g = classify(p)
        is_lfs = p in lfs
        size = (lfs_size(root, p) if is_lfs else None) or sz.get(p)
        if size is None and os.path.isfile(os.path.join(root, p)):
            size = os.path.getsize(os.path.join(root, p))
        comm = "ja" if p in sel else "nein"
        if comm == "ja" and k != "kern":
            bad.append("Positivliste nimmt %s auf, Klasse %s" % (p, k))
        if g.startswith("keine Regel"):
            bad.append("ohne Regel: %s" % p)
        rows.append((p, k, g, "ja" if is_lfs else "nein", str(size or 0), comm))
        stat[k] += 1
        stat_b[k] += size or 0
    if a.check:
        for b in bad:
            print("FEHLER: " + b)
        print("inventory --check: %d Dateien, %s, %d Fehler" % (
            len(rows), dict(stat), len(bad)))
        return 1 if bad else 0
    out = os.path.join(root, "editions", "inventory.tsv")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# inventory.tsv - erzeugt von poc-native/tools/edition/inventory.py (W116)\n")
        f.write("# klasse: kern = community-faehig | private = spielabgeleitet/Nutzer | "
                "infra = Steuerung/Agenten/Netz | unklar = Begruendung\n")
        f.write("pfad\tklasse\tgrund\tlfs\tbytes\tcommunity_export\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    # Historie: jede je hinzugefuegte Datei + Commit/Datum der Aufnahme
    log = git(root, "log", "--all", "--diff-filter=A", "--name-only", "--reverse",
              "--format=@@%h %ad", "--date=short")
    first = {}
    cur = None
    for ln in log.splitlines():
        if ln.startswith("@@"):
            cur = ln[2:].strip()
        elif ln.strip() and ln not in first:
            first[ln] = cur
    lfs_all = lfs_files(root, all_refs=True)
    # --all nennt je LFS-Objekt nur einen Pfad (gleiche Inhalte zusammen) ->
    # zusaetzlich das filter-Attribut (.gitattributes) je Pfad abfragen
    names = sorted(set(first))
    try:
        r = subprocess.run(["git", "-C", root, "check-attr", "-z", "--stdin", "filter"],
                           input="\0".join(names).encode("utf-8"), capture_output=True,
                           check=True)
        parts = r.stdout.decode("utf-8", "replace").split("\0")
        for i in range(0, len(parts) - 2, 3):
            if parts[i + 2] == "lfs":
                lfs_all.add(parts[i])
    except (OSError, subprocess.CalledProcessError):
        pass
    head = set(files)
    hist = []
    for p in sorted(set(first) | lfs_all):
        kind = None
        for rx, kd in HIST_KIND:
            if rx.search(p):
                kind = kd
                break
        if kind is None and p in lfs_all:
            kind = "lfs"
        if kind is None:
            continue
        hist.append((p, kind, "ja" if p in lfs_all else "nein",
                     "ja" if p in head else "nein (geloescht)", first.get(p, "?")))
    hout = os.path.join(root, "editions", "history.tsv")
    with open(hout, "w", encoding="utf-8", newline="\n") as f:
        f.write("# history.tsv - spielabgeleitete/binaere Dateien, die je in der Git-Historie "
                "standen (alle Refs), erzeugt von inventory.py (W116)\n")
        f.write("pfad\tart\tlfs\tnoch_im_head\taufgenommen (commit datum)\n")
        for r in hist:
            f.write("\t".join(r) + "\n")
    hk = collections.Counter(r[1] for r in hist)
    gone = sum(1 for r in hist if r[3] != "ja")
    print("inventory: %d Dateien -> %s" % (len(rows), out))
    for k in ("kern", "private", "infra", "unklar"):
        print("  %-8s %5d Dateien %12d Bytes" % (k, stat[k], stat_b[k]))
    print("  community_export: %d Dateien" % sum(1 for r in rows if r[5] == "ja"))
    print("history: %d Eintraege (%s), davon %d nicht mehr im HEAD -> %s"
          % (len(hist), dict(hk), gone, hout))
    for b in bad:
        print("WARNUNG: " + b)
    if a.md:
        print("\n| Klasse | Dateien | Bytes | davon LFS | im Community-Export |")
        print("|---|---:|---:|---:|---:|")
        for k in ("kern", "private", "infra", "unklar"):
            rk = [r for r in rows if r[1] == k]
            print("| %s | %d | %s | %d | %d |" % (
                k, len(rk), format(sum(int(r[4]) for r in rk), ",").replace(",", "."),
                sum(1 for r in rk if r[3] == "ja"), sum(1 for r in rk if r[5] == "ja")))
        print("\n| Art | Eintraege | davon LFS | nicht mehr im HEAD |")
        print("|---|---:|---:|---:|")
        for kd in sorted(hk):
            rk = [r for r in hist if r[1] == kd]
            print("| %s | %d | %d | %d |" % (kd, len(rk), sum(1 for r in rk if r[2] == "ja"),
                                           sum(1 for r in rk if r[3] != "ja")))
        print("| **gesamt** | %d | %d | %d |" % (len(hist), sum(1 for r in hist if r[2] == "ja"),
                                                gone))
    return 0


if __name__ == "__main__":
    sys.exit(main())
