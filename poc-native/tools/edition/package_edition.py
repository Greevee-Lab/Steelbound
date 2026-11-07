#!/usr/bin/env python3
"""package_edition.py - Staging einer Edition ausserhalb des Repos (W116).

  python package_edition.py --edition community --out <dir> [--with-build DIR]
                            [--with-launcher DIR] [--force] [--allow-skip]
                            [--report <datei.md>] [--xbe PFAD] [--media auto|PFAD]
  python package_edition.py --edition private --out <dir> [--with-build DIR]
                            [--with-launcher DIR] [--remaster DIR] [--force]

community: nur versionierte Dateien aus editions/community.include (Glob-
  Positivliste), sb_native.cfg mit geleerten Spielpfaden, optional die
  Binaries eines Community-Builds (bin/). Danach IMMER clean_check.py; bei
  einem Fund (Exit 1) bricht die Paketierung ab, ebenso bei uebersprungener
  Pruefung (Exit 2), ausser --allow-skip. Bericht: <out>.clean_check.md/.json
  (neben dem Staging, nicht darin).
private: lokal lauffaehige Fassung (client/: Binaries + sb_native.cfg +
  profiles/ + Launcher, docs/: Doku). Das Remaster-Paket wird NIE kopiert,
  nur als Pfad in REMASTER_PAKET.txt verwiesen.

--out muss ausserhalb des Repos (Worktree und Haupt-Checkout) liegen. Ein
vorhandenes, nicht leeres Ziel wird nur mit --force und nur dann geleert,
wenn es die Marke .sb_edition_stage traegt (frueheres Staging).
Exit: 0 = ok, 1 = Funde/Abbruch, 2 = Pruefung unvollstaendig, 3 = Aufruffehler.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import edition_common as ec  # noqa: E402
import clean_check  # noqa: E402

MARK = ".sb_edition_stage"
CLIENT_BINS = ["sb_ldr.exe", "sb_emu.dll", "input_selftest.exe",
               "render_selftest.exe", "sbc_probe.exe"]
# W152 COMM-SELFTEST: community baut render_selftest wieder (Vektoren zur
# Laufzeit aus der eigenen XBE, ohne eingebettete Spielbytes).
CLIENT_BINS_COMMUNITY = ["sb_ldr.exe", "sb_emu.dll", "input_selftest.exe",
                         "render_selftest.exe", "sbc_probe.exe"]
LAUNCHER_BINS = ["sb_launcher.exe"]
PRIVATE_EXTRA = ["sb_emu.pdb", "sb_emu.map"]  # nur private (Symbolisierung)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def fail(msg, rc=3):
    print("FEHLER: " + msg, file=sys.stderr)
    return rc


def prepare_out(out, roots, force):
    for r in roots:
        if ec.inside(out, r) or ec.inside(r, out):
            return "--out liegt im Repo oder umfasst es (%s) - Ziel ausserhalb waehlen" % r
    if os.path.isdir(out) and os.listdir(out):
        if not os.path.isfile(os.path.join(out, MARK)):
            return "--out ist nicht leer und kein frueheres Staging (Marke %s fehlt)" % MARK
        if not force:
            return "--out enthaelt ein frueheres Staging - mit --force ersetzen"
        shutil.rmtree(out)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, MARK), "w", encoding="utf-8") as f:
        f.write("Staging von package_edition.py (W116) - darf ersetzt werden\n")
    return None


def strip_cfg_paths(src, dst):
    """sb_native.cfg ohne private Spielpfade (Spieler waehlt im Launcher)."""
    out = []
    with open(src, encoding="utf-8", errors="replace") as f:
        for ln in f:
            k = ln.split("=", 1)[0].strip()
            if k in ("xbe_path", "media_root") and not ln.lstrip().startswith("#"):
                out.append("# W116 Community: Pfad zur eigenen default.xbe bzw. zum "
                           "eigenen XISO-Ordner setzt der Launcher (user.cfg)\n")
                out.append(k + "=\n")
            else:
                out.append(ln)
    with open(dst, "w", encoding="utf-8", newline="") as f:
        f.writelines(out)


def git_head(root):
    try:
        h = subprocess.check_output(["git", "-C", root, "rev-parse", "--short", "HEAD"],
                                    text=True).strip()
        dirty = subprocess.check_output(["git", "-C", root, "status", "--porcelain"],
                                        text=True).strip()
        return h + ("+lokal" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "?"


def write_manifest(out, edition, commit, extra=None):
    rows = []
    for dp, _dn, fn in os.walk(out):
        for n in sorted(fn):
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, out).replace("\\", "/")
            if rel in (MARK, "EDITION.txt"):
                continue
            rows.append("%s  %s" % (sha256(p), rel))
    with open(os.path.join(out, "EDITION.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("Steel Battalion - Game Preservation Project\n")
        f.write("Edition: %s\nStand: %s (Commit %s)\n" % (
            edition, time.strftime("%Y-%m-%d"), commit))
        for ln in (extra or []):
            f.write(ln + "\n")
        f.write("\nDateien (sha256):\n")
        f.write("\n".join(sorted(rows, key=lambda r: r[66:])) + "\n")


def copy_bins(src_dir, names, dst_dir, missing):
    os.makedirs(dst_dir, exist_ok=True)
    n = 0
    for b in names:
        p = os.path.join(src_dir, b)
        if os.path.isfile(p):
            shutil.copy2(p, os.path.join(dst_dir, b))
            n += 1
        else:
            missing.append(b)
    return n


def community(a, root):
    rules = ec.load_include(os.path.join(root, "editions", "community.include"))
    sel = ec.select(ec.git_files(root), rules)
    n = 0
    for src, dst in sorted(sel.items()):
        s = os.path.join(root, src)
        d = os.path.join(a.out, dst)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        if not os.path.isfile(s):
            continue  # versioniert, aber lokal geloescht
        # W152 COMM-CFG: die Export-cfg (sb_player.cfg => sb_native.cfg) immer
        # mit geleerten Spielpfaden (der Spieler waehlt im Launcher).
        if dst == "poc-native/harness/sb_native.cfg":
            strip_cfg_paths(s, d)
        else:
            shutil.copyfile(s, d)
        n += 1
    print("community: %d Dateien aus der Positivliste" % n)
    missing = []
    if a.with_build:
        nb = copy_bins(a.with_build, CLIENT_BINS_COMMUNITY, os.path.join(a.out, "bin"),
                       missing)
        print("community: %d Client-Binaries aus %s" % (nb, a.with_build))
    if a.with_launcher:
        nb = copy_bins(a.with_launcher, LAUNCHER_BINS, os.path.join(a.out, "bin"), missing)
        print("community: %d Launcher-Binaries aus %s" % (nb, a.with_launcher))
    if missing:
        print("  fehlend (nicht gebaut?): " + ", ".join(missing))
    write_manifest(a.out, "community", git_head(root))
    report = a.report or (a.out.rstrip("\\/") + ".clean_check.md")
    cargs = [a.out, "--report", report]
    for x in (a.xbe or []):
        cargs += ["--xbe", x]
    if a.media:
        cargs += ["--media", a.media]
    rc = clean_check.main(cargs)
    if rc == 0:
        print("community: Staging sauber -> %s" % a.out)
        return 0
    if rc == 2 and a.allow_skip:
        print("community: WARNUNG Pruefung unvollstaendig (--allow-skip) -> %s" % a.out)
        return 0
    with open(os.path.join(a.out, "CLEAN_CHECK_FEHLGESCHLAGEN.txt"), "w",
              encoding="utf-8") as f:
        f.write("clean_check Exit %d - dieses Staging ist NICHT veroeffentlichbar.\n"
                "Bericht: %s\n" % (rc, os.path.basename(report)))
    print("community: ABBRUCH (clean_check Exit %d), Bericht %s" % (rc, report))
    return 1 if rc == 1 else 2


def private(a, root):
    build = a.with_build or os.path.join(root, "poc-native", "harness", "build", "Release")
    launcher = a.with_launcher or os.path.join(root, "poc-native", "launcher", "build",
                                               "Release")
    cdir = os.path.join(a.out, "client")
    missing = []
    nb = copy_bins(build, CLIENT_BINS + PRIVATE_EXTRA, cdir, missing)
    nl = copy_bins(launcher, LAUNCHER_BINS, cdir, missing)
    shutil.copyfile(os.path.join(root, "poc-native", "harness", "sb_native.cfg"),
                    os.path.join(cdir, "sb_native.cfg"))
    shutil.copytree(os.path.join(root, "poc-native", "harness", "profiles"),
                    os.path.join(cdir, "profiles"))
    ddir = os.path.join(a.out, "docs")
    os.makedirs(ddir, exist_ok=True)
    nd = 0
    for rel in ec.git_files(root):
        if rel.endswith(".md") and (rel.startswith("poc-native/docs/") or
                                    rel.count("/") == 1 and rel.startswith("poc-native/")
                                    or rel == "README.md"):
            shutil.copyfile(os.path.join(root, rel), os.path.join(ddir, os.path.basename(rel)))
            nd += 1
    print("private: %d Client-, %d Launcher-Dateien, %d Doku-Dateien" % (nb, nl, nd))
    if missing:
        print("  fehlend (nicht gebaut?): " + ", ".join(missing))
    # Remaster-Paket nur verweisen, nie kopieren
    rdir = a.remaster
    if not rdir:
        for r in (root, ec.main_checkout()):
            cand = os.path.join(r, "remaster")
            if os.path.isdir(cand):
                rdir = cand
                break
    lines = []
    if rdir and os.path.isdir(rdir):
        rdir = os.path.abspath(rdir)
        with open(os.path.join(a.out, "REMASTER_PAKET.txt"), "w", encoding="utf-8") as f:
            f.write("Remaster-Paket (HD-Texturen, Text-Rekonstruktionen) wird NICHT\n"
                    "kopiert, nur verwiesen. Im Client per user.cfg setzen:\n\n"
                    "remaster_dir=%s\n" % rdir.replace("\\", "/"))
        lines.append("Remaster-Paket: verwiesen (REMASTER_PAKET.txt), nicht kopiert")
    else:
        lines.append("Remaster-Paket: keins gefunden (--remaster)")
    # Absicherung: nichts aus dem Remaster-Ordner im Staging
    if rdir:
        for dp, _dn, fn in os.walk(a.out):
            if ec.inside(dp, rdir):
                return fail("Remaster-Inhalt im Staging - Abbruch", 1)
    write_manifest(a.out, "private", git_head(root), lines)
    print("private: Staging -> %s (%s)" % (a.out, lines[-1]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--edition", required=True, choices=["community", "private"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--with-build", help="Client-Buildordner (Release)")
    ap.add_argument("--with-launcher", help="Launcher-Buildordner (Release)")
    ap.add_argument("--remaster", help="private: Remaster-Ordner (nur verweisen)")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--allow-skip", action="store_true")
    ap.add_argument("--report")
    ap.add_argument("--xbe", action="append")
    ap.add_argument("--media")
    a = ap.parse_args(argv)
    root = ec.repo_root()
    a.out = os.path.abspath(a.out)
    err = prepare_out(a.out, [root, ec.main_checkout()], a.force)
    if err:
        return fail(err)
    return community(a, root) if a.edition == "community" else private(a, root)


if __name__ == "__main__":
    sys.exit(main())
