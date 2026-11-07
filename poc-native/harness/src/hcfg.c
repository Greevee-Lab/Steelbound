/* hcfg.c - Mini-INI-Parser (key=value, # Kommentar, [section] egal). */
#include "hcfg.h"
#include "hlog.h"
#include "sb_edition.h"
#include "sb_userdata.h" /* W150 USER-DATA */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

hcfg_t g_cfg;

void hcfg_defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    /* W116: Standardpfade je Edition (sb_edition.h; private unveraendert) */
    strcpy_s(g_cfg.xbe_path, sizeof g_cfg.xbe_path, SB_DEFAULT_XBE_PATH);
    strcpy_s(g_cfg.media_root, sizeof g_cfg.media_root, SB_DEFAULT_MEDIA_ROOT);
    strcpy_s(g_cfg.hdd_root, sizeof g_cfg.hdd_root, "HDD");
    g_cfg.game_image[0] = 0;   /* W150: leer = Ordner-Modus (bisher) */
    g_cfg.version_check = 1;     /* W150: unbekannte Spielversion -> kein Start */
    g_cfg.width = 2560;
    g_cfg.height = 1080;
    g_cfg.present_interval = 0;
    g_cfg.fps_limit = 120;
    g_cfg.window_hidden = 0;
    g_cfg.ui_thread = 1;
    g_cfg.window_mode = 0;      /* W81 WINMODE: Fenster (bisher) */
    g_cfg.alt_enter = 1;        /* W81: Alt+Enter = eigene Umschaltung */
    g_cfg.alt_enter_mode = 1;   /* W81: Alt+Enter aus dem Fenster -> randlos */
    g_cfg.win_resize = 1;       /* W81: Backbuffer folgt der Fenstergroesse */
    g_cfg.dpi_aware = -1;       /* W81: auto */
    g_cfg.fs_interp_match = 1;  /* W81: exklusiv Interp-Takt = Hz */
    g_cfg.winmode_alt_max = 8;  /* W81 Test */
    g_cfg.fog_scale = 1.0f;
    g_cfg.draw_distance_scale = 1.0f;
    g_cfg.level = HLOG_INFO;
    g_cfg.per_call = 0;
    g_cfg.syscall_trace = 0;
    g_cfg.hle_arity_check = 0; /* W162: Arity-Selbsttest + Zaehler aus */
    g_cfg.hlog_keep = 1;
    g_cfg.diag_states = 0;
    g_cfg.state_diag = 0;
    g_cfg.state_every = 1000;
    g_cfg.startup_diag = 0;
    g_cfg.combat_diag = 0;
    g_cfg.campaign_diag = 0; /* W63: DIAG(CAMPAIGN) aus */
    g_cfg.aimat_antiwindup = 1; /* W63: aimat-Anti-Windup an */
    g_cfg.poll_census = 0;
    g_cfg.nullcall_diag = 1;
    g_cfg.contig_diag = 0; /* W86: Blockliste aus (DIAG nur mit =1) */
    g_cfg.watch[0] = '\0'; /* WATCHDR aus (W41) */
    g_cfg.fair_cs = 1;
    g_cfg.thread_priorities = 1;
    g_cfg.resume_yield_ms = 2;
    g_cfg.safe_suspend = 1;
    g_cfg.safe_suspend_sync = 2; /* W119 LOADLOCK: SUSPMARK */
    g_cfg.diag_lock_model = 0;
    g_cfg.diag_lock_inject = 0;
    g_cfg.hlock_spin = 64;
    g_cfg.audio_setbuf_len = 1; /* W22: A/B-belegt (GetPos 621M->205K, Sil 88->0%) */
    g_cfg.seek_cache = 1; /* W22: A/B-belegt (t2 60->1%, Film 23->29/s) */
    g_cfg.tsc_freq = 1; /* W22: A/B-belegt (Film 45->182 s, e1 8/9) */
    g_cfg.audio_ring_clip = 1; /* W22: A/B-belegt (Feed 1,06x->1x, g2) */
    g_cfg.audio_ring_overwrite = 1; /* W83: A/B-belegt (Film-Stotterstellen, s. BERICHT W83) */
    g_cfg.audio_process_async = 1; /* W34: A/B-belegt (Lade-Pause 5,1 s -> <0,2 s, Ton per Orakel unveraendert) */
    g_cfg.tex_size_header = 1;
    g_cfg.im_ring = 1;
    g_cfg.res_refcount = 1;
    g_cfg.fatx_stale_extend = 1; /* W63 FATXSTALE */
    g_cfg.phys_exact = 1; /* W73 PHYSALIAS: Phys->Virt exakter Blockanfang zuerst (Fix) */
    g_cfg.hdr_hybrid = 1; /* W94 HDRSWAP: Hybrid-Header statt Tausch (Fix Absturz XCalculateSignatureBegin) */
    g_cfg.hdr_race_test = 0;
    g_cfg.xbox_blend = 1;
    g_cfg.ctx_lock = 1;
    g_cfg.tex_evict = 1;
    g_cfg.tex_cache_max = 2048; /* W63 TEXCACHE */
    g_cfg.tex_cache_lru = 1;    /* W63 TEXCACHE */
    g_cfg.tex_hdr_check = 1;    /* W63 TEXCACHE */
    g_cfg.tex_upload_guard = 1;
    g_cfg.screenshot_jpg = 1;
    g_cfg.memscan_every = 0;
    g_cfg.audio = 1; /* A7B: Standard an (s. BERICHT W12) */
    g_cfg.audio_capture = 1;
    g_cfg.hrtf_ptr = 0; /* W13: 0 = aus (A/B-belegt: 1 entfernt Nullcalls,
     * legt aber 0x2250C4-MMIO-Faults frei; erst mit MMIO-Audio (W12) -> 1) */
    g_cfg.audio_live = 1;
    g_cfg.audio_pan = 1;
    g_cfg.audio_freq = 1;
    g_cfg.audio_buflen = 1;
    g_cfg.sbc_momentary = 1; /* Master: Luke/Zuendung als Taster (Fix Missionsende nach Mech-Start) */
    g_cfg.ign_toggle_fix = 1; /* W52: Gate Bit 2 in Mission (HLE-Ersatz, natuerliche Zuendung per Toggle) */
    g_cfg.audio_caps_clamp = 1; /* W24: Fix Missionsende-Nullcall (Standard an) */
    g_cfg.audio_ithis_fix = 1; /* W89: Puffer-Stop/GetStatus am I-Wrapper (Standard an) */
    g_cfg.audio_loop_fix = 1; /* W89: Loop nur mit Play(LOOPING) (Standard an) */
    g_cfg.audio_pitch_range = 1; /* W89: weite Pitch-Klemme (Standard an) */
    g_cfg.audio_adpcm_lerp = 1; /* W89: ADPCM linear resamplen (Standard an) */
    g_cfg.audio_3d = 1; /* W89: DS3D-Distanzdaempfung (Standard an) */
    g_cfg.audio_oneshot_fix = 1; /* W95: Einmal-Toene enden am Datenende (Standard an) */
    g_cfg.audio_filter = 1; /* W102: DLS2-Voice-Filter (Standard an, A/B W102_d1/W102_f1) */
    g_cfg.movie_skip = 0;
    g_cfg.tex_swz06 = 1;
    g_cfg.lin_tex_rect = 1;
    g_cfg.lin_rhw_range = 1;
    g_cfg.rtt = 1; /* W23: RTT-Umleitung (Menue sichtbar, belegt W23_menu0/rtt1b) */
    g_cfg.rtt_readback = 1; /* W59 COMBAT: RT-Rueckschrieb (Zellenkarte -> Lock-On) */
    g_cfg.rtt_stale = 1; /* W124 MAP-RETRY: veraltete RT-Eintraege nach Missions-Neustart nicht mehr sampeln */
    g_cfg.rtt_selfcopy = 1; /* W106 FX-HOLE: Selbst-Sampling liest RT-Kopie, Ziel bleibt das RT (Rauch-Hitzeflimmern stanzte sonst Loecher ins Cockpit) */
    g_cfg.rtt_selfclip = 1; /* W45 SIDESMEAR: Selbst-Sampling-Clip an (Seiten schwarz, Mitte unveraendert) */
    g_cfg.aspect_mode = 1; /* W18: 2D-Pillarbox + 3D-Hor+ (Regel S3.10, nie strecken) */
    g_cfg.edge_mode = 0; /* W107 EDGE-MODE: aus (volles Hor+) bis zum Nutzerentscheid */
    g_cfg.edge_fade_width = 12; /* W107: % der 4:3-Boxbreite */
    g_cfg.edge_fade_strength = 100; /* W107: % */
    g_cfg.cockpit_widen = 0; /* W166 COCKPIT-WIDEN: Standard Original */
    g_cfg.cockpit_widen_seam = 100; /* W166: Naht (%) = 4:3-Kante, Box bleibt bitgleich */
    g_cfg.cockpit_widen_band = 0; /* W166: Band (%) */
    g_cfg.cockpit_widen_edge = 122; /* W166: Modellkante (%) */
    g_cfg.cockpit_widen_max = 100; /* W166: max. Verschiebung (%) */
    g_cfg.cockpit_widen_alt = 0; /* DIAG(W166) */
    g_cfg.m3d_census = 0; /* W25 MISSION3D: Missions-Zensus aus (DIAG) */
    g_cfg.sbar_diag = 0; /* W98 DIAG aus */
    g_cfg.sbar_skip_xp = 0; /* W98 DIAG aus */
    g_cfg.m3d_census_skip = 0; /* W98: Zensus-Draws ueberspringen (DIAG) */
    g_cfg.m3d_census_gate = 0; /* W36 MONITORVIEW: Zensus-Gate aus (DIAG) */
    g_cfg.vsh_color = 1; /* W50 WASH: oD0-Farbquelle an (Fix) */
    g_cfg.im_zero_explicit = 1; /* W54 TITLEBG2: explizites Null-Alpha transparent (Fix) */
    g_cfg.blend_alpha = 1; /* W56 RADAR: Alpha mitblenden (Xbox), nicht ersetzen */
    g_cfg.short2_raw = 1; /* W60 COCKPIT: SHORT2-UV roh (Texel), Monitor-Faces zeigen RT-Inhalt (Fix) */
    g_cfg.getstate_fix = 1; /* W60 COCKPIT: Get*-Shader-Stand aus den Set-Hooks (Fix) */
    g_cfg.rt_vp_clip = 1; /* W65 WORLDVIEW: Clip-Raum-Draws in RTs auf Spiel-Viewport (Fix, Gelaende im Hauptmonitor) */
    g_cfg.ps_texmode = 1; /* W65 WORLDVIEW: PS-Texturmodus Stufe 0 beachten (Fix, Zellenkarte/Gelaende) */
    g_cfg.tex_hash_fast = 1; /* W168 PERF-TICK */
    g_cfg.audio_setbuf_fast = 1; /* W168 PERF-TICK */
    g_cfg.pd_gw_nopri = 1; /* W168 PERF-TICK */
    g_cfg.swap_match_unlock = 1; /* W168 PERF-TICK */
    g_cfg.hlog_async = 1; /* W168 PERF-TICK */
    g_cfg.audio_decode_par = 4; /* W168 PERF-TICK */
    g_cfg.tex_inline_data = 1; /* W65 WORLDVIEW: Inline-XPR-Daten im Header-Block (Fix, Hangar-Texturen) */
    g_cfg.rt_scale = 1; /* W88 RENDER-RES: 1 = Xbox-Groesse (alt), 0 = auto, 2..4 = Faktor */
    g_cfg.rt_scale_min = 256; /* W88 RENDER-RES */
    g_cfg.rt_scale_dyn = 0;     /* Steuer 6.11. (V_C25p): aus - Umschalten legt RTs neu an (Spitzen, Pendeln); W146 hatte 1 (wirkt nur bei skalierten RTs, rt_scale != 1) */
    g_cfg.rt_scale_dyn_min = 2;
    g_cfg.rt_scale_dyn_hi = 70;
    g_cfg.rt_scale_dyn_lo = 45;
    g_cfg.rt_scale_dyn_keep = 0; /* PO: Standard aus bis gemessen; W165 GPU-COST2: Wechsel ohne Neuanlage (wirkt nur mit rt_scale_dyn=1) */
    g_cfg.tex_lock_guard = 1; /* W86 PHYSALIAS: Lock-Schreibbarkeitspruefung an (Fix) */
    g_cfg.wv_diag = 0; /* W65 WORLDVIEW: RT-Dumps der Welt aus (DIAG) */
    g_cfg.interp_fps = 120; /* W73 FPS-INTERP-2: Spieler-Standard 120 Bilder/s in der Mission (Tests: run_once.ps1 setzt 0) */
    g_cfg.interp_diag = 0;
    g_cfg.interp_verify = 0;
    g_cfg.interp_bsets = 2;  /* W125 TICK-PERF: 2 Reserve-Saetze (0 = alt) */
    g_cfg.interp_cockpit_cache = 0;     /* W146 GPU-COST: Cockpit-Cache aus bis Abnahme */
    g_cfg.interp_cockpit_cache_min = 16;
    g_cfg.interp_vb_kv = 1536;  /* W138: Standard (W137 §5, kein Neuanlegen beim Hangar-Oeffnen) */
    g_cfg.interp_gpu_q = 2;  /* W125 TICK-PERF: Grenze 2 Bilder (0 = alt) */
    g_cfg.interp_gpu_tight = 0; /* W125 TICK-PERF: 0 = aus */
    g_cfg.interp_bsets_nod = 1; /* W125 TICK-PERF: wirkt nur mit interp_bsets>0 */
    g_cfg.render_thread = 1; /* W126 RENDER-THREAD; W168 PERF-TICK: Standard 1 (ctx_lock-Warten des Spiel-Threads, verify 47/47 diff=0) */
    g_cfg.render_thread_core = -1; /* W126: frei */
    g_cfg.present_decouple = 1; /* W138 PERF-FIX: Present-Takt-Thread (nur Echtzeit mit interp_fps>0) */
    g_cfg.present_core = -1; /* W138: frei */
    g_cfg.warm_gpu = 16;     /* W139 PERF-WARM: GPU-Takt in der Ladephase halten (0 = aus) */
    g_cfg.warm_gpu_mem = 2;  /* W139: Speicheranteil je Einheit */
    g_cfg.warm_gpu_pct = 85; /* W139: Ziel-GPU-Anteil je Bildperiode (60: Treiber bleibt in P8, W139_w6) */
    g_cfg.pd_gpu_wait = 50;     /* PO: bisheriger Wert bis gemessen; W165 GPU-COST2: Wartefenster bei GPU-Rueckstau in % der Bildperiode (W138: 50) */
    g_cfg.warm_gpu_soft = 1;    /* PO 7.11.: A/B P_W165s0/s1 (3440+4K): Retry-Spitzen 2->0, sonst gleich; W165 GPU-COST2: Bits 1 Entprellung (Retry-Blips ohne Last), 2 Start 1/8, 4 Last nach dem Bild */
    g_cfg.warm_gpu_delay = 100; /* W165: ms Ladephase vor der ersten Last */
    g_cfg.warm_shaders = 1;  /* W139: Shader-Cache vorab laden/anlegen */
    g_cfg.warm_vbpre = 1;    /* W139: Tick-Puffer beim Start anlegen */
    g_cfg.shading = 1; /* W77 RENDER-QA: Shading-Pfad; Standard 1 seit 5.11. (Master, nach W112/W117/W121, Golden mission neu) */
    g_cfg.shade_mask = 31; /* W100: mit shade_psconst alle Ketten/2D korrekt (frueher 7, W77: Bit 8 (Ketten >= 5 Stufen) aus - Konstanten der Monitor-Komposition fehlen (Ueberbelichtung) */
    g_cfg.shade_scope = 0;
    g_cfg.shade_gpuvb = 1; /* W80 SHADE-PERF: nur bei shading=1 wirksam */
    g_cfg.shade_psconst = 1; /* W84 PSCONST, W100: Standard 1 (wirkt nur mit shading=1) */
    g_cfg.shade_dbg = 0; /* W100: DIAG */
    g_cfg.shade_ab = 0; /* W100: DIAG */
    g_cfg.shade_texskip = 1; /* W112: FIX(SHADE-BLOOM), wirkt nur mit shading=1 */
    g_cfg.vis_query = 1; /* W117: FIX(SHADE-M2), 1 = nur mit shading=1 */
    g_cfg.bloom_diag = 0; g_cfg.bloom_frame = 0; g_cfg.bloom_max = 4; g_cfg.bloom_every = 0; /* W112: DIAG(SHADE-BLOOM) */
    g_cfg.shade_shadow = 0; /* W100 RENDER-QA3: Schattenpuffer (nur shading=1); 0 bis die 120-FPS-Paarung mit dem Tiefenpass geklaert ist (W100_oni2: pairs=0) */
    g_cfg.shadow_diag = 0; g_cfg.shadow_ab = 0; /* W127: DIAG(SHADOWBUF) */
    g_cfg.shade_pbconst = 0; /* W80: Befund-Schalter, Standard aus */
    g_cfg.shade_cache = 1; /* W80 SHADE-PERF: Shader-Cache im Client-Ordner (nur bei shading=1) */
    g_cfg.shade_cfix = 1;
    g_cfg.im_uv_ot0 = 1; /* W84 TERRAIN-UV: Master 4.11. Standard 1 (Abnahme V_C3) */
    g_cfg.tex_mips = 1; /* W84 TEXMIP: Master 4.11. Standard 1 (Abnahme V_C3) */
    g_cfg.tex_addr = 0; /* W84 TEXADDR: Vorschlag 1 nach Master-Pruefung */
    g_cfg.dlg_text_upload = 1; /* W77 DLGTEXT, Standard 1 seit W85 (mit dlg_text_raster: Text sichtbar) */
    g_cfg.dlg_diag = 0; /* W85 DLGTEXT: Diagnose aus */
    g_cfg.dlg_text_raster = 1; /* FIX(DLGTEXT3,W85): rep stosd 0x196033 bleibt (mit dlg_text_upload=1) */
    g_cfg.dlg_text_clear = 1; /* FIX(DLGTEXT2,W97): Xbox-Clear loescht auch den CPU-Speicher der Textebene */
    g_cfg.rq_perf = 0; /* W80 SHADE-PERF: Messung aus (DIAG) */
    g_cfg.rq_diag = 0; /* W77 RENDER-QA: Zensus aus (DIAG) */
    g_cfg.rq_every = 3000;
    g_cfg.interp_shots = 0;
    g_cfg.interp_shot_ticks = 6;
    g_cfg.interp_shot_every = 0;
    g_cfg.interp_shot_max = 8;
    g_cfg.interp_maxjump = 0.5f;
    g_cfg.pf_max = 1; /* DIAG(POPIN,W91) */
    g_cfg.interp_inst = 1; /* FIX(POPIN,W91): Instanz-Zuordnung (A/B W91_r6 vs W91_r7) */
    g_cfg.interp_pass = 1; /* FIX(POPIN,W91): Mehrpass-Konsistenz */
    g_cfg.im_fast = 1; /* W99 IM-PERF: schneller VB-Pfad (A/B W99_rt0s0/rt1s0b, L0a/L1a, Selbstpruefung W99_chk1) */
    g_cfg.im_end_reset = 1; /* W64 EDGE: IM-Batch nach End geschlossen (Fix, kein Geister-Quad im Hor+-Rand) */
    g_cfg.fx_blend_eq = 1; /* W90 FX-ORIG: Blend-Gleichung/-Farbe (Fix) */
    g_cfg.clear_xbflags = 0; /* W101 CLEAR-XB: Xbox-Clear-Flags - aus (globale Wirkung, s. BERICHT W101) */
    g_cfg.fx_zfunc = 1; /* W90 FX-ORIG / W101 FRONT-HANGAR: Tiefenfunktion an - Front-Fenster-Quad (RT31-Welt) liegt LEQUAL auf gleicher Tiefe wie sein Tiefen-Quad: Hangar statt Schwarz bei offener Luke */
    g_cfg.tex_lockwatch = 1; /* W98 STARTSEQ-BARS: Fix (A/B W98_a1/a3) */
    g_cfg.remaster = 0; /* W103 REM-FRAME: [remaster] Standard AUS */
    g_cfg.hd_textures = 0;
    g_cfg.hd_scale = 4;
    g_cfg.fx_enhanced = 0;
    g_cfg.rem_ultrawide = 0;
    g_cfg.postfx = 0;
    g_cfg.hd_cache_mb = 1024;
    g_cfg.hd_cpu_nohash = 1; /* W123 FILM-FPS: Film-Uploads ohne Remaster-Hash */
    g_cfg.hd_manual = 1; /* W113 REM-TEXT: wirkt nur mit remaster=1 + hd_textures=1 */
    g_cfg.tex_dump = 0;
    g_cfg.remaster_dir[0] = '\0';
    g_cfg.pack_loader = 1; /* W148 PACK-LAUNCHER: Standard = Paket-Lader (ohne packs.json: Original) */
    strcpy_s(g_cfg.pack_list, sizeof g_cfg.pack_list, "packs/packs.json"); /* W148: relativ zu remaster_dir */
    g_cfg.pack_fx = 1; /* W151 EFFECT-PACK: Effect-Pack-Parameter anwenden */
    g_cfg.overlay = 1;                  /* W118: ESC-Overlay an (geschlossen zeichnet es nichts) */
    g_cfg.overlay_test[0] = '\0';       /* W118: Test-Hook aus */
    g_cfg.overlay_save_path[0] = '\0';  /* W118: leer = user.cfg */
    strcpy_s(g_cfg.lang, sizeof g_cfg.lang, "en"); /* W142: [ui] lang */
    g_cfg.perf_hud = 0;                 /* W122: Leistungs-Overlay aus (Taste aktiv) */
    g_cfg.perf_hud_pos = 0;             /* W122: oben rechts */
    strcpy_s(g_cfg.perf_hud_key, sizeof g_cfg.perf_hud_key, "F2"); /* W122 */
    g_cfg.perf_log = 0;                 /* W122: DIAG aus */
    g_cfg.perf_spike_ms = 0;            /* W122: 0 = 2x Zielbildzeit */
    g_cfg.perf_prof = 0;                /* W122: DIAG Profiler aus */
    g_cfg.perf_prof_heavy = 0;          /* W168: DIAG Schwelle schwerer Tick (0 = 28 ms) */
    g_cfg.perf_blk = 0;                 /* W125: DIAG Blockier-Zeitmesser aus */
    g_cfg.perf_gpass = 0;               /* W146: DIAG GPU je Pass aus */
    g_cfg.perf_gsplit = 0;              /* W125: DIAG GPU-Aufteilung aus */
    g_cfg.vr_enable = 0;                /* W134 VR-PROTO: Standard aus (bitgleich) */
    strcpy_s(g_cfg.vr_preview, sizeof g_cfg.vr_preview, "sbs");
    g_cfg.vr_runtime_json[0] = '\0';     /* leer = XR_RUNTIME_JSON / Registry */
    g_cfg.vr_seat_height = 0;
    g_cfg.vr_world_scale = 100;
    g_cfg.vr_ipd = 640;
    g_cfg.vr_fov = 0;
    g_cfg.vr_monitor_depth = 0;
    g_cfg.vr_relief = 150;
    strcpy_s(g_cfg.vr_sim, sizeof g_cfg.vr_sim, "keys");
    strcpy_s(g_cfg.vr_recenter_key, sizeof g_cfg.vr_recenter_key, "F8");
    g_cfg.vr_diag = 0;
    g_cfg.vr_shots = 0;
    g_cfg.vr_canvas = 1;                /* W164 VR-FIX2 (wirkt nur mit vr_enable=1) */
    g_cfg.vr_canvas_dist = 2500;
    g_cfg.vr_canvas_size = 1500;
    g_cfg.vr_preview_clean = 1;
    g_cfg.vr_pose_shots = 0;
    g_cfg.vr_diag_skip = 0;
    g_cfg.vr_world_depth = 1;
    g_cfg.vr_inst_depth = 1;
    g_cfg.perf_trace = 0;               /* W137: DIAG Bild-/Tick-Spur aus */
    g_cfg.contig_fast = 1;              /* W122: low_alloc mit Merkpunkt (0 = alt) */
    g_cfg.mem_wc_strip = 1;             /* W123: WC/NOCACHE nicht ins Host-Mapping (0 = alt) */
    g_cfg.fx_alphakill = 1; /* W98 STARTSEQ-BARS: ALPHAKILL in der Mission (Fix) */
    g_cfg.fx_alphatest = 1; /* W90 FX-ORIG: Alpha-Test (Fix) */
    g_cfg.fx_ps_c0 = 1; /* W90 FX-ORIG: Effekt-PS (V0+C0)*T0 (Fix) */
    g_cfg.fx_im_uv = 1; /* W90 FX-ORIG: IM-UV aus oT0-Quelle (Fix) */
    g_cfg.fx_soft = 0; /* W133 REM-FX: Effekt-Optionen = Original */
    g_cfg.fx_embers = 0;
    g_cfg.fx_light = 0;
    g_cfg.fx_light_cockpit = 0;
    g_cfg.fx_interp = 0;
    g_cfg.fxr_ab = 0;
    g_cfg.fxr_diag = 0;
    g_cfg.monitor_fx = 1; /* W105 MONITOR-FX: Original-Monitoreffekte */
    g_cfg.monitor_grain = 100;
    g_cfg.monitor_glitch = 100;
    g_cfg.shade_mfx = 1; /* W121 SHADE-FX: Korn-Wash im Shading-Pfad (Fix, nur shading=1) */
    g_cfg.world_fog = 100; /* W121 SHADE-FX: Optionen = Original */
    g_cfg.world_contrast = 100;
    g_cfg.world_gamma = 100;
    g_cfg.world_sharpen = 0;
    g_cfg.bloom_strength = 100;
    g_cfg.cockpit_glow = 0;
    g_cfg.cockpit_spill = 0;
    g_cfg.cockpit_light = 100;
    g_cfg.vis_query_live = 1; /* W121/2 FIX: Sichtbarkeitstest auch bei Shading per ESC-Menue */
    g_cfg.shade_live_reset = 1; /* W121/2 FIX: Bloom-Akkumulatoren beim Pfadwechsel leeren */
    g_cfg.world_focus = 100; /* W121/2: Fokus-Unschaerfe Original */
    g_cfg.world_exposure = 100; /* W121/2: Optionen = Original */
    g_cfg.tonemap = 0;
    g_cfg.bloom_threshold = 0;
    g_cfg.light_world = 100;
    g_cfg.light_cockpit = 100;
    g_cfg.aa_mode = 0; /* W131 SHADER-OPT2: alles Original/aus */
    g_cfg.ssao = 0;
    g_cfg.color_sat = 100;
    g_cfg.color_temp = 0;
    g_cfg.color_lift = 0;
    g_cfg.color_gamma = 100;
    g_cfg.color_gain = 100;
    g_cfg.shadow_soft = 0;
    g_cfg.hud_label_avoid = 0; /* W153 HUD-LABEL-OPT: Standard Original (kein Patch) */
    g_cfg.vis_query_async = 1; /* W138: Standard (W137 §4: Kampf p99 10,44 -> 9,63; 1 Bild Latenz der Blendung) */
    g_cfg.bb_depth_srv = 0;
    g_cfg.ssao_view = 0;
    g_cfg.shfx_ab = 0;
    g_cfg.shot_mission = 0;
    g_cfg.shfx_diag = 0;
    g_cfg.fx_diag = 0; /* W90 FX-ORIG: Effekt-Zensus aus (DIAG) */
    g_cfg.fx_shots = 0; /* W90 FX-ORIG: Effekt-Bilder aus (DIAG) */
    g_cfg.drawdrop_diag = 0; /* W60 COCKPIT: Draw-Verwurf-DIAG aus (DIAG) */
    g_cfg.label_diag = 0; /* W153 HUD-LABEL: Etikett-DIAG aus */
    g_cfg.mon_dummy_nodepth = 1; /* W36 MONITORVIEW: Dummy-Tiefenpass ohne BB-Depth (Fix an) */
    g_cfg.mon_color_default = 1; /* W36 MONITORVIEW: Roh-Farbwerte -> Weiss (Fix an) */
    g_cfg.vsh_dump = 0; /* W38 VSH2D: VS-Mikrocode-Dump aus (DIAG) */
    g_cfg.vsh_emul = 1; /* W38 VSH2D: VSH-Emulation an (exakte oPos) */
    g_cfg.xbox_transform = 1; /* W25: Xbox-SetTransform-States (belegt: Spiel nutzt 0/1) */
    g_cfg.vsconst_mirror = 1; /* W25: VS-Konstanten-Spiegel (Original schreibt ihn, Fast nicht) */
    g_cfg.m3d_vs_mvp = 1; /* W25: MVP aus VS-Konstanten c98-c101 (Master-Prüfung V_W25a/b 8/9: Cockpit sichtbar, Titel/Menü/Film unverändert) */
    g_cfg.stall_loadpause = 1; /* W32: bekannte Lade-Pause als LOAD-PAUSE statt STALL */
    g_cfg.rs_colormask = 1; /* W33: COLOR_MASK abbilden (A/B-geprüft, s. BERICHT W33) */
    g_cfg.rs_stencil = 1; /* W33: Stencil-States abbilden (A/B-geprüft, s. BERICHT W33) */
    g_cfg.outwrite = 0; /* A8: Mapper allein (A/B C/D/F); 1 = alter Pfad */
    g_cfg.outwrite_frame = 80000;
    g_cfg.mouse_aim = 1;
    g_cfg.mouse_sight = 0;
    g_cfg.mouse_sens = 4;
    g_cfg.mouse_invert_x = 0;
    g_cfg.mouse_invert_y = 0;
    g_cfg.mouse_buttons = 1;
    g_cfg.input_diag = 0;
    g_cfg.mouse_capture = 0;    /* W79: Spieler-Standard per Launcher (user.cfg) */
    g_cfg.input_focus_only = 0; /* W79 */
    g_cfg.key_toggles = 0;      /* W79 */
    g_cfg.mouse_mode = 0;       /* W87: 0 = relativ (alt); Launcher schreibt absolute */
    g_cfg.aim_diag = 0;         /* W87: DIAG(AIMSTEP) aus */
    g_cfg.toggle_assist = 1;    /* W87: wirkt nur mit key_toggles=1 (Spielerbetrieb) */
    g_cfg.startseq_shots = 0;   /* W87: DIAG(STARTSEQ)-Bilder aus */
    g_cfg.input_source = 0;     /* W79: 0 = GetAsyncKeyState (alt) */
    g_cfg.input_test_hook = 0;  /* W79: Test-Hook aus */
    g_cfg.key_tap_queue = 0;    /* W92: 0 = alt (Tipper verschmelzen); Launcher schreibt 1 */
    g_cfg.sbc_usb = 0; /* W75: USB-Controller/Fixture aus (Default = Tastatur) */
    g_cfg.sbc_axis_arb = 1;      /* W128: zuletzt aktives Geraet gewinnt (nur sbc_usb=1) */
    g_cfg.sbc_mouse_off = 1;     /* W128: Maus-Achsen aus bei aktivem Controller */
    g_cfg.sbc_mouse_buttons = 1; /* W128: Maustasten bleiben */
    g_cfg.sbc_arb_dead = 8;
    g_cfg.sbc_mouse_wake = 3;
    g_cfg.pad_axis_arb = 1;   /* W132: Pad gegen Maus "zuletzt aktives Geraet gewinnt" */
    g_cfg.pad_mouse_off = 0;  /* W132: Maus bleibt parallel (Arbitrierung) */
    g_cfg.pad_arb_dead = 8;
    g_cfg.pad_mouse_wake = 3;
    g_cfg.pad_dz_stick = 0;  /* W143: 0 = Profil-Totzonen unveraendert */
    g_cfg.pad_dz_trig = 0;
    g_cfg.sbc_autocal = 1;     /* W128 (Key sbc_cal): Ruhelage beim Start erfassen */
    g_cfg.sbc_axis_dead = 6;
    g_cfg.sbc_pedal_dead = 16;
    g_cfg.sbc_fixture[0] = '\0';
    g_cfg.input_profile[0] = '\0'; /* W76: leer = Legacy-Belegung */
    g_cfg.screenshot_frame = 0;
    g_cfg.screenshot_every = 0;
    g_cfg.screenshot_max = 400;
    g_cfg.replay[0] = '\0';
    g_cfg.replay_steps[0] = '\0';
    g_cfg.turbo_until_frame = 0;
    g_cfg.turbo_until_mode = 0;
    g_cfg.vblank_qpc = 1; /* W73: QPC-Zeitbasis (gleichmaessige 33,3-ms-Fenster, Spieler-Standard; wirkt nur mit vblank_hz>0) */
    g_cfg.vblank_hz = 60; /* W48 REALTIME: Spieler-Standard = Xbox-Echtzeit; Tests via run_once.ps1 (vblank_hz=0) */
}

