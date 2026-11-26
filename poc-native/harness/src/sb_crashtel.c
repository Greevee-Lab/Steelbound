/* sb_crashtel.c - W522 CRASH-TELEMETRY: Datensatz, Einordnung, Datei (Beschreibung s. sb_crashtel.h).
 * Gemeinsam fuer sb_emu.dll (Absturz-Handler, Netzbericht), sb_launcher.exe (Exitcode) und die Selbsttests.
 * Alles hier ist im Absturz-Handler aufrufbar: keine Allokation, nur Stack-Puffer und Win32-Dateiaufrufe. */
#include "sb_crashtel.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *const k_cls[SBCT_C_N] = {NULL,    "exit_ok", "closed", "killed", "hang", "gpu_lost",
                                            "av",    "stack",   "oom",    "abort",  "other"};
static const char *const k_mod[SBCT_M_N] = {NULL,   "sb_emu", "sb_ldr", "game",   "d3d11",  "dxgi",   "d3dcomp",
                                            "ntdll", "kernel", "crt",    "amd",    "nvidia", "intel",  "other"};
static const char *const k_ph[SBCT_P_N] = {NULL, "menu", "lobby", "loading", "match", "debrief"};
static const char *const k_dxr[SBCT_D_N] = {NULL,      "ok",     "hung",    "removed", "reset",
                                            "driver",  "invalid", "oom",    "other"};
static const char *const k_rt[6] = {NULL, "lt1", "1_5", "5_15", "15_60", "gt60"};
static const char *const k_mem[5] = {NULL, "lt1", "1_2", "2_3", "gt3"};
static const char *const k_acc[4] = {NULL, "r", "w", "x"};
static const char *const k_game[3] = {NULL, "sb", "loc"};
static const char *const k_ed[3] = {NULL, "community", "private"};

static volatile LONG g_phase;

const char *sbct_cls_name(int c) { return c > 0 && c < SBCT_C_N ? k_cls[c] : NULL; }
const char *sbct_mod_name(int m) { return m > 0 && m < SBCT_M_N ? k_mod[m] : NULL; }
const char *sbct_ph_name(int p) { return p > 0 && p < SBCT_P_N ? k_ph[p] : NULL; }
const char *sbct_dxr_name(int d) { return d > 0 && d < SBCT_D_N ? k_dxr[d] : NULL; }
const char *sbct_rt_name(int r) { return r > 0 && r < 6 ? k_rt[r] : NULL; }
const char *sbct_mem_name(int m) { return m > 0 && m < 5 ? k_mem[m] : NULL; }

void sbct_set_phase(int ph) { InterlockedExchange(&g_phase, ph > 0 && ph < SBCT_P_N ? ph : 0); }
int sbct_phase(void) { return (int)g_phase; }

int sbct_exc_cls(unsigned code)
{
    switch (code) {
    case 0xC0000005u: /* STATUS_ACCESS_VIOLATION */
    case 0xC0000006u: /* STATUS_IN_PAGE_ERROR */
    case 0xC000001Du: /* STATUS_ILLEGAL_INSTRUCTION (Sprung ins Leere) */
    case 0xC0000096u: /* STATUS_PRIVILEGED_INSTRUCTION */
        return SBCT_C_AV;
    case 0xC00000FDu: /* STATUS_STACK_OVERFLOW */
        return SBCT_C_STACK;
    case 0xC0000017u: /* STATUS_NO_MEMORY */
    case 0xC000009Au: /* STATUS_INSUFFICIENT_RESOURCES */
    case 0xC000012Du: /* STATUS_COMMITMENT_LIMIT */
        return SBCT_C_OOM;
    case 0xC0000409u: /* STATUS_STACK_BUFFER_OVERRUN (__fastfail, /GS, abort mit Fehlerbericht) */
    case 0xC0000602u: /* STATUS_FAIL_FAST_EXCEPTION */
    case 0x80000003u: /* STATUS_BREAKPOINT (__debugbreak/assert) */
    case 0x40000015u: /* STATUS_FATAL_APP_EXIT */
        return SBCT_C_ABORT;
    default:
        return SBCT_C_OTHER;
    }
}

