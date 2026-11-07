#!/usr/bin/env python3
"""gen_ordinals_own.py - Kernel-Ordinaltabelle aus EIGENER Vermessung (W157).

Ersetzt gen_ordinals.py (las GPL-Quellen). Diese Version liest AUSSCHLIESSLICH:
  - die eigene default.xbe (Kernel-Thunk-Tabelle + Code-Sections),
  - poc-native/analyse/functions.tsv (eigene Ghidra-Analyse: Funktionsgrenzen),
  - die eigenen HLE-Quellen (REG-Map + hle_-Signaturen = eigenes Verhalten),
  - poc-native/analyse/xdk_symbols.tsv (eigene Ghidra-Analyse, MIT-Werkzeugkette;
    nur als Kreuzcheck der Variablen-Namen).
Keine GPL-Quellen, kein Netz. Braucht capstone (Analyse-Werkzeug, wie
analyse/xbe_disasm.py).

Methode je Tabellenspalte:
  Ordinal: Thunk-Tabelle der XBE (u32-Liste bis Terminator 0, High-Bit maskiert
    wie der eigene Loader). Die Tabelle ist erschoepfend: jeder Kernel-Aufruf
    des Spiels geht durch sie. Nur genutzte Ordinals + Zeile 0 kommen in die
    Ausgabe (Stubs fuer ungenutzte waren toter Code).
  Name: Schnittstellenfakt (Xbox/NT-Kernel-Exportname). Zuordnung verifiziert
    durch (i) eigene HLE-Semantik (111 Funktionen implementiert, stabile Laeufe
    beweisen jede Zuordnung - falsch zugeordnet = Spielbruch), (ii) eigene
    Call-Site-Vermessung (Stelligkeit passt zur API-Semantik), (iii) eigene
    Binaer-Semantik (Caller-Namen), (iv) MIT-Kreuzcheck der 12 Variablen-Namen.
  ist_Variable: ein Slot mit call-/jmp-Referenz (direkt, Trampolin-Caller oder
    mov-reg+call-reg) ist Funktion; nur Daten-Referenzen = Variable.
  argc (Stack-DWORDs): Push-Zaehlung rueckwaerts ab jeder Aufrufstelle, Stopp an
    Kontrollfluss und ESP-Grenzen; dazu Trampolin-Caller, indirekte Calls und
    die HLE-Signatur als Zweitbeleg. Fastcall = 0 (Register-Args). Wo die
    Automatik uneindeutig ist, gilt ein eingebetteter manueller Befund
    (OVERRIDES, je mit Stellen-Adresse) - ohne ihn bricht der Generator ab.
  ist_cdecl: Caller-Cleanup (add esp,N / pop) an den Stellen + HLE-Konvention.

Aufruf (aus Workspace-Root):
  python poc-native/tools/edition/gen_ordinals_own.py [--xbe PFAD] [--check]
Erzeugt: poc-native/harness/src/ordinal_table.inc (+ Evidenzbericht auf stdout).
--check: nur vermessen und pruefen, nichts schreiben.
"""
from __future__ import annotations

import argparse
import bisect
import glob
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_REG, CS_OP_IMM
    from capstone.x86 import X86_REG_ESP
except ImportError:
    sys.exit("capstone fehlt (Analyse-Rechner: pip install capstone)")

HERE = Path(__file__).resolve().parent
WSROOT = HERE.parent.parent.parent
DEFAULT_XBE = WSROOT / "Steel Battalion (Europe).xiso" / "default.xbe"
FALLBACK_XBE = WSROOT / "poc-native" / "work" / "default.xbe"
FUNCTSV = WSROOT / "poc-native" / "analyse" / "functions.tsv"
XDKTSV = WSROOT / "poc-native" / "analyse" / "xdk_symbols.tsv"
SRCDIR = WSROOT / "poc-native" / "harness" / "src"
OUT = SRCDIR / "ordinal_table.inc"

XOR_KT_RETAIL = 0x5B6D40B6
XOR_EP_RETAIL = 0xA8FC57AB

CTRL = {"call", "jmp", "ret", "retf", "iret", "loop", "loope", "loopne",
        "int", "int3", "hlt", "ud1", "ud2"}
# Mnemonics, die op0 (Register) nur LESEN (fuer Indirekt-Erkennung)
READ_OP0 = {"cmp", "test", "push"}