/* W103 REM-FRAME: [remaster]-Keys (eigene Funktion, set_key-Kette ist lang). */
static int set_key_remaster(const char *k, const char *v)
{
    if (strcmp(k, "remaster") == 0) { g_cfg.remaster = atoi(v); }
    else if (strcmp(k, "hd_textures") == 0) { g_cfg.hd_textures = atoi(v); }
    else if (strcmp(k, "hd_scale") == 0) { /* 2 oder 4; 0 = alt (W111 Auswahl) -> wirkt wie 4 mit Freigaben (W114) */
        g_cfg.hd_scale = (v[0] == '0' && atoi(v) == 0) ? 0 : ((atoi(v) == 2) ? 2 : 4);
    }
    else if (strcmp(k, "fx_enhanced") == 0) { g_cfg.fx_enhanced = atoi(v); }
    else if (strcmp(k, "ultrawide") == 0) { g_cfg.rem_ultrawide = atoi(v); }
    else if (strcmp(k, "postfx") == 0) { g_cfg.postfx = atoi(v); }
    else if (strcmp(k, "hd_cache_mb") == 0) { g_cfg.hd_cache_mb = atoi(v); }
    else if (strcmp(k, "hd_cpu_nohash") == 0) { g_cfg.hd_cpu_nohash = atoi(v); }
    else if (strcmp(k, "hd_manual") == 0) { g_cfg.hd_manual = atoi(v); }
    else if (strcmp(k, "tex_dump") == 0) { g_cfg.tex_dump = atoi(v); }
    else if (strcmp(k, "remaster_dir") == 0) { strcpy_s(g_cfg.remaster_dir, sizeof g_cfg.remaster_dir, v); }
    else if (strcmp(k, "pack_loader") == 0) { g_cfg.pack_loader = atoi(v); }
    else if (strcmp(k, "pack_list") == 0) { strcpy_s(g_cfg.pack_list, sizeof g_cfg.pack_list, v); }
    else if (strcmp(k, "pack_fx") == 0) { g_cfg.pack_fx = atoi(v); }
    else { return 0; }
    return 1;
}