static int lc_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return *a == *b;
}

static int lc_pre(const char *s, const char *pre)
{
    for (; *pre; s++, pre++) {
        if (!*s || tolower((unsigned char)*s) != *pre) return 0;
    }
    return 1;
}

int sbct_mod_from_name(const char *b)
{
    /* feste Kennungen; Grafiktreiber nur als Hersteller (Praefixe der D3D11-UMD-Module), alles andere "other" */
    static const char *const nv[] = {"nvwgf2um", "nvd3dum", "nvldumd", "nvumdshim", "nvapi", "nvwgf2", NULL};
    static const char *const amd[] = {"atidxx", "atiumd", "atiuxp", "aticfx", "amdxx", "amdxc", "amdihk", "amdenc", NULL};
    static const char *const intel[] = {"igd10", "igd11", "igd12", "igdumd", "igdgmm", "igc32", "igxel", "intelcontrol", NULL};
    int i;
    if (!b || !b[0]) return SBCT_M_OTHER;
    if (lc_eq(b, "sb_emu.dll")) return SBCT_M_SB_EMU;
    if (lc_eq(b, "sb_ldr.exe")) return SBCT_M_SB_LDR;
    if (lc_eq(b, "d3d11.dll")) return SBCT_M_D3D11;
    if (lc_eq(b, "dxgi.dll")) return SBCT_M_DXGI;
    if (lc_pre(b, "d3dcompiler_")) return SBCT_M_D3DCOMP;
    if (lc_eq(b, "ntdll.dll")) return SBCT_M_NTDLL;
    if (lc_eq(b, "kernelbase.dll") || lc_eq(b, "kernel32.dll")) return SBCT_M_KERNEL;
    if (lc_eq(b, "ucrtbase.dll") || lc_pre(b, "vcruntime140") || lc_pre(b, "msvcp140")) return SBCT_M_CRT;
    for (i = 0; nv[i]; i++) if (lc_pre(b, nv[i])) return SBCT_M_NVIDIA;
    for (i = 0; amd[i]; i++) if (lc_pre(b, amd[i])) return SBCT_M_AMD;
    for (i = 0; intel[i]; i++) if (lc_pre(b, intel[i])) return SBCT_M_INTEL;
    return SBCT_M_OTHER;
}

int sbct_mod_at(uintptr_t addr, unsigned *rva)
{
    HMODULE hm = NULL;
    char fn[MAX_PATH];
    const char *b;
    if (rva) *rva = 0;
    if (!addr) return SBCT_M_NONE;
    if (addr >= 0x10000u && addr < 0x2810000u) { /* Xbox-VA (Gastcode im Abbild von sb_ldr.exe, Namen: symbolize.py) */
        if (rva) *rva = (unsigned)addr;
        return SBCT_M_GAME;
    }
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &hm) || !hm)
        return SBCT_M_OTHER; /* kein Modul (JIT, Heap): Ort ohne RVA */
    if (!GetModuleFileNameA(hm, fn, sizeof fn)) return SBCT_M_OTHER;
    b = strrchr(fn, '\\');
    b = b ? b + 1 : fn;
    {
        int m = sbct_mod_from_name(b);
        if (rva && m != SBCT_M_OTHER) *rva = (unsigned)(addr - (uintptr_t)hm); /* Fremdmodul: auch keine RVA */
        return m;
    }
}

static unsigned fnv(unsigned h, unsigned v)
{
    int i;
    for (i = 0; i < 4; i++) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

unsigned sbct_bucket(int mod, unsigned rva, const CONTEXT *cx)
{
    unsigned h = 2166136261u;
    int i;
    h = fnv(h, (unsigned)mod);
    h = fnv(h, rva);
#if defined(_M_IX86)
    if (cx) { /* EBP-Kette (ohne Rahmenzeiger nur der oberste Ort) */
        uintptr_t bp = cx->Ebp;
        for (i = 0; i < 4 && bp; i++) {
            uintptr_t nbp = 0, ret = 0;
            __try {
                nbp = ((volatile uintptr_t *)bp)[0];
                ret = ((volatile uintptr_t *)bp)[1];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (!ret) break;
            {
                unsigned r2 = 0;
                int m2 = sbct_mod_at(ret, &r2);
                h = fnv(h, (unsigned)m2);
                h = fnv(h, r2);
            }
            if (nbp <= bp || nbp - bp > 0x100000u) break;
            bp = nbp;
        }
    }
#else
    (void)cx;
    (void)i;
#endif
    return h ? h : 1u;
}

unsigned sbct_bucket_frames(const int *mod, const unsigned *rva, int n)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < n && i < 5; i++) {
        h = fnv(h, (unsigned)mod[i]);
        h = fnv(h, rva[i]);
    }
    return h ? h : 1u;
}

