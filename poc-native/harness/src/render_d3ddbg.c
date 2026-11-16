/* render_d3ddbg.c - W302 AMD-ARTIFACTS: D3D11-Debug-Layer und WARP (nur Diagnose/Test).
 *
 * [render] d3d_debug=1: Geraet mit D3D11_CREATE_DEVICE_DEBUG. Die Meldungen der
 *   InfoQueue (Schwere CORRUPTION/ERROR/WARNING, INFO/MESSAGE verworfen) werden je
 *   Spiel-Present abgeholt, nach Meldungs-ID gruppiert und gezaehlt:
 *     D3DDBG neu id=<ID> sev=<S> cat=<C> p=<present>: <Text>   (erste 3 Texte je ID)
 *     D3DDBG stat p=<present> arten=<n> gesamt=<n>              (alle 30 s bei Aenderung)
 *     D3DDBG art id=<ID> sev=<S> n=<Anzahl> p0=<erstes> p1=<letztes>: <Text gekuerzt>
 *   d3d_debug=2 zusaetzlich nach jedem Binden eines Render-Ziels abholen (render_rtt.c),
 *   damit Hazard-Meldungen das Ziel tragen ("@rt<slot> <B>x<H>" bzw. "@bb").
 *   Fehlt die SDK-Schicht (d3d11sdklayers.dll, Windows-Feature "Grafiktools"),
 *   startet das Geraet ohne Debug (Log "D3DDBG nicht verfuegbar").
 * [render] d3d_warp=1: Software-Rasterizer WARP statt Grafikkarte (langsam, nur
 *   kurze Testszenen; Vergleich mit der Hardware ueber die normalen Screenshots).
 *   Der Prozess wird dabei auf hoechstens 4 Kerne ab [render] d3d_warp_core
 *   (Standard 26) begrenzt, sonst belegt WARP alle Kerne (Steuer 15.11.).
 *
 * Standard beide 0: kein Code im Bildpfad aktiv (ein Vergleich je Present).
 * Keine Eingaben, keine Pfade im Log. ASCII only. */
#include "render_int.h"
#include "sb_state.h" /* sb_in_mission (interp_shots_mrel) */
#include "sb_texhealth.h" /* W315 */

#define DDB_MAXID 256
#define DDB_TXT 200

typedef struct {
    int id;
    int sev, cat;
    unsigned n, p0, p1;
    char txt[DDB_TXT];
} ddb_art_t;

static ID3D11InfoQueue *g_ddb_q;
static ddb_art_t g_ddb[DDB_MAXID];
static int g_ddb_n;
static unsigned g_ddb_total, g_ddb_total_logged, g_ddb_last_stat;
static int g_ddb_avail = -1; /* -1 = nicht versucht, 0 = fehlt, 1 = aktiv */

UINT sb_d3ddbg_create_flags(void)
{
    UINT f = g_cfg.d3d_debug ? D3D11_CREATE_DEVICE_DEBUG : 0u;
    /* DIAG d3d_no_threadopt=1: interne Threading-Optimierungen des Treibers aus (Versuch, die
     * Command-List-Emulation der Laufzeit wie auf AMD/WARP zu erzwingen; Log zeigt DriverCommandLists) */
    if (g_cfg.d3d_no_threadopt) { f |= D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS; }
    return f;
}

/* WARP rechnet auf allen Kernen (Lauf W302_warpA: ~13 Kerne) -> Prozess-Affinitaet
 * auf hoechstens 4 Kerne ab d3d_warp_core (Standard 26), vor dem Anlegen. */
static void ddb_warp_affinity(void)
{
    DWORD_PTR pm = 0, sm = 0, want = 0;
    int c0 = g_cfg.d3d_warp_core, i;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) { return; }
    if (c0 < 0) { c0 = 0; }
    for (i = c0; i < c0 + 4 && i < (int)(sizeof(DWORD_PTR) * 8); i++) {
        if (sm & ((DWORD_PTR)1 << i)) { want |= (DWORD_PTR)1 << i; }
    }
    if (!want) { /* Rechner mit weniger Kernen: die hoechsten 4 vorhandenen */
        int k = 0;
        for (i = (int)(sizeof(DWORD_PTR) * 8) - 1; i >= 0 && k < 4; i--) {
            if (sm & ((DWORD_PTR)1 << i)) { want |= (DWORD_PTR)1 << i; k++; }
        }
    }
    if (want && SetProcessAffinityMask(GetCurrentProcess(), want)) {
        HLOG_I("D3DDBG warp: Prozess-Affinitaet 0x%IX (hoechstens 4 Kerne)", want);
    } else {
        HLOG_W("D3DDBG warp: Affinitaet nicht gesetzt (GLE=%lu)", GetLastError());
    }
}