/* W134 VR-PROTO: [vr]-Keys (flach, Praefix vr_). */
static int set_key_vr(const char *k, const char *v)
{
    if (strncmp(k, "vr_", 3) != 0) { return 0; }
    if (strcmp(k, "vr_enable") == 0) { g_cfg.vr_enable = atoi(v); }
    else if (strcmp(k, "vr_preview") == 0) { strncpy_s(g_cfg.vr_preview, sizeof g_cfg.vr_preview, v, _TRUNCATE); }
    else if (strcmp(k, "vr_runtime_json") == 0) { strncpy_s(g_cfg.vr_runtime_json, sizeof g_cfg.vr_runtime_json, v, _TRUNCATE); }
    else if (strcmp(k, "vr_seat_height") == 0) { g_cfg.vr_seat_height = atoi(v); }
    else if (strcmp(k, "vr_world_scale") == 0) { g_cfg.vr_world_scale = atoi(v); }
    else if (strcmp(k, "vr_ipd") == 0) { g_cfg.vr_ipd = atoi(v); }
    else if (strcmp(k, "vr_fov") == 0) { g_cfg.vr_fov = atoi(v); }
    else if (strcmp(k, "vr_monitor_depth") == 0) { g_cfg.vr_monitor_depth = atoi(v); }
    else if (strcmp(k, "vr_relief") == 0) { g_cfg.vr_relief = atoi(v); }
    else if (strcmp(k, "vr_sim") == 0) { strncpy_s(g_cfg.vr_sim, sizeof g_cfg.vr_sim, v, _TRUNCATE); }
    else if (strcmp(k, "vr_recenter_key") == 0) { strncpy_s(g_cfg.vr_recenter_key, sizeof g_cfg.vr_recenter_key, v, _TRUNCATE); }
    else if (strcmp(k, "vr_diag") == 0) { g_cfg.vr_diag = atoi(v); }
    else if (strcmp(k, "vr_shots") == 0) { g_cfg.vr_shots = atoi(v); }
    else if (strcmp(k, "vr_canvas") == 0) { g_cfg.vr_canvas = atoi(v); } /* W164 */
    else if (strcmp(k, "vr_canvas_dist") == 0) { g_cfg.vr_canvas_dist = atoi(v); }
    else if (strcmp(k, "vr_canvas_size") == 0) { g_cfg.vr_canvas_size = atoi(v); }
    else if (strcmp(k, "vr_preview_clean") == 0) { g_cfg.vr_preview_clean = atoi(v); }
    else if (strcmp(k, "vr_pose_shots") == 0) { g_cfg.vr_pose_shots = atoi(v); }
    else if (strcmp(k, "vr_diag_skip") == 0) { g_cfg.vr_diag_skip = atoi(v); }
    else if (strcmp(k, "vr_world_depth") == 0) { g_cfg.vr_world_depth = atoi(v); }
    else if (strcmp(k, "vr_inst_depth") == 0) { g_cfg.vr_inst_depth = atoi(v); }
    else { return 0; }
    return 1;
}

