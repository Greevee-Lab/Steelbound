#!/usr/bin/env python3
"""make_alpha.py - Alpha-Binaerpaket aus dem Community-Export bauen (W180).

  python make_alpha.py --work <ordner> [--name Steelbound-alpha1] [--zip <datei.zip>]
                       [--export <fertiger Export>] [--skip-build] [--no-first-start]
                       [--xbe PFAD] [--media auto|PFAD] [--allow-skip] [--force]

Ablauf (alles unter --work, das ausserhalb des Repos liegen muss):
  1. export/   package_edition.py --edition community (Positivliste +
               clean_check, 0 Funde Pflicht); mit --export ein fertiger Export.
  2. build/    CMake configure + Release-Build von Client (harness) und
               Launcher NUR aus dem Export (-A Win32, SB_EDITION=community);
               Logs build/*.log. Der Export darf nicht auf das Repo zeigen.
  3. stage/<name>/  Binaerpaket: sb_launcher.exe, sb_ldr.exe, sb_emu.dll,
               sb_native.cfg (Spieler-cfg, leere Spielpfade), profiles/,
               lang/, docs/ (englische Doku), README.md, LICENSE,
               THIRD_PARTY.md, VERSION.txt (Stand + sha256).
  4. Pruefung: nur erlaubte Dateien (feste Liste + profiles/*.ini,
               lang/*.lang, docs/*.md), keine Debug-/Lauf-Reste (.pdb .ilk
               .exp .lib .map .log .dmp, runs/ logs/ hdd/ ...), keine
               Spieldaten; danach clean_check.py auf dem Paket (0 Funde).
  5. <name>.zip (oder --zip) - nur ausserhalb des Repos, nie committen.
  6. Erststart (ohne --no-first-start): ZIP in firststart/ entpacken,
               portable.txt anlegen (frischer Datenordner im Programmordner),
               sb_launcher --userdata (Modus portable), --check (kein Spiel
               gesetzt -> Einrichtungskarte), --selftest (Client = Exe-
               Ordner, Exit 0), --ui-shot play (PNG der Spielen-Seite).

Keine Spiellaeufe, kein Netz, nichts wird veroeffentlicht.
Exit: 0 = ok, 1 = Pruefung/Build rot, 2 = clean_check unvollstaendig, 3 = Aufruf.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import edition_common as ec  # noqa: E402
import clean_check  # noqa: E402
import package_edition  # noqa: E402

MARK = ".sb_alpha_work"

# Paketinhalt: Ziel -> (Quelle relativ, Basis) ; Basis: build_h | build_l | export
FILES = [
    ("sb_launcher.exe", "build_l", "Release/sb_launcher.exe"),
    ("sb_ldr.exe", "build_h", "Release/sb_ldr.exe"),
    ("sb_emu.dll", "build_h", "Release/sb_emu.dll"),
    ("sb_native.cfg", "build_h", "Release/sb_native.cfg"),
    ("README.md", "export", "README.md"),
    ("LICENSE", "export", "LICENSE"),
    ("THIRD_PARTY.md", "export", "THIRD_PARTY.md"),
]
DIRS = [  # Ziel-Ordner, Basis, Quelle, erlaubtes Muster
    ("profiles", "build_h", "Release/profiles", r"[A-Za-z0-9_\-]+\.ini"),
    ("lang", "build_h", "Release/lang", r"[a-z]{2}\.lang"),
    ("docs", "export", "poc-native/docs/community", r"[A-Za-z0-9_\-]+\.md"),
]
EXTRA = ["VERSION.txt"]
# Verboten im Paket (zusaetzlich zur Positiv-Pruefung, fuer klare Meldungen)
BAD_EXT = {".pdb", ".ilk", ".exp", ".lib", ".map", ".obj", ".log", ".dmp", ".tmp",
           ".xbe", ".iso", ".xiso", ".bin", ".sav", ".xpr", ".bik", ".dds", ".png",
           ".jpg", ".bmp", ".wav", ".sbpack", ".zip", ".7z"}
BAD_DIRS = {"runs", "logs", "out", "hdd", "crash", "shadercache", "packs", "remaster",
            "golden", "hdd_seed", "media"}
BAD_NAMES = {"user.cfg", "portable.txt", "packs.json", ".env"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def log(msg):
    print("make_alpha: " + msg, flush=True)


def run(cmd, logfile, cwd=None, env=None):
    with open(logfile, "a", encoding="utf-8", errors="replace") as lf:
        lf.write("\n$ " + " ".join(cmd) + "\n")
        lf.flush()
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=lf, stderr=subprocess.STDOUT)
    return p.returncode


def prepare_work(work, roots, force):
    for r in roots:
        if ec.inside(work, r) or ec.inside(r, work):
            return "--work liegt im Repo oder umfasst es (%s)" % r
    if os.path.isdir(work) and os.listdir(work):
        if not os.path.isfile(os.path.join(work, MARK)):
            return "--work ist nicht leer und kein frueherer Lauf (Marke %s fehlt)" % MARK
        if not force:
            return "--work enthaelt einen frueheren Lauf - mit --force ersetzen"
        shutil.rmtree(work)
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, MARK), "w", encoding="utf-8") as f:
        f.write("Arbeitsordner von make_alpha.py (W180) - darf ersetzt werden\n")
    return None


def build(export, bdir, roots):
    """Client + Launcher nur aus dem Export bauen. -> (rc, [logs])"""
    os.makedirs(bdir, exist_ok=True)
    logs = []
    for comp in ("harness", "launcher"):
        src = os.path.join(export, "poc-native", comp)
        out = os.path.join(bdir, comp)
        lg = os.path.join(bdir, comp + ".log")
        logs.append(lg)
        if os.path.exists(lg):
            os.remove(lg)
        rc = run(["cmake", "-S", src, "-B", out, "-A", "Win32",
                  "-DSB_EDITION=community"], lg)
        if rc == 0:
            rc = run(["cmake", "--build", out, "--config", "Release", "--", "-m"], lg)
        if rc != 0:
            log("Build %s ROT (Log %s)" % (comp, lg))
            return 1, logs
        log("Build %s gruen (%s)" % (comp, lg))
    # Der Export-Build darf das Repo nicht kennen (CMake-Cache, Projekte)
    pats = [os.path.normcase(os.path.abspath(r)).replace("\\", "/") for r in roots]
    for dp, _dn, fn in os.walk(bdir):
        for n in fn:
            if n == "CMakeCache.txt" or n.endswith(".vcxproj"):
                with open(os.path.join(dp, n), encoding="utf-8", errors="replace") as f:
                    t = os.path.normcase(f.read()).replace("\\", "/")
                for p in pats:
                    if p in t:
                        log("Build kennt den Repo-Pfad %s (%s)" % (p, os.path.join(dp, n)))
                        return 1, logs
    return 0, logs


def assemble(stage, bases, commit):
    os.makedirs(stage, exist_ok=True)
    missing = []
    for dst, base, rel in FILES:
        s = os.path.join(bases[base], rel)
        if not os.path.isfile(s):
            missing.append(rel)
            continue
        shutil.copy2(s, os.path.join(stage, dst))
    for dst, base, rel, pat in DIRS:
        sd = os.path.join(bases[base], rel)
        if not os.path.isdir(sd):
            missing.append(rel + "/")
            continue
        os.makedirs(os.path.join(stage, dst), exist_ok=True)
        for n in sorted(os.listdir(sd)):
            if re.fullmatch(pat, n) and os.path.isfile(os.path.join(sd, n)):
                shutil.copy2(os.path.join(sd, n), os.path.join(stage, dst, n))
    rows = []
    for rel in list_files(stage):
        rows.append("%s  %s" % (sha256(os.path.join(stage, rel)), rel))
    with open(os.path.join(stage, "VERSION.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("Steelbound - Steel Battalion Game Preservation Project\n")
        f.write("Package: %s (Community Edition, binary)\n" % os.path.basename(stage))
        f.write("Built: %s from commit %s\n" % (time.strftime("%Y-%m-%d"), commit))
        f.write("No game data is included. You need your own copy of the game "
                "(European/PAL release).\n\nFiles (sha256):\n")
        f.write("\n".join(rows) + "\n")
    return missing


def list_files(root):
    out = []
    for dp, _dn, fn in os.walk(root):
        for n in fn:
            out.append(os.path.relpath(os.path.join(dp, n), root).replace("\\", "/"))
    return sorted(out)


def verify_stage(stage):
    """Positiv-Pruefung des Paketordners. -> Liste von Fehlern (leer = ok)."""
    errs = []
    allowed_files = {d for d, _b, _r in FILES} | set(EXTRA)
    dir_pat = {d: re.compile(p) for d, _b, _r, p in DIRS}
    for rel in list_files(stage):
        parts = rel.split("/")
        low = rel.lower()
        ext = os.path.splitext(low)[1]
        if ext in BAD_EXT:
            errs.append("verbotene Endung: " + rel)
            continue
        if any(p.lower() in BAD_DIRS for p in parts[:-1]):
            errs.append("verbotener Ordner: " + rel)
            continue
        if parts[-1].lower() in BAD_NAMES:
            errs.append("verbotene Datei: " + rel)
            continue
        if len(parts) == 1:
            if rel not in allowed_files:
                errs.append("nicht vorgesehen: " + rel)
        elif len(parts) == 2 and parts[0] in dir_pat:
            if not dir_pat[parts[0]].fullmatch(parts[1]):
                errs.append("nicht vorgesehen: " + rel)
        else:
            errs.append("nicht vorgesehen: " + rel)
    have = set(list_files(stage))
    for d, _b, _r in FILES:
        if d not in have:
            errs.append("fehlt: " + d)
    for d, _b, _r, _p in DIRS:
        if not any(h.startswith(d + "/") for h in have):
            errs.append("fehlt (leer): " + d + "/")
    return errs


def check_view(stage, view):
    """Pruefansicht fuer clean_check: gleiche Bytes, Binaries (exe/dll) aber
    unter bin/ wie im Quell-Export (package_edition --with-build), damit die
    begruendeten Freigaben aus editions/clean_allow.tsv (bin/<name>) gelten.
    Das Paket selbst behaelt die Binaries an der Wurzel (Launcher neben
    sb_ldr.exe)."""
    if os.path.isdir(view):
        shutil.rmtree(view)
    for rel in list_files(stage):
        dst = rel
        if "/" not in rel and os.path.splitext(rel)[1].lower() in (".exe", ".dll"):
            dst = "bin/" + rel
        d = os.path.join(view, dst)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(os.path.join(stage, rel), d)
    return view


def make_zip(stage, zpath):
    name = os.path.basename(stage)
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in list_files(stage):
            z.write(os.path.join(stage, rel), name + "/" + rel)
    return zpath


def clean_env():
    env = dict(os.environ)
    for k in list(env):
        if k.upper().startswith("SB_"):
            del env[k]
    return env


def first_start(zpath, fdir, name):
    """Entpacken, portable.txt, Launcher-Pruefungen. -> (ok, zeilen)"""
    lines = []
    if os.path.isdir(fdir):
        shutil.rmtree(fdir)
    os.makedirs(fdir)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(fdir)
    app = os.path.join(fdir, name)
    open(os.path.join(app, "portable.txt"), "w").close()
    exe = os.path.join(app, "sb_launcher.exe")
    env = clean_env()
    prof = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Steelbound")
    prof_before = os.path.isdir(prof) and set(os.listdir(prof))
    ok = True

    def call(args, timeout=600):
        p = subprocess.run([exe] + args, cwd=app, env=env, capture_output=True,
                           timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace").replace("\r\n", "\n")

    rc, out = call(["--userdata"])
    port = ("Modus: portable" in out) and (os.path.normcase(app) in os.path.normcase(out))
    lines.append("--userdata: rc=%d, Modus portable im Programmordner: %s" % (
        rc, "ja" if port else "NEIN"))
    ok &= port
    rc, out = call(["--check"])
    nogame = ("No game data set" in out) or ("Keine Spieldaten" in out)
    first = [ln for ln in out.splitlines() if ln.strip()][-3:]
    lines.append("--check: rc=%d (erwartet !=0: kein Spiel gesetzt -> Einrichtungskarte); %s"
                 % (rc, " | ".join(first)))
    ok &= (rc != 0 and nogame)
    rc, out = call(["--selftest"])
    m = re.search(r"selftest: (\d+) PASS, (\d+) FAIL", out)
    lines.append("--selftest (Client = Exe-Ordner): rc=%d, %s" % (
        rc, m.group(0) if m else "keine Zusammenfassung"))
    ok &= (rc == 0 and bool(m) and m.group(2) == "0")
    shots = os.path.join(fdir, "shots")
    os.makedirs(shots, exist_ok=True)
    rc, out = call(["--ui-shot", "play", "--shot-out", shots])
    pngs = [n for n in os.listdir(shots) if n.lower().endswith(".png")]
    lines.append("--ui-shot play: rc=%d, %s" % (rc, ", ".join(pngs) or "kein PNG"))
    ok &= (rc == 0 and bool(pngs))
    prof_after = os.path.isdir(prof) and set(os.listdir(prof))
    untouched = prof_before == prof_after
    lines.append("Profil-Datenordner %s unveraendert: %s" % (prof, "ja" if untouched else "NEIN"))
    ok &= untouched
    return ok, lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True)
    ap.add_argument("--name", default="Steelbound-alpha1")
    ap.add_argument("--zip")
    ap.add_argument("--export", help="fertiger Community-Export (statt neu exportieren)")
    ap.add_argument("--skip-build", action="store_true",
                    help="vorhandenen Build unter <work>/build nutzen")
    ap.add_argument("--no-first-start", action="store_true")
    ap.add_argument("--xbe", action="append")
    ap.add_argument("--media")
    ap.add_argument("--allow-skip", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9._\-]+", a.name):
        print("FEHLER: --name nur aus Buchstaben, Ziffern, . _ -", file=sys.stderr)
        return 3
    root = ec.repo_root()
    roots = [root, ec.main_checkout()]
    work = os.path.abspath(a.work)
    if not a.skip_build:
        err = prepare_work(work, roots, a.force)
        if err:
            print("FEHLER: " + err, file=sys.stderr)
            return 3
    zpath = os.path.abspath(a.zip or os.path.join(work, a.name + ".zip"))
    for r in roots:
        if ec.inside(zpath, r):
            print("FEHLER: --zip liegt im Repo (%s) - nie committen" % r, file=sys.stderr)
            return 3
    xargs = []
    for x in (a.xbe or []):
        xargs += ["--xbe", x]
    if a.media:
        xargs += ["--media", a.media]
    # 1. Export
    export = os.path.abspath(a.export) if a.export else os.path.join(work, "export")
    if not a.export and not a.skip_build:
        rc = package_edition.main(["--edition", "community", "--out", export, "--force",
                                   "--report", os.path.join(work, "export.clean_check.md")]
                                  + xargs + (["--allow-skip"] if a.allow_skip else []))
        if rc != 0:
            log("Export ROT (Exit %d)" % rc)
            return rc
    if not os.path.isdir(export):
        print("FEHLER: kein Export unter " + export, file=sys.stderr)
        return 3
    # 2. Build aus dem Export
    bdir = os.path.join(work, "build")
    if not a.skip_build:
        rc, _logs = build(export, bdir, roots)
        if rc != 0:
            return 1
    # 3. Paket
    stage = os.path.join(work, "stage", a.name)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    bases = {"build_h": os.path.join(bdir, "harness"),
             "build_l": os.path.join(bdir, "launcher"), "export": export}
    missing = assemble(stage, bases, package_edition.git_head(root))
    if missing:
        log("fehlende Quellen: " + ", ".join(missing))
    # 4. Pruefung
    errs = verify_stage(stage)
    for e in errs:
        log("PAKET-FEHLER " + e)
    if errs:
        return 1
    rep = os.path.join(work, "stage.clean_check.md")
    rc = clean_check.main([check_view(stage, os.path.join(work, "stage_check")),
                           "--report", rep] + xargs)
    if rc == 1 or (rc == 2 and not a.allow_skip):
        log("clean_check auf dem Paket: Exit %d (%s)" % (rc, rep))
        return 1 if rc == 1 else 2
    # 5. ZIP
    make_zip(stage, zpath)
    total = 0
    log("Inhalt %s:" % os.path.basename(zpath))
    with zipfile.ZipFile(zpath) as z:
        for i in z.infolist():
            total += i.file_size
            print("  %10d  %s" % (i.file_size, i.filename))
    log("ZIP %s: %d Bytes (entpackt %d Bytes, %d Dateien)" % (
        zpath, os.path.getsize(zpath), total, len(list_files(stage))))
    # 6. Erststart
    if not a.no_first_start:
        ok, lines = first_start(zpath, os.path.join(work, "firststart"), a.name)
        for ln in lines:
            log("Erststart " + ln)
        if not ok:
            log("Erststart ROT")
            return 1
        log("Erststart gruen")
    return 0


if __name__ == "__main__":
    sys.exit(main())
