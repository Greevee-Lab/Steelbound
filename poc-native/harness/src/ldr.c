/* ldr.c - Mini-Loader (KEIN CRT! Nur kernel32).
 * Linker: /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NODEFAULTLIB /ENTRY:rawMain
 * Belegt per 40-MB-BSS die Low-Region, laedt sb_emu.dll und springt hinein.
 * Kehrt NIE zurueck (Emu ueberschreibt diesen Speicher mit dem XBE).
 *
 * Einschraenkungen (kein CRT): keine libc-Funktionen, keine 64-bit-Division,
 * keine Floats, keine globalen Konstruktoren. Nur WinAPI + Handarbeit.
 */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

/* 40 MB BSS ab Image-Base: deckt Spiel-Region 0x10000..0xA20000 + Reserve.
 * Muss global + erste grosse Variable sein; '= {0}' -> BSS (kein File-Bloat). */
#define PLACEHOLDER_SIZE (40u * 1024u * 1024u)
static unsigned char g_placeholder[PLACEHOLDER_SIZE] = { 0 };

static unsigned ldr_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void ldr_msg(const char *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w = 0;
    unsigned n = ldr_strlen(s);
    if (h && h != INVALID_HANDLE_VALUE) {
        WriteFile(h, s, n, &w, NULL);
    }
    OutputDebugStringA(s);
}

static void ldr_die(const char *s, DWORD code)
{
    ldr_msg(s);
    ExitProcess(code);
}

/* Pfad zu sb_emu.dll neben diesem Loader bauen. */
static int ldr_emupath(char *out, unsigned cap)
{
    unsigned n, i;
    static const char dll[] = "sb_emu.dll";
    unsigned dn = 0;
    while (dll[dn]) {
        dn++;
    }
    n = GetModuleFileNameA(NULL, out, cap);
    if (n == 0 || n >= cap) {
        return -1;
    }
    /* Dateiname abschneiden */
    i = n;
    while (i > 0 && out[i - 1] != '\\' && out[i - 1] != '/') {
        i--;
    }
    /* out[0..i) + dll */
    {
        unsigned j = 0;
        while (j < dn && i + j + 1 < cap) {
            out[i + j] = dll[j];
            j++;
        }
        if (j < dn) {
            return -1;
        }
        out[i + j] = '\0';
    }
    return 0;
}

DWORD CALLBACK rawMain(void)
{
    char path[MAX_PATH];
    HMODULE emu;
    FARPROC entry;
    typedef void(__stdcall *emulate_fn)(void);

    /* BSS am Leben halten (sonst wegoptimiert) */
    g_placeholder[0] = 0;
    g_placeholder[PLACEHOLDER_SIZE - 1] = 0;
    if (g_placeholder[0] != 0) {
        ldr_die("ldr: BSS kaputt\n", 11);
    }

    /* Sanity: laufen wir wirklich auf 0x10000? */
    if ((uintptr_t)rawMain < 0x10000 || (uintptr_t)rawMain >= 0x2000000) {
        ldr_die("ldr: nicht auf 0x10000 geladen (FIXED ignoriert?)\n", 12);
    }

    if (ldr_emupath(path, sizeof path) != 0) {
        ldr_die("ldr: Pfad zu lang\n", 13);
    }
    emu = LoadLibraryA(path);
    if (!emu) {
        ldr_msg("ldr: sb_emu.dll nicht ladbar: ");
        ldr_msg(path);
        ldr_msg("\n");
        ExitProcess(14);
    }
    entry = GetProcAddress(emu, "sb_emulate");
    if (!entry) {
        ldr_die("ldr: sb_emulate fehlt\n", 15);
    }
    ((emulate_fn)entry)();

    /* Darf nie passieren: Emu lief zurueck */
    ldr_die("ldr: Emu kehrte zurueck\n", 16);
    return 16;
}