/* W118 REM-OVERLAY: [overlay]-Keys. */
static int set_key_overlay(const char *k, const char *v)
{
    if (strcmp(k, "overlay") == 0) { g_cfg.overlay = atoi(v); }
    else if (strcmp(k, "perf_hud") == 0) { g_cfg.perf_hud = atoi(v); }
    else if (strcmp(k, "perf_hud_pos") == 0) { g_cfg.perf_hud_pos = atoi(v); }
    else if (strcmp(k, "perf_hud_key") == 0) { strncpy_s(g_cfg.perf_hud_key, sizeof g_cfg.perf_hud_key, v, _TRUNCATE); }
    else if (strcmp(k, "perf_log") == 0) { g_cfg.perf_log = atoi(v); }
    else if (strcmp(k, "perf_spike_ms") == 0) { g_cfg.perf_spike_ms = atoi(v); }
    else if (strcmp(k, "perf_prof") == 0) { g_cfg.perf_prof = atoi(v); }
    else if (strcmp(k, "perf_prof_heavy") == 0) { g_cfg.perf_prof_heavy = atoi(v); } /* DIAG(PERF-TICK,W168) */
    else if (strcmp(k, "ctx_wait_diag") == 0) { g_cfg.ctx_wait_diag = atoi(v); } /* DIAG(PERF-TICK,W168) */
    else if (strcmp(k, "pd_gw_nopri") == 0) { g_cfg.pd_gw_nopri = atoi(v); } /* PERF(PERF-TICK,W168) */
    else if (strcmp(k, "swap_match_unlock") == 0) { g_cfg.swap_match_unlock = atoi(v); } /* PERF(PERF-TICK,W168) */
    else if (strcmp(k, "perf_blk") == 0) { g_cfg.perf_blk = atoi(v); }
    else if (strcmp(k, "perf_gsplit") == 0) { g_cfg.perf_gsplit = atoi(v); }
    else if (strcmp(k, "perf_gpass") == 0) { g_cfg.perf_gpass = atoi(v); } /* W146 */
    else if (strcmp(k, "perf_trace") == 0) { g_cfg.perf_trace = atoi(v); }
    else if (strcmp(k, "contig_fast") == 0) { g_cfg.contig_fast = atoi(v); }
    else if (strcmp(k, "mem_wc_strip") == 0) { g_cfg.mem_wc_strip = atoi(v); }
    else if (strcmp(k, "lang") == 0) { /* W142: [ui] lang (nur en/de) */
        strcpy_s(g_cfg.lang, sizeof g_cfg.lang, _stricmp(v, "de") == 0 ? "de" : "en");
    }
    else if (strcmp(k, "overlay_test") == 0) { strcpy_s(g_cfg.overlay_test, sizeof g_cfg.overlay_test, v); }
    else if (strcmp(k, "overlay_save_path") == 0) {
        strcpy_s(g_cfg.overlay_save_path, sizeof g_cfg.overlay_save_path, v);
    }
    else { return 0; }
    return 1;
}