int sbct_is_sys(int mod)
{
    return mod == SBCT_M_NTDLL || mod == SBCT_M_KERNEL || mod == SBCT_M_CRT;
}

int sbct_rt_class(unsigned s)
{
    return s < 60u ? 1 : s < 300u ? 2 : s < 900u ? 3 : s < 3600u ? 4 : 5;
}

int sbct_mem_class(void)
{
    MEMORYSTATUSEX ms;
    ULONGLONG used;
    ms.dwLength = sizeof ms;
    if (!GlobalMemoryStatusEx(&ms) || ms.ullTotalVirtual < ms.ullAvailVirtual) return 0;
    used = ms.ullTotalVirtual - ms.ullAvailVirtual; /* belegter Adressraum des 32-Bit-Prozesses (vgl. #101) */
    return used < (1ull << 30) ? 1 : used < (2ull << 30) ? 2 : used < (3ull << 30) ? 3 : 4;
}

int sbct_dxr_from_hr(long hr)
{
    switch ((unsigned long)hr) {
    case 0ul: return SBCT_D_OK;
    case 0x887A0006ul: return SBCT_D_HUNG;
    case 0x887A0005ul: return SBCT_D_REMOVED;
    case 0x887A0007ul: return SBCT_D_RESET;
    case 0x887A0020ul: return SBCT_D_DRIVER;
    case 0x887A0001ul: return SBCT_D_INVALID;
    case 0x8007000Eul: return SBCT_D_OOM;
    default: return SBCT_D_OTHER;
    }
}

unsigned sbct_now(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < 116444736000000000ull) return 0;
    return (unsigned)((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

void sbct_clean_ver(const char *in, char *out, int cap)
{
    int i, n = 0;
    out[0] = 0;
    for (i = 0; in && in[i]; i++) {
        char ch = in[i];
        if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '+') || n >= 16 || n >= cap - 1) {
            out[0] = 0;
            return;
        }
        out[n++] = ch;
    }
    out[n] = 0;
}

/* ------------------------------------------------------------------ Zeile */
static int ap(char *out, int cap, int o, const char *fmt, const char *s, unsigned v)
{
    int n;
    if (o < 0 || o >= cap) return -1;
    n = s ? _snprintf_s(out + o, (size_t)(cap - o), _TRUNCATE, fmt, s) : _snprintf_s(out + o, (size_t)(cap - o), _TRUNCATE, fmt, v);
    return n < 0 ? -1 : o + n;
}

