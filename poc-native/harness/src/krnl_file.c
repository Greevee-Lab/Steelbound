/* krnl_file.c - Kernel-HLE: Symlink-Tabelle, Xbox-Pfad -> Host-Pfad,
 * Nt*File/FATX-Datei-I/O, Symlink-Objekte, Seek-Cache (W22 FILMAV).
 * W68 SPLIT: 1:1 aus kernel_hle.c verschoben. */
#include "krnl_int.h"
#include "sb_remaster.h" /* W103 REM-FRAME */
#include "iso_vfs.h"     /* W150 ISO-DIRECT */
#include "sb_savecheck.h" /* W187 KIA-LOAD */
#include <ctype.h>
#include <intrin.h> /* W187: _AddressOfReturnAddress (FILE delete) */

/* ---------- W150 ISO-DIRECT: D: aus dem Spiel-Abbild (nur lesend) ----------
 * game_image=<Pfad> -> g_iso; alle D:-Zugriffe (NtCreateFile/NtOpenFile/
 * NtReadFile/NtQuery*File/NtClose, XAPI-ReadFile) laufen dann ueber iso_vfs.
 * Abbild-Handles: HT_FILE mit host=NULL, vfs=1, eigener Dateizeiger. Die
 * Pfadtabelle ist nach dem Start unveraenderlich, Lesen positionell
 * (ReadFile+OVERLAPPED-Offset) -> keine Sperre. Schreiben/Erzeugen auf D:
 * liefert ACCESS_DENIED wie auf der DVD. */
static iso_vfs_t *g_iso;

/* Schreibende Zugriffsrechte (NT-Maske): GENERIC_WRITE/ALL, WRITE_DATA,
 * APPEND_DATA, WRITE_EA, WRITE_ATTRIBUTES, DELETE, WRITE_DAC/OWNER. */
#define VFS_WRITE_MASK (0x40000000u | 0x10000000u | 0x2u | 0x4u | 0x10u | 0x100u | \
                        0x10000u | 0x40000u | 0x80000u)

int krnl_vfs_open(const char *image, char *err, unsigned cap)
{
    if (!image || !image[0]) {
        return 0;
    }
    g_iso = iso_vfs_open(image, err, cap);
    if (!g_iso) {
        return -1;
    }
    HLOG_I("ISO: Abbild '%s' geoeffnet (Partition @0x%llX, %u Eintraege, %u Verzeichnisse) - D: liest nur aus dem Abbild",
           image, (unsigned long long)iso_vfs_part_base(g_iso), iso_vfs_count(g_iso),
           iso_vfs_dir_count(g_iso));
    return 0;
}

int krnl_vfs_active(void)
{
    return g_iso != NULL;
}

int krnl_vfs_handle(unsigned xh)
{
    ht_entry_t *e;
    if (!g_iso) {
        return 0;
    }
    e = ht_get(xh);
    return e && e->kind == HT_FILE && e->vfs;
}

int krnl_vfs_read_all(const char *rel, unsigned char **out, unsigned *len)
{
    uint32_t n = 0;
    int rc;
    if (!g_iso) {
        return -1;
    }
    rc = iso_vfs_read_all(g_iso, rel, out, &n);
    *len = n;
    return rc;
}

/* Lesen auf Abbild-Handle; off = NULL oder negativ -> eigener Zeiger
 * (NT: FILE_USE_FILE_POINTER_POSITION). Rueckgabe Win32-Fehler (0 = ok). */
static DWORD vfs_read_at(ht_entry_t *e, const int64_t *off, void *buf, u32 len, DWORD *got)
{
    uint64_t pos = (off && *off >= 0) ? (uint64_t)*off : e->vfs_pos;
    uint32_t g = 0;
    DWORD er = (DWORD)iso_vfs_read(g_iso, e->vfs_node, pos, buf, len, &g);
    *got = g;
    e->vfs_pos = pos + g;
    return er;
}

int krnl_vfs_read(unsigned xh, const long long *off, void *buf, unsigned len,
                  unsigned long *got, char *path, unsigned pathcap)
{
    ht_entry_t *e = ht_get(xh);
    DWORD g = 0;
    if (!e || e->kind != HT_FILE || !e->vfs || !g_iso) {
        return 0;
    }
    vfs_read_at(e, (const int64_t *)off, buf, len, &g);
    *got = g;
    if (path && pathcap) {
        strncpy_s(path, pathcap, e->path, _TRUNCATE);
    }
    return 1;
}

/* ---------- Symlink-Tabelle ---------- */
#define SYMLINK_MAX 64
static struct {
    char from[256];
    char to[256];
} g_symlinks[SYMLINK_MAX];

static void symlink_add(const char *from, const char *to)
{
    unsigned i;
    for (i = 0; i < SYMLINK_MAX; i++) {
        if (!g_symlinks[i].from[0]) {
            strncpy_s(g_symlinks[i].from, sizeof g_symlinks[i].from, from, _TRUNCATE);
            strncpy_s(g_symlinks[i].to, sizeof g_symlinks[i].to, to, _TRUNCATE);
            return;
        }
    }
}

static const char *symlink_find(const char *from)
{
    unsigned i;
    for (i = 0; i < SYMLINK_MAX; i++) {
        if (g_symlinks[i].from[0] && _stricmp(g_symlinks[i].from, from) == 0) {
            return g_symlinks[i].to;
        }
    }
    return NULL;
}

/* ---------- Xbox-Pfad -> Host-Pfad ---------- */
static void wide_to_narrow(const wchar_t *w, size_t nchars, char *out, size_t cap)
{
    size_t i;
    for (i = 0; i + 1 < cap && i < nchars && w[i]; i++) {
        wchar_t c = w[i];
        out[i] = (char)(c < 128 ? c : '?');
    }
    out[i] = '\0';
}


static void xb_skip_sep(const char **s)
{
    while (**s == '\\' || **s == '/') {
        (*s)++;
    }
}

/* ---------- W190 SAVE-LOCK: Spielstaende schuetzen ----------
 * Aktiv: jeder Host-Pfad <hdd>\TDATA\... bzw. <hdd>\UDATA\... (T:, U: und
 * E:\TDATA, E:\UDATA) wird auf <hdd>\_savelock\TDATA|UDATA\... umgebogen
 * (Wegwerf-Arbeitskopie, sb_savecheck.h). Einmal aktiv, bleibt der Schutz
 * bis zum Prozessende an (g_sl_hdd aendert sich danach nicht mehr). */
static volatile LONG g_sl_active;
static char g_sl_hdd[600];  /* hdd_root mit Backslashes, ohne Endtrenner */
static size_t g_sl_hddn;

static void sl_redirect(char *out, size_t cap)
{
    const char *r;
    char t[1024];
    if (!g_sl_hddn || _strnicmp(out, g_sl_hdd, g_sl_hddn) != 0 || out[g_sl_hddn] != '\\') {
        return;
    }
    r = out + g_sl_hddn + 1;
    if ((_strnicmp(r, "TDATA", 5) != 0 && _strnicmp(r, "UDATA", 5) != 0) ||
        (r[5] != '\\' && r[5] != '\0')) {
        return;
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, "%s\\_savelock\\%s", g_sl_hdd, r);
    strcpy_s(out, cap, t);
}

/* W150: drel (optional) bekommt bei D:-Pfaden den Pfad relativ zur
 * Laufwerkswurzel, sonst "". Im Abbild-Modus ist der "Host-Pfad" nur ein
 * Anzeigename (<game_image>\<rel>, gleiche Dateinamen in den Logs). */
static int xb_path_to_host_ex(const char *xb, size_t nbytes, char *out, size_t cap,
                              char *drel, size_t drelcap, int *is_d)
{
    char tmp[1024];
    char base[640];
    const char *rel;
    size_t n;
    int d = 0;

    /* Xbox-Pfade sind ANSI (PSTRING), nicht UTF-16 */
    n = nbytes < sizeof tmp - 1 ? nbytes : sizeof tmp - 1;
    memcpy(tmp, xb, n);
    tmp[n] = '\0';

    /* T:/U: (Titel-Drives, beschreibbar) -> HDD/TDATA|UDATA/<titleid> */
    if (_strnicmp(tmp, "\\??\\T:", 6) == 0 ||
        ((tmp[0] == 'T' || tmp[0] == 't') && tmp[1] == ':')) {
        rel = tmp[1] == '?' ? tmp + 6 : tmp + 2;
        _snprintf_s(base, sizeof base, _TRUNCATE, "%s/TDATA/%s", g_cfg.hdd_root,
                    XB_TITLE_ID_STR);
    } else if (_strnicmp(tmp, "\\??\\U:", 6) == 0 ||
               ((tmp[0] == 'U' || tmp[0] == 'u') && tmp[1] == ':')) {
        rel = tmp[1] == '?' ? tmp + 6 : tmp + 2;
        _snprintf_s(base, sizeof base, _TRUNCATE, "%s/UDATA/%s", g_cfg.hdd_root,
                    XB_TITLE_ID_STR);
    } else if (_strnicmp(tmp, "\\??\\Z:", 6) == 0 ||
               ((tmp[0] == 'Z' || tmp[0] == 'z') && tmp[1] == ':')) {
        /* Z: (Utility/Cache, beschreibbar) -> hdd_root/ZCACHE */
        rel = tmp[1] == '?' ? tmp + 6 : tmp + 2;
        _snprintf_s(base, sizeof base, _TRUNCATE, "%s/ZCACHE", g_cfg.hdd_root);
    } else if (_strnicmp(tmp, "\\Device\\Harddisk0\\Partition",
                         sizeof "\\Device\\Harddisk0\\Partition" - 1) == 0 ||
               _strnicmp(tmp, "Device\\Harddisk0\\partition",
                         sizeof "Device\\Harddisk0\\partition" - 1) == 0) {
        /* E: (Festplatte, beschreibbar) -> hdd_root (NICHT media_root!).
         * Alle Partitionen (0=Raw,1=E:,2=C:,...) -> hdd_root (M1). */
        if (tmp[0] == '\\') {
            rel = tmp + sizeof "\\Device\\Harddisk0\\Partition1" - 1;
        } else {
            rel = tmp + sizeof "Device\\Harddisk0\\partition0" - 1;
        }
        strcpy_s(base, sizeof base, g_cfg.hdd_root);
    } else {
        static const char *dpre[] = {
            "\\Device\\CdRom0", "\\??\\D:", "D:", NULL
        };
        const char **p;
        rel = tmp;
        for (p = dpre; *p; p++) {
            size_t pn = strlen(*p);
            if (_strnicmp(tmp, *p, pn) == 0) {
                rel = tmp + pn;
                break;
            }
        }
        strcpy_s(base, sizeof base, g_iso ? g_cfg.game_image : g_cfg.media_root);
        d = 1;
    }
    xb_skip_sep(&rel);
    if (drel && drelcap) {
        strncpy_s(drel, drelcap, d ? rel : "", _TRUNCATE);
    }
    if (is_d) {
        *is_d = d;
    }
    {
        int m = _snprintf_s(out, cap, _TRUNCATE, "%s/%s", base, rel);
        char *s;
        for (s = out; *s; s++) {
            if (*s == '/') {
                *s = '\\';
            }
        }
        if (g_sl_active && !d) { /* W190 SAVE-LOCK: TDATA/UDATA -> Arbeitskopie */
            sl_redirect(out, cap);
        }
        return m > 0 ? 0 : -1;
    }
}