# ---------------------------------------------------------------- XBE-Pfad
def parse_xbe(path):
    d = Path(path).read_bytes()
    if d[0:4] != b"XBEH":
        sys.exit("kein XBEH-Magic")
    base = struct.unpack_from("<I", d, 0x104)[0]
    nsec, secaddr = struct.unpack_from("<II", d, 0x11C)
    secs = []
    for i in range(nsec):
        o = secaddr - base + i * 56
        fl, va, vs, ra, rs, nm = struct.unpack_from("<IIIIII", d, o)
        no = nm - base
        name = d[no:d.index(b"\0", no)].decode(errors="replace")
        secs.append(dict(name=name, va=va, vs=vs, ra=ra, rs=rs, flags=fl))

    def v2o(va):
        for s in secs:
            if s["va"] <= va < s["va"] + s["rs"]:
                return s["ra"] + va - s["va"]
        return None

    kt_xor = struct.unpack_from("<I", d, 0x158)[0]
    if kt_xor & 0x80000000:
        sys.exit("Debug-XBE wird nicht unterstuetzt")
    kt = kt_xor ^ XOR_KT_RETAIL
    # Selbst-Verifikation statt Fremdbeleg (Known-Plaintext):
    entry = struct.unpack_from("<I", d, 0x128)[0] ^ XOR_EP_RETAIL
    text = next(s for s in secs if s["name"] == ".text")
    if not (text["va"] <= entry < text["va"] + text["vs"] and text["flags"] & 0x04):
        sys.exit("Selbstcheck Entry/.text fehlgeschlagen")
    off = v2o(kt)
    if off is None:
        sys.exit("Thunk-Tabelle nicht abbildbar")
    thunks = []
    for i in range(4096):
        (v,) = struct.unpack_from("<I", d, off + i * 4)
        if v == 0:
            break
        if (v & 0x7FFFFFFF) >= 512:
            sys.exit(f"Ordinal ausserhalb: {v:#x}")
        thunks.append(v & 0x7FFFFFFF)
    if len(thunks) < 100:
        sys.exit("zu wenige Thunks - Schluessel/Offset falsch?")
    return d, secs, v2o, kt, thunks


def load_functions():
    out = []
    for ln in FUNCTSV.read_text(encoding="utf-8").splitlines()[1:]:
        p = ln.split("\t")
        try:
            a, sz = int(p[0], 16), int(p[3])
        except (ValueError, IndexError):
            continue
        if 0 < sz <= 0x100000:
            out.append((a, sz, p[1] if len(p) > 1 else ""))
    out.sort()
    return out


class Funcs:
    def __init__(self, funcs):
        self.f = funcs
        self.a = [x[0] for x in funcs]

    def at(self, va):
        i = bisect.bisect_right(self.a, va) - 1
        if i >= 0:
            a, sz, nm = self.f[i]
            if a <= va < a + sz:
                return (a, sz, nm)
        return None


# ------------------------------------------------------- Rueckwaertszaehlung
def backward_count(md, code, base, call_va):
    """Pushes rueckwaerts ab call_va. Liefert
    (n, grenze, start_erreicht, cdecl_add, danach_mnem, ret_imm)."""
    insns = list(md.disasm(code, base))
    idx = next((i for i, ins in enumerate(insns) if ins.address == call_va), None)
    if idx is None:
        return (None,) * 6
    after = insns[idx + 1] if idx + 1 < len(insns) else None
    cdecl_add = None
    if after is not None and after.mnemonic == "add" and len(after.operands) == 2 \
            and after.operands[0].type == CS_OP_REG \
            and after.operands[0].reg == X86_REG_ESP \
            and after.operands[1].type == CS_OP_IMM:
        cdecl_add = after.operands[1].imm
    count, walked, j = 0, 0, idx - 1
    boundary = "funktionsanfang"
    while j >= 0 and walked < 60:
        ins = insns[j]
        m = ins.mnemonic
        if m == "push":
            count += 1
        elif m in CTRL or m.startswith("j"):
            boundary = m
            break
        elif m in ("pop", "popad", "popfd", "pushad", "pushfd", "leave", "enter"):
            boundary = m
            break
        elif m in ("add", "sub", "mov", "lea", "and", "xor") \
                and len(ins.operands) >= 1 and ins.operands[0].type == CS_OP_REG \
                and ins.operands[0].reg == X86_REG_ESP:
            boundary = f"{m}-esp"
            break
        j -= 1
        walked += 1
    else:
        boundary = "funktionsanfang" if j < 0 else "limit60"
    after_m = after.mnemonic if after is not None else None
    after_imm = None
    if after is not None and after.mnemonic == "ret" and len(after.operands) == 1 \
            and after.operands[0].type == CS_OP_IMM:
        after_imm = after.operands[0].imm
    return (count, boundary, boundary == "funktionsanfang", cdecl_add,
            after_m, after_imm)