D3D_DRIVER_TYPE sb_d3ddbg_driver(void)
{
    static int once;
    if (g_cfg.d3d_warp && !once) { once = 1; ddb_warp_affinity(); }
    return g_cfg.d3d_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
}

/* Nach dem Anlegen: InfoQueue holen und filtern. */
void sb_d3ddbg_init(ID3D11Device *dev, HRESULT create_hr_debug)
{
    D3D11_INFO_QUEUE_FILTER f;
    D3D11_MESSAGE_SEVERITY deny[2] = { D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE };
    D3D11_MESSAGE_ID deny_id[2] = { D3D11_MESSAGE_ID_QUERY_BEGIN_ABANDONING_PREVIOUS_RESULTS,
                                    D3D11_MESSAGE_ID_QUERY_END_ABANDONING_PREVIOUS_RESULTS };
    if (g_cfg.d3d_warp) { HLOG_I("D3DDBG warp: Software-Rasterizer WARP aktiv (nur Test)"); }
    if (dev) { /* Faehigkeiten, die AMD/WARP von NVIDIA unterscheiden (immer, eine Zeile) */
        D3D11_FEATURE_DATA_THREADING th;
        D3D11_FEATURE_DATA_D3D11_OPTIONS fo;
        memset(&th, 0, sizeof th);
        memset(&fo, 0, sizeof fo);
        ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_THREADING, &th, sizeof th);
        ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS, &fo, sizeof fo);
        HLOG_I("render: Faehigkeiten DriverCommandLists=%d DriverConcurrentCreates=%d CBOffsetting=%d "
               "MapNoOverwriteCB=%d MapNoOverwriteSRV=%d", th.DriverCommandLists ? 1 : 0,
               th.DriverConcurrentCreates ? 1 : 0, fo.ConstantBufferOffsetting ? 1 : 0,
               fo.MapNoOverwriteOnDynamicConstantBuffer ? 1 : 0, fo.MapNoOverwriteOnDynamicBufferSRV ? 1 : 0);
        /* FIX(AMD-ARTIFACTS,W302) render_thread_emul=1: Ohne treibereigene Command Lists (AMD, WARP)
         * emuliert die Laufzeit den Deferred-Context des Zwischenbild-Render-Threads (W126). Unter WARP
         * zeigten die Zwischenbilder dann weisse Flaechen/Balken (2D-Draws), NVIDIA (DriverCommandLists=1)
         * nie. -> Wiedergabe im Spiel-Thread (render_thread=0, Weg vor W168). 0 = alt (Render-Thread immer). */
        if (!th.DriverCommandLists && g_cfg.render_thread && g_cfg.render_thread_emul) {
            g_cfg.render_thread = 0;
            HLOG_I("render: keine treibereigenen Command Lists -> render_thread=0 (Zwischenbilder im Spiel-Thread, render_thread_emul=1)");
        }
        sb_th_device(dev, th.DriverCommandLists ? 1 : 0, g_cfg.d3d_warp); /* W315 Textur-Gesundheit */
    }
    if (!g_cfg.d3d_debug) { return; }
    if (FAILED(create_hr_debug) || !dev ||
        FAILED(ID3D11Device_QueryInterface(dev, &IID_ID3D11InfoQueue, (void **)&g_ddb_q))) {
        g_ddb_avail = 0;
        g_ddb_q = NULL;
        HLOG_W("D3DDBG nicht verfuegbar (hr=0x%lX): Geraet ohne Debug-Layer", (long)create_hr_debug);
        return;
    }
    g_ddb_avail = 1;
    memset(&f, 0, sizeof f);
    f.DenyList.NumSeverities = 2;
    f.DenyList.pSeverityList = deny;
    /* Query Begin/End ohne GetData (Leistungs-/Fence-Abfragen, gueltig, ~180 je Bild) -> verworfen */
    f.DenyList.NumIDs = 2;
    f.DenyList.pIDList = deny_id;
    ID3D11InfoQueue_AddStorageFilterEntries(g_ddb_q, &f);
    ID3D11InfoQueue_SetMessageCountLimit(g_ddb_q, 4096);
    HLOG_I("D3DDBG aktiv: Debug-Layer, Meldungen CORRUPTION/ERROR/WARNING%s",
           g_cfg.d3d_debug >= 2 ? ", je Render-Ziel" : "");
}

static const char *ddb_sev(int s)
{
    switch (s) {
    case D3D11_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
    case D3D11_MESSAGE_SEVERITY_ERROR: return "ERROR";
    case D3D11_MESSAGE_SEVERITY_WARNING: return "WARNING";
    case D3D11_MESSAGE_SEVERITY_INFO: return "INFO";
    default: return "MESSAGE";
    }
}