static int xb_path_to_host(const char *xb, size_t nbytes, char *out, size_t cap)
{
    return xb_path_to_host_ex(xb, nbytes, out, cap, NULL, 0, NULL);
}

/* W150: D:-Datei aus dem Abbild oeffnen. Log-Zeilen wie im Ordner-Modus
 * (FILE open '<pfad>' -> xh / -> FEHLER <win32>), damit Meilensteine und
 * logdiff gleich bleiben. */
static u32 winerr_to_status(DWORD e);

static u32 vfs_open(const char *host, const char *drel, u32 access, u32 disp,
                    u32 options, u32 *out_xh)
{
    int po = 0;
    int node = iso_vfs_lookup(g_iso, drel, &po);
    int create = disp == X_FILE_CREATE || disp == X_FILE_OVERWRITE ||
                 disp == X_FILE_OVERWRITE_IF || disp == X_FILE_SUPERSEDE ||
                 (disp == X_FILE_OPEN_IF && node < 0);
    extern void replay_note_file(const char *path);
    u32 xh;
    ht_entry_t *en;
    if ((access & VFS_WRITE_MASK) || create) {
        static unsigned nden;
        if (nden < 32) {
            nden++;
            HLOG_W("ISO: Schreibzugriff auf D: verweigert '%s' (access=0x%X disp=%u) - Abbild ist nur lesbar",
                   host, access, disp);
        }
        HLOG_I("FILE open '%s' (disp=%u opt=0x%X) -> FEHLER %lu", host, disp,
               options, (unsigned long)ERROR_ACCESS_DENIED);
        replay_note_file(host);
        return X_STATUS_ACCESS_DENIED;
    }
    if (node < 0) {
        DWORD e = po ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
        HLOG_I("FILE open '%s' (disp=%u opt=0x%X) -> FEHLER %lu", host, disp,
               options, e);
        replay_note_file(host);
        return winerr_to_status(e);
    }
    xh = ht_alloc(HT_FILE, NULL);
    if (!xh) {
        return X_STATUS_NO_MEMORY;
    }
    en = ht_get(xh);
    strncpy_s(en->path, sizeof en->path, host, _TRUNCATE);
    en->is_dir = (options & X_FILE_DIRECTORY_FILE) != 0; /* wie Ordner-Modus */
    en->vfs_node = node;
    en->vfs_pos = 0;
    en->vfs_dir_k = 0;
    en->vfs = 1;
    *out_xh = xh;
    HLOG_I("FILE open '%s' -> xh=0x%X", host, xh);
    replay_note_file(host);
    return X_STATUS_SUCCESS;
}

/* mkdir -p (fuer TDATA/UDATA/Savegame-Verzeichnisse) */
void xb_mkdir_p(const char *path)
{
    char tmp[1024];
    char *s;
    strcpy_s(tmp, sizeof tmp, path);
    for (s = tmp + 1; *s; s++) {
        if (*s == '\\' || *s == '/') {
            char c = *s;
            *s = '\0';
            CreateDirectoryA(tmp, NULL);
            *s = c;
        }
    }
    CreateDirectoryA(tmp, NULL);
}

/* W187 KIA-LOAD: T:-Spielstaende vor dem Spielstart pruefen (save_check=1:
 * reparieren mit Sicherung unter <hdd_root>\_backup\, 2: nur melden).
 * Beschaedigte/fremde Staende (z. B. abgebrochenes Schreiben, Import von
 * einer echten Xbox) fuehren sonst zu "Unable to load KIA Record." usw. */
static void save_check_log(const char *line, void *ctx)
{
    (void)ctx;
    HLOG_W("%s", line);
}

void krnl_save_check(void)
{
    wchar_t w[640];
    sb_savecheck_t r;
    int rc;
    if (!MultiByteToWideChar(CP_ACP, 0, g_cfg.hdd_root, -1, w, 640)) {
        return;
    }
    rc = sb_savecheck_run(w, g_cfg.save_check == 1, &r, save_check_log, NULL);
    if (rc < 0) {
        return;
    }
    if (rc == 0) {
        HLOG_I("SAVECHECK: %d Spielstand-Dateien ok", r.checked);
    } else {
        char b[700];
        if (!WideCharToMultiByte(CP_ACP, 0, r.backup, -1, b, sizeof b, NULL, NULL)) {
            b[0] = '\0';
        }
        HLOG_W("SAVECHECK: %d Dateien, %d ok, %d repariert, %d verschoben, %d unveraendert%s%s",
               r.checked, r.ok, r.repaired, r.moved, r.failed, b[0] ? ", Sicherung " : "", b);
    }
}

/* ---------- W190 SAVE-LOCK: Ein-/Ausschalten, Uebernehmen ----------
 * Sitzung = Prozess plus seine XLaunchNewImage-Neustarts (W186): Wer den
 * Schutz einschaltet, setzt SB_SAVELOCK_SESSION=1 in der eigenen Umgebung,
 * das Neustart-Kind erbt sie und behaelt die Arbeitskopie (BACK TO TITLE
 * verwirft also nichts). Ein neuer Start ohne die Variable verwirft die
 * Arbeitskopie (save_lock=0) bzw. legt sie frisch an (save_lock=1). Kein
 * Aufraeumen beim Beenden noetig: die echten Staende werden nie beruehrt. */
static int sl_hdd_w(wchar_t *w, int cap)
{
    return MultiByteToWideChar(CP_ACP, 0, g_cfg.hdd_root, -1, w, cap) > 0;
}

static void sl_set_root(void)
{
    char *s;
    strcpy_s(g_sl_hdd, sizeof g_sl_hdd, g_cfg.hdd_root);
    for (s = g_sl_hdd; *s; s++) {
        if (*s == '/') {
            *s = '\\';
        }
    }
    g_sl_hddn = strlen(g_sl_hdd);
    while (g_sl_hddn > 0 && g_sl_hdd[g_sl_hddn - 1] == '\\') {
        g_sl_hdd[--g_sl_hddn] = '\0';
    }
}

int krnl_savelock_active(void)
{
    return g_sl_active != 0;
}

int krnl_savelock_begin(const char *why)
{
    wchar_t w[640];
    char err[300];
    int n = 0;
    if (g_sl_active) {
        return 0;
    }
    if (!sl_hdd_w(w, 640) || sb_savelock_prepare(w, &n, err, sizeof err) != 0) {
        HLOG_W("SAVELOCK: Arbeitskopie nicht anlegbar (%s) - Schutz AUS, Spielstaende wie ohne save_lock",
               err[0] ? err : "hdd_root");
        return -1;
    }
    sl_set_root();
    InterlockedExchange(&g_sl_active, 1);
    SetEnvironmentVariableA("SB_SAVELOCK_SESSION", "1"); /* Neustart-Kind behaelt die Kopie */
    HLOG_I("SAVELOCK: aktiv (%s) - T:/U: in Arbeitskopie '%s\\_savelock' (%d Dateien kopiert), "
           "Aenderungen an Pilot/Spielstaenden werden verworfen", why, g_sl_hdd, n);
    return 0;
}

void krnl_savelock_init(void)
{
    wchar_t w[640];
    char b[8];
    int keep = GetEnvironmentVariableA("SB_SAVELOCK_SESSION", b, sizeof b) > 0 && b[0] == '1';
    int rc;
    if (!sl_hdd_w(w, 640)) {
        return;
    }
    if (keep && sb_savelock_present(w)) {
        sl_set_root();
        InterlockedExchange(&g_sl_active, 1);
        HLOG_I("SAVELOCK: aktiv (Neustart derselben Sitzung) - Arbeitskopie '%s\\_savelock' bleibt", g_sl_hdd);
        return;
    }
    if (g_cfg.save_lock || keep) {
        krnl_savelock_begin(keep ? "Neustart, Arbeitskopie fehlte" : "Start, save_lock=1");
        return;
    }
    rc = sb_savelock_discard(w); /* save_lock=0: Rest einer frueheren Sitzung */
    if (rc != 0) {
        HLOG_I("SAVELOCK: Arbeitskopie einer frueheren Sitzung %s ('%s\\_savelock')",
               rc > 0 ? "verworfen" : "nicht vollstaendig loeschbar", g_cfg.hdd_root);
    }
}