int sbct_format(char *out, int cap, const sbct_rec *r)
{
    int o = ap(out, cap, 0, "SBCT1 ts=%u", NULL, r->ts);
    if (sbct_cls_name(r->cls)) o = ap(out, cap, o, " cls=%s", k_cls[r->cls], 0);
    if (sbct_cls_name(r->xc)) o = ap(out, cap, o, " xc=%s", k_cls[r->xc], 0);
    if (r->exc) o = ap(out, cap, o, " exc=%08x", NULL, r->exc);
    if (r->acc > 0 && r->acc < 4) o = ap(out, cap, o, " acc=%s", k_acc[r->acc], 0);
    if (sbct_mod_name(r->mod)) o = ap(out, cap, o, " mod=%s", k_mod[r->mod], 0);
    if (r->mod != SBCT_M_NONE && r->mod != SBCT_M_OTHER && r->rva) o = ap(out, cap, o, " rva=%08x", NULL, r->rva);
    if (r->bkt) o = ap(out, cap, o, " bkt=%08x", NULL, r->bkt);
    if (sbct_ph_name(r->ph)) o = ap(out, cap, o, " ph=%s", k_ph[r->ph], 0);
    if (r->game > 0 && r->game < 3) o = ap(out, cap, o, " game=%s", k_game[r->game], 0);
    if (sbct_rt_name(r->rt)) o = ap(out, cap, o, " rt=%s", k_rt[r->rt], 0);
    if (sbct_mem_name(r->mem)) o = ap(out, cap, o, " mem=%s", k_mem[r->mem], 0);
    if (sbct_dxr_name(r->dxr)) o = ap(out, cap, o, " dxr=%s", k_dxr[r->dxr], 0);
    o = ap(out, cap, o, " rb=%u", NULL, r->rb & SBCT_RB_MASK);
    if (r->gpu) o = ap(out, cap, o, " gpu=%u", NULL, r->gpu & 0xFFFFu);
    if (r->ed > 0 && r->ed < 3) o = ap(out, cap, o, " ed=%s", k_ed[r->ed], 0);
    if (r->ver[0]) o = ap(out, cap, o, " ver=%s", r->ver, 0);
    o = ap(out, cap, o, "\n", "", 0);
    return o;
}

static int find_name(const char *const *tab, int n, const char *v)
{
    int i;
    for (i = 1; i < n; i++) if (tab[i] && strcmp(tab[i], v) == 0) return i;
    return 0;
}

static int hex8(const char *v, unsigned *out)
{
    unsigned x = 0;
    int i;
    for (i = 0; i < 8; i++) {
        char c = v[i];
        x <<= 4;
        if (c >= '0' && c <= '9') x |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') x |= (unsigned)(c - 'a' + 10);
        else return 0;
    }
    if (v[8]) return 0;
    *out = x;
    return 1;
}

static int dec(const char *v, unsigned max, unsigned *out)
{
    unsigned long long x = 0;
    int i;
    if (!v[0] || strlen(v) > 10) return 0;
    for (i = 0; v[i]; i++) {
        if (v[i] < '0' || v[i] > '9') return 0;
        x = x * 10u + (unsigned)(v[i] - '0');
    }
    if (x > max) return 0;
    *out = (unsigned)x;
    return 1;
}

int sbct_parse(const char *line, sbct_rec *r)
{
    char buf[512], *ctx = NULL, *tok;
    unsigned seen = 0;
    size_t l;
    memset(r, 0, sizeof *r);
    if (!line) return 0;
    l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) l--;
    if (l == 0 || l >= sizeof buf) return 0;
    memcpy(buf, line, l);
    buf[l] = 0;
    tok = strtok_s(buf, " ", &ctx);
    if (!tok || strcmp(tok, "SBCT1") != 0) return 0;
    while ((tok = strtok_s(NULL, " ", &ctx)) != NULL) {
        char *eq = strchr(tok, '=');
        const char *k, *v;
        unsigned bit = 0, u = 0;
        int ok = 0;
        if (!eq) return 0;
        *eq = 0;
        k = tok;
        v = eq + 1;
        if (strcmp(k, "ts") == 0) { bit = 1u << 0; ok = dec(v, 0xFFFFFFFFu, &r->ts); }
        else if (strcmp(k, "cls") == 0) { bit = 1u << 1; ok = (r->cls = find_name(k_cls, SBCT_C_N, v)) != 0; }
        else if (strcmp(k, "xc") == 0) { bit = 1u << 2; ok = (r->xc = find_name(k_cls, SBCT_C_N, v)) != 0; }
        else if (strcmp(k, "exc") == 0) { bit = 1u << 3; ok = hex8(v, &r->exc); }
        else if (strcmp(k, "acc") == 0) { bit = 1u << 4; ok = (r->acc = find_name(k_acc, 4, v)) != 0; }
        else if (strcmp(k, "mod") == 0) { bit = 1u << 5; ok = (r->mod = find_name(k_mod, SBCT_M_N, v)) != 0; }
        else if (strcmp(k, "rva") == 0) { bit = 1u << 6; ok = hex8(v, &r->rva); }
        else if (strcmp(k, "bkt") == 0) { bit = 1u << 7; ok = hex8(v, &r->bkt); }
        else if (strcmp(k, "ph") == 0) { bit = 1u << 8; ok = (r->ph = find_name(k_ph, SBCT_P_N, v)) != 0; }
        else if (strcmp(k, "game") == 0) { bit = 1u << 9; ok = (r->game = find_name(k_game, 3, v)) != 0; }
        else if (strcmp(k, "rt") == 0) { bit = 1u << 10; ok = (r->rt = find_name(k_rt, 6, v)) != 0; }
        else if (strcmp(k, "mem") == 0) { bit = 1u << 11; ok = (r->mem = find_name(k_mem, 5, v)) != 0; }
        else if (strcmp(k, "dxr") == 0) { bit = 1u << 12; ok = (r->dxr = find_name(k_dxr, SBCT_D_N, v)) != 0; }
        else if (strcmp(k, "rb") == 0) { bit = 1u << 13; ok = dec(v, SBCT_RB_MASK, &u); r->rb = u; }
        else if (strcmp(k, "gpu") == 0) { bit = 1u << 14; ok = dec(v, 0xFFFFu, &u); r->gpu = u; }
        else if (strcmp(k, "ed") == 0) { bit = 1u << 15; ok = (r->ed = find_name(k_ed, 3, v)) != 0; }
        else if (strcmp(k, "ver") == 0) {
            bit = 1u << 16;
            sbct_clean_ver(v, r->ver, sizeof r->ver);
            ok = r->ver[0] != 0;
        }
        if (!ok || (seen & bit)) { /* unbekannt, ungueltig oder doppelt: ganze Zeile verwerfen */
            memset(r, 0, sizeof *r);
            return 0;
        }
        seen |= bit;
    }
    if ((seen & 3u) != 3u) { /* ts und cls sind Pflicht */
        memset(r, 0, sizeof *r);
        return 0;
    }
    return 1;
}

