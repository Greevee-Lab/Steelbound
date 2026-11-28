#!/usr/bin/env python3
"""pe_harden_check.py - W332 SEC-HARDEN: Haertungs-Schalter der ausgelieferten Binaerdateien pruefen.

Liest die PE-Kopfdaten selbst (ohne dumpbin): DllCharacteristics (ASLR, DEP, CFG, kein SEH),
erweiterte DllCharacteristics (CET), Lade-Konfiguration (Sicherheits-Cookie = /GS, SafeSEH,
CFG-Tabelle, DependentLoadFlags) und den VC-Feature-Eintrag (Anzahl Objekte mit /GS bzw. /sdl).

  python pe_harden_check.py <datei|ordner> ...          Tabelle ausgeben
  python pe_harden_check.py --check <ordner> ...        Soll je Binaerdatei pruefen (Exit 1 bei Abweichung)
  python pe_harden_check.py --md <ordner> ...           Markdown-Tabelle (Bericht)

Soll (POLICY) je ausgelieferter Datei; Begruendung der Ausnahmen in BERICHT_M2 Abschnitt W332:
sb_ldr.exe braucht die feste Basis 0x10000 (XBE-Abbild) und hat kein CRT (kein /GS, keine
Lade-Konfiguration); sb_emu.dll bleibt auf fester Basis 0x30000000 (deterministisches Speicher-Layout).
Nur Standardbibliothek.
"""
import os
import struct
import sys

DC = {  # IMAGE_DLLCHARACTERISTICS_*
    0x0020: "HIGHENTROPYVA",
    0x0040: "DYNAMICBASE",
    0x0080: "FORCE_INTEGRITY",
    0x0100: "NXCOMPAT",
    0x0400: "NO_SEH",
    0x4000: "GUARD_CF",
}

# Soll: required = muss gesetzt sein; forbidden = darf nicht gesetzt sein (bewusste Ausnahme);
# base = feste Basis; dlf = Bits in DependentLoadFlags; sdl = mind. ein Objekt mit /sdl.
POLICY = {
    "sb_launcher.exe": dict(required={"DYNAMICBASE", "NXCOMPAT", "GUARD_CF", "GS", "CETCOMPAT"},
                            forbidden=set(), dlf=0x800, sdl=True),
    # W537 LAUNCHER-SLIM: Konto-Hilfsprogramm, gleiche Haertung wie der Launcher
    "sb_account.exe": dict(required={"DYNAMICBASE", "NXCOMPAT", "GUARD_CF", "GS", "CETCOMPAT"},
                           forbidden=set(), dlf=0x800, sdl=True),
    "sb_emu.dll": dict(required={"NXCOMPAT", "GS"}, forbidden={"DYNAMICBASE"}, base=0x30000000,
                       dlf=0x800, sdl=True),
    "sb_ldr.exe": dict(required={"NXCOMPAT"}, forbidden={"DYNAMICBASE"}, base=0x10000),
}


def _rva_to_off(secs, rva):
    for va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            off = rva - va
            if off >= rsz:
                return None
            return raw + off
    return None