int krnl_savelock_commit(void)
{
    wchar_t w[640], bk[800];
    char err[300], b8[900], pre[640];
    unsigned i;
    size_t pn;
    int n = 0;
    if (!g_sl_active || !sl_hdd_w(w, 640)) {
        return -1;
    }
    /* Schreibt das Spiel gerade einen Spielstand? (offene Datei in der Kopie) */
    _snprintf_s(pre, sizeof pre, _TRUNCATE, "%s\\_savelock\\", g_sl_hdd);
    pn = strlen(pre);
    for (i = 0; i < XB_HANDLE_MAX; i++) {
        ht_entry_t *e = ht_get(XB_HANDLE_BASE + i);
        if (e && e->kind == HT_FILE && !e->vfs && !e->is_dir && _strnicmp(e->path, pre, pn) == 0) {
            HLOG_W("SAVELOCK: Uebernehmen verschoben - Spielstand offen '%s'", e->path);
            return 1;
        }
    }
    if (sb_savelock_commit(w, bk, 800, &n, err, sizeof err) != 0) {
        HLOG_W("SAVELOCK: Uebernehmen FEHLER (%s)", err);
        return -1;
    }
    if (!WideCharToMultiByte(CP_ACP, 0, bk, -1, b8, sizeof b8, NULL, NULL)) {
        b8[0] = '\0';
    }
    HLOG_I("SAVELOCK: geschuetzte Aenderungen uebernommen (%d Dateien), alter Stand gesichert '%s'", n,
           b8[0] ? b8 : "-");
    return 0;
}

/* ----- Dateien ----- */
static DWORD xb_disposition_to_win(u32 d)
{
    switch (d) {
    case X_FILE_OPEN: return OPEN_EXISTING;
    case X_FILE_CREATE: return CREATE_NEW;
    case X_FILE_OPEN_IF: return OPEN_ALWAYS;
    case X_FILE_OVERWRITE: return TRUNCATE_EXISTING;
    case X_FILE_OVERWRITE_IF:
    case X_FILE_SUPERSEDE: return CREATE_ALWAYS;
    default: return OPEN_EXISTING;
    }
}

static u32 winerr_to_status(DWORD e)
{
    switch (e) {
    case ERROR_SUCCESS: return X_STATUS_SUCCESS;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return X_STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_ACCESS_DENIED: return X_STATUS_ACCESS_DENIED;
    case ERROR_ALREADY_EXISTS: return X_STATUS_OBJECT_NAME_COLLISION;
    case ERROR_HANDLE_EOF: return X_STATUS_END_OF_FILE;
    default: return 0xC0000001u; /* UNSUCCESSFUL */
    }
}

static u32 open_by_attrs(u32 pAttr, u32 access, u32 share, u32 disp, u32 options,
                         u32 *out_xh)
{
    xb_object_attributes *oa;
    xb_ansi_string *as;
    char host[1024];
    DWORD want = 0, flags = FILE_ATTRIBUTE_NORMAL;
    HANDLE h;
    u32 xh;
    unsigned char *stale = NULL; /* W63 FATXSTALE */
    u32 stale_len = 0;

    if (!pAttr) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    oa = (xb_object_attributes *)(uintptr_t)pAttr;
    as = oa->object_name;
    if (!as || !as->buffer) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    if (oa->root_directory) {
        static unsigned nrd;
        if (nrd < 4) { nrd++; HLOG_W("NtCreateFile: RootDirectory=0x%X wird ignoriert", oa->root_directory); }
    }
    {
        char drel[1024];
        int is_d = 0;
        if (xb_path_to_host_ex(as->buffer, as->length, host, sizeof host, drel,
                               sizeof drel, &is_d) != 0) {
            return X_STATUS_OBJECT_NAME_INVALID;
        }
        if (is_d && g_iso) { /* W150 ISO-DIRECT */
            return vfs_open(host, drel, access, disp, options, out_xh);
        }
    }
    if (access & 0x80000000u) {
        want |= GENERIC_READ;
    }
    if (access & 0x40000000u) {
        want |= GENERIC_WRITE;
    }
    if (access == 0x02000000u) { /* MAXIMUM_ALLOWED */
        want = GENERIC_READ | GENERIC_WRITE;
    }
    if (want == 0) {
        want = GENERIC_READ;
    }
    if (options & X_FILE_DIRECTORY_FILE) {
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
        /* Verzeichnis-Erstellung (TDATA/<title>, Saves): CreateFile kann
         * keine Dirs anlegen -> mkdir -p, dann oeffnen. */
        if (disp != X_FILE_OPEN && disp != X_FILE_OVERWRITE) {
            xb_mkdir_p(host);
            h = CreateFileA(host, want, share ? share : (DWORD)(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
                            NULL, OPEN_EXISTING, flags, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                DWORD e = GetLastError();
                HLOG_I("FILE mkdir+open '%s' -> FEHLER %lu", host, e);
                return winerr_to_status(e);
            }
            goto opened;
        }
    } else {
        /* Geraete-/Laufwerkswurzeln (D:, T:) sind Verzeichnisse: ohne
         * BACKUP_SEMANTICS schlaegt CreateFile fehl (GLE 5). */
        DWORD attrs = GetFileAttributesA(host);
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            flags |= FILE_FLAG_BACKUP_SEMANTICS;
        }
    }
    /* W63 FATXSTALE: FATX (Xbox) fuellt beim Verlaengern einer Datei die
     * Luecke nicht mit Nullen - nach Ueberschreiben (Truncate) + Seek hinter
     * das Dateiende + Write stehen dort die alten Cluster-Daten. Das Spiel
     * verlaesst sich darauf: FUN_000C3A70 haengt die Spielstand-Signatur per
     * CreateFile(CREATE_ALWAYS) + SetFilePointer(0xAE8) + WriteFile(20 B) an
     * die gerade geschriebene Datei an (W63_c6: user01/user99/mslog00/config
     * komplett Nullen). Alten Inhalt vor dem Truncate sichern (<= 4 MB),
     * NtWriteFile fuellt Luecken hinter dem Dateiende daraus. */
    if (g_cfg.fatx_stale_extend && !(options & X_FILE_DIRECTORY_FILE) &&
        (disp == X_FILE_OVERWRITE || disp == X_FILE_OVERWRITE_IF ||
         disp == X_FILE_SUPERSEDE)) {
        HANDLE ho = CreateFileA(host, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ho != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(ho, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 0x400000) {
                stale = (unsigned char *)malloc((size_t)sz.QuadPart);
                if (stale) {
                    DWORD got = 0;
                    if (ReadFile(ho, stale, (DWORD)sz.QuadPart, &got, NULL) && got) {
                        stale_len = got;
                    } else {
                        free(stale);
                        stale = NULL;
                    }
                }
            }
            CloseHandle(ho);
        }
    }
    h = CreateFileA(host, want, share ? share : (DWORD)(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
                    NULL, xb_disposition_to_win(disp), flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (stale) { free(stale); stale = NULL; }
        HLOG_I("FILE open '%s' (disp=%u opt=0x%X) -> FEHLER %lu", host, disp,
               options, e);
        {   /* A4: auch fehlgeschlagene Opens sind Spielzustand (mslog00.sav) */
            extern void replay_note_file(const char *path);
            replay_note_file(host);
        }
        return winerr_to_status(e);
    }
opened:
    xh = ht_alloc(HT_FILE, h);
    if (!xh) {
        CloseHandle(h);
        if (stale) { free(stale); }
        return X_STATUS_NO_MEMORY;
    }
    {
        ht_entry_t *en = ht_get(xh);
        strncpy_s(en->path, sizeof en->path, host, _TRUNCATE);
        en->is_dir = (options & X_FILE_DIRECTORY_FILE) != 0;
        en->stale = stale;
        en->stale_len = stale_len;
        if (stale) {
            static unsigned nst;
            if (nst < 32) {
                nst++;
                HLOG_I("FATXSTALE: '%s' disp=%u alter Inhalt %u B gesichert", host, disp, stale_len);
            }
        }
    }
    *out_xh = xh;
    HLOG_I("FILE open '%s' -> xh=0x%X", host, xh);
    {   /* A4: Datei-Ereignis fuer zustandsgesteuertes Replay */
        extern void replay_note_file(const char *path);
        replay_note_file(host);
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtCreateFile(u32 ph, u32 access, u32 attr, u32 iosb,
                                      u32 allocSize, u32 fileAttr, u32 share,
                                      u32 disp, u32 options)
{
    u32 xh = 0, st;
    (void)allocSize;
    (void)fileAttr;
    st = open_by_attrs(attr, access, share, disp, options, &xh);
    *(u32 *)(uintptr_t)ph = xh;
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 1; /* OPENED */
    }
    return st;
}

u32 __stdcall hle_NtOpenFile(u32 ph, u32 access, u32 attr, u32 iosb, u32 share,
                                    u32 options)
{
    u32 xh = 0, st;
    st = open_by_attrs(attr, access, share, X_FILE_OPEN, options, &xh);
    *(u32 *)(uintptr_t)ph = xh;
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 1;
    }
    return st;
}

u32 __stdcall hle_NtReadFile(u32 xh, u32 ev, u32 apc, u32 apcCtx, u32 iosb,
                                    u32 buf, u32 len, u32 pOff)
{
    ht_entry_t *e = ht_get(xh);
    DWORD got = 0;
    u32 st;
    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (ev || apc) {
        static u32 warned;
        if (!warned) {
            HLOG_W("NtReadFile: async (Event/APC) -> synchron ausgefuehrt");
            warned = 1;
        }
        (void)apcCtx;
    }
    if (e->vfs) { /* W150 ISO-DIRECT: positionell aus dem Abbild */
        DWORD er = vfs_read_at(e, pOff ? (const int64_t *)(uintptr_t)pOff : NULL,
                               (void *)(uintptr_t)buf, len, &got);
        if (er) {
            st = winerr_to_status(er);
        } else {
            st = got ? X_STATUS_SUCCESS : X_STATUS_END_OF_FILE;
            if (g_cfg.tex_dump && got) { sb_rem_note_read(buf, got, e->path); } /* DIAG(REM-FRAME,W103) */
        }
        goto read_done;
    }
    if (pOff) {
        LARGE_INTEGER li;
        li.QuadPart = *(int64_t *)(uintptr_t)pOff;
        SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
    }
    if (!ReadFile(e->host, (void *)(uintptr_t)buf, len, &got, NULL)) {
        DWORD er = GetLastError();
        /* XapiSelectCachePartition liest 512B @0x800 von partition0 (Raw-
         * Platte). Bei uns ist das ein Verzeichnis (nicht lesbar) -> Fake:
         * NULLEN + ERFOLG (Funktion nimmt dann Default-Partition, M1). */
        if (len == 0x200 && pOff &&
            *(int64_t *)(uintptr_t)pOff == 0x800) {
            memset((void *)(uintptr_t)buf, 0, len);
            got = len;
            st = X_STATUS_SUCCESS;
            HLOG_T("NtReadFile: Partitions-Fake (512B @0x800 -> NULLEN)");
        } else {
            st = (er == ERROR_HANDLE_EOF)
                     ? X_STATUS_END_OF_FILE
                     : winerr_to_status(er);
        }
    } else {
        st = X_STATUS_SUCCESS;
        if (got == 0) {
            st = X_STATUS_END_OF_FILE;
        }
        if (g_cfg.tex_dump && got) { sb_rem_note_read(buf, got, e->path); } /* DIAG(REM-FRAME,W103): Textur-Quelle */
    }
read_done:
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = got;
    }
    /* W22: Seek-Cache nachfuehren (pOff-Seek laeuft unbedingt, auch got=0!) */
    if (pOff)
        krnl_seek_note(xh, (uint64_t)*(int64_t *)(uintptr_t)pOff + got);
    else
        krnl_seek_add(xh, got);
    /* M2b: Async-Read (Missions-Laden!) -> Event signalisieren, sonst
     * wartet das Spiel ewig auf I/O-Abschluss. */
    if (ev) {
        ht_entry_t *ee = ht_get(ev);
        if (ee && ee->kind == HT_EVENT && ee->host) {
            SetEvent(ee->host);
        }
    }
    return st;
}