# ------------------------------------------------------------- Xref-Scan
def scan_xrefs(d, secs, kt, thunks):
    """Vollstaendiger Byte-Scan: call-/jmp-Stellen, Daten-Refs je Slot."""
    n = len(thunks)
    calls, jmps = defaultdict(list), defaultdict(list)
    data_code = Counter()
    for i, o in enumerate(thunks):
        slot = kt + 4 * i
        pat4 = struct.pack("<I", slot)
        for s in secs:
            if not (s["flags"] & 0x04):
                continue
            blob = d[s["ra"]:s["ra"] + s["rs"]]
            p = blob.find(pat4)
            while p >= 0:
                va = s["va"] + p
                if p >= 2 and blob[p - 2] == 0xFF and blob[p - 1] == 0x15:
                    calls[o].append(va - 2)
                elif p >= 2 and blob[p - 2] == 0xFF and blob[p - 1] == 0x25:
                    jmps[o].append(va - 2)
                else:
                    data_code[o] += 1
                p = blob.find(pat4, p + 1)
    return calls, jmps, data_code


def scan_callers(d, secs, target):
    """Direkte call-Ziele (E8 rel32) auf eine Adresse (Trampolin-Caller)."""
    out = []
    for s in secs:
        if not (s["flags"] & 0x04):
            continue
        blob = d[s["ra"]:s["ra"] + s["rs"]]
        for p in range(len(blob) - 5):
            if blob[p] == 0xE8:
                rel = struct.unpack_from("<i", blob, p + 1)[0]
                if s["va"] + p + 5 + rel == target:
                    out.append(s["va"] + p)
    return out


def find_indirect(md, d, secs, v2o, funcs, kt, thunks, o):
    """mov reg,[slot] + call reg im Fenster (Register-indirekte Aufrufe)."""
    slot = kt + 4 * thunks.index(o)
    sites = []
    # Fenster um jede Daten-Referenz im Code legen (robust ohne Funktionsliste)
    pat4 = struct.pack("<I", slot)
    for s in secs:
        if not (s["flags"] & 0x04):
            continue
        blob = d[s["ra"]:s["ra"] + s["rs"]]
        p = blob.find(pat4)
        while p >= 0:
            va = s["va"] + p
            if not (p >= 2 and blob[p - 2] == 0xFF
                    and blob[p - 1] in (0x15, 0x25)):
                wva = max(s["va"], va - 16)
                wend = min(va + 96, s["va"] + s["rs"])
                code = d[v2o(wva):v2o(wva) + (wend - wva)]
                insns = list(md.disasm(code, wva))
                for i, ins in enumerate(insns):
                    if ins.mnemonic == "mov" and len(ins.operands) == 2 \
                            and ins.operands[0].type == CS_OP_REG:
                        from capstone import CS_OP_MEM
                        op1 = ins.operands[1]
                        if op1.type == CS_OP_MEM \
                                and op1.mem.base == 0 and op1.mem.index == 0 \
                                and (op1.mem.disp & 0xFFFFFFFF) == slot:
                            reg = ins.operands[0].reg
                            for nx in insns[i + 1:i + 25]:
                                if nx.mnemonic == "call" and len(nx.operands) == 1 \
                                        and nx.operands[0].type == CS_OP_REG \
                                        and nx.operands[0].reg == reg:
                                    sites.append(nx.address)
                                    # kein break: dasselbe Register kann mehrfach
                                    # gerufen werden (Aufrufstellen sammeln)
                                if len(nx.operands) >= 1 \
                                        and nx.operands[0].type == CS_OP_REG \
                                        and nx.operands[0].reg == reg \
                                        and nx.mnemonic not in READ_OP0:
                                    break
            p = blob.find(pat4, p + 1)
    return sorted(set(sites))


# ------------------------------------------------------- Eigenes HLE-Verhalten
def parse_hle():
    """REG-Map + hle_-Signaturen aus den eigenen Quellen (Zweitbeleg)."""
    reg = {}
    for f in glob.glob(str(SRCDIR / "*.c")):
        txt = Path(f).read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"REG\(\s*(\d+)\s*,\s*(\w+)\s*\)", txt):
            reg[int(m.group(1))] = m.group(2)
    sig = {}
    for f in glob.glob(str(SRCDIR / "*.c")):
        txt = Path(f).read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(
                r"(?:static\s+)?(?:u32|void|int|DWORD|LONG|unsigned\s+__int64|"
                r"uint64_t|uint32_t|int32_t|BOOL|HANDLE)\s+"
                r"(__stdcall|__cdecl|__fastcall|WINAPI|NTAPI)?\s*(hle_\w+)\s*\((.*?)\)\s*\{",
                txt, re.S):
            conv, name, params = m.group(1) or "", m.group(2), m.group(3).strip()
            params = re.sub(r"/\*.*?\*/", "", params, flags=re.S).strip()
            if params in ("", "void"):
                n = 0
            else:
                depth, n = 0, 1
                for ch in params:
                    if ch == "(":
                        depth += 1
                    elif ch == ")":
                        depth -= 1
                    elif ch == "," and depth == 0:
                        n += 1
            sig[name] = (n, conv)
    return reg, sig