static void ddb_clean(char *d, size_t dn, const char *s, size_t sn)
{
    size_t i, o = 0;
    for (i = 0; i < sn && s[i] && o + 1 < dn; i++) {
        char c = s[i];
        d[o++] = (c >= 32 && c < 127) ? c : ' ';
    }
    d[o] = 0;
}

/* Alle gespeicherten Meldungen abholen. where = Zusatz fuers Log (NULL = Present). */
void sb_d3ddbg_drain(unsigned present, const char *where)
{
    UINT64 i, n;
    static D3D11_MESSAGE *msg;
    static SIZE_T cap;
    if (!g_ddb_q) { return; }
    n = ID3D11InfoQueue_GetNumStoredMessagesAllowedByRetrievalFilter(g_ddb_q);
    for (i = 0; i < n; i++) {
        SIZE_T len = 0;
        int k;
        if (FAILED(ID3D11InfoQueue_GetMessage(g_ddb_q, i, NULL, &len)) || !len) { continue; }
        if (len > cap) {
            void *p = realloc(msg, len);
            if (!p) { continue; }
            msg = (D3D11_MESSAGE *)p;
            cap = len;
        }
        if (FAILED(ID3D11InfoQueue_GetMessage(g_ddb_q, i, msg, &len))) { continue; }
        g_ddb_total++;
        for (k = 0; k < g_ddb_n; k++) {
            if (g_ddb[k].id == (int)msg->ID) { break; }
        }
        if (k == g_ddb_n && g_ddb_n < DDB_MAXID) {
            g_ddb_n++;
            g_ddb[k].id = (int)msg->ID;
            g_ddb[k].sev = (int)msg->Severity;
            g_ddb[k].cat = (int)msg->Category;
            g_ddb[k].p0 = present;
            ddb_clean(g_ddb[k].txt, sizeof g_ddb[k].txt, msg->pDescription, msg->DescriptionByteLength);
        }
        if (k < g_ddb_n) {
            g_ddb[k].n++;
            g_ddb[k].p1 = present;
            if (g_ddb[k].n <= 3u) {
                char t[600];
                ddb_clean(t, sizeof t, msg->pDescription, msg->DescriptionByteLength);
                HLOG_W("D3DDBG neu id=%d sev=%s cat=%d p=%u%s%s: %s", (int)msg->ID, ddb_sev((int)msg->Severity),
                       (int)msg->Category, present, where ? " " : "", where ? where : "", t);
            }
        }
    }
    if (n) { ID3D11InfoQueue_ClearStoredMessages(g_ddb_q); }
}

/* Zusammenfassung (alle 900 Spiel-Presents bei Aenderung bzw. force). */
void sb_d3ddbg_stat(unsigned present, int force)
{
    int k;
    if (!g_ddb_q) { return; }
    if (!force && present - g_ddb_last_stat < 900u) { return; }
    g_ddb_last_stat = present;
    if (!force && g_ddb_total == g_ddb_total_logged) { return; }
    g_ddb_total_logged = g_ddb_total;
    HLOG_I("D3DDBG stat p=%u arten=%d gesamt=%u", present, g_ddb_n, g_ddb_total);
    for (k = 0; k < g_ddb_n; k++) {
        HLOG_I("D3DDBG art id=%d sev=%s cat=%d n=%u p0=%u p1=%u: %.120s", g_ddb[k].id, ddb_sev(g_ddb[k].sev),
               g_ddb[k].cat, g_ddb[k].n, g_ddb[k].p0, g_ddb[k].p1, g_ddb[k].txt);
    }
}

/* DIAG(W302) interp_shots_mrel=N: Bildfolgen der Zwischenbilder (interp_shots) ab
 * Paar <Paare beim Beginn der Spielermission> + N. Bildnummern und Paarzaehler
 * schwanken zwischen Laeufen (Titel-Demo), der Abstand zum Missionsbeginn kaum. */
static void ddb_shots_mrel(unsigned present)
{
    static int done;
    if (done || g_cfg.interp_shots_mrel <= 0 || !sb_in_mission()) { return; }
    done = 1;
    g_cfg.interp_shots = (int)(sb_interp_pairs() + (unsigned)g_cfg.interp_shots_mrel);
    HLOG_I("D3DDBG mrel: Missionsbeginn p=%u Paare=%u -> interp_shots=%d", present, sb_interp_pairs(),
           g_cfg.interp_shots);
}

/* DIAG(TEX-WHITE-AMD,W315) interp_shots_scene=0x1B,0x2F: bei jedem Eintritt in eine der
 * Szenen interp_shots auf Paare + interp_shots_sdelay setzen und interp_shot_max um den
 * cfg-Wert erhoehen (Bildfolgen der Zwischenbilder in Menues/Briefings/Brief). */