u32 __stdcall hle_NtWriteFile(u32 xh, u32 ev, u32 apc, u32 apcCtx, u32 iosb,
                                     u32 buf, u32 len, u32 pOff)
{
    ht_entry_t *e = ht_get(xh);
    DWORD wrote = 0;
    u32 st;
    (void)ev;
    (void)apc;
    (void)apcCtx;
    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (e->vfs) { /* W150: Abbild ist nur lesbar (DVD) */
        HLOG_W("ISO: NtWriteFile auf D: verweigert '%s' (%u B)", e->path, len);
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_ACCESS_DENIED;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
        }
        return X_STATUS_ACCESS_DENIED;
    }
    if (pOff) {
        LARGE_INTEGER li;
        li.QuadPart = *(int64_t *)(uintptr_t)pOff;
        SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
    }
    krnl_seek_invalidate(xh); /* W22: Write bewegt den Zeiger -> Cache weg */
    if (e->stale) {
        /* W63 FATXSTALE: Schreiben hinter das Dateiende -> Luecke mit dem
         * alten Inhalt fuellen (FATX nullt neue Cluster nicht). */
        LARGE_INTEGER zero, cur, sz;
        zero.QuadPart = 0;
        if (SetFilePointerEx(e->host, zero, &cur, FILE_CURRENT) &&
            GetFileSizeEx(e->host, &sz) && cur.QuadPart > sz.QuadPart &&
            sz.QuadPart < (LONGLONG)e->stale_len) {
            LONGLONG end = cur.QuadPart < (LONGLONG)e->stale_len ? cur.QuadPart : (LONGLONG)e->stale_len;
            DWORD w2 = 0;
            SetFilePointerEx(e->host, sz, NULL, FILE_BEGIN);
            WriteFile(e->host, e->stale + sz.QuadPart, (DWORD)(end - sz.QuadPart), &w2, NULL);
            SetFilePointerEx(e->host, cur, NULL, FILE_BEGIN);
            HLOG_I("FATXSTALE: '%s' Luecke %lld..%lld aus altem Inhalt gefuellt (%lu B)",
                   e->path, sz.QuadPart, end, w2);
        }
    }
    if (strstr(e->path, "TDATA") || strstr(e->path, "UDATA")) {
        /* W63: Spielstand-Schreibzugriffe belegen (Pfad, Offset, Laenge,
         * Anteil Nicht-Null-Bytes), gedeckelt. */
        static unsigned nsw;
        if (nsw < 64) {
            LARGE_INTEGER zero, cur;
            u32 k, nz = 0;
            zero.QuadPart = 0;
            cur.QuadPart = -1;
            SetFilePointerEx(e->host, zero, &cur, FILE_CURRENT);
            __try {
                for (k = 0; k < len; k++) {
                    if (((const unsigned char *)(uintptr_t)buf)[k]) nz++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            nsw++;
            HLOG_I("FILE write '%s' off=%lld len=%u nichtnull=%u", e->path, cur.QuadPart, len, nz);
        }
    }
    if (!WriteFile(e->host, (void *)(uintptr_t)buf, len, &wrote, NULL)) {
        st = winerr_to_status(GetLastError());
    } else {
        st = X_STATUS_SUCCESS;
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = wrote;
    }
    return st;
}

u32 __stdcall hle_NtClose(u32 xh)
{
    ht_entry_t *e = ht_get(xh);
    if (!e) {
        HLOG_T("NtClose(0x%X) -> INVALID_HANDLE", xh);
        return X_STATUS_INVALID_HANDLE;
    }
    HLOG_T("NtClose(0x%X kind=%d '%s')", xh, e->kind, e->path);
    if (e->kind == HT_FILE) {
        if (!e->vfs) { /* W150: Abbild-Handles haben kein Host-Handle */
            CloseHandle(e->host);
        }
        if (e->stale) { /* W63 FATXSTALE */
            free(e->stale);
            e->stale = NULL;
            e->stale_len = 0;
        }
        if (e->delete_on_close && e->path[0]) {
            DeleteFileA(e->path);
        }
    } else if (e->host && e->host != GetCurrentThread() &&
               e->host != GetCurrentProcess()) {
        CloseHandle(e->host);
    }
    ht_free(xh);
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtQueryFullAttributesFile(u32 pAttr, u32 pInfo)
{
    xb_object_attributes *oa;
    xb_ansi_string *as;
    char host[1024];
    WIN32_FILE_ATTRIBUTE_DATA ad;
    xb_file_network_open_info *fi;

    if (!pAttr || !pInfo) {
        return X_STATUS_INVALID_PARAMETER;
    }
    oa = (xb_object_attributes *)(uintptr_t)pAttr;
    as = oa->object_name;
    {
        char drel[1024];
        int is_d = 0;
        if (!as || !as->buffer ||
            xb_path_to_host_ex(as->buffer, as->length, host, sizeof host, drel,
                               sizeof drel, &is_d) != 0) {
            return X_STATUS_OBJECT_NAME_INVALID;
        }
        if (is_d && g_iso) { /* W150 ISO-DIRECT */
            iso_vfs_stat_t vs;
            uint32_t tlo, thi;
            int node = iso_vfs_lookup(g_iso, drel, NULL);
            if (node < 0 || iso_vfs_stat(g_iso, node, &vs) != 0) {
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            }
            iso_vfs_volume_time(g_iso, &tlo, &thi);
            fi = (xb_file_network_open_info *)(uintptr_t)pInfo;
            fi->creation_time_lo = fi->last_access_lo = fi->last_write_lo = fi->change_lo = tlo;
            fi->creation_time_hi = fi->last_access_hi = fi->last_write_hi = fi->change_hi = thi;
            fi->alloc_lo = fi->eof_lo = vs.is_dir ? 0 : vs.size;
            fi->alloc_hi = fi->eof_hi = 0;
            fi->attributes = vs.attr ? vs.attr : FILE_ATTRIBUTE_NORMAL;
            HLOG_T("NtQueryFullAttributesFile '%s' ok (Abbild)", host);
            return X_STATUS_SUCCESS;
        }
    }
    {
        wchar_t w[1024];
        size_t i;
        for (i = 0; i + 1 < 1024 && host[i]; i++) {
            w[i] = (wchar_t)(unsigned char)host[i];
        }
        w[i] = 0;
        if (!GetFileAttributesExW(w, GetFileExInfoStandard, &ad)) {
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }
    fi = (xb_file_network_open_info *)(uintptr_t)pInfo;
    fi->creation_time_lo = ad.ftCreationTime.dwLowDateTime;
    fi->creation_time_hi = ad.ftCreationTime.dwHighDateTime;
    fi->last_access_lo = ad.ftLastAccessTime.dwLowDateTime;
    fi->last_access_hi = ad.ftLastAccessTime.dwHighDateTime;
    fi->last_write_lo = ad.ftLastWriteTime.dwLowDateTime;
    fi->last_write_hi = ad.ftLastWriteTime.dwHighDateTime;
    fi->change_lo = ad.ftLastWriteTime.dwLowDateTime;
    fi->change_hi = ad.ftLastWriteTime.dwHighDateTime;
    fi->alloc_lo = ad.nFileSizeLow;
    fi->alloc_hi = ad.nFileSizeHigh;
    fi->eof_lo = ad.nFileSizeLow;
    fi->eof_hi = ad.nFileSizeHigh;
    fi->attributes = ad.dwFileAttributes;
    HLOG_T("NtQueryFullAttributesFile '%s' ok", host);
    return X_STATUS_SUCCESS;
}

/* W150 ISO-DIRECT: NtQueryInformationFile fuer Abbild-Handles. Gleiche
 * Klassen/Groessen/Rueckgaben wie der Host-Pfad unten; Zeiten = Zeitstempel
 * des Volumes, Attribute aus dem Verzeichniseintrag. */
static u32 vfs_query_info(ht_entry_t *e, u32 iosb, u32 info, u32 len, u32 cls)
{
    iso_vfs_stat_t vs;
    uint32_t tlo, thi;
    u32 sz, attr;
    if (iso_vfs_stat(g_iso, e->vfs_node, &vs) != 0) {
        return X_STATUS_INVALID_HANDLE;
    }
    iso_vfs_volume_time(g_iso, &tlo, &thi);
    sz = vs.is_dir ? 0 : vs.size;
    attr = vs.attr ? vs.attr : FILE_ATTRIBUTE_NORMAL;
    switch (cls) {
    case X_FileBasicInformation: {
        u32 *b = (u32 *)(uintptr_t)info;
        if (len < 40) return X_STATUS_INFO_LENGTH_MISMATCH;
        b[0] = b[2] = b[4] = b[6] = tlo;
        b[1] = b[3] = b[5] = b[7] = thi;
        b[8] = attr;
        break;
    }
    case X_FileStandardInformation: {
        u32 *s = (u32 *)(uintptr_t)info;
        if (len < 24) return X_STATUS_INFO_LENGTH_MISMATCH;
        s[0] = sz; s[1] = 0; s[2] = sz; s[3] = 0;
        s[4] = 1; s[5] = 0; s[6] = e->is_dir ? 1 : 0;
        break;
    }
    case X_FilePositionInformation:
        if (len < 8) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(uint64_t *)(uintptr_t)info = e->vfs_pos;
        break;
    case X_FileEndOfFileInformation:
        if (len < 8) return X_STATUS_INFO_LENGTH_MISMATCH;
        *(uint64_t *)(uintptr_t)info = sz;
        break;
    case X_FileNetworkOpenInformation: {
        u32 *n = (u32 *)(uintptr_t)info;
        if (len < 56) return X_STATUS_INFO_LENGTH_MISMATCH;
        n[0] = n[2] = n[4] = n[6] = tlo;
        n[1] = n[3] = n[5] = n[7] = thi;
        n[8] = sz; n[9] = 0; n[10] = sz; n[11] = 0;
        n[12] = attr;
        break;
    }
    default:
        HLOG_W("NtQueryInformationFile: Klasse %u -> NOT_SUPPORTED", cls);
        return X_STATUS_NOT_SUPPORTED;
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtQueryInformationFile(u32 xh, u32 iosb, u32 info, u32 len,
                                                u32 cls)
{
    ht_entry_t *e = ht_get(xh);
    BY_HANDLE_FILE_INFORMATION fi;
    u32 written = 0, st = X_STATUS_SUCCESS;

    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (e->vfs) { /* W150 ISO-DIRECT */
        return vfs_query_info(e, iosb, info, len, cls);
    }
    if (!GetFileInformationByHandle(e->host, &fi)) {
        return winerr_to_status(GetLastError());
    }
    switch (cls) {
    case X_FileBasicInformation: { /* 40 B */
        struct {
            u32 ct[2], at[2], wt[2], ch[2];
            u32 attr;
        } *b;
        if (len < 40) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        b = (void *)(uintptr_t)info;
        b->ct[0] = fi.ftCreationTime.dwLowDateTime;
        b->ct[1] = fi.ftCreationTime.dwHighDateTime;
        b->at[0] = fi.ftLastAccessTime.dwLowDateTime;
        b->at[1] = fi.ftLastAccessTime.dwHighDateTime;
        b->wt[0] = fi.ftLastWriteTime.dwLowDateTime;
        b->wt[1] = fi.ftLastWriteTime.dwHighDateTime;
        b->ch[0] = b->wt[0];
        b->ch[1] = b->wt[1];
        b->attr = fi.dwFileAttributes;
        written = 40;
        break;
    }
    case X_FileStandardInformation: { /* 24 B */
        struct {
            u32 alloc[2], eof[2];
            u32 links, del, dir;
        } *s;
        LARGE_INTEGER sz;
        if (len < 24) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        s = (void *)(uintptr_t)info;
        GetFileSizeEx(e->host, &sz);
        s->alloc[0] = sz.LowPart;
        s->alloc[1] = (u32)sz.HighPart;
        s->eof[0] = sz.LowPart;
        s->eof[1] = (u32)sz.HighPart;
        s->links = 1;
        s->del = 0;
        s->dir = e->is_dir ? 1 : 0;
        written = 24;
        break;
    }
    case X_FilePositionInformation: {
        LARGE_INTEGER zero, pos;
        if (len < 8) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        zero.QuadPart = 0;
        SetFilePointerEx(e->host, zero, &pos, FILE_CURRENT);
        *(uint64_t *)(uintptr_t)info = (uint64_t)pos.QuadPart;
        written = 8;
        break;
    }
    case X_FileEndOfFileInformation: {
        LARGE_INTEGER sz;
        if (len < 8) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        GetFileSizeEx(e->host, &sz);
        *(uint64_t *)(uintptr_t)info = (uint64_t)sz.QuadPart;
        written = 8;
        break;
    }
    case X_FileNetworkOpenInformation: { /* 56 B */
        struct {
            u32 ct[2], at[2], wt[2], ch[2];
            u32 alloc[2], eof[2];
            u32 attr;
        } *n;
        LARGE_INTEGER sz;
        if (len < 56) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        n = (void *)(uintptr_t)info;
        GetFileSizeEx(e->host, &sz);
        n->ct[0] = fi.ftCreationTime.dwLowDateTime;
        n->ct[1] = fi.ftCreationTime.dwHighDateTime;
        n->at[0] = fi.ftLastAccessTime.dwLowDateTime;
        n->at[1] = fi.ftLastAccessTime.dwHighDateTime;
        n->wt[0] = fi.ftLastWriteTime.dwLowDateTime;
        n->wt[1] = fi.ftLastWriteTime.dwHighDateTime;
        n->ch[0] = n->wt[0];
        n->ch[1] = n->wt[1];
        n->alloc[0] = sz.LowPart;
        n->alloc[1] = (u32)sz.HighPart;
        n->eof[0] = sz.LowPart;
        n->eof[1] = (u32)sz.HighPart;
        n->attr = fi.dwFileAttributes;
        written = 56;
        break;
    }
    default:
        HLOG_W("NtQueryInformationFile: Klasse %u -> NOT_SUPPORTED", cls);
        return X_STATUS_NOT_SUPPORTED;
    }
    if (iosb) {
        /* NT: Query-Klassen melden Information=0 (nur Name/Dir-Queries
         * melden Laengen). Das Spiel prueft exakt darauf (TitleMeta). */
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    (void)written;
    return st;
}

static u32 __stdcall hle_NtSetInformationFile(u32 xh, u32 iosb, u32 info, u32 len,
                                              u32 cls)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (e->vfs) { /* W150 ISO-DIRECT: Zeiger setzen ja, Aendern nein */
        u32 st = X_STATUS_SUCCESS;
        if (cls == X_FilePositionInformation) {
            if (len < 8) {
                return X_STATUS_INFO_LENGTH_MISMATCH;
            }
            e->vfs_pos = *(uint64_t *)(uintptr_t)info;
        } else if (cls == X_FileEndOfFileInformation || cls == 13 /* Disposition */ ||
                   cls == 10 /* Rename */ || cls == 19 /* Allocation */) {
            HLOG_W("ISO: NtSetInformationFile(Klasse %u) auf D: verweigert '%s'", cls, e->path);
            st = X_STATUS_ACCESS_DENIED;
        } else {
            HLOG_W("NtSetInformationFile: Klasse %u -> SUCCESS (ignoriert)", cls);
        }
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
        }
        return st;
    }
    switch (cls) {
    case X_FilePositionInformation: {
        LARGE_INTEGER li;
        if (len < 8) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        li.QuadPart = *(int64_t *)(uintptr_t)info;
        SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
        break;
    }
    case X_FileEndOfFileInformation: {
        LARGE_INTEGER li, cur;
        LARGE_INTEGER zero;
        if (len < 8) {
            return X_STATUS_INFO_LENGTH_MISMATCH;
        }
        li.QuadPart = *(int64_t *)(uintptr_t)info;
        zero.QuadPart = 0;
        SetFilePointerEx(e->host, zero, &cur, FILE_CURRENT);
        SetFilePointerEx(e->host, li, NULL, FILE_BEGIN);
        krnl_seek_invalidate(xh); /* W22: Truncate -> Cache weg */
        SetEndOfFile(e->host);
        SetFilePointerEx(e->host, cur, NULL, FILE_BEGIN);
        break;
    }
    case 13: { /* FileDispositionInformation */
        e->delete_on_close = (*(uint8_t *)(uintptr_t)info) ? 1 : 0;
        break;
    }
    default:
        HLOG_W("NtSetInformationFile: Klasse %u -> SUCCESS (ignoriert)", cls);
        break;
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_SUCCESS;
}

/* W150: Platzhalter * und ? ohne Gross/Klein (Xbox-Masken wie "*.sav"). */
static int vfs_wild(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return 1;
            for (; *s; s++) {
                if (vfs_wild(p, s)) return 1;
            }
            return 0;
        }
        if (!*s) return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
        p++;
        s++;
    }
    return *s == 0;
}

/* W150 ISO-DIRECT: Verzeichnis im Abbild auflisten (je Aufruf ein Eintrag,
 * Xbox-FILE_DIRECTORY_INFORMATION mit ANSI-Namen, Klasse 1). Der
 * Ordner-Modus liefert weiter NO_MORE_FILES (M1, unten). */
static u32 vfs_query_dir(ht_entry_t *e, u32 ev, u32 iosb, u32 buf, u32 len,
                         u32 cls, u32 pMask, u32 restart)
{
    static unsigned nlog;
    u32 st = X_STATUS_NO_MORE_FILES, info_n = 0;
    xb_ansi_string *m = (xb_ansi_string *)(uintptr_t)pMask;
    if (restart & 0xFFu) {
        e->vfs_dir_k = 0;
        e->vfs_mask[0] = 0;
    }
    if (!e->vfs_mask[0]) {
        if (m && m->buffer && m->length && m->length < sizeof e->vfs_mask) {
            memcpy(e->vfs_mask, m->buffer, m->length);
            e->vfs_mask[m->length] = 0;
        } else {
            strcpy_s(e->vfs_mask, sizeof e->vfs_mask, "*");
        }
    }
    if (cls != 1) {
        st = X_STATUS_INVALID_INFO_CLASS;
    } else {
        int c;
        while ((c = iso_vfs_child(g_iso, e->vfs_node, e->vfs_dir_k)) >= 0) {
            const char *nm = iso_vfs_name(g_iso, c);
            u32 nl = (u32)strlen(nm), need = 0x40 + nl;
            iso_vfs_stat_t vs;
            uint32_t tlo, thi;
            u32 *o = (u32 *)(uintptr_t)buf;
            if (!vfs_wild(e->vfs_mask, nm)) {
                e->vfs_dir_k++;
                continue;
            }
            if (!buf || len < need) {
                st = 0xC0000023u; /* BUFFER_TOO_SMALL, Eintrag bleibt */
                break;
            }
            iso_vfs_stat(g_iso, c, &vs);
            iso_vfs_volume_time(g_iso, &tlo, &thi);
            memset(o, 0, 0x40);
            o[1] = e->vfs_dir_k;                         /* FileIndex */
            o[2] = o[4] = o[6] = o[8] = tlo;             /* Zeiten */
            o[3] = o[5] = o[7] = o[9] = thi;
            o[10] = o[12] = vs.is_dir ? 0 : vs.size;     /* EOF, Allocation */
            o[14] = vs.attr ? vs.attr : FILE_ATTRIBUTE_NORMAL;
            o[15] = nl;
            memcpy((char *)(uintptr_t)buf + 0x40, nm, nl);
            info_n = need;
            e->vfs_dir_k++;
            st = X_STATUS_SUCCESS;
            break;
        }
    }
    if (nlog < 16) {
        nlog++;
        HLOG_I("ISO: NtQueryDirectoryFile '%s' Maske '%s' k=%u -> 0x%X", e->path,
               e->vfs_mask, e->vfs_dir_k, st);
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = info_n;
    }
    if (ev) {
        ht_entry_t *ee = ht_get(ev);
        if (ee && ee->kind == HT_EVENT && ee->host) {
            SetEvent(ee->host);
        }
    }
    return st;
}

u32 __stdcall hle_NtQueryDirectoryFile(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4,
                                              u32 a5, u32 a6, u32 a7, u32 a8, u32 a9)
{
    if (g_iso) { /* W150 ISO-DIRECT */
        ht_entry_t *e = ht_get(a0);
        if (e && e->kind == HT_FILE && e->vfs) {
            return vfs_query_dir(e, a1, a4, a5, a6, a7, a8, a9);
        }
    }
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4;
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9;
    HLOG_T("NtQueryDirectoryFile -> NO_MORE_FILES (M1)");
    return X_STATUS_NO_MORE_FILES;
}

u32 __stdcall hle_NtFlushBuffersFile(u32 xh, u32 iosb)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || e->kind != HT_FILE) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (!e->vfs) { /* W150: Abbild-Handles: nichts zu tun */
        FlushFileBuffers(e->host);
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtQueryVolumeInformationFile(u32 xh, u32 iosb, u32 buf, u32 len,
                                                      u32 cls)
{
    ht_entry_t *e = ht_get(xh);
    u32 st = X_STATUS_SUCCESS;
    if (cls == 3 && buf && len >= 24) {
        /* FILE_FS_SIZE_INFORMATION: Xbox meldet FATX-Geometrie (Cluster =
         * 0x4000 Bytes; XapiInitProcess prueft exakt das). Belegung kommt
         * vom Host (64-bit, ohne DWORD-Cap). */
        const char *dir = (e && e->path[0]) ? e->path : g_cfg.media_root;
        ULARGE_INTEGER freeAvail, totalBytes, totalFree;
        ULONGLONG unit = 0x4000u, totalU, freeU;
        u32 spc = 32, bps = 512;
        u32 *o = (u32 *)(uintptr_t)buf;
        if (GetDiskFreeSpaceExA(dir, &freeAvail, &totalBytes, &totalFree)) {
            totalU = totalBytes.QuadPart / unit;
            freeU = freeAvail.QuadPart / unit;
        } else {
            totalU = (8ull * 1024 * 1024 * 1024) / unit; /* 8 GB */
            freeU = totalU / 2;
        }
        if (totalU == 0) {
            totalU = (8ull * 1024 * 1024 * 1024) / unit;
            freeU = totalU / 2;
        }
        o[0] = (u32)(totalU & 0xFFFFFFFFu);
        o[1] = (u32)(totalU >> 32);
        o[2] = (u32)(freeU & 0xFFFFFFFFu);
        o[3] = (u32)(freeU >> 32);
        o[4] = spc;
        o[5] = bps;
        HLOG_T("NtQueryVolumeInformationFile(FsSize) total=%llu frei=%llu (xh=0x%X)",
               totalU, freeU, xh);
    } else {
        HLOG_W("NtQueryVolumeInformationFile(Klasse %u) -> zeros (M1)", cls);
        if (buf && len) {
            memset((void *)(uintptr_t)buf, 0, len < 64 ? len : 64);
        }
    }
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = st;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return st;
}

u32 __stdcall hle_NtDeviceIoControlFile(u32 xh, u32 ev, u32 apc, u32 apcCtx,
                                               u32 iosb, u32 code, u32 inBuf,
                                               u32 inLen, u32 outBuf, u32 outLen)
{
    (void)xh; (void)ev; (void)apc; (void)apcCtx; (void)inBuf;
    /* IOCTL_DISK_GET_DRIVE_GEOMETRY (0x70000): 24B DISK_GEOMETRY.
     * Xbox-Platte (8GB, 512B-Sektoren, 63 S/T, 255 T/C, 1044 Zyl). */
    if (code == 0x70000 && outBuf && outLen >= 24) {
        uint8_t *o = (uint8_t *)(uintptr_t)outBuf;
        *(int64_t *)(o + 0) = 1044;   /* Cylinders */
        *(u32 *)(o + 8) = 12;         /* MediaType FixedMedia */
        *(u32 *)(o + 12) = 255;       /* TracksPerCylinder */
        *(u32 *)(o + 16) = 63;        /* SectorsPerTrack */
        *(u32 *)(o + 20) = 512;       /* BytesPerSector */
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status =
                X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 24;
        }
        HLOG_T("NtDeviceIoControlFile(GET_DRIVE_GEOMETRY) -> 8GB/512B");
        return X_STATUS_SUCCESS;
    }
    /* IOCTL_DISK_GET_PARTITION_INFO (0x74004): 32B PARTITION_INFORMATION.
     * Partition 1 (E:/Cache, 4GB, Typ 0x83). */
    if (code == 0x74004 && outBuf && outLen >= 28) {
        uint8_t *o = (uint8_t *)(uintptr_t)outBuf;
        *(int64_t *)(o + 0) = 0;               /* StartingOffset */
        *(int64_t *)(o + 8) = 4294967296LL;    /* PartitionLength 4GB */
        *(u32 *)(o + 16) = 0;                  /* HiddenSectors */
        *(u32 *)(o + 20) = 1;                  /* PartitionNumber */
        o[24] = 0x83;                          /* PartitionType */
        o[25] = 0;                             /* BootIndicator */
        o[26] = 1;                             /* RecognizedPartition */
        o[27] = 0;                             /* RewritePartition */
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status =
                X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 28;
        }
        HLOG_T("NtDeviceIoControlFile(GET_PARTITION_INFO) -> P1/4GB");
        return X_STATUS_SUCCESS;
    }
    HLOG_W("NtDeviceIoControlFile(code=0x%X in=%u out=%u) -> INVALID_DEVICE_REQUEST",
           code, inLen, outLen);
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_INVALID_DEVICE_REQUEST;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_INVALID_DEVICE_REQUEST;
}

u32 __stdcall hle_NtFsControlFile(u32 a0, u32 a1, u32 a2, u32 a3, u32 iosb,
                                         u32 code, u32 a6, u32 a7, u32 a8, u32 a9)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a6; (void)a7; (void)a8; (void)a9;
    /* FSCTL_DISMOUNT_VOLUME (0x90020): Volume aushaengen (vor Reboot).
     * M1: ERFOLG (nichts zu tun, Host-FS bleibt gemountet). */
    if (code == 0x90020) {
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status =
                X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
        }
        HLOG_T("NtFsControlFile(DISMOUNT_VOLUME) -> SUCCESS");
        return X_STATUS_SUCCESS;
    }
    HLOG_W("NtFsControlFile(code=0x%X) -> INVALID_DEVICE_REQUEST", code);
    if (iosb) {
        ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_INVALID_DEVICE_REQUEST;
        ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
    }
    return X_STATUS_INVALID_DEVICE_REQUEST;
}

