#!/usr/bin/env python3
"""build_release.py - build the Steelbound Windows release package from this repository.

Used by the GitHub Actions workflow .github/workflows/build.yml and runnable on any
Windows machine with Visual Studio (MSVC, Win32), CMake >= 3.24, Python 3 and git:

  python poc-native/tools/ci/build_release.py --out <dir> [--version X.Y.Z|dev]
         [--generator "Visual Studio 17 2022"] [--toolset v143] [--sdk 10.0.26100.0]
         [--probe] [--no-first-start] [--repro] [--skip-build] [--strict-ui]

Steps (everything below --out, which must be outside the source tree):
  1. build/      CMake configure + Release build of the client (poc-native/harness) and
                 the launcher (poc-native/launcher), -A Win32, SB_EDITION=community,
                 SB_LOC_PLAY=ON, SB_VERSION=<version>. Also a test-only launcher with
                 the built-in self test (-DSB_LAUNCHER_SELFTEST=ON); it is never shipped.
  2. stage/<name>/  package folder: sb_launcher.exe, sb_ldr.exe, sb_emu.dll, sb_native.cfg,
                 profiles/, lang/, docs/, README.md, LICENSE, THIRD_PARTY.md, VERSION.txt.
  3. checks:     only the expected files (allow list), no debug or run leftovers, PE
                 hardening flags (poc-native/tools/sec/pe_harden_check.py, if present).
  4. <name>.zip  deterministic ZIP (sorted entries, timestamps = commit date).
  5. SHA256SUMS.txt (every packaged file + the ZIP) and BUILDINFO.txt (toolchain).
  6. first start (unless --no-first-start): unpack the ZIP, run the launcher in portable
                 mode (--userdata, --check, --selftest, --ui-shot play) without game data.

--probe: quick check build (client + launcher, package checks, no ZIP, no first start).
--repro: experimental, adds /Brepro (compiler + linker) for reproducible PE/PDB output.
Exit: 0 = ok, 1 = build or check failed, 3 = usage.
No network access, nothing is published.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# Package content: target -> (base, source path relative to base); base: build_h | build_l | src
FILES = [
    ("sb_launcher.exe", "build_l", "Release/sb_launcher.exe"),
    ("sb_ldr.exe", "build_h", "Release/sb_ldr.exe"),
    ("sb_emu.dll", "build_h", "Release/sb_emu.dll"),
    ("sb_native.cfg", "build_h", "Release/sb_native.cfg"),
    ("README.md", "src", "README.md"),
    ("LICENSE", "src", "LICENSE"),
    ("THIRD_PARTY.md", "src", "THIRD_PARTY.md"),
]
DIRS = [  # target folder, base, source, allowed file name pattern
    ("profiles", "build_h", "Release/profiles", r"[A-Za-z0-9_\-]+\.ini"),
    ("lang", "build_h", "Release/lang", r"[a-z]{2}\.lang"),
    ("docs", "src", "poc-native/docs/community", r"(?!ALPHA_TESTING\.md$)[A-Za-z0-9_\-]+\.md"),
]
EXTRA = ["VERSION.txt"]
BAD_EXT = {".pdb", ".ilk", ".exp", ".lib", ".map", ".obj", ".log", ".dmp", ".tmp",
           ".xbe", ".iso", ".xiso", ".bin", ".sav", ".xpr", ".bik", ".dds", ".png",
           ".jpg", ".bmp", ".wav", ".sbpack", ".zip", ".7z"}
BAD_DIRS = {"runs", "logs", "out", "hdd", "crash", "shadercache", "packs", "remaster",
            "golden", "hdd_seed", "media"}
BAD_NAMES = {"user.cfg", "portable.txt", "packs.json", ".env"}
COMPONENTS = (("harness", "harness", []), ("launcher", "launcher", []),
              ("launcher_selftest", "launcher", ["-DSB_LAUNCHER_SELFTEST=ON"]))
SEMVER_RE = r"\d{1,9}\.\d{1,9}\.\d{1,9}(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z\-]+)*)?(?:\+[0-9A-Za-z.\-]+)?"
MARK = ".sb_ci_work"


def log(msg):
    print("build_release: " + msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def inside(path, root):
    p = os.path.normcase(os.path.abspath(path))
    r = os.path.normcase(os.path.abspath(root))
    return p == r or p.startswith(r.rstrip("\\/") + os.sep)


def git(*args):
    try:
        return subprocess.check_output(["git", "-C", ROOT] + list(args), text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def commit_info():
    """-> (short sha, commit date YYYY-MM-DD, unix time). GITHUB_SHA wins if set."""
    sha = os.environ.get("GITHUB_SHA") or git("rev-parse", "HEAD") or "?"
    ts = git("log", "-1", "--format=%ct")
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH") or ts or time.time())
    return sha[:8], time.strftime("%Y-%m-%d", time.gmtime(epoch)), epoch


def central_version():
    try:
        with open(os.path.join(ROOT, "poc-native", "VERSION"), encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def release_stage(v):
    """Phase like sb_version.h: 0.x without suffix = Beta, -alpha/-beta/-rc, from 1.0.0 none."""
    m = re.fullmatch(r"v?(\d+)\.\d+\.\d+(?:-([^+]*))?(?:\+.*)?", v or "")
    if not m:
        return ""
    pre = m.group(2) or ""
    for k, n in (("alpha", "Alpha"), ("beta", "Beta"), ("rc", "RC")):
        if k in pre:
            return n
    return "Beta" if int(m.group(1)) == 0 else ""


def run(cmd, logfile, env=None, cwd=None):
    with open(logfile, "a", encoding="utf-8", errors="replace") as lf:
        lf.write("\n$ " + " ".join(cmd) + "\n")
        lf.flush()
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=lf, stderr=subprocess.STDOUT)
    return p.returncode


def tail(path, n=40):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return "".join(f.readlines()[-n:])
    except OSError:
        return ""


def build(bdir, version, a):
    os.makedirs(bdir, exist_ok=True)
    env = dict(os.environ)
    if a.repro:  # CMake reads CFLAGS/CXXFLAGS/LDFLAGS once at configure time
        for k in ("CFLAGS", "CXXFLAGS", "LDFLAGS"):
            env[k] = (env.get(k, "") + " /Brepro").strip()
    comps = COMPONENTS[:2] if a.probe else COMPONENTS
    for name, srcdir, extra in comps:
        src = os.path.join(ROOT, "poc-native", srcdir)
        out = os.path.join(bdir, name)
        lg = os.path.join(bdir, name + ".log")
        if os.path.exists(lg):
            os.remove(lg)
        cfg = ["cmake", "-S", src, "-B", out, "-A", "Win32"]
        if a.generator:
            cfg += ["-G", a.generator]
        if a.toolset:
            cfg += ["-T", a.toolset]
        if a.sdk:
            cfg += ["-DCMAKE_SYSTEM_VERSION=" + a.sdk]
        cfg += ["-DSB_EDITION=community", "-DSB_VERSION=" + version, "-DSB_LOC_PLAY=ON"] + extra
        t0 = time.time()
        rc = run(cfg, lg, env)
        if rc == 0:
            rc = run(["cmake", "--build", out, "--config", "Release", "--", "-m"], lg, env)
        if rc != 0:
            log("build %s FAILED (log %s), last lines:" % (name, lg))
            print(tail(lg), flush=True)
            return 1
        log("build %s ok in %ds" % (name, time.time() - t0))
    return 0


def list_files(root):
    out = []
    for dp, _dn, fn in os.walk(root):
        for n in fn:
            out.append(os.path.relpath(os.path.join(dp, n), root).replace("\\", "/"))
    return sorted(out)


def assemble(stage, bases, commit, date, version):
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
    rows = ["%s  %s" % (sha256(os.path.join(stage, rel)), rel) for rel in list_files(stage)]
    with open(os.path.join(stage, "VERSION.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("Steelbound - Steel Battalion Game Preservation Project\n")
        f.write("Package: %s (Community Edition, binary)\n" % os.path.basename(stage))
        st = release_stage(version)
        f.write("Version: %s%s\n" % (version or "dev", " (%s)" % st if st else ""))
        f.write("Built: %s from commit %s\n" % (date, commit))
        f.write("No game data is included. You need your own copy of the game "
                "(European/PAL release).\n\nFiles (sha256):\n")
        f.write("\n".join(rows) + "\n")
    return missing


def verify_stage(stage):
    """Allow-list check of the package folder. -> list of errors (empty = ok)."""
    errs = []
    allowed_files = {d for d, _b, _r in FILES} | set(EXTRA)
    dir_pat = {d: re.compile(p) for d, _b, _r, p in DIRS}
    for rel in list_files(stage):
        parts = rel.split("/")
        ext = os.path.splitext(rel.lower())[1]
        if ext in BAD_EXT:
            errs.append("forbidden extension: " + rel)
        elif any(p.lower() in BAD_DIRS for p in parts[:-1]):
            errs.append("forbidden folder: " + rel)
        elif parts[-1].lower() in BAD_NAMES:
            errs.append("forbidden file: " + rel)
        elif len(parts) == 1:
            if rel not in allowed_files:
                errs.append("unexpected: " + rel)
        elif len(parts) == 2 and parts[0] in dir_pat:
            if not dir_pat[parts[0]].fullmatch(parts[1]):
                errs.append("unexpected: " + rel)
        else:
            errs.append("unexpected: " + rel)
    have = set(list_files(stage))
    errs += ["missing: " + d for d, _b, _r in FILES if d not in have]
    errs += ["missing (empty): %s/" % d for d, _b, _r, _p in DIRS
             if not any(h.startswith(d + "/") for h in have)]
    return errs


def harden_errors(stage):
    """PE hardening policy (ASLR/DEP/CFG/GS/sdl, DependentLoadFlags) if the checker is present."""
    sec = os.path.join(ROOT, "poc-native", "tools", "sec")
    if not os.path.isfile(os.path.join(sec, "pe_harden_check.py")):
        log("note: pe_harden_check.py not present, hardening check skipped")
        return []
    sys.path.insert(0, sec)
    import pe_harden_check  # noqa: E402
    errs = []
    for n in sorted(pe_harden_check.POLICY):
        p = os.path.join(stage, n)
        if not os.path.isfile(p):
            continue
        try:
            bad = pe_harden_check.check(pe_harden_check.analyze(p))
        except (OSError, ValueError) as e:
            bad = ["unreadable: %s" % e]
        errs += ["hardening %s: %s" % (n, b) for b in bad]
    return errs


def make_zip(stage, zpath, epoch):
    """Deterministic ZIP: sorted entries, fixed timestamps (commit date), fixed attributes."""
    name = os.path.basename(stage)
    dt = time.gmtime(max(epoch, 315532800))[:6]  # ZIP cannot store dates before 1980
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in list_files(stage):
            zi = zipfile.ZipInfo(name + "/" + rel, dt)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o100644 << 16
            zi.create_system = 0
            with open(os.path.join(stage, rel), "rb") as f:
                z.writestr(zi, f.read(), compresslevel=9)
    return zpath


def cache_value(cache, key):
    try:
        with open(cache, encoding="utf-8", errors="replace") as f:
            for ln in f:
                if ln.startswith(key + ":"):
                    return ln.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


def vs_version(instance=""):
    """Visual Studio product + version; instance = CMAKE_GENERATOR_INSTANCE (the one CMake used)."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    # -path is not compatible with other selection options (-latest, -products)
    sel = ["-path", os.path.normpath(instance)] if instance else ["-latest", "-products", "*"]
    try:
        out = subprocess.check_output([vswhere] + sel + ["-format", "json", "-utf8"],
                                      stderr=subprocess.DEVNULL).decode("utf-8", "replace")
        d = (json.loads(out) or [{}])[0]
        return "%s %s" % (d.get("displayName", "?"), d.get("installationVersion", "?"))
    except (OSError, subprocess.CalledProcessError, ValueError):
        return "?"


