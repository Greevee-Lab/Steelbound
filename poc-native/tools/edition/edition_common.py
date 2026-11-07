"""edition_common.py - gemeinsame Helfer der Editions-Werkzeuge (W116).

Repo-Wurzel, versionierte Dateien, Positivliste (editions/community.include).
Keine Netzwerk-, keine Schreibzugriffe.
"""
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def repo_root():
    out = subprocess.check_output(["git", "-C", HERE, "rev-parse", "--show-toplevel"],
                                  text=True).strip()
    return os.path.abspath(out)


def main_checkout():
    """Wurzel des Haupt-Checkouts (bei Worktrees: Eltern von --git-common-dir)."""
    # W152: --path-format=absolute - --git-common-dir ist sonst relativ zum
    # -C-Verzeichnis (im Haupt-Checkout z. B. "../../../.git"); der alte
    # Join mit --show-toplevel lag dann falsch (C:/ statt Haupt-Checkout).
    common = subprocess.check_output(["git", "-C", HERE, "rev-parse",
                                      "--path-format=absolute",
                                      "--git-common-dir"],
                                     text=True).strip()
    return os.path.dirname(os.path.abspath(common))


def git_files(root):
    out = subprocess.check_output(["git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"])
    return [p for p in out.decode("utf-8").split("\0") if p]


def glob_to_re(pat):
    """Glob -> Regex: '**' beliebig tief, '*' ohne '/', '?' ein Zeichen."""
    i, out = 0, []
    while i < len(pat):
        c = pat[i]
        if pat.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pat.startswith("**", i):
            out.append(".*")
            i += 2
        elif c == "*":
            out.append("[^/]*")
            i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(c))
            i += 1
    return re.compile("^" + "".join(out) + "$")


def load_include(path):
    """-> Liste von Regeln (negiert, regex, muster, ziel|None)."""
    rules = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            ln = ln.split("#", 1)[0].strip() if not ln.lstrip().startswith("#") else ""
            if not ln:
                continue
            neg = ln.startswith("!")
            if neg:
                ln = ln[1:].strip()
            dest = None
            if "=>" in ln:
                ln, dest = (x.strip() for x in ln.split("=>", 1))
            rules.append((neg, glob_to_re(ln), ln, dest))
    return rules


def select(files, rules):
    """-> {quelle: ziel} fuer alle Dateien, die die Positivliste aufnimmt."""
    sel = {}
    for p in files:
        hit, dest = False, None
        for neg, rx, _pat, d in rules:
            if rx.match(p):
                hit, dest = (not neg), (d if not neg else None)
        if hit:
            sel[p] = dest or p
    return sel


def inside(path, root):
    path = os.path.normcase(os.path.abspath(path))
    root = os.path.normcase(os.path.abspath(root))
    return path == root or path.startswith(root.rstrip("\\/") + os.sep)