static void set_key(const char *k, const char *v)
{
    if (set_key_remaster(k, v) || set_key_overlay(k, v) || set_key_vr(k, v)) {
        return;
    }
    if (strcmp(k, "xbe_path") == 0) {
        strcpy_s(g_cfg.xbe_path, sizeof g_cfg.xbe_path, v);
    } else if (strcmp(k, "media_root") == 0) {
        strcpy_s(g_cfg.media_root, sizeof g_cfg.media_root, v);
    } else if (strcmp(k, "hdd_root") == 0) {
        strcpy_s(g_cfg.hdd_root, sizeof g_cfg.hdd_root, v);
    } else if (strcmp(k, "game_image") == 0) {
        strcpy_s(g_cfg.game_image, sizeof g_cfg.game_image, v);
    } else if (strcmp(k, "version_check") == 0) {
        g_cfg.version_check = atoi(v);
    } else if (strcmp(k, "iso_selftest") == 0) {
        g_cfg.iso_selftest = atoi(v);
    } else if (strcmp(k, "width") == 0) {
        g_cfg.width = (unsigned)atoi(v);
    } else if (strcmp(k, "height") == 0) {
        g_cfg.height = (unsigned)atoi(v);
    } else if (strcmp(k, "present_interval") == 0) {
        g_cfg.present_interval = atoi(v);
    } else if (strcmp(k, "fps_limit") == 0) {
        g_cfg.fps_limit = atoi(v);
    } else if (strcmp(k, "window_hidden") == 0) {
        g_cfg.window_hidden = atoi(v);
    } else if (strcmp(k, "ui_thread") == 0) {
        g_cfg.ui_thread = atoi(v);
    } else if (strcmp(k, "window_mode") == 0) {
        g_cfg.window_mode = atoi(v);
    } else if (strcmp(k, "fs_width") == 0) {
        g_cfg.fs_width = (unsigned)atoi(v);
    } else if (strcmp(k, "fs_height") == 0) {
        g_cfg.fs_height = (unsigned)atoi(v);
    } else if (strcmp(k, "fs_hz") == 0) {
        g_cfg.fs_hz = atoi(v);
    } else if (strcmp(k, "fs_monitor") == 0) {
        g_cfg.fs_monitor = atoi(v);
    } else if (strcmp(k, "alt_enter") == 0) {
        g_cfg.alt_enter = atoi(v);
    } else if (strcmp(k, "alt_enter_mode") == 0) {
        g_cfg.alt_enter_mode = atoi(v);
    } else if (strcmp(k, "win_resize") == 0) {
        g_cfg.win_resize = atoi(v);
    } else if (strcmp(k, "dpi_aware") == 0) {
        g_cfg.dpi_aware = atoi(v);
    } else if (strcmp(k, "fs_interp_match") == 0) {
        g_cfg.fs_interp_match = atoi(v);
    } else if (strcmp(k, "winmode_test") == 0) {
        g_cfg.winmode_test = atoi(v);
    } else if (strcmp(k, "winmode_alt_every") == 0) {
        g_cfg.winmode_alt_every = atoi(v);
    } else if (strcmp(k, "winmode_alt_from") == 0) {
        g_cfg.winmode_alt_from = atoi(v);
    } else if (strcmp(k, "winmode_alt_max") == 0) {
        g_cfg.winmode_alt_max = atoi(v);
    } else if (strcmp(k, "fog_scale") == 0) {
        g_cfg.fog_scale = (float)atof(v);
    } else if (strcmp(k, "draw_distance_scale") == 0) {
        g_cfg.draw_distance_scale = (float)atof(v);
    } else if (strcmp(k, "level") == 0) {
        g_cfg.level = atoi(v);
    } else if (strcmp(k, "per_call") == 0) {
        g_cfg.per_call = atoi(v);
    } else if (strcmp(k, "syscall_trace") == 0) {
        g_cfg.syscall_trace = atoi(v);
    } else if (strcmp(k, "hle_arity_check") == 0) {
        g_cfg.hle_arity_check = atoi(v);
    } else if (strcmp(k, "hlog_keep") == 0) {
        g_cfg.hlog_keep = atoi(v);
    } else if (strcmp(k, "diag_states") == 0) {
        g_cfg.diag_states = atoi(v);
    } else if (strcmp(k, "state_diag") == 0) {
        g_cfg.state_diag = atoi(v);
    } else if (strcmp(k, "state_every") == 0) {
        g_cfg.state_every = atoi(v);
    } else if (strcmp(k, "startup_diag") == 0) {
        g_cfg.startup_diag = atoi(v);
    } else if (strcmp(k, "combat_diag") == 0) {
        g_cfg.combat_diag = atoi(v);
    } else if (strcmp(k, "campaign_diag") == 0) {
        g_cfg.campaign_diag = atoi(v);
    } else if (strcmp(k, "gear_diag") == 0) {
        g_cfg.gear_diag = atoi(v);
    } else if (strcmp(k, "aimat_antiwindup") == 0) {
        g_cfg.aimat_antiwindup = atoi(v);
    } else if (strcmp(k, "poll_census") == 0) {
        g_cfg.poll_census = atoi(v);
    } else if (strcmp(k, "nullcall_diag") == 0) {
        g_cfg.nullcall_diag = atoi(v);
    } else if (strcmp(k, "contig_diag") == 0) {
        g_cfg.contig_diag = atoi(v);
    } else if (strcmp(k, "watch") == 0) {
        strncpy_s(g_cfg.watch, sizeof g_cfg.watch, v,
                  sizeof g_cfg.watch - 1);
    } else if (strcmp(k, "fair_cs") == 0) {
        g_cfg.fair_cs = atoi(v);
    } else if (strcmp(k, "thread_priorities") == 0) {
        g_cfg.thread_priorities = atoi(v);
    } else if (strcmp(k, "resume_yield_ms") == 0) {
        g_cfg.resume_yield_ms = atoi(v);
    } else if (strcmp(k, "safe_suspend") == 0) {
        g_cfg.safe_suspend = atoi(v);
    } else if (strcmp(k, "safe_suspend_sync") == 0) {
        g_cfg.safe_suspend_sync = atoi(v);
    } else if (strcmp(k, "diag_lock_model") == 0) {
        g_cfg.diag_lock_model = atoi(v);
    } else if (strcmp(k, "diag_lock_inject") == 0) {
        g_cfg.diag_lock_inject = atoi(v);
    } else if (strcmp(k, "hlock_spin") == 0) {
        g_cfg.hlock_spin = atoi(v);
    } else if (strcmp(k, "hlog_async") == 0) { /* PERF(PERF-TICK,W168) */
        g_cfg.hlog_async = atoi(v);
    } else if (strcmp(k, "audio_decode_par") == 0) { /* PERF(PERF-TICK,W168) */
        g_cfg.audio_decode_par = atoi(v);
    } else if (strcmp(k, "audio_mix_yield") == 0) { /* PERF(PERF-TICK,W168) */
        g_cfg.audio_mix_yield = atoi(v);
    } else if (strcmp(k, "audio_lock_diag") == 0) { /* DIAG(PERF-TICK,W168) */
        g_cfg.audio_lock_diag = atoi(v);
    } else if (strcmp(k, "audio_setbuf_fast") == 0) { /* PERF(PERF-TICK,W168) */
        g_cfg.audio_setbuf_fast = atoi(v);
    } else if (strcmp(k, "audio_setbuf_len") == 0) {
        g_cfg.audio_setbuf_len = atoi(v);
    } else if (strcmp(k, "seek_cache") == 0) {
        g_cfg.seek_cache = atoi(v);
    } else if (strcmp(k, "tsc_freq") == 0) {
        g_cfg.tsc_freq = atoi(v);
    } else if (strcmp(k, "audio_ring_clip") == 0) {
        g_cfg.audio_ring_clip = atoi(v);
    } else if (strcmp(k, "pad_replay_gate") == 0) {
        g_cfg.pad_replay_gate = atoi(v);
    } else if (strcmp(k, "pad_axis_arb") == 0) {
        g_cfg.pad_axis_arb = atoi(v);
    } else if (strcmp(k, "pad_arb_dead") == 0) {
        g_cfg.pad_arb_dead = atoi(v);
    } else if (strcmp(k, "pad_mouse_wake") == 0) {
        g_cfg.pad_mouse_wake = atoi(v);
    } else if (strcmp(k, "pad_mouse_off") == 0) {
        g_cfg.pad_mouse_off = atoi(v);
    } else if (strcmp(k, "pad_dz_stick") == 0) {
        g_cfg.pad_dz_stick = atoi(v);
        if (g_cfg.pad_dz_stick < 0) g_cfg.pad_dz_stick = 0;
        if (g_cfg.pad_dz_stick > 90) g_cfg.pad_dz_stick = 90;
    } else if (strcmp(k, "pad_dz_trig") == 0) {
        g_cfg.pad_dz_trig = atoi(v);
        if (g_cfg.pad_dz_trig < 0) g_cfg.pad_dz_trig = 0;
        if (g_cfg.pad_dz_trig > 90) g_cfg.pad_dz_trig = 90;
    } else if (strcmp(k, "audio_ring_overwrite") == 0) {
        g_cfg.audio_ring_overwrite = atoi(v);
    } else if (strcmp(k, "audio_wrap_diag") == 0) {
        g_cfg.audio_wrap_diag = atoi(v);
    } else if (strcmp(k, "audio_ithis_fix") == 0) {
        g_cfg.audio_ithis_fix = atoi(v);
    } else if (strcmp(k, "audio_sfx_diag") == 0) {
        g_cfg.audio_sfx_diag = atoi(v);
    } else if (strcmp(k, "audio_oneshot_fix") == 0) {
        g_cfg.audio_oneshot_fix = atoi(v);
    } else if (strcmp(k, "audio_filter") == 0) {
        g_cfg.audio_filter = atoi(v);
    } else if (strcmp(k, "audio_level_diag") == 0) {
        g_cfg.audio_level_diag = atoi(v);
    } else if (strcmp(k, "audio_voice_diag") == 0) {
        g_cfg.audio_voice_diag = atoi(v);
    } else if (strcmp(k, "audio_loop_fix") == 0) {
        g_cfg.audio_loop_fix = atoi(v);
    } else if (strcmp(k, "audio_pitch_range") == 0) {
        g_cfg.audio_pitch_range = atoi(v);
    } else if (strcmp(k, "audio_adpcm_lerp") == 0) {
        g_cfg.audio_adpcm_lerp = atoi(v);
    } else if (strcmp(k, "audio_3d") == 0) {
        g_cfg.audio_3d = atoi(v);
    } else if (strcmp(k, "audio_process_async") == 0) {
        g_cfg.audio_process_async = atoi(v);
    } else if (strcmp(k, "tex_size_header") == 0) {
        g_cfg.tex_size_header = atoi(v);
    } else if (strcmp(k, "im_ring") == 0) {
        g_cfg.im_ring = atoi(v);
    } else if (strcmp(k, "res_refcount") == 0) {
        g_cfg.res_refcount = atoi(v);
    } else if (strcmp(k, "phys_exact") == 0) {
        g_cfg.phys_exact = atoi(v);
    } else if (strcmp(k, "hdr_hybrid") == 0) {
        g_cfg.hdr_hybrid = atoi(v);
    } else if (strcmp(k, "hdr_race_test") == 0) {
        g_cfg.hdr_race_test = atoi(v);
    } else if (strcmp(k, "fatx_stale_extend") == 0) {
        g_cfg.fatx_stale_extend = atoi(v);
    } else if (strcmp(k, "xbox_blend") == 0) {
        g_cfg.xbox_blend = atoi(v);
    } else if (strcmp(k, "ctx_lock") == 0) {
        g_cfg.ctx_lock = atoi(v);
    } else if (strcmp(k, "tex_evict") == 0) {
        g_cfg.tex_evict = atoi(v);
    } else if (strcmp(k, "tex_cache_max") == 0) {
        g_cfg.tex_cache_max = atoi(v);
    } else if (strcmp(k, "tex_cache_lru") == 0) {
        g_cfg.tex_cache_lru = atoi(v);
    } else if (strcmp(k, "tex_hdr_check") == 0) {
        g_cfg.tex_hdr_check = atoi(v);
    } else if (strcmp(k, "tex_upload_guard") == 0) {
        g_cfg.tex_upload_guard = atoi(v);
    } else if (strcmp(k, "screenshot_jpg") == 0) {
        g_cfg.screenshot_jpg = atoi(v);
    } else if (strcmp(k, "memscan_every") == 0) {
        g_cfg.memscan_every = atoi(v);
    } else if (strcmp(k, "tex_swz06") == 0) {
        g_cfg.tex_swz06 = atoi(v);
    } else if (strcmp(k, "lin_tex_rect") == 0) {
        g_cfg.lin_tex_rect = atoi(v);
    } else if (strcmp(k, "lin_rhw_range") == 0) {
        g_cfg.lin_rhw_range = atoi(v);
    } else if (strcmp(k, "rtt") == 0) {
        g_cfg.rtt = atoi(v);
    } else if (strcmp(k, "rtt_readback") == 0) {
        g_cfg.rtt_readback = atoi(v);
    } else if (strcmp(k, "rtt_stale") == 0) {
        g_cfg.rtt_stale = atoi(v);
    } else if (strcmp(k, "rtt_selfcopy") == 0) {
        g_cfg.rtt_selfcopy = atoi(v);
    } else if (strcmp(k, "rtt_selfclip") == 0) {
        g_cfg.rtt_selfclip = atoi(v);
    } else if (strcmp(k, "aspect_mode") == 0) {
        g_cfg.aspect_mode = atoi(v);
    } else if (strcmp(k, "edge_mode") == 0) { /* W107 */
        g_cfg.edge_mode = atoi(v);
    } else if (strcmp(k, "edge_fade_width") == 0) {
        g_cfg.edge_fade_width = atoi(v);
    } else if (strcmp(k, "edge_fade_strength") == 0) {
        g_cfg.edge_fade_strength = atoi(v);
    } else if (strcmp(k, "cockpit_widen") == 0) { /* W166 COCKPIT-WIDEN */
        g_cfg.cockpit_widen = atoi(v);
    } else if (strcmp(k, "cockpit_widen_seam") == 0) {
        g_cfg.cockpit_widen_seam = atoi(v);
    } else if (strcmp(k, "cockpit_widen_band") == 0) {
        g_cfg.cockpit_widen_band = atoi(v);
    } else if (strcmp(k, "cockpit_widen_edge") == 0) {
        g_cfg.cockpit_widen_edge = atoi(v);
    } else if (strcmp(k, "cockpit_widen_max") == 0) {
        g_cfg.cockpit_widen_max = atoi(v);
    } else if (strcmp(k, "cockpit_widen_alt") == 0) {
        g_cfg.cockpit_widen_alt = atoi(v);
    } else if (strcmp(k, "edge_ab") == 0) {
        g_cfg.edge_ab = atoi(v);
    } else if (strcmp(k, "m3d_census") == 0) {
        g_cfg.m3d_census = atoi(v);
    } else if (strcmp(k, "sbar_diag") == 0) {
        g_cfg.sbar_diag = atoi(v);
    } else if (strcmp(k, "sbar_skip_xp") == 0) {
        g_cfg.sbar_skip_xp = (unsigned)strtoul(v, NULL, 0);
    } else if (strcmp(k, "sbar_probe") == 0) {
        sscanf_s(v, "%d:%d:%d:%d", &g_cfg.sbar_probe[0], &g_cfg.sbar_probe[1], &g_cfg.sbar_probe[2], &g_cfg.sbar_probe[3]);
    } else if (strcmp(k, "m3d_census_skip") == 0) {
        g_cfg.m3d_census_skip = atoi(v);
    } else if (strcmp(k, "m3d_census_gate") == 0) {
        g_cfg.m3d_census_gate = atoi(v);
    } else if (strcmp(k, "vsh_color") == 0) {
        g_cfg.vsh_color = atoi(v);
    } else if (strcmp(k, "im_zero_explicit") == 0) {
        g_cfg.im_zero_explicit = atoi(v);
    } else if (strcmp(k, "blend_alpha") == 0) {
        g_cfg.blend_alpha = atoi(v);
    } else if (strcmp(k, "short2_raw") == 0) {
        g_cfg.short2_raw = atoi(v);
    } else if (strcmp(k, "getstate_fix") == 0) {
        g_cfg.getstate_fix = atoi(v);
    } else if (strcmp(k, "rt_vp_clip") == 0) {
        g_cfg.rt_vp_clip = atoi(v);
    } else if (strcmp(k, "ps_texmode") == 0) {
        g_cfg.ps_texmode = atoi(v);
    } else if (strcmp(k, "tex_hash_fast") == 0) { /* PERF(PERF-TICK,W168) */
        g_cfg.tex_hash_fast = atoi(v);
    } else if (strcmp(k, "tex_inline_data") == 0) {
        g_cfg.tex_inline_data = atoi(v);
    } else if (strcmp(k, "rt_scale") == 0) {
        g_cfg.rt_scale = (_stricmp(v, "auto") == 0) ? 0 : atoi(v);
        if (g_cfg.rt_scale < 0) { g_cfg.rt_scale = 0; }
        if (g_cfg.rt_scale > 4) { g_cfg.rt_scale = 4; }
    } else if (strcmp(k, "rt_scale_dyn") == 0) { g_cfg.rt_scale_dyn = atoi(v); /* W146 */
    } else if (strcmp(k, "rt_scale_dyn_min") == 0) { g_cfg.rt_scale_dyn_min = atoi(v);
    } else if (strcmp(k, "rt_scale_dyn_hi") == 0) { g_cfg.rt_scale_dyn_hi = atoi(v);
    } else if (strcmp(k, "rt_scale_dyn_lo") == 0) { g_cfg.rt_scale_dyn_lo = atoi(v);
    } else if (strcmp(k, "rt_scale_dyn_keep") == 0) { g_cfg.rt_scale_dyn_keep = atoi(v); /* W165 */
    } else if (strcmp(k, "rt_scale_min") == 0) {
        g_cfg.rt_scale_min = atoi(v);
    } else if (strcmp(k, "tex_lock_guard") == 0) {
        g_cfg.tex_lock_guard = atoi(v);
    } else if (strcmp(k, "wv_diag") == 0) {
        g_cfg.wv_diag = atoi(v);
    } else if (strcmp(k, "interp_fps") == 0) {
        g_cfg.interp_fps = atoi(v);
    } else if (strcmp(k, "interp_diag") == 0) {
        g_cfg.interp_diag = atoi(v);
    } else if (strcmp(k, "shading") == 0) {
        g_cfg.shading = atoi(v);
    } else if (strcmp(k, "shade_mask") == 0) {
        g_cfg.shade_mask = atoi(v);
    } else if (strcmp(k, "im_uv_ot0") == 0) {
        g_cfg.im_uv_ot0 = atoi(v);
    } else if (strcmp(k, "tex_mips") == 0) {
        g_cfg.tex_mips = atoi(v);
    } else if (strcmp(k, "tex_addr") == 0) {
        g_cfg.tex_addr = atoi(v);
    } else if (strcmp(k, "shade_cfix") == 0) {
        g_cfg.shade_cfix = atoi(v);
    } else if (strcmp(k, "shade_psconst") == 0) {
        g_cfg.shade_psconst = atoi(v);
    } else if (strcmp(k, "shade_shadow") == 0) {
        g_cfg.shade_shadow = atoi(v);
    } else if (strcmp(k, "shadow_diag") == 0) { /* DIAG(SHADOWBUF,W127) */
        g_cfg.shadow_diag = atoi(v);
    } else if (strcmp(k, "shadow_ab") == 0) {
        g_cfg.shadow_ab = atoi(v);
    } else if (strcmp(k, "shade_ab") == 0) {
        g_cfg.shade_ab = atoi(v);
    } else if (strcmp(k, "shade_dbg") == 0) {
        g_cfg.shade_dbg = atoi(v);
    } else if (strcmp(k, "shade_pbconst") == 0) {
        g_cfg.shade_pbconst = atoi(v);
    } else if (strcmp(k, "shade_cache") == 0) {
        g_cfg.shade_cache = atoi(v);
    } else if (strcmp(k, "shade_gpuvb") == 0) {
        g_cfg.shade_gpuvb = atoi(v);
    } else if (strcmp(k, "shade_scope") == 0) {
        g_cfg.shade_scope = atoi(v);
    } else if (strcmp(k, "dlg_text_upload") == 0) {
        g_cfg.dlg_text_upload = atoi(v);
    } else if (strcmp(k, "dlg_diag") == 0) {
        g_cfg.dlg_diag = atoi(v);
    } else if (strcmp(k, "dlg_text_raster") == 0) {
        g_cfg.dlg_text_raster = atoi(v);
    } else if (strcmp(k, "dlg_text_clear") == 0) {
        g_cfg.dlg_text_clear = atoi(v);
    } else if (strcmp(k, "rq_perf") == 0) {
        g_cfg.rq_perf = atoi(v);
    } else if (strcmp(k, "rq_diag") == 0) {
        g_cfg.rq_diag = atoi(v);
    } else if (strcmp(k, "rq_every") == 0) {
        g_cfg.rq_every = atoi(v);
    } else if (strcmp(k, "interp_bsets") == 0) {
        g_cfg.interp_bsets = atoi(v);
    } else if (strcmp(k, "interp_bsets_nod") == 0) {
        g_cfg.interp_bsets_nod = atoi(v);
    } else if (strcmp(k, "interp_gpu_tight") == 0) {
        g_cfg.interp_gpu_tight = atoi(v);
    } else if (strcmp(k, "render_thread") == 0) {
        g_cfg.render_thread = atoi(v);
    } else if (strcmp(k, "render_thread_core") == 0) {
        g_cfg.render_thread_core = atoi(v);
    } else if (strcmp(k, "present_decouple") == 0) {
        g_cfg.present_decouple = atoi(v);
    } else if (strcmp(k, "present_core") == 0) {
        g_cfg.present_core = atoi(v);
    } else if (strcmp(k, "pd_diag") == 0) {
        g_cfg.pd_diag = atoi(v);
    } else if (strcmp(k, "pd_diag_ms") == 0) {
        g_cfg.pd_diag_ms = atoi(v);
    } else if (strcmp(k, "rtt_dump") == 0) {
        g_cfg.rtt_dump = atoi(v);
    } else if (strcmp(k, "warm_shaders") == 0) {
        g_cfg.warm_shaders = atoi(v);
    } else if (strcmp(k, "warm_vb") == 0) {
        g_cfg.warm_vb = atoi(v);
    } else if (strcmp(k, "warm_gpu") == 0) {
        g_cfg.warm_gpu = atoi(v);
    } else if (strcmp(k, "warm_gpu_mem") == 0) {
        g_cfg.warm_gpu_mem = atoi(v);
    } else if (strcmp(k, "warm_gpu_pct") == 0) {
        g_cfg.warm_gpu_pct = atoi(v);
    } else if (strcmp(k, "pd_gpu_wait") == 0) {
        g_cfg.pd_gpu_wait = atoi(v);
    } else if (strcmp(k, "warm_gpu_soft") == 0) {
        g_cfg.warm_gpu_soft = atoi(v);
    } else if (strcmp(k, "warm_gpu_delay") == 0) {
        g_cfg.warm_gpu_delay = atoi(v);
    } else if (strcmp(k, "warm_vbpre") == 0) {
        g_cfg.warm_vbpre = atoi(v);
    } else if (strcmp(k, "warm_diag") == 0) {
        g_cfg.warm_diag = atoi(v);
    } else if (strcmp(k, "warm_diag_ms") == 0) {
        g_cfg.warm_diag_ms = atoi(v);
    } else if (strcmp(k, "interp_cockpit_cache") == 0) { /* W146 */
        g_cfg.interp_cockpit_cache = atoi(v);
    } else if (strcmp(k, "interp_cockpit_cache_min") == 0) {
        g_cfg.interp_cockpit_cache_min = atoi(v);
    } else if (strcmp(k, "interp_vb_kv") == 0) {
        g_cfg.interp_vb_kv = atoi(v);
    } else if (strcmp(k, "interp_gpu_q") == 0) {
        g_cfg.interp_gpu_q = atoi(v);
    } else if (strcmp(k, "interp_verify") == 0) {
        g_cfg.interp_verify = atoi(v);
    } else if (strcmp(k, "interp_shots") == 0) {
        g_cfg.interp_shots = atoi(v);
    } else if (strcmp(k, "interp_shot_ticks") == 0) {
        g_cfg.interp_shot_ticks = atoi(v);
    } else if (strcmp(k, "interp_shot_every") == 0) {
        g_cfg.interp_shot_every = atoi(v);
    } else if (strcmp(k, "interp_shot_max") == 0) {
        g_cfg.interp_shot_max = atoi(v);
    } else if (strcmp(k, "interp_inst") == 0) {
        g_cfg.interp_inst = atoi(v);
    } else if (strcmp(k, "interp_pass") == 0) {
        g_cfg.interp_pass = atoi(v);
    } else if (strcmp(k, "pf_ring") == 0) {
        g_cfg.pf_ring = atoi(v);
    } else if (strcmp(k, "pf_from") == 0) {
        g_cfg.pf_from = atoi(v);
    } else if (strcmp(k, "pf_every") == 0) {
        g_cfg.pf_every = atoi(v);
    } else if (strcmp(k, "pf_max") == 0) {
        g_cfg.pf_max = atoi(v);
    } else if (strcmp(k, "rt_from_mission") == 0) {
        g_cfg.rt_from_mission = atoi(v);
    } else if (strcmp(k, "rt_delay") == 0) {
        g_cfg.rt_delay = atoi(v);
    } else if (strcmp(k, "rt_fps_limit") == 0) {
        g_cfg.rt_fps_limit = atoi(v);
    } else if (strcmp(k, "rt_from_film") == 0) {
        g_cfg.rt_from_film = atoi(v);
    } else if (strcmp(k, "rt_mission_any") == 0) {
        g_cfg.rt_mission_any = atoi(v);
    } else if (strcmp(k, "rt_interp_fps") == 0) {
        g_cfg.rt_interp_fps = atoi(v);
    } else if (strcmp(k, "movie_skip_delay") == 0) {
        g_cfg.movie_skip_delay = atoi(v);
    } else if (strcmp(k, "interp_maxjump") == 0) {
        g_cfg.interp_maxjump = (float)atof(v);
    } else if (strcmp(k, "im_fast") == 0) {
        g_cfg.im_fast = atoi(v);
    } else if (strcmp(k, "im_end_reset") == 0) {
        g_cfg.im_end_reset = atoi(v);
    } else if (strcmp(k, "fx_blend_eq") == 0) {
        g_cfg.fx_blend_eq = atoi(v);
    } else if (strcmp(k, "clear_xbflags") == 0) {
        g_cfg.clear_xbflags = atoi(v);
    } else if (strcmp(k, "fx_shot_gap") == 0) {
        g_cfg.fx_shot_gap = atoi(v);
    } else if (strcmp(k, "fx_census") == 0) {
        g_cfg.fx_census = atoi(v);
    } else if (strcmp(k, "fx_zfunc") == 0) {
        g_cfg.fx_zfunc = atoi(v);
    } else if (strcmp(k, "tex_lockwatch") == 0) {
        g_cfg.tex_lockwatch = atoi(v);
    } else if (strcmp(k, "fx_alphakill") == 0) {
        g_cfg.fx_alphakill = atoi(v);
    } else if (strcmp(k, "fx_alphatest") == 0) {
        g_cfg.fx_alphatest = atoi(v);
    } else if (strcmp(k, "fx_ps_c0") == 0) {
        g_cfg.fx_ps_c0 = atoi(v);
    } else if (strcmp(k, "fx_shots") == 0) {
        g_cfg.fx_shots = atoi(v);
    } else if (strcmp(k, "fx_im_uv") == 0) {
        g_cfg.fx_im_uv = atoi(v);
    } else if (strcmp(k, "fx_soft") == 0) { /* W133 REM-FX */
        g_cfg.fx_soft = atoi(v);
    } else if (strcmp(k, "fx_embers") == 0) {
        g_cfg.fx_embers = atoi(v);
    } else if (strcmp(k, "fx_light") == 0) {
        g_cfg.fx_light = atoi(v);
    } else if (strcmp(k, "fx_light_cockpit") == 0) {
        g_cfg.fx_light_cockpit = atoi(v);
    } else if (strcmp(k, "fx_interp") == 0) {
        g_cfg.fx_interp = atoi(v);
    } else if (strcmp(k, "fxr_ab") == 0) {
        g_cfg.fxr_ab = atoi(v);
    } else if (strcmp(k, "fxr_diag") == 0) {
        g_cfg.fxr_diag = atoi(v);
    } else if (strcmp(k, "monitor_fx") == 0) {
        g_cfg.monitor_fx = atoi(v);
    } else if (strcmp(k, "monitor_grain") == 0) {
        g_cfg.monitor_grain = atoi(v);
    } else if (strcmp(k, "monitor_glitch") == 0) {
        g_cfg.monitor_glitch = atoi(v);
    } else if (strcmp(k, "shade_mfx") == 0) { /* FIX(SHADE-FX,W121) */
        g_cfg.shade_mfx = atoi(v);
    } else if (strcmp(k, "world_fog") == 0) { /* SHADE-FX (W121) */
        g_cfg.world_fog = atoi(v);
    } else if (strcmp(k, "world_contrast") == 0) {
        g_cfg.world_contrast = atoi(v);
    } else if (strcmp(k, "world_gamma") == 0) {
        g_cfg.world_gamma = atoi(v);
    } else if (strcmp(k, "world_sharpen") == 0) {
        g_cfg.world_sharpen = atoi(v);
    } else if (strcmp(k, "bloom_strength") == 0) {
        g_cfg.bloom_strength = atoi(v);
    } else if (strcmp(k, "cockpit_glow") == 0) {
        g_cfg.cockpit_glow = atoi(v);
    } else if (strcmp(k, "cockpit_spill") == 0) {
        g_cfg.cockpit_spill = atoi(v);
    } else if (strcmp(k, "cockpit_light") == 0) {
        g_cfg.cockpit_light = atoi(v);
    } else if (strcmp(k, "shade_live_reset") == 0) { /* FIX(SHADE-FX,W121) */
        g_cfg.shade_live_reset = atoi(v);
    } else if (strcmp(k, "vis_query_live") == 0) { /* FIX(SHADE-FX,W121) */
        g_cfg.vis_query_live = atoi(v);
    } else if (strcmp(k, "world_focus") == 0) { /* SHADE-FX (W121/2) */
        g_cfg.world_focus = atoi(v);
    } else if (strcmp(k, "world_exposure") == 0) { /* SHADE-FX (W121/2) */
        g_cfg.world_exposure = atoi(v);
    } else if (strcmp(k, "tonemap") == 0) {
        g_cfg.tonemap = (_stricmp(v, "reinhard") == 0) ? 1 : (_stricmp(v, "aces") == 0) ? 2 : atoi(v);
    } else if (strcmp(k, "bloom_threshold") == 0) {
        g_cfg.bloom_threshold = atoi(v);
    } else if (strcmp(k, "light_world") == 0) {
        g_cfg.light_world = atoi(v);
    } else if (strcmp(k, "light_cockpit") == 0) {
        g_cfg.light_cockpit = atoi(v);
    } else if (strcmp(k, "aa_mode") == 0) { /* SHADER-OPT2 (W131) */
        g_cfg.aa_mode = (_stricmp(v, "fxaa") == 0) ? 1 : atoi(v);
    } else if (strcmp(k, "ssao") == 0) {
        g_cfg.ssao = atoi(v);
    } else if (strcmp(k, "color_sat") == 0) {
        g_cfg.color_sat = atoi(v);
    } else if (strcmp(k, "color_temp") == 0) {
        g_cfg.color_temp = atoi(v);
    } else if (strcmp(k, "color_lift") == 0) {
        g_cfg.color_lift = atoi(v);
    } else if (strcmp(k, "color_gamma") == 0) {
        g_cfg.color_gamma = atoi(v);
    } else if (strcmp(k, "color_gain") == 0) {
        g_cfg.color_gain = atoi(v);
    } else if (strcmp(k, "shadow_soft") == 0) {
        g_cfg.shadow_soft = atoi(v);
    } else if (strcmp(k, "hud_label_avoid") == 0) { /* HUD-LABEL-OPT (W153) */
        g_cfg.hud_label_avoid = atoi(v);
    } else if (strcmp(k, "vis_query_async") == 0) {
        g_cfg.vis_query_async = atoi(v);
    } else if (strcmp(k, "bb_depth_srv") == 0) {
        g_cfg.bb_depth_srv = atoi(v);
    } else if (strcmp(k, "ssao_view") == 0) { /* DIAG(SHADER-OPT2,W131) */
        g_cfg.ssao_view = atoi(v);
    } else if (strcmp(k, "shfx_ab") == 0) { /* DIAG(SHADE-FX,W121) */
        g_cfg.shfx_ab = atoi(v);
    } else if (strcmp(k, "shot_mission") == 0) {
        g_cfg.shot_mission = atoi(v);
    } else if (strcmp(k, "shfx_diag") == 0) {
        g_cfg.shfx_diag = atoi(v);
    } else if (strcmp(k, "shade_texskip") == 0) { /* FIX(SHADE-BLOOM,W112) */
        g_cfg.shade_texskip = atoi(v);
    } else if (strcmp(k, "vis_query") == 0) { /* FIX(SHADE-M2,W117) */
        g_cfg.vis_query = atoi(v);
    } else if (strcmp(k, "vis_query_all") == 0) { /* W117 */
        g_cfg.vis_query_all = atoi(v);
    } else if (strcmp(k, "vis_diag") == 0) { /* DIAG(SHADE-M2,W117) */
        g_cfg.vis_diag = atoi(v);
    } else if (strcmp(k, "bloom_diag") == 0) { /* DIAG(SHADE-BLOOM,W112) */
        g_cfg.bloom_diag = atoi(v);
    } else if (strcmp(k, "bloom_frame") == 0) {
        g_cfg.bloom_frame = atoi(v);
    } else if (strcmp(k, "bloom_max") == 0) {
        g_cfg.bloom_max = atoi(v);
    } else if (strcmp(k, "bloom_every") == 0) {
        g_cfg.bloom_every = atoi(v);
    } else if (strcmp(k, "mfx_diag") == 0) {
        g_cfg.mfx_diag = atoi(v);
    } else if (strcmp(k, "mfx_ab") == 0) {
        g_cfg.mfx_ab = atoi(v);
    } else if (strcmp(k, "mfx_shots") == 0) {
        g_cfg.mfx_shots = atoi(v);
    } else if (strcmp(k, "fx_diag") == 0) {
        g_cfg.fx_diag = atoi(v);
    } else if (strcmp(k, "drawdrop_diag") == 0) {
        g_cfg.drawdrop_diag = atoi(v);
    } else if (strcmp(k, "label_diag") == 0) { /* DIAG(HUD-LABEL,W153) */
        g_cfg.label_diag = atoi(v);
    } else if (strcmp(k, "mon_dummy_nodepth") == 0) {
        g_cfg.mon_dummy_nodepth = atoi(v);
    } else if (strcmp(k, "mon_color_default") == 0) {
        g_cfg.mon_color_default = atoi(v);
    } else if (strcmp(k, "vsh_dump") == 0) {
        g_cfg.vsh_dump = atoi(v);
    } else if (strcmp(k, "vsh_emul") == 0) {
        g_cfg.vsh_emul = atoi(v);
    } else if (strcmp(k, "xbox_transform") == 0) {
        g_cfg.xbox_transform = atoi(v);
    } else if (strcmp(k, "vsconst_mirror") == 0) {
        g_cfg.vsconst_mirror = atoi(v);
    } else if (strcmp(k, "m3d_vs_mvp") == 0) {
        g_cfg.m3d_vs_mvp = atoi(v);
    } else if (strcmp(k, "stall_loadpause") == 0) {
        g_cfg.stall_loadpause = atoi(v);
    } else if (strcmp(k, "rs_colormask") == 0) {
        g_cfg.rs_colormask = atoi(v);
    } else if (strcmp(k, "rs_stencil") == 0) {
        g_cfg.rs_stencil = atoi(v);
    } else if (strcmp(k, "screenshot_frame") == 0) {
        g_cfg.screenshot_frame = atoi(v);
    } else if (strcmp(k, "screenshot_every") == 0) {
        g_cfg.screenshot_every = atoi(v);
    } else if (strcmp(k, "screenshot_max") == 0) {
        g_cfg.screenshot_max = atoi(v);
    } else if (strcmp(k, "replay_steps") == 0) {
        strncpy_s(g_cfg.replay_steps, sizeof g_cfg.replay_steps, v,
                  sizeof g_cfg.replay_steps - 1);
    } else if (strcmp(k, "turbo_until_frame") == 0) {
        g_cfg.turbo_until_frame = atoi(v);
    } else if (strcmp(k, "turbo_until_mode") == 0) {
        g_cfg.turbo_until_mode = atoi(v);
    } else if (strcmp(k, "audio") == 0) {
        g_cfg.audio = atoi(v);
    } else if (strcmp(k, "audio_capture") == 0) {
        g_cfg.audio_capture = atoi(v);
    } else if (strcmp(k, "hrtf_ptr") == 0) {
        g_cfg.hrtf_ptr = atoi(v);
    } else if (strcmp(k, "outwrite") == 0) {
        g_cfg.outwrite = atoi(v);
    } else if (strcmp(k, "outwrite_frame") == 0) {
        g_cfg.outwrite_frame = atoi(v);
    } else if (strcmp(k, "mouse_aim") == 0) {
        g_cfg.mouse_aim = atoi(v);
    } else if (strcmp(k, "mouse_sight") == 0) {
        g_cfg.mouse_sight = atoi(v);
    } else if (strcmp(k, "mouse_sens") == 0) {
        g_cfg.mouse_sens = atoi(v);
    } else if (strcmp(k, "mouse_invert_x") == 0) {
        g_cfg.mouse_invert_x = atoi(v);
    } else if (strcmp(k, "mouse_invert_y") == 0) {
        g_cfg.mouse_invert_y = atoi(v);
    } else if (strcmp(k, "mouse_buttons") == 0) {
        g_cfg.mouse_buttons = atoi(v);
    } else if (strcmp(k, "input_diag") == 0) {
        g_cfg.input_diag = atoi(v);
    } else if (strcmp(k, "mouse_capture") == 0) {
        g_cfg.mouse_capture = atoi(v);
    } else if (strcmp(k, "input_focus_only") == 0) {
        g_cfg.input_focus_only = atoi(v);
    } else if (strcmp(k, "input_source") == 0) {
        g_cfg.input_source = atoi(v);
    } else if (strcmp(k, "input_test_hook") == 0) {
        g_cfg.input_test_hook = atoi(v);
    } else if (strcmp(k, "key_toggles") == 0) {
        g_cfg.key_toggles = atoi(v);
    } else if (strcmp(k, "key_tap_queue") == 0) {
        g_cfg.key_tap_queue = atoi(v);
    } else if (strcmp(k, "mouse_mode") == 0) {
        /* W87: absolute|relative (auch 1/0) */
        g_cfg.mouse_mode = (_stricmp(v, "absolute") == 0 ||
                            _stricmp(v, "absolut") == 0 || atoi(v) == 1) ? 1 : 0;
    } else if (strcmp(k, "toggle_assist") == 0) {
        g_cfg.toggle_assist = atoi(v);
    } else if (strcmp(k, "aim_diag") == 0) {
        g_cfg.aim_diag = atoi(v);
    } else if (strcmp(k, "startseq_shots") == 0) {
        g_cfg.startseq_shots = atoi(v);
    } else if (strcmp(k, "sbc_fixture") == 0) {
        strcpy_s(g_cfg.sbc_fixture, sizeof g_cfg.sbc_fixture, v);
    } else if (strcmp(k, "sbc_usb") == 0) {
        g_cfg.sbc_usb = atoi(v);
    } else if (strcmp(k, "sbc_axis_arb") == 0) {
        g_cfg.sbc_axis_arb = atoi(v);
    } else if (strcmp(k, "sbc_mouse_off") == 0) {
        g_cfg.sbc_mouse_off = atoi(v);
    } else if (strcmp(k, "sbc_mouse_buttons") == 0) {
        g_cfg.sbc_mouse_buttons = atoi(v);
    } else if (strcmp(k, "sbc_arb_dead") == 0) {
        g_cfg.sbc_arb_dead = atoi(v);
    } else if (strcmp(k, "sbc_mouse_wake") == 0) {
        g_cfg.sbc_mouse_wake = atoi(v);
    } else if (strcmp(k, "sbc_cal") == 0) {
        g_cfg.sbc_autocal = atoi(v);
    } else if (strcmp(k, "sbc_axis_dead") == 0) {
        g_cfg.sbc_axis_dead = atoi(v);
    } else if (strcmp(k, "sbc_pedal_dead") == 0) {
        g_cfg.sbc_pedal_dead = atoi(v);
    } else if (strncmp(k, "sbc_cal_", 8) == 0 || strncmp(k, "sbc_dead_", 9) == 0 ||
               strcmp(k, "sbc_gear_map") == 0) {
        /* W128: Kalibrierung Original-Controller (input_sbc_usb.h) */
        {
            int scr = sbc_usb_cal_set(&g_cfg.sbc_cal, k, v);
            if (scr == SBC_CAL_NOTE) {
                /* W141: Zielhebel absolut - Mitte/Totzone ignoriert. */
                if (strncmp(k, "sbc_dead_aim_", 13) == 0) {
                    HLOG_I("cfg: %s=%s ignoriert (Zielhebel absolut: keine Totzone)", k, v);
                } else {
                    HLOG_I("cfg: %s=%s: Mitte ignoriert (Zielhebel absolut: linear lo..hi)", k, v);
                }
            } else if (scr != 1) {
                HLOG_W("cfg: %s=%s ungueltig - ignoriert", k, v);
            }
        }
    } else if (strcmp(k, "input_profile") == 0) {
        strncpy_s(g_cfg.input_profile, sizeof g_cfg.input_profile, v,
                  sizeof g_cfg.input_profile - 1);
    } else if (strcmp(k, "audio_live") == 0) {
        g_cfg.audio_live = atoi(v);
    } else if (strcmp(k, "audio_pan") == 0) {
        g_cfg.audio_pan = atoi(v);
    } else if (strcmp(k, "audio_freq") == 0) {
        g_cfg.audio_freq = atoi(v);
    } else if (strcmp(k, "audio_buflen") == 0) {
        g_cfg.audio_buflen = atoi(v);
    } else if (strcmp(k, "sbc_momentary") == 0) {
        g_cfg.sbc_momentary = atoi(v);
    } else if (strcmp(k, "ign_toggle_fix") == 0) {
        g_cfg.ign_toggle_fix = atoi(v);
    } else if (strcmp(k, "audio_caps_clamp") == 0) {
        g_cfg.audio_caps_clamp = atoi(v);
    } else if (strcmp(k, "vblank_qpc") == 0) {
        g_cfg.vblank_qpc = atoi(v);
    } else if (strcmp(k, "vblank_hz") == 0) {
        g_cfg.vblank_hz = atoi(v);
    } else if (strcmp(k, "movie_skip") == 0) {
        g_cfg.movie_skip = atoi(v);
    } else if (strcmp(k, "replay") == 0) {
        strncpy_s(g_cfg.replay, sizeof g_cfg.replay, v,
                  sizeof g_cfg.replay - 1);
    } else {
        HLOG_W("cfg: unbekannter Key '%s'", k);
    }
}