def load_xdk_kernel_names():
    """Kernel-Exportnamen aus der eigenen Ghidra-Analyse (MIT-Werkzeugkette)."""
    out = set()
    for ln in XDKTSV.read_text(encoding="utf-8").splitlines()[1:]:
        p = ln.split("\t")
        if len(p) >= 4 and p[2] == "xboxkrnl.exe":
            out.add(p[1])
    return out


# ------------------------------------------------- Schnittstellenfakten (Namen)
# Ordinal -> (Name, Herkunft): H = eigener HLE-Bezeichner + funktional verifiziert
# (falsch zugeordnet = Spielbruch; stabile Laeufe beweisen jede Zuordnung),
# M = kreuzbelegt durch eigene Ghidra-Analyse (MIT-Werkzeugkette, xdk_symbols.tsv),
# S = zusaetzlich Binaer-Semantik (Caller-Namen, Aufrufmuster) - Belege im Bericht.
NAMES = {
    1: ("AvGetSavedDataAddress", "H"),
    2: ("AvSendTVEncoderOption", "H"),
    3: ("AvSetDisplayMode", "H"),
    4: ("AvSetSavedDataAddress", "H"),
    5: ("DbgBreakPoint", "H"),
    8: ("DbgPrint", "H"),
    14: ("ExAllocatePool", "H"),
    15: ("ExAllocatePoolWithTag", "H,S"),
    16: ("ExEventObjectType", "M,S"),
    17: ("ExFreePool", "H"),
    23: ("ExQueryPoolBlockSize", "H"),
    24: ("ExQueryNonVolatileSetting", "H"),
    40: ("HalDiskCachePartitionCount", "M"),
    44: ("HalGetInterruptVector", "H,S"),
    46: ("HalReadWritePCISpace", "H,S"),
    47: ("HalRegisterShutdownNotification", "H"),
    49: ("HalReturnToFirmware", "H"),
    65: ("IoCreateDevice", "H"),
    67: ("IoCreateSymbolicLink", "H"),
    71: ("IoFileObjectType", "M,S"),
    74: ("IoInvalidDeviceRequest", "H,M"),
    81: ("IoStartNextPacket", "H"),
    83: ("IoStartPacket", "H"),
    87: ("IofCompleteRequest", "H"),
    95: ("KeBugCheck", "H"),
    97: ("KeCancelTimer", "H"),
    98: ("KeConnectInterrupt", "H"),
    99: ("KeDelayExecutionThread", "H"),
    100: ("KeDisconnectInterrupt", "H"),
    107: ("KeInitializeDpc", "H"),
    109: ("KeInitializeInterrupt", "H"),
    113: ("KeInitializeTimerEx", "H"),
    119: ("KeInsertQueueDpc", "H"),
    125: ("KeQueryInterruptTime", "H"),
    128: ("KeQuerySystemTime", "H"),
    129: ("KeRaiseIrqlToDpcLevel", "H"),
    137: ("KeRemoveQueueDpc", "H"),
    139: ("KeRestoreFloatingPointState", "H"),
    142: ("KeSaveFloatingPointState", "H"),
    143: ("KeSetBasePriorityThread", "H,S"),
    145: ("KeSetEvent", "H"),
    149: ("KeSetTimer", "H"),
    151: ("KeStallExecutionProcessor", "H"),
    153: ("KeSynchronizeExecution", "H"),
    156: ("KeTickCount", "M,S"),
    159: ("KeWaitForSingleObject", "H"),
    160: ("KfRaiseIrql", "H"),
    161: ("KfLowerIrql", "H"),
    164: ("LaunchDataPage", "M,S"),
    165: ("MmAllocateContiguousMemory", "H"),
    166: ("MmAllocateContiguousMemoryEx", "H"),
    167: ("MmAllocateSystemMemory", "H"),
    168: ("MmClaimGpuInstanceMemory", "H"),
    171: ("MmFreeContiguousMemory", "H"),
    172: ("MmFreeSystemMemory", "H"),
    173: ("MmGetPhysicalAddress", "H"),
    175: ("MmLockUnlockBufferPages", "H"),
    176: ("MmLockUnlockPhysicalPage", "H"),
    178: ("MmPersistContiguousMemory", "H"),
    179: ("MmQueryAddressProtect", "H"),
    180: ("MmQueryAllocationSize", "H"),
    181: ("MmQueryStatistics", "H"),
    182: ("MmSetAddressProtect", "H"),
    184: ("NtAllocateVirtualMemory", "H"),
    186: ("NtClearEvent", "H"),
    187: ("NtClose", "H"),
    189: ("NtCreateEvent", "H"),
    190: ("NtCreateFile", "H"),
    192: ("NtCreateMutant", "H"),
    196: ("NtDeviceIoControlFile", "H"),
    198: ("NtFlushBuffersFile", "H"),
    199: ("NtFreeVirtualMemory", "H"),
    200: ("NtFsControlFile", "H"),
    202: ("NtOpenFile", "H"),
    203: ("NtOpenSymbolicLinkObject", "H"),
    207: ("NtQueryDirectoryFile", "H"),
    210: ("NtQueryFullAttributesFile", "H"),
    211: ("NtQueryInformationFile", "H"),
    215: ("NtQuerySymbolicLinkObject", "H"),
    217: ("NtQueryVirtualMemory", "H"),
    218: ("NtQueryVolumeInformationFile", "H"),
    219: ("NtReadFile", "H"),
    221: ("NtReleaseMutant", "H"),
    224: ("NtResumeThread", "H,S"),
    225: ("NtSetEvent", "H"),
    226: ("NtSetInformationFile", "H"),
    231: ("NtSuspendThread", "H,S"),
    233: ("NtWaitForSingleObject", "H"),
    234: ("NtWaitForSingleObjectEx", "H"),
    235: ("NtWaitForMultipleObjectsEx", "H"),
    236: ("NtWriteFile", "H"),
    246: ("ObReferenceObjectByHandle", "H"),
    250: ("ObfDereferenceObject", "H,S"),
    255: ("PsCreateSystemThreadEx", "H"),
    258: ("PsTerminateSystemThread", "H"),
    259: ("PsThreadObjectType", "M,S"),
    260: ("RtlAnsiStringToUnicodeString", "H"),
    269: ("RtlCompareMemoryUlong", "H"),
    277: ("RtlEnterCriticalSection", "H"),
    279: ("RtlEqualString", "H"),
    289: ("RtlInitAnsiString", "H"),
    291: ("RtlInitializeCriticalSection", "H"),
    294: ("RtlLeaveCriticalSection", "H"),
    301: ("RtlNtStatusToDosError", "H"),
    302: ("RtlRaiseException", "H"),
    304: ("RtlTimeFieldsToTime", "H"),
    305: ("RtlTimeToTimeFields", "H"),
    308: ("RtlUnicodeStringToAnsiString", "H"),
    312: ("RtlUnwind", "H,S"),
    322: ("XboxHardwareInfo", "M,S"),
    323: ("XboxHDKey", "M,S"),
    325: ("XboxSignatureKey", "M,S"),
    327: ("XeLoadSection", "H"),
    328: ("XeUnloadSection", "H"),
    335: ("XcSHAInit", "H"),
    336: ("XcSHAUpdate", "H"),
    337: ("XcSHAFinal", "H"),
    354: ("XboxAlternateSignatureKeys", "M,S"),
    356: ("HalBootSMCVideoMode", "M"),
    357: ("IdexChannelObject", "M"),
    358: ("HalIsResetOrShutdownPending", "H"),
    359: ("IoMarkIrpMustComplete", "H"),
    360: ("HalInitiateShutdown", "H"),
}