/* ----- Objekte / Symlinks / Io ----- */
u32 __stdcall hle_IoCreateSymbolicLink(u32 pLink, u32 pTarget)
{
    /* PSTRING = ANSI_STRING auf Xbox */
    xb_ansi_string *l = (xb_ansi_string *)(uintptr_t)pLink;
    xb_ansi_string *t = (xb_ansi_string *)(uintptr_t)pTarget;
    char from[256], to[256];
    unsigned i;
    if (!l || !t || !l->buffer || !t->buffer) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    for (i = 0; i < sizeof from - 1 && i < l->length; i++) {
        from[i] = l->buffer[i];
    }
    from[i] = '\0';
    for (i = 0; i < sizeof to - 1 && i < t->length; i++) {
        to[i] = t->buffer[i];
    }
    to[i] = '\0';
    symlink_add(from, to);
    HLOG_I("IoCreateSymbolicLink '%s' -> '%s'", from, to);
    return X_STATUS_SUCCESS;
}

/* Ord 203 (W162): Xbox-Konvention stdcall/2 (ph, pAttr) - einzige Aufrufstelle
 * @0x1923D3 in XLaunchNewImageA pusht 2 (lea/push x2, kein Caller-Cleanup).
 * Das fruehere stdcall/3 las pAttr aus [esp+12] = Ruecksprungadresse und
 * driftete ESP +4. Pfad derzeit tot: XLaunchNewImageA ist jmp-gehookt
 * (xdk_hooks.c patch_jmp -> hook_XLaunchNewImageA, loggt XLAUNCH). */