/* PARALLEL (31.10.): Overrides aus der Umgebung, damit mehrere Instanzen
 * mit derselben sb_native.cfg parallel laufen koennen (run_parallel.ps1):
 *   SB_CFG_SET  = "key=wert;key=wert" (beliebige cfg-Keys)
 *   SB_HDD_ROOT = eigener HDD-Ordner (Spielstand/ZCACHE pro Lauf)
 * SB_CORE (Xbox-Thread-Kern) wertet kernel_hle.c aus. */

/* Eine "key=wert;..."-Kette auf die cfg legen (W159: SB_CFG_SET und der
 * Start-Parameter --set teilen sich Parser + "\;"-Maskierung).
 * tag = Log-Herkunft ("SB_CFG_SET" bzw. "cmdline"). */
void hcfg_apply_set(const char *set, const char *tag)
{
    static char buf[8192];
    char *p, *ctx = NULL;
    if (!set || !*set) return;
    strncpy_s(buf, sizeof buf, set, sizeof buf - 1); /* Kette > 8K: kappen */
    /* A1: "\;" = maskiertes Semikolon fuer lange replay_steps-Ketten.
     * Einzelne Backslashes (Windows-Pfade) bleiben unveraendert. */
    for (p = buf; *p; p++) {
        if (p[0] == '\\' && p[1] == ';') {
            *p = '\x01';
            memmove(p + 1, p + 2, strlen(p + 2) + 1);
        }
    }
    for (p = strtok_s(buf, ";", &ctx); p; p = strtok_s(NULL, ";", &ctx)) {
        char *eq = strchr(p, '=');
        char *q;
        for (q = p; *q; q++) {
            if (*q == '\x01') *q = ';';
        }
        if (!eq) continue;
        *eq = '\0';
        set_key(p, eq + 1);
        HLOG_I("cfg: Override %s=%s (%s)", p, eq + 1, tag ? tag : "?");
    }
}