# ------------------------------------------------- Manuelle Befunde (W157)
# Nur wo die Automatik uneindeutig ist: Ordinal -> (ist_var, argc, cdecl, Grund).
# Jede Stelle per Adresse belegt; ohne Eintrag bricht der Generator dort ab.
OVERRIDES = {
    5: (0, 0, 0, "Trampolin jmp [slot] @0x1FC322 ohne Caller; argc aus HLE (0)"),
    8: (0, 2, 1, "varargs direkt gemessen: 6 Trampolin-Caller, pushes 1-13, "
        "Caller-Cleanup add esp,N=4x (4x) bzw. pop (2x); 2 = HLE-Namen-Parameter"),
    14: (0, 1, 0, "Stimmen 1-5: echte Args je 1 (z.B. push 0x60 @0x255834), "
        "Rest Saved-Regs nach sub-esp"),
    15: (0, 2, 0, "push esi = Save + 2 weitergeleitete Args @0x2219EB "
        "(DSound_CMemoryManager_PoolAlloc passt semantisch)"),
    24: (0, 5, 0, "Trampolin-Caller @0x191154: 5 weitergeleitete Args "
        "(ret 0x14 des Wrappers passt)"),
    44: (0, 2, 0, "push edi @0x217357 = Save (pop edi im Epilog @0x217433)"),
    74: (0, 2, 0, "Adress-Abnahme: mov eax,[slot] @0x2566EA in Dispatch-Tabelle; "
        "argc aus HLE (2), kein direkter Call messbar"),
    87: (0, 0, 0, "10/10 Stellen 0 pushes, Args in ecx/edx (z.B. @0x256BF3) = "
        "fastcall(2); HLE-stdcall/2 weicht ab (Fund, Pfad in Laeufen stumm)"),
    129: (0, 0, 0, "Modus-Patt 0/2: Nicht-Null-Stimmen = Saved-Regs "
        "(z.B. push esi/edi @0x22A8AF); HLE 0"),
    153: (0, 3, 0, "5 Saved-Regs + 3 Args @0x22A2C9"),
    160: (0, 0, 0, "fastcall/1: mov cl + call @0x25A12C; Stimme 1 = Saved-Reg; "
        "HLE fastcall"),
    161: (0, 0, 0, "46/46 Stellen 0 pushes (fastcall/1, z.B. mov cl,al @0x20AEA2); "
        "HLE fastcall"),
    165: (0, 1, 0, "Stimme 2 = Save + Arg @0x256220; Zweitstelle 1"),
    167: (0, 2, 0, "2 Saved + 2 Args (4, size) @0x18E0B4"),
    168: (0, 2, 0, "aeussere pushes + 2 Args (out, 0x5000) @0x2180D0"),
    176: (0, 2, 0, "push edi @0x25BEF7 = Save (pop edi @0x25BF3A)"),
    198: (0, 2, 0, "push ecx/ecx = Frame-Allok, 2 Args @0x18DE6C"),
    203: (0, 2, 0, "1 Stelle @0x1923D3 pusht 2 (XLaunch-Pfad); HLE liest 3 "
        "(Fund, Pfad in Laeufen stumm)"),
    207: (0, 10, 0, "Stimmen 10/11/13 = 10 Args + 0-3 Saved "
        "(pop esi @0x19140B belegt Save)"),
    215: (0, 3, 0, "push edi @0x1923DD = Save (pop edi @0x19240C); 3 Args passen "
        "zur Semantik (xh, target, NULL-retlen)"),
    218: (0, 5, 0, "saubere 5er-Stelle @0x19262B; 6/7 = 5 + Saved"),
    224: (0, 2, 0, "push ebp (Frame) + 2 Args @0x190C9D"),
    231: (0, 2, 0, "push ebp (Frame) + 2 Args @0x190C77"),
    246: (0, 3, 0, "push ebp (Frame) + 3 Args @0x190C2B"),
    250: (0, 0, 0, "fastcall/1: mov ecx + call (3/3, z.B. @0x190C56); HLE fastcall"),
    277: (0, 1, 0, "Modus 1 (212/381); Tails = Saved-Regs/aeussere Args "
        "(z.B. @0x190BCF)"),
    291: (0, 1, 0, "Modus 1 (27/59); Tails = Saved-Regs (z.B. @0x1A0FF7)"),
    294: (0, 1, 0, "559/560; Einzelstimme 2 = Save (pop esi @0x1FAFB2); "
        "1x add esp,8 = aeusseres Cleanup"),
    312: (0, 4, 0, "Trampolin-Caller @0x201F4F: 3 Saved + 4 Args "
        "(SEH-Unwind, Caller-Name passt)"),
    327: (0, 1, 0, "Save + 1 Arg @0x19149B"),
    335: (0, 1, 0, "Trampolin-Caller 2x je 1 @0x19207A/@0x1920E3"),
    336: (0, 3, 0, "Trampolin-Caller 4x je 3"),
    337: (0, 2, 0, "Trampolin-Caller 2x je 2 @0x1920DD/@0x1920F8"),
    358: (0, 0, 0, "0 Args, nur Frame/Saved @0x2576D6; test al,al = Bool-Ret"),
    360: (0, 0, 0, "0 pushes @0x193588 (cmp/je davor); Fenster-Stimme 4 war "
        "fehl-synchronisiert"),
}
# ------------------------------------------------- Vermessung + Befund
def vote_at(md, d, secs, v2o, funcs, site):
    """Stimme an einer call-Stelle (Funktion oder Fenster-Fallback)."""
    def sec_at(va):
        for s in secs:
            if s["va"] <= va < s["va"] + s["rs"]:
                return s
        return None

    f = funcs.at(site)
    if f is not None:
        fva, fsz, _fnm = f
        code = d[v2o(fva):v2o(fva) + fsz]
        return backward_count(md, code, fva, site) + ("funktion",)
    s = sec_at(site)
    for back in (64, 128, 192):
        wva = max(s["va"], site - back)
        wend = min(site + 16, s["va"] + s["rs"])
        code = d[v2o(wva):v2o(wva) + (wend - wva)]
        r = backward_count(md, code, wva, site)
        if r[0] is not None:
            return r + ("fenster",)
    return (None,) * 6 + ("kein-sync",)