u32 __stdcall hle_NtOpenSymbolicLinkObject(u32 ph, u32 pAttr)
{
    xb_object_attributes *oa;
    xb_ansi_string *as;
    char name[256];
    size_t n;
    u32 xh;
    if (g_cfg.hle_arity_check) {
        InterlockedIncrement(&g_arity203_calls);
    }
    if (!pAttr) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    oa = (xb_object_attributes *)(uintptr_t)pAttr;
    as = oa->object_name;
    if (!as || !as->buffer) {
        return X_STATUS_OBJECT_NAME_INVALID;
    }
    n = as->length < sizeof name - 1 ? as->length : sizeof name - 1;
    memcpy(name, as->buffer, n);
    name[n] = '\0';
    xh = ht_alloc(HT_SYMLINK, NULL);
    strncpy_s(ht_get(xh)->path, sizeof ht_get(xh)->path, name, _TRUNCATE);
    *(u32 *)(uintptr_t)ph = xh;
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtQuerySymbolicLinkObject(u32 xh, u32 pTarget, u32 pRetLen)
{
    ht_entry_t *e = ht_get(xh);
    const char *to;
    xb_unicode_string *t;
    unsigned i, n;
    if (!e || e->kind != HT_SYMLINK) {
        return X_STATUS_INVALID_HANDLE;
    }
    to = symlink_find(e->path);
    if (!to) {
        HLOG_W("NtQuerySymbolicLinkObject '%s': unbekannt", e->path);
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    t = (xb_unicode_string *)(uintptr_t)pTarget;
    n = (unsigned)strlen(to);
    if (t->max_length < n * 2) {
        return 0xC0000023u; /* BUFFER_TOO_SMALL */
    }
    for (i = 0; i < n; i++) {
        t->buffer[i] = (wchar_t)(unsigned char)to[i];
    }
    t->length = (xb_u16)(n * 2);
    if (pRetLen) {
        *(u32 *)(uintptr_t)pRetLen = n * 2;
    }
    return X_STATUS_SUCCESS;
}

/* W22 FILMAV: Seek-Cache (t2-Spin: 350K redundante Seeks/s auf pausiertem BGM).
 * Nur Offsets < 4 GB (alle Spieldateien): u32 atomar, volatile gegen
 * Compiler-Caching (x86-TSO ordnet); Cross-Thread-gleiches-Handle war schon
 * immer racy (Host-Zeiger geteilt) - der Cache aendert die Klasse nicht. */
int krnl_seek_skip(u32 xh, uint64_t off)
{
    ht_entry_t *e;
    if (!g_cfg.seek_cache || off >= 0x100000000ull)
        return 0;
    e = ht_get(xh);
    if (!e || e->kind != HT_FILE || !e->seek_valid)
        return 0;
    return e->seek_pos == (u32)off;
}

void krnl_seek_note(u32 xh, uint64_t off)
{
    ht_entry_t *e;
    if (!g_cfg.seek_cache)
        return;
    e = ht_get(xh);
    if (!e || e->kind != HT_FILE)
        return;
    if (off >= 0x100000000ull) {
        e->seek_valid = 0;
        return;
    }
    e->seek_pos = (u32)off;
    e->seek_valid = 1;
}

void krnl_seek_add(u32 xh, uint64_t n)
{
    ht_entry_t *e;
    uint64_t np;
    if (!g_cfg.seek_cache || n == 0)
        return;
    e = ht_get(xh);
    if (!e || e->kind != HT_FILE || !e->seek_valid)
        return;
    np = (uint64_t)e->seek_pos + n;
    if (np >= 0x100000000ull) {
        e->seek_valid = 0;
        return;
    }
    e->seek_pos = (u32)np;
}

void krnl_seek_invalidate(u32 xh)
{
    ht_entry_t *e;
    if (!g_cfg.seek_cache)
        return;
    e = ht_get(xh);
    if (e && e->kind == HT_FILE)
        e->seek_valid = 0;
}

/* W22 FILMAV: Zaehler-Wrapper (t2-Seek-Spin: Datei/Ursache am Laufende). */
u32 __stdcall hle_NtSetInformationFile_counted(u32 xh, u32 iosb, u32 info,
                                                      u32 len, u32 cls)
{
    LONG n = InterlockedIncrement(&s_si_n);
    u32 st;
    if (n == 1 || (n % 2000000) == 0) {
        ht_entry_t *e = ht_get(xh);
        s_si_xh = xh;
        s_si_cls = cls;
        s_si_caller = (u32)_ReturnAddress();
        s_si_tid = GetCurrentThreadId(); /* W32 */
        s_si_off = (cls == X_FilePositionInformation && len >= 8)
                       ? *(uint64_t *)(uintptr_t)info
                       : 0;
        if (e && e->kind == HT_FILE) {
            strcpy_s(s_si_path, sizeof s_si_path, e->path);
        } else {
            s_si_path[0] = 0;
        }
    }
    if (cls == X_FilePositionInformation && len >= 8 &&
        krnl_seek_skip(xh, *(uint64_t *)(uintptr_t)info)) {
        InterlockedIncrement(&s_si_skip);
        w32_seek_note_skip(); /* W32: nur Zaehler, kein Verhalten */
        /* W22: redundanter Seek = kein Fortschritt (gleicher Offset, keine
         * Bytes dazwischen). Nur billiger machen laesst den Poll schneller
         * spinnen (c1: 10x Calls); throttlen gibt Main den Kern zurueck. */
        Sleep(1);
        if (iosb) {
            ((xb_io_status_block *)(uintptr_t)iosb)->status = X_STATUS_SUCCESS;
            ((xb_io_status_block *)(uintptr_t)iosb)->information = 0;
        }
        return X_STATUS_SUCCESS;
    }
    if (cls == 13 && info && *(uint8_t *)(uintptr_t)info) {
        /* W187: Loeschen per FileDispositionInformation(TRUE) protokollieren
         * (nur Log, Verhalten unveraendert). Kommt der Aufruf aus XAPI
         * DeleteFileA (FUN_0018E47D, Ruecksprung 0x18E4EA), steht dessen
         * Aufrufer 0x40 Byte ueber dem Ruecksprung (5 Args, esi, 0x20 Lokale,
         * ebp). Spielstand-Loescher: 0xCA17A (Missionsende, Pilot KIA/
         * degradiert), 0xD67AD (Degradierung -> sasen00.sav), 0x10A325/
         * 0x10A350 (Speichern gescheitert). */
        ht_entry_t *e = ht_get(xh);
        u32 *ra = (u32 *)_AddressOfReturnAddress();
        u32 caller = ra[0] == 0x18E4EAu ? ra[0x40 / 4] : 0;
        HLOG_I("FILE delete '%s' (Disposition, Spiel-Ret=0x%X, DeleteFileA-Aufrufer=0x%X)",
               e && e->kind == HT_FILE ? e->path : "?", ra[0], caller);
    }
    st = hle_NtSetInformationFile(xh, iosb, info, len, cls);
    if (st == X_STATUS_SUCCESS && cls == X_FilePositionInformation && len >= 8)
        krnl_seek_note(xh, *(uint64_t *)(uintptr_t)info);
    return st;
}

/* ---------- W150 ISO-DIRECT: Selbsttest (iso_selftest=1, vor dem Spiel) ----------
 * Ruft die HLE-Funktionen wie das Spiel (Xbox-Strukturen, D:-Pfade) und
 * prueft: Lesen (XBEH, Groesse, Dateizeiger, EOF), Gross/Klein, fehlende
 * Datei, Verzeichnisliste, und dass jeder Schreib-/Erzeugungsversuch auf D:
 * ACCESS_DENIED liefert. Aendert nichts (Abbild nur lesend geoeffnet). */
static u32 st_open(const char *p, u32 access, u32 disp, u32 opt, u32 *xh)
{
    xb_ansi_string as;
    xb_object_attributes oa;
    xb_io_status_block io;
    as.length = (xb_u16)strlen(p);
    as.max_length = as.length;
    as.buffer = (char *)p;
    oa.root_directory = 0;
    oa.object_name = &as;
    oa.attributes = 0;
    *xh = 0;
    return hle_NtCreateFile((u32)(uintptr_t)xh, access, (u32)(uintptr_t)&oa,
                            (u32)(uintptr_t)&io, 0, 0, 1, disp, opt);
}

int krnl_vfs_selftest(void)
{
    static unsigned char buf[0x200];
    xb_io_status_block io;
    u32 xh = 0, st, n = 0, fail = 0;
    int64_t off;
    uint64_t pos = 0;
    if (!g_iso) {
        HLOG_W("ISO-SELFTEST: kein Abbild (game_image leer) - uebersprungen");
        return 0;
    }
#define ST_CHECK(cond, what) do { n++; if (!(cond)) { fail++; HLOG_E("ISO-SELFTEST: FEHLER %s", what); } } while (0)
    /* 1. Lesen */
    st = st_open("\\Device\\CdRom0\\DEFAULT.XBE", 0x80000000u, X_FILE_OPEN, 0x60, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS && xh, "open default.xbe (Gross/Klein)");
    if (st == X_STATUS_SUCCESS) {
        off = 0;
        st = hle_NtReadFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)buf, 4, (u32)(uintptr_t)&off);
        ST_CHECK(st == X_STATUS_SUCCESS && io.information == 4 && memcmp(buf, "XBEH", 4) == 0, "read XBEH");
        st = hle_NtReadFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)buf, 4, 0);
        ST_CHECK(st == X_STATUS_SUCCESS && io.information == 4, "read ohne Offset (Dateizeiger)");
        st = hle_NtQueryInformationFile(xh, (u32)(uintptr_t)&io, (u32)(uintptr_t)&pos, 8, X_FilePositionInformation);
        ST_CHECK(st == X_STATUS_SUCCESS && pos == 8, "Position = 8");
        off = 0x7FFFFFF0;
        st = hle_NtReadFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)buf, 16, (u32)(uintptr_t)&off);
        ST_CHECK(st == X_STATUS_END_OF_FILE && io.information == 0, "read hinter EOF -> END_OF_FILE");
        st = hle_NtWriteFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)buf, 4, 0);
        ST_CHECK(st == X_STATUS_ACCESS_DENIED, "NtWriteFile auf Lese-Handle verweigert");
        pos = 1;
        st = hle_NtSetInformationFile(xh, (u32)(uintptr_t)&io, (u32)(uintptr_t)&pos, 8, X_FileEndOfFileInformation);
        ST_CHECK(st == X_STATUS_ACCESS_DENIED, "EndOfFile setzen verweigert");
        hle_NtClose(xh);
    }
    /* 2. Schreiben/Erzeugen auf D: */
    st = st_open("\\Device\\CdRom0\\default.xbe", 0x40000000u, X_FILE_OPEN, 0x60, &xh);
    ST_CHECK(st == X_STATUS_ACCESS_DENIED && !xh, "GENERIC_WRITE verweigert");
    st = st_open("\\??\\D:\\default.xbe", 0x80000000u, X_FILE_OVERWRITE_IF, 0x60, &xh);
    ST_CHECK(st == X_STATUS_ACCESS_DENIED && !xh, "OVERWRITE_IF verweigert");
    st = st_open("D:\\w150_neu.txt", 0x80000000u | 0x40000000u, X_FILE_CREATE, 0x60, &xh);
    ST_CHECK(st == X_STATUS_ACCESS_DENIED && !xh, "CREATE verweigert");
    st = st_open("\\Device\\CdRom0\\media\\w150_neu", 0x80000000u, X_FILE_CREATE, 0x61, &xh);
    ST_CHECK(st == X_STATUS_ACCESS_DENIED && !xh, "Verzeichnis anlegen verweigert");
    /* 3. Fehlende Datei, Verzeichnis */
    st = st_open("\\Device\\CdRom0\\media\\gibt_es_nicht.bin", 0x80000000u, X_FILE_OPEN, 0x60, &xh);
    ST_CHECK(st == X_STATUS_OBJECT_NAME_NOT_FOUND, "fehlende Datei -> NOT_FOUND");
    st = st_open("\\Device\\CdRom0", 0x80000000u, X_FILE_OPEN, 0x21, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS && xh, "Wurzel als Verzeichnis oeffnen");
    if (st == X_STATUS_SUCCESS) {
        static unsigned char di[0x200];
        xb_ansi_string m;
        unsigned k = 0;
        m.buffer = "*";
        m.length = m.max_length = 1;
        while (hle_NtQueryDirectoryFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)di,
                                        sizeof di, 1, (u32)(uintptr_t)&m, k == 0) == X_STATUS_SUCCESS && k < 64) {
            k++;
        }
        {
            unsigned want = 0;
            while (iso_vfs_child(g_iso, 0, want) >= 0) {
                want++;
            }
            ST_CHECK(k == want && k > 0, "Wurzel auflisten");
        }
        HLOG_I("ISO-SELFTEST: Wurzel hat %u Eintraege", k);
        hle_NtClose(xh);
    }