static void hcfg_apply_env(void)
{
    static char buf[8192];
    DWORD n = GetEnvironmentVariableA("SB_HDD_ROOT", g_cfg.hdd_root,
                                      sizeof g_cfg.hdd_root);
    if (n == 0 || n >= sizeof g_cfg.hdd_root) {
        if (n >= sizeof g_cfg.hdd_root) strcpy_s(g_cfg.hdd_root, sizeof g_cfg.hdd_root, "HDD");
    } else {
        HLOG_I("cfg: hdd_root aus SB_HDD_ROOT = %s", g_cfg.hdd_root);
    }
    n = GetEnvironmentVariableA("SB_CFG_SET", buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return;
    hcfg_apply_set(buf, "SB_CFG_SET");
}

/* W26 (Launcher, A10): eine INI-Datei parsen. missing_ok=1 -> fehlende
 * Datei ist kein Fehler (die Nutzer-cfg user.cfg ist optional).
 * Rueckgabe: 0 = geparst, 1 = fehlt. */
static int hcfg_parse_file(const char *path, int missing_ok)
{
    FILE *f;
    char line[4096];

    if (fopen_s(&f, path, "r") != 0 || !f) {
        if (!missing_ok) {
            HLOG_W("cfg: %s fehlt, nutze Defaults", path);
        }
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *k, *v, *end;
        k = line + strspn(line, " \t");
        if (*k == '#' || *k == ';' || *k == '[' || *k == '\n' || *k == '\0') {
            continue;
        }
        eq = strchr(k, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        v = eq + 1 + strspn(eq + 1, " \t");
        end = v + strcspn(v, "\r\n");
        while (end > v && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        *end = '\0';
        end = k + strlen(k);
        while (end > k && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        *end = '\0';
        set_key(k, v);
    }
    fclose(f);
    return 0;
}

static char g_user_cfg_path[1024]; /* W118: Spieler-cfg (Overlay-Speichern) */

const char *hcfg_user_cfg_path(void)
{
    return g_user_cfg_path;
}

int hcfg_load(const char *path)
{
    /* W26 (Launcher, A10): Ladereihenfolge sb_native.cfg -> user.cfg
     * (gleicher Ordner, optional, ueberschreibt) -> SB_CFG_SET/Umgebung.
     * Ohne user.cfg aendert sich nichts. */
    char user_path[1024];
    const char *sep1, *sep2;
    hcfg_parse_file(path, 0);
    sep1 = strrchr(path, '/');
    sep2 = strrchr(path, '\\');
    if (sep1 && sep2) {
        sep1 = (sep1 > sep2) ? sep1 : sep2;
    } else if (sep2) {
        sep1 = sep2;
    }
    if (sep1) {
        size_t dirlen = (size_t)(sep1 - path) + 1;
        if (dirlen + 8 < sizeof user_path) {
            memcpy(user_path, path, dirlen);
            strcpy_s(user_path + dirlen, sizeof user_path - dirlen, "user.cfg");
        } else {
            user_path[0] = '\0';
        }
    } else {
        strcpy_s(user_path, sizeof user_path, "user.cfg");
    }
    /* W150 USER-DATA: Profil/portabel -> user.cfg im Datenordner (legacy:
     * unveraendert neben sb_native.cfg). */
    {
        char ud[1024];
        if (sb_userdata_path(SB_UD_USERCFG, ud, sizeof ud) == 1) {
            strcpy_s(user_path, sizeof user_path, ud);
        }
        sb_userdata_path_peek(SB_UD_ROOT, ud, sizeof ud);
        HLOG_I("USERDATA: Modus %s (Quelle %s)%s%s",
               sb_userdata_mode_name(sb_userdata_mode()), sb_userdata_mode_source(),
               ud[0] ? ", Wurzel " : " (bisherige Orte)", ud);
    }
    /* W118 (Test): SB_USER_CFG = andere Spieler-cfg (run_once.ps1 -usercfg,
     * Overlay-Speichern -> naechster Start uebernimmt). */
    if (getenv("SB_USER_CFG") && getenv("SB_USER_CFG")[0])
        strcpy_s(user_path, sizeof user_path, getenv("SB_USER_CFG"));
    strcpy_s(g_user_cfg_path, sizeof g_user_cfg_path, user_path);
    /* Testlaeufe (run_once.ps1) setzen SB_NO_USERCFG=1: Spieler-Einstellungen
     * duerfen Tests nicht beeinflussen. */
    if (getenv("SB_NO_USERCFG"))
        user_path[0] = '\0';
    if (user_path[0] && hcfg_parse_file(user_path, 1) == 0) {
        HLOG_I("cfg: %s geladen (Nutzer-cfg, ueberschreibt)", user_path);
    }
    { /* W150 USER-DATA: Standardorte (nicht explizit gesetzte) in den
       * Datenordner; SB_HDD_ROOT/SB_CFG_SET (unten) gewinnen weiter. */
        char ud[1024];
        if (strcmp(g_cfg.hdd_root, "HDD") == 0 &&
            sb_userdata_path(SB_UD_HDD, ud, sizeof ud) == 1) {
            strcpy_s(g_cfg.hdd_root, sizeof g_cfg.hdd_root, ud);
            HLOG_I("USERDATA: hdd_root=%s", g_cfg.hdd_root);
        }
        if (strcmp(g_cfg.pack_list, "packs/packs.json") == 0 &&
            sb_userdata_path(SB_UD_PACKS, ud, sizeof ud) == 1) {
            _snprintf_s(g_cfg.pack_list, sizeof g_cfg.pack_list, _TRUNCATE, "%s\\packs.json", ud);
            HLOG_I("USERDATA: pack_list=%s", g_cfg.pack_list);
        }
    }
    hcfg_apply_env();
    HLOG_I("cfg: %s geladen (%ux%u, fps_limit=%d, vblank_hz=%d, fog=%.2f, drawdist=%.2f)",
           path, g_cfg.width, g_cfg.height, g_cfg.fps_limit, g_cfg.vblank_hz,
           g_cfg.fog_scale, g_cfg.draw_distance_scale);
    HLOG_I("cfg: fair_cs=%d thread_priorities=%d resume_yield_ms=%d "
           "tex_size_header=%d im_ring=%d safe_suspend=%d out=%s",
           g_cfg.fair_cs, g_cfg.thread_priorities,
           g_cfg.resume_yield_ms,
           g_cfg.tex_size_header, g_cfg.im_ring, g_cfg.safe_suspend,
           hcfg_out_dir());
    HLOG_I("cfg: LOADLOCK safe_suspend_sync=%d diag_lock_model=%d diag_lock_inject=%d hlock_spin=%d",
           g_cfg.safe_suspend_sync, g_cfg.diag_lock_model,
           g_cfg.diag_lock_inject, g_cfg.hlock_spin);
    HLOG_I("cfg: lang=%s", g_cfg.lang); /* W142: Spielertexte */
    return 0;
}

const char *hcfg_out_dir(void)
{
    static char dir[512];
    if (!dir[0]) {
        char shots[600];
        char *s;
        DWORD n = GetEnvironmentVariableA("SB_OUT_DIR", dir, sizeof dir);
        if (n == 0 || n >= sizeof dir) {
            /* W150 USER-DATA: Profil/portabel -> <Datenordner>\logs */
            if (sb_userdata_path(SB_UD_LOGS, dir, sizeof dir) != 1) {
                strcpy_s(dir, sizeof dir, "out");
            }
        }
        /* mkdir -p (runs/<tag> hat 2 Ebenen) */
        for (s = dir; *s; s++) {
            if (*s == '/' || *s == '\\') {
                char c = *s;
                *s = '\0';
                CreateDirectoryA(dir, NULL);
                *s = c;
            }
        }
        CreateDirectoryA(dir, NULL);
        _snprintf_s(shots, sizeof shots, _TRUNCATE, "%s/shots", dir);
        CreateDirectoryA(shots, NULL);
    }
    return dir;
}