static void ddb_shots_scene(unsigned present)
{
    static unsigned last = 0xFFFFFFFFu;
    static int per = -1;
    unsigned sc;
    int i, hit = 0;
    if (!g_cfg.interp_shots_scene[0]) { return; }
    sc = sb_state_scene();
    if (sc == last) { return; }
    last = sc;
    for (i = 0; i < 4 && g_cfg.interp_shots_scene[i]; i++) { hit |= g_cfg.interp_shots_scene[i] == sc; }
    if (!hit) { return; }
    if (per < 0) { per = g_cfg.interp_shot_max > 0 ? g_cfg.interp_shot_max : 8; g_cfg.interp_shot_max = 0; }
    g_cfg.interp_shot_max += per;
    g_cfg.interp_shots = (int)(sb_interp_pairs() + (unsigned)(g_cfg.interp_shots_sdelay > 0 ? g_cfg.interp_shots_sdelay : 1));
    HLOG_I("D3DDBG scene: Szene 0x%X p=%u Paare=%u -> interp_shots=%d max=%d", sc, present, sb_interp_pairs(),
           g_cfg.interp_shots, g_cfg.interp_shot_max);
}

/* Je Spiel-Present (unter ctx_lock). */
void sb_d3ddbg_present(unsigned present)
{
    ddb_shots_mrel(present);
    ddb_shots_scene(present);
    if (!g_ddb_q) { return; }
    sb_d3ddbg_drain(present, NULL);
    sb_d3ddbg_stat(present, 0);
}

/* ---- W302 NaN/Inf: HLSL-Schnipsel vor "return c;" (Variable c, float4) ----
 * shader_nan_mark=1: nicht endliche Ausgabe -> magenta (DIAG, Zaehlung ueber die Bilder).
 * post_sanitize=1 (Standard): NaN -> 0, +-Inf -> +-1 je Kanal. Endliche Werte bleiben
 * bitgleich; NaN/Inf wuerden sonst im Blending (Inf*0, NaN*a) herstellerabhaengig
 * weiterwirken. fxc entfernt isnan() unter OPTIMIZATION_LEVEL3 ggf. -> Bit-Test. */
const char *sb_nan_hlsl_c(void)
{
    if (g_cfg.shader_nan_mark) {
        return "  if (any((asuint(c) & 0x7F800000u) == 0x7F800000u)) { return float4(1.0, 0.0, 1.0, 1.0); }\n";
    }
    if (g_cfg.post_sanitize) {
        return "  { bool4 nf = (asuint(c) & 0x7F800000u) == 0x7F800000u;\n"
               "    if (any(nf)) { bool4 nn = (asuint(c) & 0x007FFFFFu) != 0u; c = nf ? (nn ? 0.0 : sign(c)) : c; } }\n";
    }
    return "";
}

/* DIAG shader_nan_mark: nicht endliche Positionen/UV in den CPU-Vertices eines Draws
 * (gleiche Daten auf jeder Grafikkarte; ein Rasterizer behandelt Inf/NaN-Ecken
 * herstellerabhaengig). Log NANMARK vtx (gedeckelt), Summe alle 900 Presents. */
static unsigned g_nm_draws, g_nm_vpos, g_nm_vuv, g_nm_vcol, g_nm_logged, g_nm_last;
void sb_nanmark_verts(const void *verts, unsigned n, unsigned prim, int rt)
{
    const float *f = (const float *)verts;
    unsigned i, bp = 0, bu = 0, bc = 0;
    if (!g_cfg.shader_nan_mark || !f) { return; }
    for (i = 0; i < n; i++, f += 10) { /* im_vertex_t: pos[4], col[4], uv[2] */
        const unsigned *u = (const unsigned *)f;
        int k;
        for (k = 0; k < 4; k++) { if ((u[k] & 0x7F800000u) == 0x7F800000u) { bp++; break; } }
        for (k = 4; k < 8; k++) { if ((u[k] & 0x7F800000u) == 0x7F800000u) { bc++; break; } }
        for (k = 8; k < 10; k++) { if ((u[k] & 0x7F800000u) == 0x7F800000u) { bu++; break; } }
    }
    if (bp | bu | bc) {
        g_nm_draws++; g_nm_vpos += bp; g_nm_vuv += bu; g_nm_vcol += bc;
        if (g_nm_logged < 32u) {
            g_nm_logged++;
            HLOG_W("NANMARK vtx p=%u prim=%u n=%u rt=%d pos=%u uv=%u col=%u", g_present_no, prim, n, rt, bp, bu, bc);
        }
    }
    if (g_present_no - g_nm_last >= 900u) {
        g_nm_last = g_present_no;
        HLOG_I("NANMARK stat p=%u draws=%u pos=%u uv=%u col=%u", g_present_no, g_nm_draws, g_nm_vpos, g_nm_vuv, g_nm_vcol);
    }
}