#undef ST_CHECK
    HLOG_I("ISO-SELFTEST: %s (%u/%u Pruefungen)", fail ? "FEHLER" : "ok", n - fail, n);
    return (int)fail;
}

/* ---------- W190 SAVE-LOCK: Selbsttest (savelock_selftest=1, vor dem Spiel) ----------
 * Spielt den Tod eines Piloten ueber dieselben HLE-Wege wie das Spiel nach:
 * XAPI DeleteFileA (NtOpenFile -> FileDispositionInformation(TRUE) ->
 * NtClose) auf T:\user00.sav, dann T:\death00.sav schreiben, dann die Sicht
 * des Spiels (T:) und die echten Ordner pruefen. save_lock=1: echte Staende
 * unveraendert, die Arbeitskopie zeigt den Tod (bleibt so, Beleg auf der
 * Platte). save_lock=0: Original-Verhalten (echter Pilot geloescht). */
int krnl_savelock_selftest(void)
{
    static const char kMark[16] = "W190 SAVE-LOCK!";
    char ru[700], rd[700];
    xb_io_status_block io;
    u32 xh = 0, st, n = 0, fail = 0;
    int act = g_sl_active != 0, had_d;
    unsigned char del = 1;
    _snprintf_s(ru, sizeof ru, _TRUNCATE, "%s\\TDATA\\%s\\user00.sav", g_cfg.hdd_root, XB_TITLE_ID_STR);
    _snprintf_s(rd, sizeof rd, _TRUNCATE, "%s\\TDATA\\%s\\death00.sav", g_cfg.hdd_root, XB_TITLE_ID_STR);
    if (GetFileAttributesA(ru) == INVALID_FILE_ATTRIBUTES) {
        HLOG_W("SAVELOCK-SELFTEST: kein Pilot '%s' - uebersprungen", ru);
        return 0;
    }
    had_d = GetFileAttributesA(rd) != INVALID_FILE_ATTRIBUTES;
#define ST_CHECK(cond, what) do { n++; if (!(cond)) { fail++; HLOG_E("SAVELOCK-SELFTEST: FEHLER %s", what); } } while (0)
    /* 1. Pilot loeschen wie DeleteFileA */
    st = st_open("T:\\user00.sav", 0x00010000u | 0x00100000u, X_FILE_OPEN, 0x40, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS && xh, "T:\\user00.sav oeffnen (DELETE)");
    if (st == X_STATUS_SUCCESS) {
        hle_NtSetInformationFile_counted(xh, (u32)(uintptr_t)&io, (u32)(uintptr_t)&del, 1, 13);
        hle_NtClose(xh);
    }
    /* 2. KIA-Liste schreiben */
    st = st_open("T:\\death00.sav", 0x80000000u | 0x40000000u, X_FILE_OVERWRITE_IF, 0x40, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS && xh, "T:\\death00.sav anlegen");
    if (st == X_STATUS_SUCCESS) {
        st = hle_NtWriteFile(xh, 0, 0, 0, (u32)(uintptr_t)&io, (u32)(uintptr_t)kMark, sizeof kMark, 0);
        ST_CHECK(st == X_STATUS_SUCCESS && io.information == sizeof kMark, "death00.sav schreiben");
        hle_NtClose(xh);
    }
    /* 3. Sicht des Spiels in der Sitzung: konsistent */
    st = st_open("T:\\user00.sav", 0x80000000u, X_FILE_OPEN, 0x40, &xh);
    ST_CHECK(st == X_STATUS_OBJECT_NAME_NOT_FOUND, "Spiel sieht den Piloten geloescht");
    if (st == X_STATUS_SUCCESS) {
        hle_NtClose(xh);
    }
    st = st_open("T:\\death00.sav", 0x80000000u, X_FILE_OPEN, 0x40, &xh);
    ST_CHECK(st == X_STATUS_SUCCESS, "Spiel sieht death00.sav");
    if (st == X_STATUS_SUCCESS) {
        hle_NtClose(xh);
    }
    /* 4. echte Ordner */
    if (act) {
        ST_CHECK(GetFileAttributesA(ru) != INVALID_FILE_ATTRIBUTES, "echter Pilot unveraendert da");
        ST_CHECK((GetFileAttributesA(rd) != INVALID_FILE_ATTRIBUTES) == had_d, "kein neues echtes death00.sav");
    } else {
        ST_CHECK(GetFileAttributesA(ru) == INVALID_FILE_ATTRIBUTES, "Original: echter Pilot geloescht");
        ST_CHECK(GetFileAttributesA(rd) != INVALID_FILE_ATTRIBUTES, "Original: echtes death00.sav");
    }
#undef ST_CHECK
    HLOG_I("SAVELOCK-SELFTEST: %s (%u/%u Pruefungen, Modus %s)", fail ? "FEHLER" : "ok", n - fail, n,
           act ? "geschuetzt" : "original");
    return (int)fail;
}