def compiler_version(bdir):
    """MSVC version from the CMake compiler probe of the client build."""
    for dp, _dn, fn in os.walk(os.path.join(bdir, "harness", "CMakeFiles")):
        if "CMakeCCompiler.cmake" in fn:
            with open(os.path.join(dp, "CMakeCCompiler.cmake"), encoding="utf-8", errors="replace") as f:
                m = re.search(r'set\(CMAKE_C_COMPILER_VERSION "([^"]+)"\)', f.read())
            return m.group(1) if m else "?"
    return "?"


def sdk_version(bdir):
    """Windows SDK the Visual Studio projects target (WindowsTargetPlatformVersion)."""
    try:
        with open(os.path.join(bdir, "harness", "sb_emu.vcxproj"), encoding="utf-8", errors="replace") as f:
            m = re.search(r"<WindowsTargetPlatformVersion>([^<]+)<", f.read())
        return m.group(1) if m else "?"
    except OSError:
        return "?"


def write_buildinfo(path, bdir, version, commit, date, a):
    hc = os.path.join(bdir, "harness", "CMakeCache.txt")
    lines = [
        "Steelbound build information",
        "version: %s" % (version or "dev"),
        "commit: %s (%s)" % (commit, date),
        "repository: %s" % (os.environ.get("GITHUB_REPOSITORY") or "local"),
        "ref: %s" % (os.environ.get("GITHUB_REF") or git("rev-parse", "--abbrev-ref", "HEAD") or "?"),
        "workflow run: %s" % (("%s/%s/actions/runs/%s" % (os.environ.get("GITHUB_SERVER_URL", ""),
                                os.environ.get("GITHUB_REPOSITORY", ""), os.environ["GITHUB_RUN_ID"]))
                               if os.environ.get("GITHUB_RUN_ID") else "local"),
        "runner image: %s %s" % (os.environ.get("ImageOS", "local"), os.environ.get("ImageVersion", "")),
        "os: %s" % platform.platform(),
        "visual studio: %s" % vs_version(cache_value(hc, "CMAKE_GENERATOR_INSTANCE")),
        "generator: %s" % (cache_value(hc, "CMAKE_GENERATOR") or "?"),
        "toolset: %s" % (cache_value(hc, "CMAKE_GENERATOR_TOOLSET") or "default"),
        "c compiler: MSVC %s" % compiler_version(bdir),
        "cmake: %s" % (subprocess.run(["cmake", "--version"], capture_output=True, text=True)
                       .stdout.splitlines() or ["?"])[0],
        "python: %s" % platform.python_version(),
        "windows sdk: %s" % sdk_version(bdir),
        "reproducible flags (/Brepro): %s" % ("on" if a.repro else "off"),
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return lines


def clean_env():
    env = dict(os.environ)
    for k in list(env):
        if k.upper().startswith("SB_"):
            del env[k]
    return env


def first_start(zpath, fdir, name, selftest_exe, strict_ui):
    """Unpack, portable mode, launcher checks without game data. -> (ok, lines)"""
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
    ok = True

    def call(args, exe_path=None, cwd=None, timeout=600):
        p = subprocess.run([exe_path or exe] + args, cwd=cwd or app, env=env, capture_output=True,
                           timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace").replace("\r\n", "\n")

    rc, out = call(["--userdata"])
    port = ("Modus: portable" in out or "Mode: portable" in out) and \
        os.path.normcase(app) in os.path.normcase(out)
    lines.append("--userdata: rc=%d, portable mode in program folder: %s" % (rc, "yes" if port else "NO"))
    ok &= port
    rc, out = call(["--check"])
    nogame = ("No game data set" in out) or ("Keine Spieldaten" in out)
    lines.append("--check: rc=%d (expected != 0, no game data set): %s" % (rc, "yes" if nogame else "NO"))
    ok &= (rc != 0 and nogame)
    rc, out = call(["--selftest"])
    shipped_ok = rc == 2 and "not included in this build" in out
    lines.append("--selftest (shipped launcher): rc=%d, %s" % (
        rc, "not included (expected)" if shipped_ok else "UNEXPECTED"))
    ok &= shipped_ok
    if selftest_exe and os.path.isfile(selftest_exe):
        tapp = os.path.join(fdir, "selftest_app")
        shutil.copytree(app, tapp)
        shutil.copy2(selftest_exe, os.path.join(tapp, "sb_launcher.exe"))
        rc, out = call(["--selftest"], exe_path=os.path.join(tapp, "sb_launcher.exe"), cwd=tapp)
        shutil.rmtree(tapp, ignore_errors=True)
        m = re.search(r"selftest: (\d+) PASS, (\d+) FAIL", out)
        lines.append("--selftest (test launcher, package copy): rc=%d, %s" % (
            rc, m.group(0) if m else "no summary"))
        ok &= (rc == 0 and bool(m) and m.group(2) == "0")
    else:
        lines.append("--selftest (test launcher): MISSING (%s)" % selftest_exe)
        ok = False
    shots = os.path.join(fdir, "shots")
    os.makedirs(shots, exist_ok=True)
    rc, out = call(["--ui-shot", "play", "--shot-out", shots])
    pngs = [n for n in os.listdir(shots) if n.lower().endswith(".png")]
    ui_ok = rc == 0 and bool(pngs)
    lines.append("--ui-shot play: rc=%d, %s%s" % (rc, ", ".join(pngs) or "no PNG",
                                                  "" if ui_ok or strict_ui else " (warning only)"))
    if strict_ui:
        ok &= ui_ok
    return ok, lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="work folder outside the source tree")
    ap.add_argument("--version", help="release version (SemVer) or dev; default poc-native/VERSION")
    ap.add_argument("--name", help="package name; default Steelbound-<version>-win32")
    ap.add_argument("--generator", default="", help='CMake generator, e.g. "Visual Studio 17 2022"')
    ap.add_argument("--toolset", default="", help="MSVC toolset, e.g. v143")
    ap.add_argument("--sdk", default="", help="Windows SDK version, e.g. 10.0.26100.0")
    ap.add_argument("--probe", action="store_true", help="quick check build: no ZIP, no first start")
    ap.add_argument("--no-first-start", action="store_true")
    ap.add_argument("--repro", action="store_true", help="experimental: /Brepro for compiler and linker")
    ap.add_argument("--skip-build", action="store_true", help="reuse <out>/build")
    ap.add_argument("--strict-ui", action="store_true", help="a failing --ui-shot is an error")
    ap.add_argument("--force", action="store_true", help="replace an earlier run in --out")
    a = ap.parse_args(argv)

    version = central_version() if a.version is None else ("" if a.version == "dev" else a.version)
    if version and not re.fullmatch(SEMVER_RE, version):
        print("ERROR: --version must be SemVer (x.y.z[-pre][+build]) or dev", file=sys.stderr)
        return 3
    name = a.name or "Steelbound-%s-win32" % (version or "dev")
    if not re.fullmatch(r"[A-Za-z0-9._\-]+", name):
        print("ERROR: --name may only contain letters, digits, . _ -", file=sys.stderr)
        return 3
    out = os.path.abspath(a.out)
    if inside(out, ROOT) or inside(ROOT, out):
        print("ERROR: --out must be outside the source tree", file=sys.stderr)
        return 3
    if not a.skip_build:
        if os.path.isdir(out) and os.listdir(out):
            if not os.path.isfile(os.path.join(out, MARK)):
                print("ERROR: --out is not empty and not an earlier run", file=sys.stderr)
                return 3
            if not a.force:
                print("ERROR: --out holds an earlier run, use --force", file=sys.stderr)
                return 3
            shutil.rmtree(out)
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, MARK), "w", encoding="utf-8") as f:
            f.write("work folder of build_release.py, may be replaced\n")
    commit, date, epoch = commit_info()
    st = release_stage(version)
    log("version %s%s, package %s, commit %s (%s)%s" % (
        version or "dev", " (%s)" % st if st else "", name, commit, date, ", probe" if a.probe else ""))

    bdir = os.path.join(out, "build")
    if not a.skip_build and build(bdir, version, a) != 0:
        return 1
    stage = os.path.join(out, "stage", name)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    bases = {"build_h": os.path.join(bdir, "harness"), "build_l": os.path.join(bdir, "launcher"), "src": ROOT}
    missing = assemble(stage, bases, commit, date, version)
    if missing:
        log("missing sources: " + ", ".join(missing))
    errs = verify_stage(stage) + harden_errors(stage)
    for e in errs:
        log("PACKAGE ERROR " + e)
    if errs:
        return 1
    log("package checks ok (%d files)" % len(list_files(stage)))
    info = write_buildinfo(os.path.join(out, "BUILDINFO.txt"), bdir, version, commit, date, a)
    for ln in info[1:]:
        log("  " + ln)
    if a.probe:
        for n in ("sb_launcher.exe", "sb_ldr.exe", "sb_emu.dll"):
            log("  %s  %s" % (sha256(os.path.join(stage, n)), n))
        log("probe build ok")
        return 0
    zpath = make_zip(stage, os.path.join(out, name + ".zip"), epoch)
    rows = ["%s  %s/%s" % (sha256(os.path.join(stage, rel)), name, rel) for rel in list_files(stage)]
    rows.append("%s  %s" % (sha256(zpath), os.path.basename(zpath)))
    with open(os.path.join(out, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")
    log("ZIP %s: %d bytes" % (os.path.basename(zpath), os.path.getsize(zpath)))
    for r in rows:
        if r.endswith((".exe", ".dll", ".zip")):
            log("  " + r)
    if not a.no_first_start:
        ok, lines = first_start(zpath, os.path.join(out, "firststart"), name,
                                os.path.join(bdir, "launcher_selftest", "Release", "sb_launcher.exe"),
                                a.strict_ui)
        for ln in lines:
            log("first start " + ln)
        if not ok:
            log("first start FAILED")
            return 1
        log("first start ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