int sbct_json(char *out, int cap, const sbct_rec *r, unsigned now)
{
    int o = 0;
    if (cap > 0) out[0] = 0;
    if (!sbct_cls_name(r->cls)) return -1;
    o = ap(out, cap, o, ",\"cls\":\"%s\"", k_cls[r->cls], 0);
    if (sbct_cls_name(r->xc)) o = ap(out, cap, o, ",\"xc\":\"%s\"", k_cls[r->xc], 0);
    if (r->exc) o = ap(out, cap, o, ",\"exc\":\"%08x\"", NULL, r->exc);
    if (r->acc > 0 && r->acc < 4) o = ap(out, cap, o, ",\"acc\":\"%s\"", k_acc[r->acc], 0);
    if (sbct_mod_name(r->mod)) o = ap(out, cap, o, ",\"mod\":\"%s\"", k_mod[r->mod], 0);
    if (r->mod != SBCT_M_NONE && r->mod != SBCT_M_OTHER && r->rva) o = ap(out, cap, o, ",\"rva\":\"%08x\"", NULL, r->rva);
    if (r->bkt) o = ap(out, cap, o, ",\"bkt\":\"%08x\"", NULL, r->bkt);
    if (sbct_ph_name(r->ph)) o = ap(out, cap, o, ",\"ph\":\"%s\"", k_ph[r->ph], 0);
    if (r->game > 0 && r->game < 3) o = ap(out, cap, o, ",\"game\":\"%s\"", k_game[r->game], 0);
    if (sbct_rt_name(r->rt)) o = ap(out, cap, o, ",\"rt\":\"%s\"", k_rt[r->rt], 0);
    if (sbct_mem_name(r->mem)) o = ap(out, cap, o, ",\"mem\":\"%s\"", k_mem[r->mem], 0);
    if (sbct_dxr_name(r->dxr)) o = ap(out, cap, o, ",\"dxr\":\"%s\"", k_dxr[r->dxr], 0);
    o = ap(out, cap, o, ",\"rb\":%u", NULL, r->rb & SBCT_RB_MASK);
    if (r->ts && now >= r->ts && now - r->ts <= SBCT_RESTART_S) o = ap(out, cap, o, ",\"rs\":1", NULL, 0);
    return o;
}