def measure(md, d, secs, v2o, funcs, kt, thunks, calls, jmps):
    """Stimmen je Ordinal: direkte Calls + Trampolin-Caller + indirekte Calls."""
    votes = defaultdict(list)   # o -> [(n, cdecl_add, danach, quelle)]
    # direkte Calls
    for o, sites in calls.items():
        for site in sites:
            cnt, _bnd, _rs, cadd, after_m, after_imm, _q = vote_at(
                md, d, secs, v2o, funcs, site)
            if cnt is None:
                continue
            if cnt == 0 and after_m == "ret" and after_imm:
                votes[o].append((after_imm // 4, None, "retN", "direkt"))
            elif cnt == 0 and after_m == "ret":
                continue  # Forwarder (call; ret) ohne Info
            else:
                votes[o].append((cnt, cadd, after_m, "direkt"))
    # Trampolin-Caller (jmp-Stellen selbst tragen keine Args)
    for o, jsites in jmps.items():
        for j in jsites:
            for caller in scan_callers(d, secs, j):
                cnt, _bnd, _rs, cadd, after_m, after_imm, _q = vote_at(
                    md, d, secs, v2o, funcs, caller)
                if cnt is None:
                    continue
                votes[o].append((cnt, cadd, after_m, f"trampolin@0x{j:X}"))
    # indirekte Calls (nur wo sonst nichts messbar - spart Laufzeit)
    need_indirect = [o for o in thunks if not votes[o] and not calls[o]]
    for o in need_indirect:
        for site in find_indirect(md, d, secs, v2o, funcs, kt, thunks, o):
            cnt, _bnd, _rs, cadd, after_m, _ai, _q = vote_at(
                md, d, secs, v2o, funcs, site)
            if cnt is not None:
                votes[o].append((cnt, cadd, after_m, f"indirekt@0x{site:X}"))
    return votes


def main():
    ap = argparse.ArgumentParser(description="Kernel-Ordinaltabelle aus eigener "
                                 "Vermessung erzeugen")
    ap.add_argument("--xbe", default=None, help="default.xbe (Default: XISO im "
                    "Repo, Fallback poc-native/work/default.xbe)")
    ap.add_argument("--check", action="store_true", help="nur pruefen")
    args = ap.parse_args()

    cands = [Path(args.xbe)] if args.xbe else [DEFAULT_XBE, FALLBACK_XBE]
    xbe = next((c for c in cands if c.exists() and c.stat().st_size >= 1000000),
               None)
    if xbe is None:
        sys.exit(f"keine echte default.xbe gefunden ({', '.join(map(str, cands))})"
                 " - ggf. --xbe auf das eigene Spielabbild zeigen")
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    d, secs, v2o, kt, thunks = parse_xbe(str(xbe))
    print(f"XBE: {xbe} ({len(d)} Bytes), kt=0x{kt:X}, {len(thunks)} Thunks")
    funcs = Funcs(load_functions())
    reg, sig = parse_hle()
    print(f"HLE: {len(reg)} REG-Eintraege, {len(sig)} Signaturen")
    xdk = load_xdk_kernel_names()

    # Kreuzcheck 1: Thunk-Menge == Namen-Tabelle (Spiel-Update faellt auf)
    if set(thunks) != set(NAMES):
        sys.exit(f"FEHLER: Thunks ({len(thunks)}) != NAMES ({len(NAMES)}): "
                 f"nur-Thunks={sorted(set(thunks) - set(NAMES))} "
                 f"nur-NAMES={sorted(set(NAMES) - set(thunks))}")
    # Kreuzcheck 2: Funktionsnamen == eigene REG-Map
    for o in thunks:
        if o in reg:
            want = reg[o][4:]
            if want.endswith("_counted"):
                want = want[:-8]
            if NAMES[o][0] != want:
                sys.exit(f"FEHLER: Name {o} = {NAMES[o][0]}, REG sagt {want}")
    # Kreuzcheck 3: Variablen-Namen in MIT-Ghidra-Menge
    for o in thunks:
        if o not in reg and NAMES[o][0] not in xdk:
            sys.exit(f"FEHLER: Variablen-Name {o} = {NAMES[o][0]} ohne MIT-Beleg")

    calls, jmps, data_code = scan_xrefs(d, secs, kt, thunks)
    votes = measure(md, d, secs, v2o, funcs, kt, thunks, calls, jmps)

    rows = {}   # o -> (name, is_var, argc, cdecl, beleg)
    fails = []
    print("=" * 100)
    print("ORD  NAME                              KIND  ARGC C  BELEG")
    for o in sorted(thunks):
        name, prov = NAMES[o]
        h = reg.get(o)
        hs, hconv = sig.get(h, (None, "")) if h else (None, "")
        is_fast = "fastcall" in hconv
        is_cdecl_hle = "cdecl" in hconv
        vs = [v for (v, _c, _a, _q) in votes[o]]
        dist = Counter(vs)
        top = dist.most_common()
        mode = top[0][0] if top and (len(top) == 1 or top[0][1] > top[1][1]) else None
        share = (top[0][1] / len(vs)) if top else 0
        cleanups = sum(1 for (_v, c, a, _q) in votes[o]
                       if c is not None or a == "pop")
        has_call = bool(calls[o]) or bool(vs) or bool(jmps[o])

        beleg, is_var, argc, cdecl = "", 0, None, 0
        if o in OVERRIDES:
            is_var, argc, cdecl, beleg = OVERRIDES[o]
            beleg = "MANUELL: " + beleg
            if has_call and is_var:
                fails.append(f"{o}: Override sagt VAR trotz Call-Referenz")
            if not has_call and not data_code[o] and not is_var:
                fails.append(f"{o}: Override sagt FUNC ohne jede Referenz")
        elif not has_call:
            if not data_code[o]:
                fails.append(f"{o}: UNREF (keine Referenz, kein Override)")
                continue
            is_var, argc = 1, 0
            beleg = f"VAR: {data_code[o]} Daten-Refs, 0 Calls"
            if h:
                fails.append(f"{o}: VAR, aber HLE-Impl {h} vorhanden")
        else:
            if hs is None:
                fails.append(f"{o}: FUNC, aber keine HLE-Signatur ({h})")
                continue
            if is_cdecl_hle:
                fails.append(f"{o}: cdecl ohne Override ({name})")
                continue
            if is_fast:
                if mode == 0 and all(v == 0 for v in vs):
                    argc = 0
                    beleg = f"fastcall: {len(vs)}x 0 pushes, HLE fastcall"
                else:
                    fails.append(f"{o}: fastcall, aber Stimmen {dict(dist)}")
                    continue
            elif mode is not None and mode == hs and (share >= 0.6 or len(vs) <= 2):
                argc = mode
                beleg = (f"Modus {mode} ({top[0][1]}/{len(vs)}), HLE={hs}"
                         + (f", Verteilung {dict(sorted(dist.items()))}"
                            if len(dist) > 1 else ", einstimmig"))
            else:
                fails.append(f"{o}: kein Auto-Befund (Modus {mode}, HLE {hs}, "
                             f"n={len(vs)} {dict(sorted(dist.items()))})")
                continue
            if hs is not None and not is_fast and not is_cdecl_hle and cleanups \
                    and cleanups / max(1, len(vs)) >= 0.5:
                fails.append(f"{o}: cdecl-Verdacht ({cleanups}/{len(vs)} Cleanups)")
                continue
        rows[o] = (name, is_var, argc, cdecl, beleg, prov)
        print(f"{o:3d}  {name:32s}  {'VAR' if is_var else 'FUNC'}  "
              f"{argc:4d} {cdecl}  {beleg[:62]}")

    if fails:
        print("=" * 100)
        print("ABBRUCH - offene Befunde:")
        for f in fails:
            print("  " + f)
        sys.exit(1)

    header = (
        "/* Ordinaltabelle des Xbox-Kernels (Steel Battalion, PAL 43430002).\n"
        " * Neu hergeleitet (W157) - Clean-Room aus der eigenen default.xbe,\n"
        " * ohne GPL-Quellen. Generator: poc-native/tools/edition/gen_ordinals_own.py.\n"
        " * Spalten: Ordinal = Thunk-Tabelle der eigenen XBE (erschoepfend; nur\n"
        " * genutzte Ordinals + Zeile 0; ungenutzte entfallen - ihre Stubs waren\n"
        " * toter Code). Name = Schnittstellenfakt (Xbox/NT-Exportname), Zuordnung\n"
        " * verifiziert durch eigene HLE-Semantik + Call-Site-Vermessung + stabile\n"
        " * Laeufe (VAR-Namen zusaetzlich MIT-kreuzbelegt, s. Generator). Variable =\n"
        " * nur Daten-Referenzen (kein call/jmp). argc = Stack-DWORDs aus eigener\n"
        " * Vermessung (Push-Zaehlung, Trampolin-Caller, indirekte Calls) bzw. aus\n"
        " * der HLE-Signatur wo nicht direkt messbar; fastcall = 0. cdecl =\n"
        " * Caller-Cleanup gemessen (DbgPrint) bzw. HLE-Konvention. */\n"
        "/* K(Ordinal, Name, ist_Variable, argc_DWORDs, ist_cdecl) */\n"
    )
    body = ["K(  0, zeroptr, 0,  0, 0)  /* Zeile 0: eigener Default-Stub, kein Export */"]
    for o in sorted(rows):
        name, is_var, argc, cdecl = rows[o][:4]
        body.append(f"K({o:3d}, {name}, {is_var}, {argc:2d}, {cdecl})")
    out_text = header + "\n".join(body) + "\n"

    if args.check:
        print(f"--check: {len(rows)} Zeilen + Kopf OK (nichts geschrieben)")
        return 0
    OUT.write_text(out_text, encoding="utf-8")
    print(f"{len(rows) + 1} Zeilen -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