def analyze(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"MZ":
        raise ValueError("kein MZ")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise ValueError("kein PE")
    machine, nsec = struct.unpack_from("<HH", d, pe + 4)
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    is64 = magic == 0x20B
    if is64:
        base = struct.unpack_from("<Q", d, opt + 24)[0]
        dllch = struct.unpack_from("<H", d, opt + 70)[0]
        dd = opt + 112
    else:
        base = struct.unpack_from("<I", d, opt + 28)[0]
        dllch = struct.unpack_from("<H", d, opt + 70)[0]
        dd = opt + 96
    ndd = struct.unpack_from("<I", d, dd - 4)[0]

    def ddir(i):
        if i >= ndd:
            return 0, 0
        return struct.unpack_from("<II", d, dd + 8 * i)

    secs = []
    so = opt + opt_size
    for i in range(nsec):
        vsz, va, rsz, raw = struct.unpack_from("<IIII", d, so + 40 * i + 8)
        secs.append((va, vsz, raw, rsz))
    flags = {name for bit, name in DC.items() if dllch & bit}
    info = dict(path=path, machine=machine, is64=is64, base=base, dllch=dllch, flags=flags,
                cookie=0, safeseh=None, guard_flags=0, cf_count=0, dlf=0, gs_objs=None, sdl_objs=None,
                cet=False)
    # Lade-Konfiguration (Verzeichnis 10)
    lc_rva, lc_size = ddir(10)
    lo = _rva_to_off(secs, lc_rva) if lc_rva else None
    if lo is not None:
        size = struct.unpack_from("<I", d, lo)[0]
        if is64:
            fld = dict(dlf=(78, "<H"), cookie=(88, "<Q"), seh=(96, "<Q"), sehn=(104, "<Q"),
                       cfn=(136, "<Q"), gf=(144, "<I"))
        else:
            fld = dict(dlf=(54, "<H"), cookie=(60, "<I"), seh=(64, "<I"), sehn=(68, "<I"),
                       cfn=(84, "<I"), gf=(88, "<I"))

        def g(k):
            o, fmt = fld[k]
            if o + struct.calcsize(fmt) > size:
                return 0
            return struct.unpack_from(fmt, d, lo + o)[0]
        info["dlf"] = g("dlf")
        info["cookie"] = g("cookie")
        if not is64:
            info["safeseh"] = g("sehn") if g("seh") else None
        info["cf_count"] = g("cfn")
        info["guard_flags"] = g("gf")
    if info["cookie"]:
        flags.add("GS")
    # Debug-Verzeichnis (6): VC-Feature (12), erweiterte DllCharacteristics (20)
    dbg_rva, dbg_size = ddir(6)
    do = _rva_to_off(secs, dbg_rva) if dbg_rva else None
    if do is not None:
        for i in range(dbg_size // 28):
            e = do + 28 * i
            typ, sz, _rva, praw = struct.unpack_from("<IIII", d, e + 12)
            if typ == 12 and sz >= 20:
                _pre, _cc, gs, sdl, _gn = struct.unpack_from("<IIIII", d, praw)
                info["gs_objs"], info["sdl_objs"] = gs, sdl
            elif typ == 20 and sz >= 4:
                ext = struct.unpack_from("<I", d, praw)[0]
                if ext & 0x01:  # IMAGE_DLLCHARACTERISTICS_EX_CET_COMPAT
                    info["cet"] = True
                    flags.add("CETCOMPAT")
    return info


def check(info):
    """Liste der Abweichungen vom Soll (leer = ok; Datei ohne Soll = ok)."""
    pol = POLICY.get(os.path.basename(info["path"]).lower())
    if not pol:
        return []
    bad = []
    for r in sorted(pol.get("required", ())):
        if r not in info["flags"]:
            bad.append("fehlt " + r)
    for r in sorted(pol.get("forbidden", ())):
        if r in info["flags"]:
            bad.append("unerwartet " + r)
    if "base" in pol and info["base"] != pol["base"]:
        bad.append("Basis 0x%X statt 0x%X" % (info["base"], pol["base"]))
    if pol.get("dlf") and (info["dlf"] & pol["dlf"]) != pol["dlf"]:
        bad.append("DependentLoadFlags 0x%X ohne 0x%X" % (info["dlf"], pol["dlf"]))
    # /sdl laesst sich nur mit Debug-Verzeichnis (VC-Feature-Eintrag, /DEBUG) belegen; ohne: nicht pruefbar
    if pol.get("sdl") and info["sdl_objs"] == 0:
        bad.append("kein Objekt mit /sdl")
    return bad


def row(info):
    f = info["flags"]

    def yn(k):
        return "ja" if k in f else "nein"
    seh = "-" if info["is64"] else ("kein SEH" if "NO_SEH" in f else
                                    ("SafeSEH %d" % info["safeseh"] if info["safeseh"] is not None else "ohne Tabelle"))
    gs = yn("GS")
    if info["gs_objs"] is not None:
        gs += " (%d Obj.)" % info["gs_objs"]
    sdl = "-" if info["sdl_objs"] is None else ("%d Obj." % info["sdl_objs"] if info["sdl_objs"] else "nein")
    return [os.path.basename(info["path"]), "x64" if info["is64"] else "x86", "0x%X" % info["base"],
            yn("DYNAMICBASE"), yn("HIGHENTROPYVA") if info["is64"] else "n/a (32-bit)", yn("NXCOMPAT"), gs, sdl,
            yn("GUARD_CF") + (" (%d Ziele)" % info["cf_count"] if "GUARD_CF" in f else ""),
            yn("CETCOMPAT"), seh, "0x%X" % info["dlf"] if info["dlf"] else "-"]


HDR = ["Datei", "Arch", "Basis", "ASLR", "HighEntropyVA", "DEP/NX", "/GS", "/sdl", "CFG", "CET", "SEH",
       "DependentLoadFlags"]


def collect(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            for n in sorted(os.listdir(a)):
                if n.lower() in POLICY:
                    out.append(os.path.join(a, n))
        else:
            out.append(a)
    return out


def main(argv):
    mode = "table"
    args = []
    for a in argv:
        if a in ("--check", "--md"):
            mode = a[2:]
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            args.append(a)
    files = collect(args)
    if not files:
        print("pe_harden_check: keine Binaerdateien gefunden", file=sys.stderr)
        return 2
    rc = 0
    rows = []
    for p in files:
        try:
            info = analyze(p)
        except (OSError, ValueError, struct.error) as e:
            print("FEHLER %s: %s" % (p, e))
            rc = 1
            continue
        bad = check(info)
        rows.append((row(info), bad))
        if bad:
            rc = 1
    if mode == "md":
        print("| " + " | ".join(HDR) + " |")
        print("|" + "---|" * len(HDR))
        for r, _ in rows:
            print("| " + " | ".join(r) + " |")
    else:
        for r, bad in rows:
            print("%-16s %s" % (r[0], "  ".join("%s=%s" % (h, v) for h, v in zip(HDR[1:], r[1:]))))
            if mode == "check":
                print("  %s" % ("OK" if not bad else "ABWEICHUNG: " + "; ".join(bad)))
    if mode == "check":
        print("pe_harden_check: %s" % ("OK" if rc == 0 else "ABWEICHUNG"))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