/* ------------------------------------------------------------------ Datei */
int sbct_path(wchar_t *out, int cap, const char *dir, int utf8, const char *name)
{
    wchar_t wd[600], wn[64];
    out[0] = 0;
    if (!dir || !dir[0] || !name) return 0;
    if (!MultiByteToWideChar(utf8 ? CP_UTF8 : CP_ACP, 0, dir, -1, wd, 600)) return 0;
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wn, 64)) return 0;
    if (_snwprintf_s(out, (size_t)cap, _TRUNCATE, L"%s\\%s", wd, wn) < 0) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

int sbct_write(const wchar_t *path, const sbct_rec *r)
{
    char line[400];
    HANDLE f;
    DWORD w = 0;
    int n;
    if (!path || !path[0]) return 0;
    n = sbct_format(line, (int)sizeof line, r);
    if (n <= 0) return 0;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    WriteFile(f, line, (DWORD)n, &w, NULL); /* kein FlushFileBuffers: blockiert; der Systempuffer ueberlebt den Prozess */
    CloseHandle(f);
    return w == (DWORD)n;
}

int sbct_read(const wchar_t *path, sbct_rec *r)
{
    char line[512];
    HANDLE f;
    DWORD got = 0;
    memset(r, 0, sizeof *r);
    if (!path || !path[0]) return 0;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(f, line, sizeof line - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    line[got] = 0;
    if (got == 0 || got >= sizeof line - 1 || strchr(line, '\n') != line + got - 1) return -1; /* genau eine Zeile */
    return sbct_parse(line, r) ? 1 : -1;
}

/* ------------------------------------------------------------------ Launcher */
int sbct_exit_cls(unsigned long code, int marker)
{
    switch (code) {
    case 0: return marker ? SBCT_C_CLOSED : SBCT_C_NONE; /* geordnet (exit_ok): nichts festzuhalten */
    case 2: case 5: case 8: return SBCT_C_NONE;          /* Startfehler mit eigener Meldung (Spieldaten, Fassung) */
    case 16: return SBCT_C_GPU_LOST;                     /* W516 */
    case 3: return SBCT_C_ABORT;                         /* CRT abort(), Xbox-Panik (KeBugCheck) */
    case 4: case 7: case 9: case 14: case 39: return SBCT_C_OTHER; /* Client-Abbrueche (Renderer, Kernel, W339) */
    default: break;
    }
    if ((code & 0xF0000000ul) == 0xC0000000ul || code == 0x80000003ul || code == 0x40000015ul)
        return sbct_exc_cls((unsigned)code);
    return SBCT_C_KILLED; /* 1 = Task-Manager/taskkill /F (auch Startfehler mit Exit 1, s. rt), fremde Codes */
}

int sbct_launcher_note(const char *dir_u8, unsigned long code, int game, unsigned run_s)
{
    wchar_t cp[700], mp[700];
    sbct_rec r;
    int marker, have, xc;
    if (!sbct_path(cp, 700, dir_u8, 1, SBCT_FILE) || !sbct_path(mp, 700, dir_u8, 1, SBCT_MARK)) return -1;
    marker = GetFileAttributesW(mp) != INVALID_FILE_ATTRIBUTES;
    have = sbct_read(cp, &r);
    xc = sbct_exit_cls(code, marker);
    if (have == 1) {
        /* Datensatz des Clients: nur ein Haenger bekommt die Einordnung des Endes (danach von aussen beendet bzw.
         * geschlossen); bei Ausnahmen ist der Exitcode der eigene des Handlers (1) und sagt nichts Neues. */
        if (r.cls != SBCT_C_HANG || r.xc || !xc) return 0;
        r.xc = xc;
        return sbct_write(cp, &r) ? 2 : -1;
    }
    if (!xc) return 0;
    memset(&r, 0, sizeof r);
    r.ts = sbct_now();
    r.cls = xc;
    if ((code & 0xF0000000ul) == 0xC0000000ul) r.exc = (unsigned)code;
    r.game = game == SBCT_G_SB || game == SBCT_G_LOC ? game : SBCT_G_NONE;
    r.rt = run_s ? sbct_rt_class(run_s) : 0;
    return sbct_write(cp, &r) ? 1 : -1;
}
