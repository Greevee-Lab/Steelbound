#!/usr/bin/env python3
"""clean_check.py - Export-Verzeichnis auf Spielinhalte/Geheimnisse pruefen (W116).

Prueft ein Staging-/Export-Verzeichnis (z. B. aus package_edition.py) darauf,
dass es nichts enthaelt, was nicht in eine Community-Veroeffentlichung darf:

  medien     Bild/Audio/Video/Archiv (Endung ODER Magic-Bytes); erlaubt nur
             ueber editions/assets_allow.tsv (Pfad + SHA256 + Herkunft)
  spieldatei bekannte Spieldatei-Typen (.xbe .xpr .bik .xiso .iso .sav .xbx),
             Magic (XBEH, XPR0, BIK), Dateinamen aus dem lokalen Media-Ordner,
             verbotene Ordner (dump, runs, shots, golden, texsrc, hdd_seed, ...)
  artefakt   Build-/Laufartefakte mit Pfaden/Spieltexten (.pdb .ilk .obj .log .dmp ...)
  lfs        Git-LFS-Zeiger
  geheimnis  .env-Dateien, Webhook-URLs (Wert wird NIE ausgegeben, nur Datei:Zeile)
  pfad       absolute Rechner-/Nutzerpfade (Text und Binaries, ASCII + UTF-16LE)
  netz       Netzwerk: PE-Importe (ws2_32, wininet, winhttp, urlmon, ...),
             ShellExecute + URL, Netz-APIs/-Module und URL-Literale in Quellen
  eingebettet  Spieldaten-Bytes in Export-Dateien: 64-Byte-Fenster-Hashes ueber
             die lokale default.xbe (optional Media-Dateien) + Byte-Literale in
             Quellen (>= --literal-min Bytes am Stueck)

Die Spieldaten werden nur zur Pruefzeit gelesen. Fehlt die lokale XBE, wird
die Pruefung 'eingebettet' als UEBERSPRUNGEN gemeldet (nicht bestanden).

Exit: 0 = bestanden, 1 = Funde, 2 = keine Funde, aber Pruefung uebersprungen
(unvollstaendig), 3 = Aufruffehler.

  python clean_check.py <dir> [--report <datei.md>] [--xbe PFAD] [--media auto|PFAD]
                        [--media-max-mb N] [--window 64] [--stride 16]
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# --- Kategorien -------------------------------------------------------------

EXT_IMAGE = {"png", "jpg", "jpeg", "bmp", "gif", "tga", "dds", "webp", "ico",
             "tif", "tiff", "psd", "xcf", "exr", "hdr", "ktx", "ktx2", "cur"}
EXT_AUDIO = {"wav", "mp3", "ogg", "flac", "wma", "xwb", "xma", "aac", "m4a",
             "opus", "adpcm"}
EXT_VIDEO = {"bik", "bk2", "mp4", "avi", "mkv", "webm", "mov", "wmv", "mpg",
             "mpeg", "m2v", "xmv"}
EXT_ARCHIVE = {"zip", "7z", "rar", "tar", "gz", "tgz", "bz2", "xz", "cab",
               "zst", "lz4", "lzma"}
EXT_GAME = {"xbe", "xpr", "xiso", "iso", "sav", "xbx", "bik", "xbx", "xtf",
            "xsb", "xwb", "wdt"}
EXT_ARTEFACT = {"pdb", "ilk", "obj", "exp", "idb", "ipch", "log", "dmp",
                "mdmp", "map", "tlog", "pch", "lib"}
# Ordnernamen, die nie in einen Export gehoeren (Vergleich ohne Gross/Klein)
FORBIDDEN_DIRS = {"dump", "dumps", "runs", "shots", "golden", "texsrc",
                  "hdd_seed", "hdd", "tdata", "udata", "remaster", "media",
                  "steel battalion (europe).xiso", "screenshots", "ref_tex",
                  "orchestra", "workers", ".git"}

MAGICS = [  # (bytes, offset, kategorie, art)
    (b"\x89PNG\r\n\x1a\n", 0, "medien", "PNG"),
    (b"\xff\xd8\xff", 0, "medien", "JPEG"),
    (b"GIF87a", 0, "medien", "GIF"),
    (b"GIF89a", 0, "medien", "GIF"),
    (b"DDS ", 0, "medien", "DDS"),
    (b"OggS", 0, "medien", "Ogg"),
    (b"fLaC", 0, "medien", "FLAC"),
    (b"ID3", 0, "medien", "MP3"),
    (b"BIK", 0, "spieldatei", "Bink-Video"),
    (b"KB2", 0, "spieldatei", "Bink2-Video"),
    (b"XBEH", 0, "spieldatei", "XBE"),
    (b"XPR0", 0, "spieldatei", "XPR"),
    (b"MICROSOFT*XBOX*MEDIA", 0x10000, "spieldatei", "XISO"),
    (b"PK\x03\x04", 0, "medien", "ZIP-Archiv"),
    (b"7z\xbc\xaf\x27\x1c", 0, "medien", "7z-Archiv"),
    (b"Rar!\x1a\x07", 0, "medien", "RAR-Archiv"),
    (b"\x1f\x8b", 0, "medien", "gzip"),
]

LFS_HEAD = b"version https://git-lfs.github.com/spec/v1"

TEXT_EXT = {"c", "h", "cpp", "hpp", "py", "ps1", "psm1", "md", "txt", "cfg",
            "ini", "json", "tsv", "csv", "cmake", "rc", "def", "manifest",
            "js", "html", "css", "java", "sbc", "include", "yml", "yaml",
            "toml", "bat", "cmd", "sh", "xml", "gitignore", "gitattributes"}

# Webhook-/Token-Muster (nur Fundstelle melden, nie den Wert)
SECRET_RES = [
    ("discord-webhook", re.compile(
        rb"discord(?:app)?\.com/api/webhooks/\d{5,}/[A-Za-z0-9_\-]{10,}",
        re.I)),
    ("slack-webhook", re.compile(
        rb"hooks\.slack\.com/services/[A-Za-z0-9/]{10,}", re.I)),
    # Zuweisung an eine GROSS geschriebene Variable (DISCORD_WEBHOOK=...,
    # API_KEY = "..."); camelCase wie publicKeyToken im Manifest zaehlt nicht.
    ("geheimnis-zuweisung", re.compile(
        rb"\b[A-Z0-9_]*(?:WEBHOOK|TOKEN|SECRET|API_KEY|PASSWORD|PASSWD)[A-Z0-9_]*"
        rb"\s*[=:]\s*['\"]?[A-Za-z0-9_\-/.:+=]{16,}")),
    ("bot-token", re.compile(
        rb"\b[MN][A-Za-z\d]{23,25}\.[\w-]{6}\.[\w-]{27,}\b")),
]

# Absolute Pfade: Laufwerksbuchstabe + mind. zwei Pfadteile (Xbox-Laufwerke
# wie 'T:' oder 'E:' mit einem Teil sind keine Rechnerpfade), Nutzerordner
# unter Users bzw. home, Git-Bash-Form (Schraegstrich + Laufwerksbuchstabe).
# Systempfade (Windows, Programme) sind erlaubt.
ABS_RE = re.compile(
    rb"(?<![A-Za-z0-9])([A-Za-z]:[\\/]{1,2}[A-Za-z0-9_$][^\s\"'<>|*?\x00\\/:]*"
    rb"[\\/]+[A-Za-z0-9_$.(][^\s\"'<>|*?\x00]{0,200})"
    rb"|(?<![A-Za-z0-9_.])(/(?:[a-z]/|home/|Users/)[A-Za-z0-9_.\-]+[^\s\"'<>|*?\x00]*)")
ABS_ALLOW_RE = re.compile(
    r"^[A-Za-z]:[\\/]{1,2}(Windows|Program Files|Program Files \(x86\)|"
    r"ProgramData)([\\/]|$)", re.I)


# Netzwerk: Community-Build hat keine externen Zugaenge (kein Netz, keine
# Downloads, kein Discord/Webhook, keine Telemetrie).
NET_DLLS = {"ws2_32.dll", "wsock32.dll", "wininet.dll", "winhttp.dll",
            "urlmon.dll", "mswsock.dll", "dnsapi.dll", "websocket.dll",
            "httpapi.dll", "iphlpapi.dll", "bits.dll", "webio.dll"}
NET_SRC_RE = re.compile(
    rb"\b(WinHttp[A-Z]\w*|Internet(?:Open|Connect|ReadFile|OpenUrl)\w*|"
    # (Endbuchstabe in [] - so findet die Pruefung sich nicht selbst)
    rb"HttpSendReques[t]\w*|URLDownloadTo\w+|URLOpen\w*Stream\w*|WSAStartu[p]|"
    rb"getaddrinf[o]|gethostbynam[e]|WSASocke[t]\w*|"
    rb"Invoke-WebReques[t]|Invoke-RestMetho[d]|Net\.WebClient|Start-BitsTransfe[r]|"
    rb"New-Object\s+System\.Net\.\w+)\b"
    rb"|^\s*(?:import|from)\s+(?:urllib|requests|http\.client|httpx|aiohttp|socket|"
    rb"ftplib|smtplib|websocket)\b"
    rb"|#\s*pragma\s+comment\s*\(\s*lib\s*,\s*\"(?:ws2_32|wininet|winhttp|urlmon)",
    re.M)
URL_LIT_RE = re.compile(rb"\"(?:https?|ftp)://(?!schemas\.microsoft\.com)[^\"\s]+\"")


def pe_imports(data):
    """-> {dll_klein: set(funktionen)} oder None (kein PE). Nur lesend."""
    import struct
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    try:
        e = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e:e + 4] != b"PE\0\0":
            return None
        nsec = struct.unpack_from("<H", data, e + 6)[0]
        optsz = struct.unpack_from("<H", data, e + 20)[0]
        opt = e + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        dd = opt + (96 if magic == 0x10B else 112)
        imp_rva = struct.unpack_from("<I", data, dd + 8)[0]
        secs = []
        so = opt + optsz
        for i in range(nsec):
            vs, va, raw_sz, raw_off = struct.unpack_from("<IIII", data, so + i * 40 + 8)
            secs.append((va, max(vs, raw_sz), raw_off))

        def off(rva):
            for va, sz, ro in secs:
                if va <= rva < va + sz:
                    return rva - va + ro
            return None

        def cstr(o):
            end = data.find(b"\0", o)
            return data[o:end].decode("latin-1")

        res = {}
        o = off(imp_rva) if imp_rva else None
        while o is not None and o + 20 <= len(data):
            oft, _t, _f, name_rva, ft = struct.unpack_from("<IIIII", data, o)
            if not name_rva:
                break
            dll = cstr(off(name_rva)).lower()
            funcs = set()
            th = off(oft or ft)
            step = 4 if magic == 0x10B else 8
            while th is not None and th + step <= len(data):
                v = struct.unpack_from("<I" if step == 4 else "<Q", data, th)[0]
                if not v:
                    break
                if not (v >> (31 if step == 4 else 63)):
                    fo = off(v & 0x7FFFFFFF)
                    if fo is not None:
                        funcs.add(cstr(fo + 2))
                th += step
            res[dll] = funcs
            o += 20
        return res
    except (struct.error, TypeError, ValueError):
        return None


def norm(p):
    return p.replace("\\", "/")


# --- Referenz-Spieldaten finden ---------------------------------------------

def git_roots():
    """(toplevel, haupt-checkout) des Repos, in dem dieses Skript liegt."""
    try:
        top = subprocess.check_output(
            ["git", "-C", HERE, "rev-parse", "--show-toplevel"],
            stderr=subprocess.DEVNULL, text=True).strip()
        common = subprocess.check_output(
            ["git", "-C", HERE, "rev-parse", "--git-common-dir"],
            stderr=subprocess.DEVNULL, text=True).strip()
        if not os.path.isabs(common):
            common = os.path.join(top, common)
        main = os.path.dirname(os.path.abspath(common))
        return os.path.abspath(top), main
    except (OSError, subprocess.CalledProcessError):
        return None, None


def is_lfs_pointer_file(path):
    try:
        with open(path, "rb") as f:
            return f.read(len(LFS_HEAD)) == LFS_HEAD
    except OSError:
        return False


def find_reference_xbes(explicit):
    """Liste vorhandener XBE-Dateien (keine LFS-Zeiger), dedupliziert per SHA."""
    cands = []
    if explicit:
        cands = list(explicit)
    else:
        env = os.environ.get("SB_CLEAN_XBE")
        if env:
            cands.append(env)
        for root in git_roots():
            if not root:
                continue
            cands.append(os.path.join(root, "poc-native", "work", "default.xbe"))
            cands.append(os.path.join(root, "Steel Battalion (Europe).xiso",
                                      "default.xbe"))
    out, seen = [], set()
    for c in cands:
        if not os.path.isfile(c) or is_lfs_pointer_file(c):
            continue
        h = sha256_file(c)
        if h in seen:
            continue
        seen.add(h)
        out.append(os.path.abspath(c))
    return out


def find_media_dir(arg):
    if arg and arg != "auto":
        return arg if os.path.isdir(arg) else None
    for root in git_roots():
        if not root:
            continue
        d = os.path.join(root, "Steel Battalion (Europe).xiso", "Media")
        if os.path.isdir(d):
            # nur echte Dateien (nicht LFS-Zeiger): erste Datei entscheidet
            for dp, _dn, fn in os.walk(d):
                if fn:
                    if not is_lfs_pointer_file(os.path.join(dp, sorted(fn)[0])):
                        return d
                    break
    return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --- Fenster-Hashes (Rabin-Karp, mod 2^64, numpy) ---------------------------

P_HASH = 0x100000001B3  # ungerade -> invertierbar mod 2^64
MIN_DISTINCT = 10       # Fenster mit < 10 verschiedenen Bytes zaehlen nicht


class WindowIndex:
    def __init__(self, np, window, stride):
        self.np = np
        self.W = window
        self.S = stride
        self.hashes = []
        self.refs = []      # (ref_id, offset) parallel zu hashes (als Arrays)
        self.ref_names = []
        self.ref_data = []  # Bytes fuer die Verifikation (nur XBE, klein)
        self.windows = 0

    CHUNK = 1 << 22  # 4 MiB je Block (Speicher), Ueberlappung W-1

    def _pows(self, n):
        np = self.np
        if getattr(self, "_pw", None) is None or self._pw[0].size < n:
            with np.errstate(over="ignore"):
                pows = np.full(n, P_HASH, dtype=np.uint64)
                pows[0] = 1
                pows = np.cumprod(pows, dtype=np.uint64)
                inv = pow(P_HASH, -1, 1 << 64)
                ipows = np.full(n, inv, dtype=np.uint64)
                ipows[0] = 1
                ipows = np.cumprod(ipows, dtype=np.uint64)
            self._pw = (pows, ipows)
        return self._pw

    def _all_hashes(self, a):
        """Hash jedes W-Byte-Fensters (Wert haengt nur vom Inhalt ab)."""
        np = self.np
        n = a.size
        W = self.W
        if n < W:
            return np.zeros(0, dtype=np.uint64)
        out = np.empty(n - W + 1, dtype=np.uint64)
        pows, ipows = self._pows(min(n, self.CHUNK + W - 1))
        for s in range(0, n - W + 1, self.CHUNK):
            seg = a[s:s + self.CHUNK + W - 1]
            m = seg.size
            with np.errstate(over="ignore"):
                H = np.zeros(m + 1, dtype=np.uint64)
                np.cumsum(seg.astype(np.uint64) * pows[:m], dtype=np.uint64, out=H[1:])
                out[s:s + m - W + 1] = (H[W:] - H[:-W]) * ipows[: m - W + 1]
        return out

    def _distinct(self, a, offs):
        np = self.np
        if offs.size == 0:
            return np.zeros(0, dtype=np.int32)
        idx = offs[:, None] + np.arange(self.W)[None, :]
        win = np.sort(a[idx], axis=1)
        return 1 + (np.diff(win, axis=1) != 0).sum(axis=1)

    def add(self, name, data, stride=None, keep=True):
        np = self.np
        S = stride or self.S
        a = np.frombuffer(data, dtype=np.uint8)
        if a.size < self.W:
            return
        rid = len(self.ref_names)
        self.ref_names.append(name)
        self.ref_data.append(data if keep else None)
        nwin = a.size - self.W + 1
        step = (self.CHUNK // S) * S
        for s in range(0, nwin, step):
            seg = a[s:s + step + self.W - 1]
            h_seg = self._all_hashes(seg)
            offs = np.arange(0, h_seg.size, S, dtype=np.int64)
            # Entropie-Filter in Bloecken (Speicher)
            keep_mask = np.zeros(offs.size, dtype=bool)
            blk = 1 << 16
            for i in range(0, offs.size, blk):
                keep_mask[i:i + blk] = self._distinct(seg, offs[i:i + blk]) >= MIN_DISTINCT
            offs = offs[keep_mask]
            self.hashes.append(h_seg[offs])
            self.refs.append((offs + s) | (np.int64(rid) << np.int64(40)))
            self.windows += int(offs.size)

    def freeze(self):
        np = self.np
        if self.hashes:
            h = np.concatenate(self.hashes)
            r = np.concatenate(self.refs)
        else:
            h = np.zeros(0, dtype=np.uint64)
            r = np.zeros(0, dtype=np.int64)
        order = np.argsort(h, kind="stable")
        self.h = h[order]
        self.r = r[order]
        self.hashes = self.refs = None

    def scan(self, data):
        """-> Liste von (exp_off, ref_name, ref_off, laenge) zusammengefasster Treffer."""
        np = self.np
        a = np.frombuffer(data, dtype=np.uint8)
        if a.size < self.W or self.h.size == 0:
            return []
        h = self._all_hashes(a)
        pos = np.searchsorted(self.h, h)
        pos_c = np.minimum(pos, self.h.size - 1)
        hit = self.h[pos_c] == h
        cand = np.nonzero(hit)[0]
        found = []
        for off in cand.tolist():
            p = int(pos_c[off])
            while p < self.h.size and self.h[p] == h[off]:
                rv = int(self.r[p])
                rid, roff = rv >> 40, rv & ((1 << 40) - 1)
                ref = self.ref_data[rid]
                win = data[off:off + self.W]
                ok = True
                if ref is not None:
                    ok = ref[roff:roff + self.W] == win
                if ok and len(set(win)) >= MIN_DISTINCT:
                    found.append((off, rid, roff))
                    break
                p += 1
        # zu Bereichen zusammenfassen
        ranges = []
        for off, rid, roff in found:
            if ranges and ranges[-1][1] == rid and off <= ranges[-1][0] + ranges[-1][3]:
                r0 = ranges[-1]
                ranges[-1] = (r0[0], rid, r0[2], max(r0[3], off + self.W - r0[0]))
            else:
                ranges.append((off, rid, roff, self.W))
        return [(o, self.ref_names[rid], ro, ln) for o, rid, ro, ln in ranges]


# --- Byte-Literale in Quellen ----------------------------------------------

LIT_HEX_RUN = re.compile(
    r"(?:0x[0-9A-Fa-f]{2}u?\s*,\s*(?:/\*.*?\*/\s*)?){3,}0x[0-9A-Fa-f]{2}\b", re.S)
LIT_HEX16_RUN = re.compile(
    r"(?:0x[0-9A-Fa-f]{4}u?\s*,\s*(?:/\*.*?\*/\s*)?){3,}0x[0-9A-Fa-f]{4}\b", re.S)
LIT_HEX32_RUN = re.compile(
    r"(?:0x[0-9A-Fa-f]{8}u?\s*,\s*(?:/\*.*?\*/\s*)?){1,}0x[0-9A-Fa-f]{8}\b", re.S)
LIT_ESC_RUN = re.compile(r"(?:\\x[0-9A-Fa-f]{2}){4,}")


def source_literals(text):
    """-> Liste (zeile, bytes) zusammenhaengender Byte-/Wort-Literale
    (8/16/32 Bit hex, little-endian wie x86) und Escape-Folgen."""
    out = []
    for rx, width in ((LIT_HEX_RUN, 1), (LIT_HEX16_RUN, 2), (LIT_HEX32_RUN, 4)):
        for m in rx.finditer(text):
            vals = re.findall(r"0x([0-9A-Fa-f]{%d})\b" % (2 * width), m.group(0))
            bs = b"".join(int(v, 16).to_bytes(width, "little") for v in vals)
            out.append((text.count("\n", 0, m.start()) + 1, bs))
    for m in LIT_ESC_RUN.finditer(text):
        bs = bytes(int(x, 16) for x in re.findall(r"\\x([0-9A-Fa-f]{2})", m.group(0)))
        out.append((text.count("\n", 0, m.start()) + 1, bs))
    return out


# --- Hauptpruefung -----------------------------------------------------------

def load_assets_allow(path):
    allow = {}
    if not path or not os.path.isfile(path):
        return allow
    with open(path, encoding="utf-8") as f:
        for ln in f:
            ln = ln.rstrip("\r\n")
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            parts = ln.split("\t")
            if len(parts) < 3 or not parts[2].strip():
                continue  # ohne Herkunft keine Freigabe
            allow[norm(parts[0]).lower()] = (parts[1].strip().lower(), parts[2].strip())
    return allow


def load_clean_allow(path):
    """editions/clean_allow.tsv: kategorie TAB datei-glob TAB muster TAB
    begruendung. Nur fuer bewusst erlaubte Fundstellen; ohne Begruendung
    ungueltig. Nie fuer 'geheimnis', 'medien', 'lfs'.
    - sonstige Kategorien: muster = Regex auf den Fund-Text.
    - 'eingebettet': muster = 'xbe:<offset>+<laenge>:<sha256>' - erlaubt nur
      Treffer, die ganz in diesem XBE-Bereich liegen, und nur wenn die
      SHA256 der Referenzbytes stimmt (oeffentliche Laufzeit-/Standard-
      tabellen, die Spiel und Client gleichermassen enthalten).
    -> (regex_regeln, eingebettet_regeln)"""
    out, emb = [], []
    if not path or not os.path.isfile(path):
        return out, emb
    with open(path, encoding="utf-8") as f:
        for ln in f:
            ln = ln.rstrip("\r\n")
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            p = ln.split("\t")
            if len(p) < 4 or not p[3].strip():
                continue
            if p[0] in ("geheimnis", "medien", "lfs"):
                continue
            if p[0] == "eingebettet":
                m = re.match(r"^xbe:(0x[0-9A-Fa-f]+|\d+)\+(\d+):([0-9a-f]{64})$", p[2].strip())
                if m:
                    emb.append((p[1], int(m.group(1), 0), int(m.group(2)), m.group(3),
                                p[3].strip()))
                continue
            out.append((p[0], p[1], re.compile(p[2]), p[3].strip()))
    return out, emb


def load_clean_deny(path):
    """editions/clean_deny.tsv (Steuer 6.11.): sha256 TAB signatur-hex TAB grund.
    Gesperrte Dateien sind immer ein Fund (keine Freigabe moeglich): exakt per
    SHA-256 oder eingebettet per Signatur (48 Bytes aus der Dateimitte)."""
    out = []
    if not path or not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8") as fh:
        for ln in fh:
            ln = ln.rstrip(chr(10))
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            parts = ln.split(chr(9))
            if len(parts) < 3:
                continue
            out.append((parts[0].strip().lower(), bytes.fromhex(parts[1].strip()), parts[2].strip()))
    return out


def load_media_names(media_dir):
    names = set()
    if not media_dir:
        return names
    for _dp, _dn, fn in os.walk(media_dir):
        for n in fn:
            names.add(n.lower())
    return names


def is_text(path, head):
    ext = path.rsplit(".", 1)[-1].lower() if "." in os.path.basename(path) else ""
    if ext in TEXT_EXT:
        return True
    if b"\x00" in head[:4096]:
        return False
    try:
        head[:4096].decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def line_of(data, pos):
    return data.count(b"\n", 0, pos) + 1


class Checker:
    def __init__(self, root, args):
        self.root = os.path.abspath(root)
        self.args = args
        self.findings = []
        self.checks = {}
        self.files = 0
        self.bytes = 0
        self.assets = load_assets_allow(args.assets_allow)
        self.allow, self.emb_allow = load_clean_allow(args.clean_allow)
        self.deny = load_clean_deny(getattr(args, "clean_deny", None))
        self.pe_seen = 0
        self._ref_names = []
        self._xbe_data = []
        self.allowed = []
        self.user = os.environ.get("USERNAME") or os.environ.get("USER") or ""

    def add(self, cat, rel, line, detail):
        f = {"kategorie": cat, "datei": rel, "zeile": line, "detail": detail}
        for acat, aglob, arx, why in self.allow:
            if acat == cat and fnmatch.fnmatch(rel, aglob) and arx.search(detail):
                f["freigabe"] = why
                self.allowed.append(f)
                return
        self.findings.append(f)

    def emb_allowed(self, rel, ref, roff, ln):
        """Freigabe fuer einen XBE-Treffer nur bei passender SHA256 (s.o.)."""
        for glob, a_off, a_len, sha, why in self.emb_allow:
            if not fnmatch.fnmatch(rel, glob) or roff < a_off or roff + ln > a_off + a_len:
                continue
            for name, data in zip(self._ref_names, self._xbe_data):
                if name == ref and hashlib.sha256(data[a_off:a_off + a_len]).hexdigest() == sha:
                    return why
        return None

    def walk(self):
        for dp, dn, fn in os.walk(self.root):
            dn.sort()
            relp = norm(os.path.relpath(dp, self.root))
            for d in list(dn):
                if d.lower() in FORBIDDEN_DIRS:
                    rel = d if relp == "." else relp + "/" + d
                    self.add("spieldatei", rel + "/", None,
                             "verbotener Ordnername '%s'" % d)
            for n in sorted(fn):
                full = os.path.join(dp, n)
                rel = norm(os.path.relpath(full, self.root))
                yield full, rel

    def run(self):
        t0 = time.time()
        np = None
        try:
            import numpy as np  # noqa: F811
        except ImportError:
            np = None
        # Referenzen
        xbes = find_reference_xbes(self.args.xbe)
        media_dir = None
        if self.args.media:
            media_dir = find_media_dir(self.args.media)
        media_names = load_media_names(media_dir or find_media_dir("auto"))
        index = None
        ref_info = []
        if np is None:
            self.checks["eingebettet"] = {"status": "uebersprungen",
                                          "grund": "numpy fehlt"}
        elif not xbes:
            self.checks["eingebettet"] = {
                "status": "uebersprungen",
                "grund": "keine lokale default.xbe gefunden (--xbe/SB_CLEAN_XBE)"}
        else:
            index = WindowIndex(np, self.args.window, self.args.stride)
            xbe_data = []
            for x in xbes:
                with open(x, "rb") as f:
                    d = f.read()
                xbe_data.append(d)
                self._ref_names.append(os.path.basename(x))
                index.add(os.path.basename(x), d)
                ref_info.append({"datei": x, "bytes": len(d),
                                 "sha256": hashlib.sha256(d).hexdigest()})
            media_files = 0
            media_bytes = 0
            if media_dir:
                cap = self.args.media_max_mb * (1 << 20) if self.args.media_max_mb else None
                for dp, _dn, fn in os.walk(media_dir):
                    for n in sorted(fn):
                        p = os.path.join(dp, n)
                        sz = os.path.getsize(p)
                        if cap is not None and media_bytes + sz > cap:
                            continue
                        if is_lfs_pointer_file(p):
                            continue
                        with open(p, "rb") as f:
                            d = f.read()
                        index.add("Media/" + norm(os.path.relpath(p, media_dir)), d,
                                  stride=self.args.window, keep=False)
                        media_files += 1
                        media_bytes += sz
            index.freeze()
            self.checks["eingebettet"] = {
                "status": "ausgefuehrt", "referenzen": ref_info,
                "fenster": self.args.window, "schritt_xbe": self.args.stride,
                "garantie_ab_bytes": self.args.window + self.args.stride - 1,
                "referenz_fenster": index.windows,
                "media": ({"ordner": media_dir, "dateien": media_files,
                           "bytes": media_bytes,
                           "garantie_ab_bytes": 2 * self.args.window - 1}
                          if media_dir else "nicht angefordert"),
                "literal_min": self.args.literal_min}
            self._xbe_data = xbe_data
        self.checks["medien"] = {"status": "ausgefuehrt",
                                 "positivliste": len(self.assets)}
        self.checks["spieldatei"] = {"status": "ausgefuehrt",
                                     "media_namen": len(media_names)}
        for k in ("lfs", "geheimnis", "pfad", "artefakt", "netz"):
            self.checks[k] = {"status": "ausgefuehrt"}

        for full, rel in self.walk():
            self.files += 1
            try:
                with open(full, "rb") as f:
                    data = f.read()
            except OSError as e:
                self.add("lesefehler", rel, None, str(e))
                continue
            self.bytes += len(data)
            self.check_file(rel, data, media_names, index)
        self.seconds = round(time.time() - t0, 1)
        self.checks["netz"]["pe_binaries"] = self.pe_seen

    def check_file(self, rel, data, media_names, index):
        base = os.path.basename(rel)
        if self.deny:
            fsha = hashlib.sha256(data).hexdigest()
            for dsha, dsig, why in self.deny:
                if fsha == dsha or (dsig and dsig in data):
                    # gesperrt: nie freigebbar -> direkt in die Funde
                    self.findings.append({"kategorie": "gesperrt", "datei": rel, "zeile": None,
                                          "detail": "gesperrte Datei (%s): %s" % (
                                              "SHA-256" if fsha == dsha else "eingebettet", why)})
        ext = base.rsplit(".", 1)[-1].lower() if "." in base else ""
        # Medien (Endung oder Magic)
        kind = None
        if ext in EXT_IMAGE:
            kind = "Bild"
        elif ext in EXT_AUDIO:
            kind = "Audio"
        elif ext in EXT_VIDEO:
            kind = "Video"
        elif ext in EXT_ARCHIVE:
            kind = "Archiv"
        magic = None
        for mb, off, cat, art in MAGICS:
            if data[off:off + len(mb)] == mb:
                magic = (cat, art)
                break
        if not magic and data[:4] == b"RIFF" and data[8:12] in (b"WAVE", b"AVI ", b"WEBP"):
            magic = ("medien", "RIFF-" + data[8:12].decode().strip())
        if not magic and data[:2] == b"BM" and len(data) > 26 and \
                int.from_bytes(data[2:6], "little") == len(data):
            magic = ("medien", "BMP")
        if kind or (magic and magic[0] == "medien"):
            allow = self.assets.get(rel.lower())
            sha = hashlib.sha256(data).hexdigest()
            if not allow:
                self.add("medien", rel, None, "%s ohne Eintrag in assets_allow.tsv"
                         % (kind or magic[1]))
            elif allow[0] != sha:
                self.add("medien", rel, None, "SHA256 weicht von assets_allow.tsv ab")
        # Spieldateien
        if ext in EXT_GAME:
            self.add("spieldatei", rel, None, "Spieldatei-Endung .%s" % ext)
        elif magic and magic[0] == "spieldatei":
            self.add("spieldatei", rel, None, "Magic %s" % magic[1])
        elif base.lower() in media_names and ext not in ("txt", "md"):
            self.add("spieldatei", rel, None, "Dateiname wie im Spiel-Media-Ordner")
        if ext in EXT_ARTEFACT:
            self.add("artefakt", rel, None, "Build-/Laufartefakt .%s" % ext)
        # LFS
        if data.startswith(LFS_HEAD):
            self.add("lfs", rel, 1, "Git-LFS-Zeiger (Inhalt fehlt/gehoert ins LFS)")
            return
        # Geheimnisse
        if base.lower() == ".env" or base.lower().startswith(".env."):
            self.add("geheimnis", rel, None, ".env-Datei")
        for name, rx in SECRET_RES:
            for m in rx.finditer(data):
                self.add("geheimnis", rel, line_of(data, m.start()),
                         "%s (Wert nicht ausgegeben)" % name)
        text = is_text(rel, data)
        # absolute Pfade (Text: Zeile; Binaer: ASCII + UTF-16LE)
        self.check_paths(rel, data, text)
        # Netzwerk (Binaries: PE-Importe; Quellen: Netz-APIs/URL-Literale)
        imps = None if text else pe_imports(data)
        if imps is not None:
            self.pe_seen += 1
            for dll, funcs in sorted(imps.items()):
                if dll in NET_DLLS:
                    self.add("netz", rel, None, "importiert %s (%s)" % (
                        dll, ", ".join(sorted(funcs)[:6])))
                for fn in funcs:
                    if fn.startswith("ShellExecute") and (
                            re.search(rb"https?://(?!schemas\.microsoft\.com)", data) or
                            re.search(r"https?://(?!schemas\.microsoft\.com)".encode("utf-16-le"),
                                      data)):
                        self.add("netz", rel, None, "%s + URL-Text im Binary" % fn)
        elif text and ext in ("c", "h", "cpp", "hpp", "py", "ps1", "psm1", "js", "cs",
                              "bat", "cmd", "inc", "rc"):
            for m in NET_SRC_RE.finditer(data):
                self.add("netz", rel, line_of(data, m.start()),
                         "Netzwerk-API/-Modul: %s" % m.group(0).decode("latin-1").strip()[:60])
            if ext in ("c", "h", "cpp", "hpp", "inc", "rc"):
                for m in URL_LIT_RE.finditer(data):
                    self.add("netz", rel, line_of(data, m.start()), "URL-Literal im Code")
        # eingebettete Spieldaten
        if index is not None:
            for off, ref, roff, ln in index.scan(data):
                f = {"kategorie": "eingebettet", "datei": rel,
                     "zeile": line_of(data, off) if text else None,
                     "detail": "%d Bytes ab Datei-Offset 0x%X = %s @0x%X"
                               % (ln, off, ref, roff)}
                why = self.emb_allowed(rel, ref, roff, ln)
                if why:
                    f["freigabe"] = why
                    self.allowed.append(f)
                else:
                    self.findings.append(f)
            if text and ext in ("c", "h", "cpp", "hpp", "py", "inc", "js", "java"):
                t = data.decode("utf-8", "replace")
                L = self.args.literal_min
                for ln_no, bs in source_literals(t):
                    if len(bs) < L:
                        continue
                    hit = None
                    # Teilstuecke der Laenge L (Schritt L/2): auch ein Literal,
                    # das nur teilweise aus der XBE stammt, wird gefunden
                    for k in range(0, len(bs) - L + 1, max(1, L // 2)):
                        piece = bs[k:k + L]
                        if len(set(piece)) < 4:
                            continue
                        for xd in self._xbe_data:
                            p = xd.find(piece)
                            if p >= 0:
                                hit = (k, p)
                                break
                        if hit:
                            break
                    if hit:
                        self.add("eingebettet", rel, ln_no,
                                 "Literal (%d Bytes) enthaelt %d Bytes aus default.xbe @0x%X"
                                 % (len(bs), L, hit[1]))

    def check_paths(self, rel, data, text):
        hits = []
        if text:
            hits = [(m.start(), m.group(0)) for m in ABS_RE.finditer(data)]
        else:
            hits = [(m.start(), m.group(0)) for m in ABS_RE.finditer(data)]
            for start in (0, 1):
                u16 = data[start:].decode("utf-16-le", "ignore").encode("latin-1", "ignore")
                hits += [(None, m.group(0)) for m in ABS_RE.finditer(u16)]
        seen = set()
        for pos, raw in hits:
            s = raw.decode("latin-1")
            if ABS_ALLOW_RE.match(s):
                continue
            # Binaer: nur Pfade mit mind. 2 Komponenten (Rauschen)
            if not text and len(re.split(r"[\\/]+", s.strip("/\\"))) < 3:
                continue
            short = s[:60] + ("..." if len(s) > 60 else "")
            key = (pos if text else short)
            if key in seen:
                continue
            seen.add(key)
            self.add("pfad", rel, line_of(data, pos) if (text and pos is not None) else None,
                     "absoluter Pfad: %s" % short)
        if self.user and len(self.user) >= 3:
            rx = re.compile(re.escape(self.user.encode()), re.I)
            for m in rx.finditer(data):
                if text:
                    self.add("pfad", rel, line_of(data, m.start()),
                             "lokaler Nutzername")
                else:
                    self.add("pfad", rel, None, "lokaler Nutzername (binaer)")
                    break

    def status(self):
        if self.findings:
            return 1
        if any(c.get("status") == "uebersprungen" for c in self.checks.values()):
            return 2
        return 0


def write_reports(chk, report):
    rc = chk.status()
    verdict = {0: "BESTANDEN", 1: "FUNDE", 2: "UNVOLLSTAENDIG (Pruefung uebersprungen)"}[rc]
    js = {"verzeichnis": chk.root, "ergebnis": verdict, "exit": rc,
          "dateien": chk.files, "bytes": chk.bytes, "sekunden": chk.seconds,
          "pruefungen": chk.checks, "funde": chk.findings,
          "freigegeben": chk.allowed,
          "zeit": time.strftime("%Y-%m-%d %H:%M:%S")}
    by = {}
    for f in chk.findings:
        by.setdefault(f["kategorie"], []).append(f)
    md = ["# clean_check: %s" % verdict, "",
          "- Verzeichnis: `%s`" % chk.root,
          "- Dateien: %d (%d Bytes), Dauer %.1f s" % (chk.files, chk.bytes, chk.seconds),
          "- Funde: %d (freigegeben laut clean_allow.tsv: %d)" % (
              len(chk.findings), len(chk.allowed)), "", "## Pruefungen", ""]
    for k, v in chk.checks.items():
        st = v.get("status")
        extra = ""
        if k == "eingebettet" and st == "ausgefuehrt":
            refs = ", ".join("`%s` (%d B, sha256 %s...)" % (r["datei"], r["bytes"], r["sha256"][:12])
                             for r in v["referenzen"])
            med = v["media"]
            mtxt = med if isinstance(med, str) else "%d Dateien / %d Bytes (Garantie ab %d B)" % (
                med["dateien"], med["bytes"], med["garantie_ab_bytes"])
            extra = (" - Referenz %s; Fenster %d B, Schritt %d (Garantie ab %d B), "
                     "%d Referenzfenster; Media: %s; Literal ab %d B"
                     % (refs, v["fenster"], v["schritt_xbe"], v["garantie_ab_bytes"],
                        v["referenz_fenster"], mtxt, v["literal_min"]))
        elif st == "uebersprungen":
            extra = " - " + v.get("grund", "")
        md.append("- **%s**: %s%s" % (k, st, extra))
    md += ["", "## Funde", ""]
    if not chk.findings:
        md += ["keine", ""]
    for cat in sorted(by):
        md.append("### %s (%d)" % (cat, len(by[cat])))
        md.append("")
        for f in by[cat][:500]:
            loc = f["datei"] + (":%d" % f["zeile"] if f["zeile"] else "")
            md.append("- `%s` - %s" % (loc, f["detail"]))
        if len(by[cat]) > 500:
            md.append("- ... %d weitere (s. JSON)" % (len(by[cat]) - 500))
        md.append("")
    if chk.allowed:
        md += ["## Freigegebene Fundstellen (editions/clean_allow.tsv)", ""]
        for f in chk.allowed:
            loc = f["datei"] + (":%d" % f["zeile"] if f["zeile"] else "")
            md.append("- `%s` [%s] - %s -> %s" % (loc, f["kategorie"], f["detail"], f["freigabe"]))
        md.append("")
    if report:
        base = report[:-3] if report.lower().endswith(".md") else report
        with open(base + ".md", "w", encoding="utf-8") as f:
            f.write("\n".join(md) + "\n")
        with open(base + ".json", "w", encoding="utf-8") as f:
            json.dump(js, f, ensure_ascii=False, indent=1)
    return rc, verdict, md


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--report", help="Bericht (.md; JSON daneben)")
    ap.add_argument("--xbe", action="append",
                    help="Referenz-XBE (mehrfach); Standard: lokale default.xbe")
    ap.add_argument("--media", nargs="?", const="auto",
                    help="auch Media-Dateien hashen (auto = lokaler xiso/Media)")
    ap.add_argument("--media-max-mb", type=int, default=0,
                    help="Media nur bis N MB lesen (0 = alles)")
    ap.add_argument("--window", type=int, default=64)
    ap.add_argument("--stride", type=int, default=16)
    ap.add_argument("--literal-min", type=int, default=16)
    ap.add_argument("--assets-allow", default=None,
                    help="Positivliste (Standard editions/assets_allow.tsv im Repo)")
    ap.add_argument("--clean-allow", default=None,
                    help="Ausnahmen (Standard editions/clean_allow.tsv im Repo)")
    ap.add_argument("--clean-deny", default=None,
                    help="Sperrliste (Standard editions/clean_deny.tsv im Repo)")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    if not os.path.isdir(a.dir):
        print("FEHLER: kein Verzeichnis: %s" % a.dir, file=sys.stderr)
        return 3
    top, _ = git_roots()
    edir = os.path.join(top or os.path.join(HERE, "..", "..", ".."), "editions")
    if a.assets_allow is None:
        cand = os.path.join(edir, "assets_allow.tsv")
        a.assets_allow = cand if os.path.isfile(cand) else None
    if a.clean_allow is None:
        cand = os.path.join(edir, "clean_allow.tsv")
        a.clean_allow = cand if os.path.isfile(cand) else None
    if a.clean_deny is None:
        cand = os.path.join(edir, "clean_deny.tsv")
        a.clean_deny = cand if os.path.isfile(cand) else None
    chk = Checker(a.dir, a)
    chk.run()
    rc, verdict, md = write_reports(chk, a.report)
    if not a.quiet:
        cnt = {}
        for f in chk.findings:
            cnt[f["kategorie"]] = cnt.get(f["kategorie"], 0) + 1
        print("clean_check: %s - %d Dateien, %d Funde %s" % (
            verdict, chk.files, len(chk.findings),
            json.dumps(cnt, ensure_ascii=False) if cnt else ""))
        for k, v in chk.checks.items():
            if v.get("status") != "ausgefuehrt":
                print("  %s: %s (%s)" % (k, v.get("status"), v.get("grund", "")))
        for f in chk.findings[:40]:
            loc = f["datei"] + (":%d" % f["zeile"] if f["zeile"] else "")
            print("  [%s] %s - %s" % (f["kategorie"], loc, f["detail"]))
        if len(chk.findings) > 40:
            print("  ... %d weitere (Bericht)" % (len(chk.findings) - 40))
        if a.report:
            print("Bericht: %s(.md/.json)" % (a.report[:-3] if a.report.lower().endswith(".md") else a.report))
    return rc


if __name__ == "__main__":
    sys.exit(main())
