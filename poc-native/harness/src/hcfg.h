#pragma once
/* hcfg.h - sb_native.cfg: Tuning von Aufloesung, FPS, Sichtweite etc. */
#include "input_sbc_usb.h" /* W128: sbc_cal_t (Kalibrierung Original-Controller) */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char xbe_path[512];
    char media_root[512];
    char hdd_root[512]; /* Partition1 (E:, beschreibbar): TDATA/UDATA */
    char game_image[512]; /* W150 ISO-DIRECT: Spiel-Abbild (XISO/Redump), Vorrang vor media_root/xbe_path; leer = Ordner-Modus */
    int version_check;    /* W150/W202: 1 = unbekannte XBE ablehnen (Standard), 0 = nur warnen (Experten) */
    int allow_ntsc_u;     /* W229 [expert]: seit W230 ohne Wirkung (US startet automatisch, s. ntsc_u); wird weiter eingelesen */
    int ntsc_u;           /* W230 [expert]: 1 = USA-Fassung (NTSC-U) mit US-Profil starten (Standard), 0 = Notbremse (abgelehnt wie unbekannte Fassung) */
    char hdd_root_us[512]; /* W230: Spielstandordner der US-Fassung; leer = <hdd_root>_US (gleiche Title-ID wie EU, nie mischen) */
    int iso_selftest;     /* W150 DIAG: 1 = D:-Selbsttest (Lesen, Schreibschutz) vor dem Spielstart */
    int save_check;       /* W187 KIA-LOAD: Spielstaende (T:) beim Start pruefen: 1 = reparieren (mit Sicherung), 2 = nur melden, 0 = aus */
    int save_lock;        /* W190 SAVE-LOCK: 1 = Spielstaende schuetzen (T:/U: in eine Wegwerf-Arbeitskopie <hdd>\_savelock, beim naechsten Start verworfen), 0 = Original (Standard) */
    int savelock_selftest; /* W190 DIAG: 1 = Selbsttest Pilot-Loeschen/death00 ueber die Nt*File-Wege vor dem Spiel */
    int xlaunch_self;     /* W186 RESET-TITLE: XLaunchNewImage auf D:\default.xbe: 1 = Client-Neustart, 2 = sauberes Ende, 0 = ignorieren (alt) */
    int xlaunch_replay;   /* W186: 1 = Neustart-Kind spielt replay_steps/replay erneut ab, 0 = Kind ohne Replay (Standard) */
    /* [display] */
    unsigned width, height;
    int present_interval; /* 0 = uncapped, 1 = vsync */
    int window_hidden;    /* 1 = Fenster ausserhalb aller Monitore, kein Fokus/Taskleiste (Testlaeufe) */
    int ui_thread;        /* 1 = Fenster gehoert eigenem UI-Thread (erzeugt+pumpt), 0 = alt */
    /* W81 WINMODE (render_winmode.c) */
    int window_mode;      /* 0 = Fenster (bisher), 1 = randloses Vollbild (Monitor des Fensters), 2 = exklusives Vollbild */
    unsigned fs_width, fs_height; /* exklusiv: Modusgroesse (0 = Desktop); Trockenlauf randlos: simulierter Monitor */
    int fs_hz;            /* exklusiv: Bildwiederholrate (0 = Desktop) */
    int fs_monitor;       /* 0 = Monitor des Fensters, N = N-ter DXGI-Ausgang (1-basiert) */
    int alt_enter;        /* 1 = Alt+Enter schaltet Fenster <-> Vollbild (eigene Logik), 0 = alt (DXGI-Standard) */
    int alt_enter_mode;   /* Vollbild-Art fuer Alt+Enter bei window_mode=0: 1 = randlos, 2 = exklusiv */
    int win_resize;       /* 1 = Backbuffer folgt der Fenstergroesse (nie strecken), 0 = alt (DXGI streckt) */
    int dpi_aware;        /* -1 = auto (Per-Monitor-DPI nur bei window_mode>0), 0 = aus (bisher), 1 = an */
    int fs_interp_match;  /* 1 = exklusiv: interp_fps = Bildwiederholrate (60..240), 0 = interp_fps bleibt */
    int winmode_test;     /* Test: 1 = Trockenlauf (Fenster ausserhalb des Bildschirms, kein SetFullscreenState); window_hidden=1 erzwingt das */
    int winmode_alt_every; /* Test (nur Trockenlauf): alle N Presents Alt+Enter/Fokus per PostMessage, 0 = aus */
    int winmode_alt_from; /* Test: erster Schritt ab Present N (0 = winmode_alt_every; <0 = |N| Presents nach Missionsbeginn, erste Interp-Aufzeichnung) */
    int winmode_alt_max;  /* Test: Anzahl Schritte (Default 8 = 2 Zyklen Alt+Enter, Fokus weg, Fokus da, Alt+Enter) */
    int fps_limit;        /* 0 = aus */
    int turbo_until_frame; /* fps_limit erst ab Frame N (Menues im Zeitraffer), 0 = aus */
    int turbo_until_mode; /* fps_limit erst ab Ladezaehler N (u32 0x2EEA34, W232: kein Spielmodus), 0 = aus */
    int vblank_qpc; /* W69: 1 = VBlank-Zaehler aus QPC statt GetTickCount (exakte 16,7-ms-Raster statt 15,6-ms-Stufen), 0 = alt */
    int film_diag; /* TEST(PAL50,W234): 1 = FILMDIAG-Zeilen (Filmbeginn/-ende, Bink-Bilder, Wanddauer), Standard 0 */
    int game_tick; /* W234 PAL50: Spieltakt 30 (Standard, wie PAL60/NTSC) oder 25 (PAL50: 50 VBlanks/s + PAL-EEPROM -> Spiel waehlt 50-Hz-Pfad) */
    int vblank_hz; /* 0 = aus (VBlank-Waits sofort, Testtakt via run_once.ps1), sonst Ticks/s (Xbox: 60 = Echtzeit, Default) */
    /* [render] - greifen an den D3D-Hooks (M2) */
    float fog_scale;
    float draw_distance_scale;
    /* [log] */
    int level;
    int per_call;
    int syscall_trace; /* 1 = jeden Kernel-Call loggen (Diagnose, langsam) */
    int hle_arity_check; /* DIAG(ARITY,W162): 1 = HLE-Arity-Selbsttest (Ord 87/203 durch echte Thunk-Slots, ESP-Pruefung) + Aufruf-Zaehler */
    int hlog_keep;     /* 1 = zweites hlog_init trunkiert nicht (cfg-Zeilen bleiben), 0 = alt */
    int diag_states; /* DIAG(STATES): Modus/Szene/Missionsflag bei Aenderung + Heartbeat (A3) */
    int state_diag;  /* DIAG(STATE): kompakte Missions-Zustandszeile 1x/s (A11c) */
    int state_every; /* DIAG(STATE)-Intervall in ms (<=0 = 1000) */
    int startup_diag; /* DIAG(STARTUP): Balken/Linie/Stufe/Engine pro Intervall (W21-R2) */
    int combat_diag; /* DIAG(COMBAT): Entity-HP/Schaden pro Intervall (W35) */
    int campaign_diag; /* DIAG(CAMPAIGN): Missionsziel/Phase/Ergebnis/Tutorial-Regie (W63) */
    int gear_diag; /* DIAG(GEAR): Block-Gang/Out+0xB/Mapper-Latch/Position pro Intervall (W141) */
    int aimat_antiwindup; /* W63: aimat-Schirmregler mit Anti-Windup (1) bzw. alt (0) */
    int poll_census; /* DIAG(POLLCENSUS,W59): 48CF0-Aufrufer-Zensus (RA/Port/Gruppe/Button, Treffer != 0) */
    int nullcall_diag; /* DIAG(NULLCALL): Sprung auf 0: Aufrufer+SEH je einmal, 0 = alter VEH-Dump */
    int contig_fast; /* PERF(PERF-HUD,W122): 1 = low_alloc (MmAllocateContiguousMemory) ueberspringt belegte Allokationen am Stueck (gleiche Adresse, viel weniger VirtualQuery), 2 = Pruefmodus (alte Suche vergleicht, Zeile LOWALLOC), 3 = EXPERIMENT Merkpunkt (nicht exakt), 0 = alt */
    int mem_wc_strip; /* PERF(FILM-FPS,W123): 1 = PAGE_WRITECOMBINE/PAGE_NOCACHE der Xbox-Speicheranforderungen (MmAllocateContiguousMemoryEx, NtAllocateVirtualMemory, MmSetAddressProtect) beim Host-Mapping weglassen (CPU-Lesen aus WC-Speicher ist ungecacht: Film-Textur-Hashes ~75 ms/Bild), 0 = alt */
    int contig_diag; /* DIAG(PHYSALIAS,W86): 1 = Contig-Blockliste (alle MmContig(Ex) Block + D3D TexLock neu + PHYSALIAS), 0 = aus (nur PHYSALIAS, max 16) */
    char watch[256]; /* WATCHDR (W41): DR0-3-Watchpoints "ADDR[:len][:w|rw],..." oder "ent:IDX+OFF...", leer = aus */
    /* [kernel] */
    int fair_cs; /* 1 = Xbox/NT5-CS (Uebergabe an Wartenden), 0 = Host-CS */
    int thread_priorities; /* 1 = KeSetBasePriorityThread -> SetThreadPriority */
    int resume_yield_ms;   /* Sleep nach NtResumeThread (Hack), 0 = aus */
    int safe_suspend;      /* 1 = NtSuspendThread laesst kein Ziel suspendiert, das eine Host-Sperre haelt */
    int safe_suspend_sync; /* FIX(LOADLOCK,W119): 2 = SUSPMARK (Ziel vor SuspendThread markieren, gibt danach gegriffene Sperre zurueck), 1 = GetThreadContext nach SuspendThread (teuer), 0 = alt */
    int diag_lock_model;   /* DIAG(LOADLOCK,W119): 0 = hlock mit atomarem Owner (Fix), 1 = ntdll-Nachbildung (Owner getrennt gesetzt, nur Nachweis) */
    int hlock_spin;        /* LOADLOCK(W119): Spin-Runden (pause) vor WaitOnAddress bei belegter hlock, 0 = 64 */
    int diag_lock_inject;  /* DIAG(LOADLOCK,W119): >0 = jedes N-te hlock-Erwerb/-Freigabe Sleep(1) im Fenster (Modell 1) bzw. in der Region (Modell 0), 0 = aus */
    int contig_alias_test; /* TEST(FILM-WHITE,W177): >0 = Contig-Anforderung genau dieser Groesse bekommt vorher einen 12-KB-Koeder mit gleichem Low28-Anfang (Lage V_C30isokill nachstellen), 0 = aus */
    int contig_reuse;      /* FIX(SEQ-RENDER,W214): 1 = freigegebene Contig-Bloecke (contig_free) nach contig_reuse_ms Quarantaene fuer passende Anforderungen wiederverwenden (Adressraum/Commit wachsen nicht je Mission/Effekt), 0 = immer neu */
    int contig_reuse_ms;   /* W214: Quarantaene seit der Freigabe in ms (Default 2000) */
    int contig_free;       /* FIX(SEQ-RENDER,W214): 1 = MmFreeContiguousMemory gibt das Low28-Bild des Blocks frei (Speicher bleibt), sonst ist das 256-MB-Bild nach einigen Missionen voll -> Ueberlappung -> Texturen weiss; nur mit contig_noalias=1, 0 = alt */
    int contig_noalias;    /* FIX(FILM-WHITE,W177): 1 = Contig-Bloecke ohne Low28-Ueberlappung (eigene Contig-Tabelle, HIGH-Bloecke gezielt platziert, low_alloc-Treffer auf HIGH-Bilder stillgelegt) -> Phys->Virt eindeutig, 0 = alt */
    int phys_exact;        /* FIX(PHYSALIAS,W73): 1 = Phys->Virt bevorzugt den Contig-Block, der exakt an der Adresse beginnt (Low28-Mehrdeutigkeit, Absturz 0x24E1D3), 0 = alt (erster Treffer) */
    int hdr_hybrid;        /* FIX(HDRSWAP,W94): 1 = Hybrid-Header an 0x10000 (MZ+e_lfanew auf PE-Kopie hinter dem XBE-Header), krnl_CreateThread tauscht nicht mehr; 0 = alt (Header-Tausch je CreateThread, Race) */
    int hdr_race_test;     /* DIAG(HDRSWAP,W94): >0 = Stress-Thread erzeugt in den ersten N s laufend Leer-Threads per krnl_CreateThread (Race-Reproduktion), 0 = aus */
    /* main_sbcore revertiert (W22-A/B: traf nur den idlen Loader-Thread, 16,1 vs 15,6/s) */
    /* [render] */
    int tex_size_header;   /* 1 = Linear-Textur-Masse aus Xbox-Size-Feld */
    int im_ring;           /* 1 = IM-Vertexbuffer als Ring (NO_OVERWRITE) */
    int res_refcount;      /* 1 = D3DResource_Release zaehlt Refcount (Xbox) */
    int fatx_stale_extend; /* W63: 1 = Datei-Verlaengerung nach Ueberschreiben fuellt mit altem Inhalt (FATX), 0 = Nullen (alt) */
    int xbox_blend;        /* 1 = Blend aus NV2A-Methoden (SetRenderState_Simple) */
    int ctx_lock;          /* 1 = Immediate-Context-Aufrufe per Lock serialisieren, 0 = alt */
    int tex_evict;         /* 1 = Cache-Eintrag bei Release rc=0 entfernen (TEXLIFE) */
    int tex_cache_max;     /* W63: Textur-Cache-Kapazitaet (Default 2048, 256 = alt) */
    int tex_cache_lru;     /* W63: 1 = Cache voll -> laengst unbenutzten Eintrag freigeben, 0 = alt (NULL) */
    int tex_hdr_check;     /* W63: 1 = Cache-Treffer prueft Texturkopf (Data/Format) auf Neubelegung, 0 = alt */
    int tex_upload_guard;  /* 1 = Upload-Quelle voll per VirtualQuery pruefen (TEXLIFE) */
    /* [input] */
    char replay[4096];   /* "pkt:vk,len;..." (M2b-Test, 0 = aus) */
    /* A4: zustandsgesteuerte Schritte "trigger>vk,len;..." (s. BUILD.md):
     * f+N | file:NAME[+N] | mem:0xADDR=0xVAL[+N]. Leer = aus. */
    char replay_steps[4096];
    /* A8: OUTWRITE-Hack (Buttons direkt nach Out, d3d11_render.c) */
    int outwrite;       /* 1 = aktiv (wie bisher), 0 = aus (nur Mapper) */
    int outwrite_frame; /* OUTWRITE erst ab Present-Frame N */
    /* A8: Maus (nur aktiv, wenn KEIN Replay laeuft; s. BUTTONMAP.md) */
    int mouse_aim;      /* 1 = Maus-Analog an (relativ, Mitte 0x7F/0) */
    int mouse_sight;    /* 1 = Ziel ist Sight statt Aiming */
    int mouse_sens;     /* Empfindlichkeit: Akku += dx*sens/4 */
    int mouse_invert_x; /* 1 = X-Achse invertiert */
    int mouse_invert_y; /* 1 = Y-Achse invertiert */
    int mouse_buttons;  /* 1 = Maustasten L/R/M -> Haupt/Sub-Waffe, Lock-On */
    int input_diag;     /* DIAG(INPUT): SBC/Out/Flags bei Aenderung + Heartbeat */
    int mouse_capture_outside_mission; /* W196: 0 = visible/free outside mission */
    int mouse_capture;    /* W79: 1 = Maus fangen (Raw Input, Cursor versteckt + aufs Fenster begrenzt, F10 = freigeben/fangen), 0 = alt (GetCursorPos) */
    int input_focus_only; /* W79: 1 = echte Tastatur/Maus nur bei Fokus des Client-Fensters, 0 = alt (global) */
    int input_source;     /* W79: 1 = echte Tasten/Maustasten aus Fenster-Nachrichten (nur mit Fokus), 0 = alt (GetAsyncKeyState, global) */
    int input_test_hook;  /* W79 (Test): 1 = WM_APP+0x7A/0x7B (Fokus/Mausdelta per PostMessage) annehmen, 0 = aus */
    int key_toggles;      /* W79: 1 = Tasten 1-5 rasten die 5 Kippschalter (bi34-38) ein/aus (nur ohne Replay), 0 = alt */
    int mouse_mode;       /* W87: 1 = absolut (Fadenkreuz folgt der Maus ohne Spielglaettung, Vorsteuerung auf [0x356D08]/[0x3521BC]), 0 = relativ (alt) */
    int startseq_shots;   /* W87 DIAG(STARTSEQ): 1 = Client-Bild bei jedem Startsequenz-Wechsel + alle 300 Frames bis VT online (braucht input_diag=1), 0 = aus */
    int toggle_assist;    /* W87: 1 = Druck 1-5 in der Schalterphase auf einen schon eingerasteten, vom Spiel nicht erkannten Kippschalter erzeugt die Flanke neu (bleibt AN), nur mit key_toggles=1; 0 = reine Rast (W79) */
    int key_tap_queue;    /* W92: 1 = schnelle Tipper je Taste anstauen (mind. 1 Poll an + 1 Poll Luecke, Deckel 8; nur Nachrichtenpfad input_source=1), 0 = alt (Tipper zwischen Polls verschmelzen) */
    int aim_diag;         /* W87 DIAG(AIMSTEP): 1 = Zielkette pro Frame loggen (nach Hebel-/Mausaenderung, Mission), 0 = aus */
    int sbc_usb; /* W75: 1 = Original-Controller lesen (USB-Geraet bzw. sbc_fixture), 0 = Tastatur/Maus (Default) */
    int sbc_axis_arb;     /* W128: 1 = Achsen SBC gegen Maus "zuletzt aktives Geraet gewinnt" (nur sbc_usb=1), 0 = alt W75 (USB nur bei neutraler Achse) */
    int sbc_mouse_off;    /* W128: 1 = Maus-Achsen aus, solange der Original-Controller aktiv ist (Default), 0 = Maus + Controller per Arbitrierung */
    int sbc_mouse_buttons;/* W128: mit sbc_mouse_off=1: 1 = Maustasten bleiben (Default), 0 = auch Maustasten aus */
    int sbc_arb_dead;     /* W128: SBC-Bewegung > N Spiel-Einheiten holt die Achse (Default 8) */
    int sbc_mouse_wake;   /* W128: Mausbewegung >= N Hebelstufen holt die Achse zurueck (Default 3) */
    int sbc_autocal;      /* W128 (Key sbc_cal): 1 = Ruhelage beim Start erfassen (Mitte/Pedal-Null + Totzone), 0 = roh wie das Original (nur cfg-Werte) */
    int sbc_axis_dead;    /* W128: Auto-Kalibrierung: Totzone zentrierter Achsen (Spiel-Einheiten, Default 6) */
    int sbc_pedal_dead;   /* W128: Auto-Kalibrierung: Totzone ueber der Pedal-Ruhe (Default 16) */
    sbc_cal_t sbc_cal; /* W128: sbc_cal_<achse>/sbc_dead_<achse>/sbc_gear_map (nur mit sbc_usb=1), ungesetzt = Rohwerte (Default) */
    char sbc_fixture[512]; /* W75: Fixture-Datei mit Reports statt USB-Geraet (nur mit sbc_usb=1), leer = aus (Default) */
    char input_profile[64]; /* W76: Eingabe-Profil (profiles/<name>.ini), leer = Legacy-Belegung (Default) */
    int movie_skip;     /* W14: Briefing-Film per 'L' ueberspringen (Testpfad); W63: 2 = jeden Film */
    /* [capture] */
    int screenshot_frame; /* 0 = aus, sonst BMP bei Frame N */
    int screenshot_every; /* 0 = 3 Shots (N/N+500/N+1000), sonst Intervall */
    int screenshot_max;   /* Deckel fuer screenshot_every-Shots (Default 400) */
    int screenshot_jpg;   /* 1 = JPG (WIC, ~0,4 MB), 0 = BMP (~11 MB) */
    int memscan_every;    /* DIAG(MEMSCAN): .data-Snapshot alle N Frames, 0 = aus */
    /* [audio] */
    int audio;          /* 1 = DirectSound-HLE (Mixer + Live-Ton), 0 = stumm (alt) */
    int audio_capture;  /* 1 = <out>/audio.wav schreiben (nur bei audio=1) */
    int hrtf_ptr; /* NULLCALL: HRTF-Zeiger vorbelegen (wie UseFullHRTF), 0 = aus */
    int audio_live;     /* 0 = kein Live-Ton, 1 = WASAPI (neu), 2 = WinMM (alt) */
    int audio_pan;      /* 1 = MixBin-Pan + Headroom auswerten, 0 = alt */
    int audio_freq;     /* 1 = SetFrequency/SetPitch wirken auf Puffer, 0 = alt */
    int audio_buflen;   /* 1 = Lock-Groesse aus dwBufferBytes (Fix Heap-Tod), 0 = alt */
    int sbc_momentary; /* Master: 1 = Luke/Zuendung als Taster (echter Controller), 0 = alt (dauerhaftes Toggle-Bit) */
    int ign_toggle_fix; /* W52: 1 = Gate Bit 2 in Mission setzen (HLE-Ersatz, Toggle 0x376BE lebt), 0 = Original (Toggle tot) */
    int audio_caps_clamp; /* W24: 1 = GetCaps-dwSize auf 12 (Xbox-DSOUNDCAPS) klemmen (Fix Missionsende-Nullcall), 0 = alt */
    int audio_setbuf_len; /* W22: 1 = SetBufferData setzt buflen (Bink-Ring statt Lazy-64K) */
    int hlog_async;        /* PERF(PERF-TICK,W168): 1 = Log-Zeilen ueber Schreib-Thread (kein WriteFile im Spiel-Thread) */
    int audio_decode_par;  /* PERF(PERF-TICK,W168): N > 1 = grosse SetBufferData-Puffer auf N Threadpool-Teile (bitgleich) */
    int audio_mix_yield;   /* PERF(PERF-TICK,W168): 1 = Mischer gibt au_lock je Stimme ab, wenn ein anderer Thread wartet */
    int audio_lock_diag;   /* DIAG(PERF-TICK,W168): 1 = Zeile AUDIOLOCK (Haltezeit Mischer, Warten anderer Threads) */
    int audio_setbuf_fast; /* PERF(PERF-TICK,W168): 1 = SetBufferData dekodiert direkt in den Stimmenpuffer (gleiche Werte, ohne FIFO-Umweg) */
    int seek_cache;        /* W22: 1 = redundante Seeks cachen (t2-Spin), 0 = alt */
    /* (audio_pause_cap revertiert: wirkungslos, s. BERICHT W22) */
    int bink_pause; /* W192: pausierbarer XAPI-TSC, Standard 1 */
    int pause_diag; /* W192: Film-Bildnummern bei Pause, Standard 0 */
    int tsc_freq;          /* W22: 1 = Host-TSC-Frequenz melden (RDTSC/733-Nutzer 1x), 0 = alt (733 MHz, ~4,8x) */
    int audio_ring_clip;   /* W22: 1 = Unlock nur ab Ring-Frontier appenden (Bink-Ueberlapp nicht doppelt), 0 = alt (alles, 6 % Ueberlauf) */
    int pad_replay_gate;   /* W83: 1 = echtes Pad bei aktivem Replay ignorieren (wie Tastatur/Maus), 0 = alt (Pad wirkt immer) */
    int pad_axis_arb;    /* W132: 1 = Achsen Pad gegen Maus "zuletzt aktives Geraet gewinnt" (Default), 0 = alt (betragsgroesste Auslenkung gewinnt) */
    int pad_arb_dead;    /* W132: Pad-Bewegung > N Spiel-Einheiten holt die Achse (Default 8) */
    int pad_mouse_wake;  /* W132: Mausbewegung >= N Hebelstufen holt die Achse zurueck (Default 3) */
    int pad_mouse_off;   /* W132: 1 = Maus-Achsen aus, solange ein Gamepad aktiv ist (Maustasten bleiben), 0 = parallel per Arbitrierung (Default) */
    int pad_dz_stick;    /* W143: globale Totzone Sticks in % (0..90, Default 0 = Profilwerte); wirkt als Untergrenze (max) */
    int pad_dz_trig;     /* W143: globale Totzone Trigger in % (0..90, Default 0 = Profilwerte); wirkt als Untergrenze (max) */
    /* W218 SIMPLE-CONTROLS (nur Maus/Tastatur/Gamepad, nie Original-Controller; Standard aus = bitgleich) */
    int aim_assist;      /* W236: 0 = Original, 1 = Fadenkreuz am Rand dreht die Sicht, 2 = Waffenarm fest (Sicht folgt dem Arm). Altwert aim_turn=1 -> 1 */
    int aim_assist_edge; /* W236: Rand fuer Stufe 1 in % der halben Bildbreite/-hoehe (30..100, Default 80) */
    int auto_gear;       /* 1 = automatischer Gangwechsel (Gas: hoch/runter nach Geschwindigkeit, Bremse im Stand = R) */
    int audio_ring_overwrite; /* W83: 1 = Ring-Ueberlapp: spaetere Schreibung ersetzt noch nicht gespielte FIFO-Frames (Xbox-Ring), 0 = alt (Clip: erste gewinnt) */
    int audio_ithis_fix;   /* W89: 1 = Puffer-Stop/GetStatus an den I-Wrappern 0x21F53C/0x21F598 hooken (diese rufen C-Ebene mit this-0x1C -> Stimme nie erkannt: Stop wirkungslos, Status immer STOPPED = Stimmenklau), 0 = alt */
    int audio_loop_fix;    /* W89: 1 = SetLoopRegion legt nur die Region fest (Schleife nur mit Play(LOOPING)), Region/Position in Daten-Frames (Resampling), Einmal-Ton bis Datenende; 0 = alt (jede Region loopt, Quell-Frames) */
    int audio_pitch_range; /* W89: 1 = SetPitch-Klemme weit (-98304..32767; Spiel nutzt -8576 = 11,3 kHz), 0 = alt (-8192..8192) */
    int audio_adpcm_lerp;  /* W89: 1 = ADPCM-Resampling linear interpoliert, 0 = alt (naechster Nachbar) */
    int audio_3d;          /* W89: 1 = DS3D-Distanzdaempfung (min/max/rolloff) + Balance aus Listener-Raum fuer positionierte Stimmen, 0 = alt (3D-Setter ignoriert, alles voll + mittig) */
    int audio_oneshot_fix; /* W95: 1 = Loop-Region-Wrap im Mixer nur bei Schleife (Play LOOPING), Einmal-Ton endet am Datenende; 0 = alt (jede gesetzte Loop-Region wrappt -> Einmal-Toene enden nie) */
    int audio_filter;      /* W102: 1 = Voice-Filter DLS2 (SetFilter Mode 1: resonanter 2-Pol-Tiefpass, Chamberlin-SVF wie MCPX) im Mixer, 0 = aus (alt: SetFilter ignoriert) */
    int audio_level_diag;  /* W102: 1 = DIAG(LVL) Pegel je Stimme und Sekunde -> <out>/voice_levels.csv + DIAG(LVLT)-Rangliste (Laufzeit x Pegel) alle 30 s, 0 = aus */
    int audio_voice_diag;  /* W95: DIAG(SFXN) aktive Puffer-Stimmen je Sekunde (Zahl/Loop/stumm/Alter + aelteste), 0 = aus */
    int audio_sfx_diag;    /* W89: DIAG(SFX) Ereignis-Spur der Puffer-Stimmen (ohne Bink), Wert = max. Zeilen, 0 = aus */
    int audio_wrap_diag;   /* W83: DIAG(BINKWRAP) Ring-Wrap-Unlocks (Lock-Puffer vs. SetBufferData-Speicher), 0 = aus */
    int audio_sync_diag;   /* W205: 1=ring/frame/device trace, 2=also mute device only, 0=off */
    int audio_bink_sync;   /* W205: overwrite the initial Bink ring before Play */
    int audio_bink_delay;  /* W205: align the movie start with device output */
    int audio_low_latency; /* W205: WASAPI short queue, default 1 */
    int audio_process_async; /* W34: 1 = Process async (PENDING + Echtzeit-Vollzug, Xbox), 0 = alt (sync + Sleep(10)x50-Backpressure -> 5,1-s-Ladepause) */
    int tex_swz06; /* FIX(W38): swizzled 32-Bit (Code 0x06, z.B. 128x128 Wash-Textur) per dokumentiertem NV2A-Morton-Swizzle laden (0=aus, 1=an, 2=an+Einmal-Dump) */
    int lin_tex_rect; /* A2: Lineartextur = Texel-Koords, /(w,h) auch ohne RHW */
    int lin_rhw_range; /* W2B-FIX: linearer VB-Quad/Strip in Pixel-Range = RHW (kein 3D-MVP) */
    int edge_mode; /* W107 EDGE-MODE: Bildraender ausserhalb der 4:3-Box im Breitbild: 0 = aus (volles Hor+), 1 = harter Schnitt (schwarz), 2 = Abdunklung (Verlauf ins Schwarze) */
    int edge_fade_width; /* W107: Verlaufsbreite in % der 4:3-Boxbreite (edge_mode=2), 0 = harte Kante */
    int edge_fade_strength; /* W107: Endabdunklung in % (100 = schwarz, edge_mode=2) */
    int edge_ab; /* DIAG(EDGE-MODE,W107): 1 = zu jedem Screenshot Varianten _e1/_e2 aus demselben Bild */
    int cockpit_persp;      /* W191 UW-PERSP: Cockpit-Perspektive (nur Desktop, Cockpit-Ansicht der Mission, in VR aus), 0 = Original (aus), 1..100 = Staerke in % von cockpit_persp_dist; Dolly-Zoom um die Ebene cockpit_persp_ref, nie gestreckt */
    int cockpit_panini;     /* W191 UW-PERSP: Panini-Projektion der Cockpit-Ansicht (Nachbearbeitung, nur Desktop), 0 = aus, 1..100 = Staerke in % von cockpit_panini_d; Mitte formtreu vergroessert, Raender waagerecht verdichtet */
    int cockpit_panini_d;   /* W191: groesstes Panini-d (x100); mit cockpit_panini_frame=1 begrenzt der Rahmen frueher */
    int cockpit_panini_frame; /* W191: 1 = Radar oben und Frontview unten bleiben voll im Bild (100 % = Grenze ohne Anschnitt), 0 = d = cockpit_panini % x cockpit_panini_d (Experiment) */
    int cockpit_width;      /* W235 COCKPIT-WIDTH: leichte waagerechte Streckung der Cockpit-Ansicht um die Bildmitte (Nachbearbeitung im Panini-Pass, nur Desktop, in VR aus), 0 = Original (aus), 1..10 = Breite + n %; Nutzer-Option, sonst gilt: nie strecken */
    int cockpit_persp_pause; /* W209: 1 = live camera in pause, 0 = legacy fallback */
    int cockpit_persp_dist; /* W191: Kamera-Weg bei 100 % in mm (Spiel-Einheit ~ 1 m): > 0 = zurueck + engeres Sichtfeld (Tele, Standard 1500; Near-Ebene am alten Auge), < 0 = vor + weiteres Sichtfeld (Breitbildrand fuellen, Experiment) */
    int cockpit_persp_ref;  /* W191: Abstand der festen Ebene (Hauptmonitor/HUD) in mm */
    int cockpit_zoom;       /* W191: zusaetzlicher gleichmaessiger Zoom der Cockpit-Ansicht in % (100 = aus; > 100 schneidet oben/unten ab) */
    char cockpit_persp_cycle[160]; /* DIAG(UW-PERSP,W191): Liste 'p[/z[/n[/w]]],...' (Prozent Vorschub/Zoom/Panini, W235: Breite), je Spiel-Bild der naechste Eintrag (Bildpaare aus Folgebildern), leer = aus */
    char cockpit_persp_shots[64]; /* DIAG(UW-PERSP,W191): Liste 'F1,F2,..' - nach F Bildern Cockpit-Ansicht (Modus 4; -F = auch Titel-Demo) je 2x Zyklus Folgebilder als shots/cpersp_<f>_p<vorschub>_z<zoom>.jpg */
    int cockpit_persp_diag; /* DIAG(UW-PERSP,W191): 1 = Tiefen-/Kanten-Statistik des Cockpit-Meshs (CPERSP mesh) bei jeder Aenderung, 2 = + je Draw-Klasse Zaehler */
    int aspect_mode; /* W18: 1 = 2D in Spiel-Aspect-Box (Pillarbox, schwarze Balken) + 3D Hor+, 0 = alt (2D gestreckt) */
    int rtt; /* W23: 1 = Render-to-Texture umleiten (RTV+SRV je Surface) + sampeln, 0 = nur notieren */
    int rtt_readback; /* FIX(COMBAT,W59): 1 = LIN_X8R8G8B8-RTs bis 128x128 (Zellenkarte [0x35149C], Lock-On) nach jeder Session GPU->CPU in den Xbox-Speicher, 2 = + LIN_A8R8G8B8 bis 128x128, 3 = beide bis 512x512, 9 = DIAG ohne Schreiben, 0 = alt (GPU-only) */
    int rtt_rb_sync; /* FIX(CULL-POPIN,W215): Standard 2. 1 = Zellenkarte (X8R8G8B8 bis 128x128) im selben Spiel-Bild zurueckschreiben (Kopie am Session-Ende, Abholung am BlockOnTime/BlockOnFence des Spiels), 2 = Zellenkarte auf der CPU rastern (kein GPU-Warten, Rueckfall auf 1 bei unbekannten Draws), 0 = alt (asynchron, >= 1 Session alt -> Gelaende fuer aeltere Kamera ausgewaehlt) */
    int rtt_rb_cpu_ofs; /* W215: Versatz der CPU-Rasterung (rtt_rb_sync=2) in 1/256 Pixel auf die Xbox-Pixelkoordinaten (0 = Pixelmitte bei x+0,5) */
    int rtt_rb_diag; /* DIAG(CULL-POPIN,W215): N > 0 = alle N Zellenkarten-Sessions Zeile RTTRB (Alter, fehlende Zellen gegen frischen Inhalt, Wartezeit); 1 = 300; blockierender Zusatz-Readback */
    int rtt_stale; /* FIX(MAP-RETRY,W124): 1 = veraltete RT-Eintraege (Textur freigegeben, neue RT auf derselben Datenadresse, Surface mit neuer Datenadresse) beim Sampeln ueberspringen, unter mehreren Treffern den zuletzt gerenderten nehmen, volle Tabelle verdraengt zuerst veraltete; 0 = alt (erster Daten-Treffer, nach Missions-Neustart sampelten Texturen alte RT-Inhalte) */
    int rtt_selfcopy; /* FIX(FX-HOLE,W106): 1 = Draws, die ihr eigenes RT sampeln, zeichnen ins RT und lesen eine Kopie (Xbox: Lesen+Schreiben derselben Flaeche); 0 = alt (Umbinden auf den Backbuffer, Rauch-Hitzeflimmern stanzt Loecher ins Cockpit) */
    int rtt_selfclip; /* FIX(SIDESMEAR,W45): 1 = Draws, die ihr eigenes RT sampeln, per Scissor auf Aspect-Box clippen (Seiten schwarz, Mitte exakt wie bisher), 0 = alt (Vollbild-Schmier) */
    int m3d_census; /* DIAG(MISSION3D,W25): Missions-Zensus alle N Frames (Szene 0x2C, mloop=1), 0 = aus */
    int sbar_diag; /* DIAG(STARTSEQ-BARS,W98): 1 = im Zensus-Frame jede neue Stage-0-Textur als shots/rqtex_<xptr>_{rgb,a}.bmp (max 64/Lauf) */
    unsigned sbar_skip_xp; /* DIAG(STARTSEQ-BARS,W98): != 0 = Draws mit dieser Stage-0-Textur (xptr) in der Mission verwerfen (A/B-Bild) */
    int sbar_probe[4]; /* DIAG(STARTSEQ-BARS,W98): BB-Pixel x1:y1:x2:y2, im Zensus-Frame nach jedem BB-Draw lesen, Aenderung loggen (SBAR px) */
    int m3d_census_skip; /* DIAG(STARTSEQ-BARS,W98): erste N Draws des Zensus-Frames nur zaehlen, nicht loggen (Draws hinter der 1600er-Kappe sehen) */
    int m3d_census_gate; /* DIAG(MONITORVIEW,W36): 1 = Missions-Zensus erst ab VT-online ([0x53831C]==1, nach START), 2 = nur Modus 4 (W101), 0 = ab 1. Missions-Frame */
    int vsh_color; /* FIX(WASH,W50): 1 = IM-Diffuse ohne Color-Call aus oD0-Quellregister des VSH (statt stale Slot 3), 0 = alt */
    int im_zero_explicit; /* FIX(TITLEBG2,W54): 1 = explizites (0,0,0,0) per Color-Call bleibt transparent (kein Weiss-Fallback), 0 = alt */
    int blend_alpha; /* FIX(RADAR,W56): 1 = Alpha-Kanal mit denselben Blend-Faktoren wie Farbe (Xbox/D3D8), 0 = alt (Alpha-Replace -> Radar-Geist) */
    int short2_raw; /* FIX(COCKPIT,W60): 1 = VB-UV SHORT2 (0x25) roh = Texel-Koords (NV2A, eigene Lauf-Belege; Lineartextur -> /(w,h) per lin_tex_rect), 0 = alt (/32767 -> Monitor-Faces sampeln eine Ecke) */
    int getstate_fix; /* FIX(COCKPIT,W60): 1 = Get(Vertex|Pixel)Shader/GetVertexShaderConstant liefern den zuletzt gesetzten Stand (statt 0 aus dem Fake-Device), 0 = alt */
    int rt_vp_clip; /* FIX(WORLDVIEW,W65): 1 = Clip-Raum-Draws (alter 3D-Pfad) in RTs auf den Spiel-Viewport (z. B. 512x422 in 512x512) abbilden statt auf das ganze RT, 0 = alt (vertikal gestreckt, Gelaende verfehlt den Tiefenvorpass) */
    int ps_texmode; /* FIX(WORLDVIEW,W65): 1 = Pixel-Shader mit Textur-Stufe 0 = NONE sampeln die gebundene Stage-0-Textur nicht (Xbox-Semantik; Zellenkarte der Sichtbarkeit sonst mit Fremd-Texel multipliziert), 0 = alt */
    int tex_hash_fast;   /* PERF(PERF-TICK,W168): 1 = Streaming-Hash der Texturen wortweise + eine VirtualQuery statt Seiten-Walks (gleiche Bytes, gleiche Pruefung), 0 = alt (FNV byteweise) */
    int tex_inline_data; /* FIX(WORLDVIEW,W65): 1 = Textur-Daten im selben VirtualAlloc-Block wie der Header (Inline-XPR, <= 16 MB dahinter) direkt adressieren statt per Low28-Contig-Suche (traf fremde Bloecke -> Nullen, Hangar schwarz), 0 = alt */
    int rt_scale; /* W88 RENDER-RES: Welt-/Monitor-RTs (256..512 px, kein CPU-Rueckschrieb) mit Faktor N (1..4) anlegen, 0 = auto (nach Ausgabehoehe), 1 = alt (Xbox-Groesse) */
    int rt_scale_dyn;     /* W146 GPU-COST: 1 = Render-Skalierung dynamisch (Faktor der Welt-/Monitor-RTs folgt der GPU-Zeit der Zwischenbilder, Hysterese, zwischen rt_scale_dyn_min und dem auto-/rt_scale-Faktor). 0 = aus */
    int rt_scale_dyn_min; /* W146: kleinster Faktor (Standard 2) */
    int rt_scale_dyn_hi;  /* W146: GPU je Zwischenbild > hi (0,1 ms, Standard 70 = 7,0 ms) ueber 1 s -> eine Stufe runter */
    int rt_scale_dyn_lo;  /* W146: GPU je Zwischenbild < lo (0,1 ms, Standard 45) ueber 8 s -> eine Stufe hoch */
    int rt_scale_min; /* W88 RENDER-RES: kleinste RT-Kante, ab der rt_scale greift (Default 256; kleinere RTs = Effekt-/Messketten bleiben 1x) */
    int tex_lock_guard; /* FIX(PHYSALIAS,W86): 1 = LockRect prueft Schreibbarkeit des vollen Bereichs (fremder/zu kleiner Block -> Fallback + Warnung, kein AV 0x24E1D3), 0 = nur Lesbarkeit (alt) */
    int wv_diag; /* DIAG(WORLDVIEW,W65): 1 = grosse RTs (>= 512 px: RT31-Welt, S40) am Session-Ende im Zensus-Frame dumpen, 2 = nach jedem Farb-Draw (cm != 0) dorthin (monslot_*.jpg, MONDUMP-Kappe) */
    /* FPS-INTERP (W69): Zwischenbilder in der Mission (render_interp.c) */
    int interp_fps; /* 0 = aus (heutiges Verhalten exakt), sonst Ziel-Presents/s in der Mission (W223: Liste sb_fpsrate.h 60..240, cfg -1 = Monitor auto; Echtzeit-Takt beliebig, Testtakt K = interp_fps/30 Bilder je 30-Hz-Tick) */
    int interp_auto; /* W223 FPS-RATES: kein cfg-Schluessel; 1 = cfg hatte interp_fps=-1 (Monitor auto), interp_fps ist dann die aufgeloeste Monitor-Rate (60..240) */
    int interp_diag; /* DIAG(FPS-INTERP): 1 = INTERP-Statistik/Tick-Analyse, 2 = zusaetzlich Draw-Liste der ersten Paare */
    int shading; /* RENDER-QA (W77): 1 = Shading-Pfad (VSH-Programm als GPU-VS, Register-Combiner-PS, Nebel, Stufen 0..3) in der Mission, 0 = alt (Vertexfarbe x Stufe 0) */
    int shade_mask; /* RENDER-QA (W77, DIAG): Teilfunktionen bei shading=1: 1 Nebel, 2 Vertex-Licht (oD0/oD1), 4 Stufen 1..3, 8 lange Combiner-Ketten >= 5 Stufen, 16 2D-Kompositionen (P1-Programme) (Default 7 = ohne 8/16) */
    int im_uv_ot0; /* FIX(TERRAIN-UV,W84): 1 = IM-Draws (ohne VB) nehmen die UV aus dem oT0-Quellregister ihres Vertex-Programms (MOV vN -> oT0, auch ILU-MOV der Licht-Programme) statt Slot 9/Fallback 2 (= Vertexfarbe v2 -> Boden 'Marmor/Wellen'), 0 = alt */
    int tex_mips; /* FIX(TEXMIP,W84): 1 = Mip-Kette der Xbox-Textur (Format-Bits 16..19 MIPMAP_LEVELS; DXT1/3/5 und swizzled A8R8G8B8, Stufen dicht hintereinander) mit hochladen statt nur Stufe 0 (ohne Mips flimmert/moiriert verkleinerter Boden als 'Marmor'-Wellen, Nutzer-Playtest 3.11.), 0 = nur Stufe 0 (alt) */
    int tex_addr; /* FIX(TEXADDR,W84): 1 = alter Pfad sampelt Stufe 0 in der Mission mit dem Adressmodus des Spiels (D3DTSS_ADDRESSU/V aus den Deferred-TextureStates 0x21A8B0: WRAP/MIRROR/CLAMP/BORDER) statt immer CLAMP (Boden-UVs ausserhalb [0,1] -> Randtexel verschmiert, Nutzer-Playtest 3.11.), 0 = alt (immer CLAMP) */
    int shade_cfix; /* RENDER-QA (W77): 1 = Draws, deren PS eine gemappte, bei uns nie gelieferte Combiner-Konstante (Def 0) als Faktor nutzt, zeichnen wie bisher (HLE-Luecke, s. BERICHT W77), 0 = Konstante 0 */
    int shade_gpuvb; /* SHADE-PERF (W80): 1 = VB-Eingaenge des Shading-Pfads roh hochladen und auf der GPU dekodieren/expandieren (SV_VertexID), IM-Draws direkt in den Puffer expandieren; 0 = W77-CPU-Packen (nur bei shading=1 wirksam) */
    int shade_cache; /* SHADE-PERF (W80): 1 = uebersetzte Shading-Shader je Quelltext-Hash in <exe>\\shadercache\\ ablegen/laden (kein Kompilier-Stocken), 0 = immer D3DCompile (nur bei shading=1 wirksam) */
    int shade_psconst; /* FIX(PSCONST,W84, nur bei shading=1): 1 = D3DDevice_SetPixelShaderConstant (0x210220) in den Konstanten-Spiegel (PSC0/PSC1/Final-Mapping des aktiven PS, wie XDK) uebernehmen - wirkt auf Shading-Pfad und fx_ps_c0; 0 = Def-Werte (W77/W80-Stand; der W77-Mitschreiber war von hook_D3D_PSConst ueberdeckt). rq_diag zaehlt die Aufrufe nur */
    int shade_shadow; /* FIX(RENDER-QA3,W100, nur shading=1): 1 = Schattenpuffer: Tiefenpass FUN_00031590 (Dummy-Farbe + LIN_F16-Tiefe) in ein eigenes Tiefen-RT, Stufe 1 der Welt-PS vergleicht dagegen (NV2A Shadow-Compare, 2x2-PCF); 0 = T1 = 1 (alt) */
    int shadow_diag; /* DIAG(SHADOWBUF,W127): mit bloom_diag: je Diag-Bild bis N Draws mit Schattenpuffer-Stufe protokollieren (BLOOM shadow/st/v/o/pre/post, Tiefe roh als uint32) -> scripts/shadow_check.py; 0 = aus */
    int shadow_ab; /* DIAG(SHADOWBUF,W127): 1 = Screenshot-Bild ohne Schattenvergleich (T1 = 1), Partnerbild +1 mit -> Bildpaare shade_shadow 0/1 aus einem Lauf; 0 = aus */
    int shade_ab; /* DIAG(RENDER-QA3,W100): 1 = Shading nur im Bild nach jedem Screenshot (screenshot_frame/every), das ebenfalls geschossen wird -> A/B-Paare aus einem Lauf */
    int shade_dbg; /* DIAG(RENDER-QA3,W100, nur shading=1): Shading-PS gibt einen Term aus: 1 T0, 2 T1, 3 V0, 4 V1, 5 C0 Stufe 0, 6 R0 vor Final, 7 FOG.a, 8 UV0 (frac), 9 R1; 99 = je 1500 Frames weiterschalten (0..9) */
    int shade_pbconst; /* SHADE-PERF (W80, Befund): 1 = Combiner-Laufzeitkonstanten aus D3DDevice_SetRenderState_Simple (NV2A SET_COMBINER_FACTOR0/1, SPECULAR_FOG_FACTOR) in den Shading-Pfad uebernehmen, 0 = nur zaehlen (Default) */
    int shade_scope; /* RENDER-QA (W77, DIAG): 0 = nur Mission (Szene 0x2C, mloop=1), 1 = ueberall */
    int dlg_text_upload; /* FIX(DLGTEXT,W77): 1 = per CPU gerasterter Text (TextOut 0x194D25) wird in die Render-Target-Textur hochgeladen (Ausbilder-Szene), 0 = alt */
    int dlg_diag; /* DIAG(DLGTEXT,W85): 1 = je TextOut-Aufruf (Szene 0x30) Font-Felder, Zeichen, Farbe, Glyphen-Suche, Rueckgabe, geschriebene Pixel (Zeilen DLGDIAG) */
    int dlg_text_raster; /* FIX(DLGTEXT3,W85): 1 = Glyphen-Fuellung 'rep stosd' 0x196033 im TextOut-Rasterer 0x195E1C NICHT wegpatchen (M1-NOP 'MMIO-STOSD' hielt sie fuer GPU-Apertur-Fill), TextOut mit ungueltigem Ziel wird uebersprungen; 0 = alt (NOP); wirkt nur mit dlg_text_upload=1 */
    int dlg_text_clear; /* FIX(DLGTEXT2,W97): 1 = Clear (Xbox D3DCLEAR_TARGET 0xF0) einer TextOut-Textebene fuellt auch den Xbox-Speicher (wie GPU-Clear auf der Xbox), 0 = alt; wirkt nur mit dlg_text_upload=1 */
    int rq_perf; /* DIAG(SHADE-PERF,W80): 1 = QPC-Zaehler je Abschnitt des IM-/Shading-Pfads, Zeile RQPERF alle 300 Missions-Bilder */
    int rq_diag; /* DIAG(RENDER-QA,W77): 1 = Zensus Pixel-/Vertex-Shader, Nebel-/Stufen-Zustand (RQPS/RQVS/RQFOG/RQTSS/RQCOMBO) */
    int rq_every; /* DIAG(RENDER-QA,W77): RQCOMBO-Tabelle alle N Frames (Default 3000) */
    int flash_diag; /* DIAG(W194): 1..6 Vertexproben je Draw in Screenshot-Bildern, Standard 0 */
    int eye_clip_all; /* OPTION(W224, H1; Standard 0): 1 = P2-Renderziel-Draws mit Ecke w<=0 behalten fuer ALLE Blendmodi homogene Koordinaten bis zum Rasterizer (Augenebenen-Clipping); 0 = alt (nur additiv ONE+ONE, W194) */
    int eyeclip_diag; /* DIAG(W224): N = hoechstens N Zeilen EYECLIP je P2-Renderziel-Draw mit Ecke w<=0 (Blend, Primitiv, w-Bereich, Shader); 0 = aus */
    int flash_pixel; /* DIAG(W194): 1+x+65536*y, Pixelspur mit bloom_diag; 0 = aus */
    int interp_verify; /* DIAG(FPS-INTERP): N = jeden N-ten Tick exakte Wiedergabe gegen Spiel-Bild pruefen (INTERP verify), 0 = aus */
    int interp_bsets;  /* PERF(TICK-PERF,W125): N = Reserve-Saetze Tick-Puffer (VB/Stream2/Konstanten) mit GPU-Fence; die Aufnahme nimmt einen Satz, den die GPU nicht mehr liest (kein Warten im DISCARD-Map). 0 = alt (Puffer je Liste) */
    int interp_bsets_nod; /* PERF(TICK-PERF,W125): 1 = freier Satz (Fence fertig) -> erstes Map NO_OVERWRITE statt DISCARD; 0 = DISCARD */
    int interp_gpu_tight; /* PERF(TICK-PERF,W125): T ms > 0 = letzter Tick laenger als T -> interp_gpu_q wirkt mit Grenze 1; 0 = aus */
    int interp_cockpit_cache; /* W146 GPU-COST: 1 = fuer alle t gleichen BB-Anfang (Cockpit) je Paar einmal zeichnen, in weiteren Zwischenbildern kopieren. 0 = alt */
    int interp_cockpit_cache_min; /* W146: mindestens N Draws im BB-Anfang, sonst kein Cache (Standard 16) */
    int interp_vb_kv;  /* PERF(PERF-BENCH,W137): Startgroesse der Interp-Tick-VBs in 1024 Vertices (z. B. 1536 = 60 MB je Satz: kein Neuanlegen + Ueberlauf-Tick, wenn die Draws beim Oeffnen des Hangars springen). 0 = alt (1024 = 40 MB) */
    int interp_gpu_q;  /* PERF(TICK-PERF,W125): N = Zwischenbild (Wiedergabe t<1) nur zeigen, wenn die GPU das N-letzte Bild fertig hat, sonst verfaellt es (Spiel-Tick vor Zwischenbild). 0 = alt (immer) */
    int render_thread; /* PERF(RENDER-THREAD,W126): 1 = Zwischenbild-Wiedergaben (interp_fps>0) auf einem eigenen Render-Thread in einen D3D11-Deferred-Context aufzeichnen; der Spiel-Thread fuehrt nur die fertige Command-List aus. 0 = alt (Wiedergabe im Spiel-Thread) */
    int render_thread_core; /* PERF(RENDER-THREAD,W126): Kern des Render-Threads (-1 = frei, Betriebssystem waehlt) */
    int present_decouple; /* PERF(PERF-FIX,W138): 1 = eigener Present-Takt-Thread (Echtzeit, interp_fps>0): Zwischenbilder der Mission zeigt nur noch er (120 Hz, unabhaengig vom Spiel-Tick); ausserhalb bzw. waehrend langer Ticks wiederholt er das letzte gezeigte Bild. 0 = alt (Bilder nur aus den Spiel-Hooks) */
    int present_core; /* PERF(PERF-FIX,W138): Kern des Present-Takt-Threads (-1 = frei) */
    int pd_diag;      /* DIAG(PERF-FIX,W138): 1 = lange ctx_lock-Regionen (> pd_diag_ms) mit Ruecksprungadressen loggen */
    int pd_diag_ms;   /* DIAG(PERF-FIX,W138): Schwelle in ms (0 = 4) */
    int rtt_dump;     /* DIAG(RTT-DIAG,W38 -> W139): 1 = RT-Inhalt beim ersten Servieren als JPG (shots/rtrd_*.jpg, Zeile RTT dump); blockierendes Ruecklesen unter ctx_lock -> Standard 0 (W139: Ruckler beim Missionsstart/Retry) */
    int warm_shaders; /* PERF(PERF-WARM,W139): 1 = shadercache beim Start auf Hilfs-Thread lesen + D3D-Objekte vorab anlegen (bitgleich) */
    int warm_vb;      /* PERF(PERF-WARM,W139): 1 = Interp-Tick-Puffer mit Anfangsdaten und ohne ctx_lock anlegen (erstes Map DISCARD sonst 8-36 ms unter ctx_lock) */
    int warm_gpu;     /* PERF(PERF-WARM,W139): N>0 = in der Ladephase je Wiederholbild GPU-Last (N*1024 Runden, Hilfsziel) -> GPU bleibt aus P8 (Missionsstart/Retry) */
    int warm_gpu_mem; /* PERF(PERF-WARM,W139): Speicheranteil der warm_gpu-Last: M Kopien 2048x2048 RGBA8 (haelt den Speichertakt) */
    int warm_gpu_pct; /* PERF(PERF-WARM,W139): Ziel-GPU-Anteil der warm_gpu-Last je Bildperiode in % (0 = 85) */
    int pd_gpu_wait;    /* PERF(GPU-COST2,W165): Takt-Thread wartet bei GPU-Rueckstau bis pd_gpu_wait % der Bildperiode (50 = W138), dann verfaellt das Bild */
    int rt_scale_dyn_keep; /* PERF(GPU-COST2,W165): 1 = rt_scale_dyn wechselt ohne RT-Neuanlage (Saetze je Faktor, Hilfs-Thread, Hochschalten nur mit Prognose) */
    int warm_gpu_soft;  /* PERF(GPU-COST2,W165): Bits: 1 = Last erst nach warm_gpu_delay ms Ladephase, 2 = erste Last 1/8-Einheit, 4 = Last nach dem Wiederholbild (Standard 1) */
    int warm_gpu_delay; /* PERF(GPU-COST2,W165): Entprellung der Ladephase in ms (nur mit warm_gpu_soft) */
    int warm_vbpre;   /* PERF(PERF-WARM,W139): 1 = alle Interp-Tick-Puffersaetze beim ersten Present mit Offscreen-BB anlegen + beruehren (nicht erst beim ersten Missionsstart) */
    int warm_diag;    /* DIAG(PERF-WARM,W139): 1 = D3D-Context-Aufrufe > warm_diag_ms loggen (WARM-DIAG lang) */
    int warm_diag_ms; /* DIAG(PERF-WARM,W139): Schwelle in ms (0 = 2) */
    int interp_shots; /* DIAG(FPS-INTERP): ab Interp-Paar N (Zaehler ab Missionsbeginn) alle praesentierten Bilder von interp_shot_ticks Ticks speichern (ip_<f>_<k>), 0 = aus */
    int interp_shot_ticks; /* Anzahl Ticks je Bildfolge (Default 6) */
    int interp_shot_every; /* Bildfolge alle N Interp-Paare wiederholen (0 = einmal) */
    int interp_shot_max; /* Deckel Bildfolgen (Default 8) */
    int interp_inst; /* FIX(POPIN,W91): 1 = mehrfach gezeichnete Modelle (gleiche Signatur + Eingangsdaten, z. B. Haeuser) dem Vortick-Draw mit kleinstem Bildsprung zuordnen (gegenseitig naechster, eindeutig; sonst exakt), 0 = alt (Reihenfolge -> falsche Instanz, Haeuser poppen in Zwischenbildern) */
    int interp_pass; /* FIX(POPIN,W91): 1 = Durchgaenge derselben Instanz (gleiches Ziel + Geometrie + Matrix) im Tick gleich behandeln: ist einer exakt, alle exakt (kein Z-Fighting gelerpt gegen exakt in Zwischenbildern), 0 = alt */
    int pf_ring; /* DIAG(POPIN,W91): N = die naechsten N praesentierten Bilder (jedes Present, Spiel- und Zwischenbilder) ab pf_from Spiel-Frames nach Missionsbeginn auf der GPU in einen Ring kopieren (kein CPU-Stall), danach als shots/pf<s>_<i>_<art>.(jpg|bmp) schreiben + Zeilen PF; 0 = aus */
    int pf_mode; /* DIAG(CULL-POPIN,W215): Anker der Bildfolge = Spielmodus [0x2EEA34] (Standard 4 = Mission, 2 = Titel-Demo/Replay) */
    int pf_from; /* DIAG(POPIN,W91): Start der Bildfolge in Spiel-Frames nach Missionsbeginn (Szene 0x2C, mloop=1) */
    int pf_every; /* DIAG(POPIN,W91): weitere Bildfolge alle N Spiel-Frames (0 = nur eine) */
    int pf_max; /* DIAG(POPIN,W91): Deckel Bildfolgen (Default 1) */
    int rt_from_mission; /* TEST(POPIN,W91): >0 = Testtakt (vblank_hz=0) bis Missionsbeginn, dann Echtzeit mit vblank_hz=N (Zaehler monoton); 0 = aus */
    int rt_delay; /* TEST(POPIN,W91): mit rt_from_mission: Echtzeit erst N Spiel-Frames nach Missionsbeginn (Startsequenz im Testtakt) */
    int rt_fps_limit; /* TEST(POPIN,W91): mit rt_from_mission: fps_limit ab Missionsbeginn (0 = unveraendert) */
    int rt_from_film; /* TEST(PERF-BENCH,W137): >0 = Testtakt (vblank_hz=0) bis zum ersten Film (*.bik geoeffnet), dann Echtzeit mit vblank_hz=N; 0 = aus */
    int rt_mission_any; /* TEST(PERF-BENCH,W137): mit rt_from_mission: erster Missionsbeginn (Szene 0x2C, mloop 1) unabhaengig vom Ladezaehler 0x2EEA34, auch Titel-Demo; 0 = Spielermission (W232 sb_in_mission, vorher Ladezaehler 4) */
    int rt_interp_fps; /* TEST(PERF-BENCH,W137): mit rt_from_film/rt_from_mission: interp_fps beim Echtzeit-Wechsel setzen (Weg wie Overlay-Bildratenwechsel; Testtakt davor ohne Zwischenbilder), 0 = unveraendert */
    int movie_skip_delay; /* TEST(PERF-BENCH,W137): movie_skip-Tasten erst N Spiel-Frames nach der Filmerkennung (Film-Messfenster), 0 = bisher */
    int interp_eyefix;    /* FIX(W224 LINE-FLICKER): 1 = Zwischenbild-VS nimmt fuer Vertices mit w-Vorzeichenwechsel zwischen Tick N-1 und N die Tick-N-Position (kein Clip-Lerp durch w=0 -> Linie quer durchs Bild); 0 = alt */
    int interp_eyediag;   /* DIAG(W224): N = hoechstens N Zeilen INTERP eyex (zwischengerechnete Draws mit w-Wechsel einzelner Vertices) + Summen in INTERP stat2; CPU-Kopie der Clip-w je Vertex */
    float interp_maxjump; /* Plausibilitaet: max. NDC-Sprung eines Draws zwischen zwei Ticks (Default 0.5) */
    int im_fast; /* PERF(IM-PERF,W99): 1 = VB-Draws kompakt dekodieren (40 B statt 256-B-Slots je Vertex), Primitiv-Expansion direkt in den gemappten Puffer (IM-Ring/Tick-VB), keine Zwischenkopien; Bild bitgleich. 0 = alt. Diagnosen (rq_diag/fx_diag/Zensus/sbar_diag/interp_diag>=3) nehmen immer den alten Weg */
    int im_end_reset; /* FIX(EDGE,W64): 1 = End() schliesst den IM-Batch (kein Geister-Wiederholdraw beim naechsten VB-Draw), 0 = alt */
    int fx_blend_eq; /* FIX(FX-ORIG,W90): 1 = NV2A BLEND_EQUATION (0x350: ADD/SUB/REVSUB/MIN/MAX) + BLEND_COLOR (0x34C) abbilden, 0 = alt (immer ADD, Faktor 1) */
    int clear_xbflags; /* FIX(CLEAR-XB,W101/W177): 1 = Clear-Flags nach Xbox-D3D8 (TARGET 0xF0, Z 0x1, STENCIL 0x2) inkl. Textebenen-Anpassung (W177), 2 = TEST: Xbox-Flags ohne Textebenen-Anpassung (Stand W101, Hilfetext fehlt), 0 = alt (PC-Werte 1/2/4) */
    int fx_shot_gap; /* DIAG(FX-HOLE,W106): Mindestabstand der fx_shots-Bilder in Frames (0 = 15) */
    int fx_census; /* DIAG(FX-HOLE,W106): mit m3d_census>0: die ersten N FXSHOT-Bilder loesen einen Zensus im Folgeframe aus */
    int fx_zfunc; /* FIX(FX-ORIG,W90; Standard 1 seit W101 FRONT-HANGAR): 1 = NV2A DEPTH_FUNC (0x354, GL-Enum) als D3D11-DepthFunc, 0 = alt (immer LESS) */
    int tex_lockwatch; /* FIX(STARTSEQ-BARS,W98): 1 = per LockRect gesperrte Texturen einmal je Frame voll hashen + bei Aenderung neu laden (Spiel schreibt ueber behaltenen Lock-Zeiger), 0 = alt */
    int fx_alphakill; /* FIX(STARTSEQ-BARS,W98): 1 = Xbox D3DTSS_ALPHAKILL Stufe 0 (Deferred-TSS 0x21A8DC Bit 2, NV2A TEXTURE_CONTROL0.ALPHAKILLEN): Texel-Alpha 0 -> discard (nicht fuer RT-Texturen), 0 = alt */
    int fx_alphatest; /* FIX(FX-ORIG,W90): 1 = NV2A Alpha-Test (0x300/0x33C/0x340) im IM-Pixelshader (discard), 0 = alt (kein Alpha-Test) */
    int fx_ps_c0; /* FIX(FX-ORIG,W90): 1 = Standardpfad (shading=0): Combiner-Muster R0=(V0+C0)*T0 (Effekt-PS) -> C0 zur Vertexfarbe addieren, 0 = alt (nur V0*T0) */
    /* W133 REM-FX: Effekt-Remaster-Optionen (Standard 0 = Original, ESC-Seite "Effekte") */
    int fx_soft;          /* weiche Partikel (Rauch/Explosion schneiden Geometrie weich) 0..200 % Staerke */
    int fx_embers;        /* Glut/Funken bei Explosionen/Feuer/Muendungsfeuer 0..200 % Menge/Helligkeit */
    int fx_light;         /* Explosions-Licht auf die Welt (Monitorbild) 0..200 % */
    int fx_light_cockpit; /* Explosions-Licht auf das Cockpit (Nachbearbeitung) 0..200 % */
    int fx_interp;        /* 120 FPS: Effekt-Partikel mit normaler statt strenger Sprunggrenze zwischenrechnen (0/1) */
    int fxr_ab;           /* DIAG(REM-FX,W133): N Bildpaare an Explosionen (A Original, B Option, je Paar naechste Option) */
    int fxr_diag;         /* DIAG(REM-FX,W133): 1 = FXR-Zeilen (Glut, Licht, Interp-Zuordnung der Effekt-Draws) */
    int fx_im_uv; /* FIX(FX-ORIG,W90): 1 = IM-Draws mit Transform-VS (P2) lesen die UV aus dem oT0-Quellregister des Programms (Effekte v3), 0 = alt (Slot 9, Fallback 2) */
    int monitor_fx; /* MONITOR-FX (W105): 1 = Monitor-Effekte wie im Original (Korn/Scanlines = Wash mit normierten UVs, Signalstoerung = Zeilen aus RT-Kopie), 0 = aus (kein Korn, keine Stoerung), 2 = alter Pfad vor W105 (A/B) */
    int monitor_grain; /* MONITOR-FX (W105): Staerke Korn/Scanlines 0..100 % (100 = Original) */
    int monitor_glitch; /* MONITOR-FX (W105): Staerke Signalstoerung (Zeilen-Zittern, Rausch-Spitzen) 0..100 % (100 = Original) */
    int shade_mfx; /* FIX(SHADE-FX,W121, nur shading=1): 1 = Korn-Wash der Monitore (render_mfx) auch im Shading-Pfad mit normierten UVs (keine Texel-Skala 1/128 -> sonst glatter Schleier statt Korn), 0 = alt */
    int world_fog;      /* SHADE-FX (W121, nur shading=1): Nebelstaerke der Aussenwelt 0..100 % (100 = Original) */
    int world_contrast; /* SHADE-FX (W121, nur shading=1): Kontrast der Welt-RTs 50..200 % (100 = Original) */
    int world_gamma;    /* SHADE-FX (W121, nur shading=1): Gamma der Welt-RTs 50..200 % (Exponent v/100, 100 = Original) */
    int world_sharpen;  /* SHADE-FX (W121, nur shading=1): Schaerfe der Welt-RTs 0..100 % (0 = Original) */
    int bloom_strength; /* SHADE-FX (W121, nur shading=1): Bloom der Monitor-Komposition 0..200 % (100 = Original) */
    int cockpit_glow;   /* SHADE-FX (W121): Leuchten heller Cockpit-Pixel (Lampen/LEDs/Anzeigen) 0..100 % (0 = Original) */
    int cockpit_spill;  /* SHADE-FX (W121): Licht-Abstrahlen auf die Umgebung 0..100 % (0 = Original) */
    int cockpit_light;  /* SHADE-FX (W121): Helligkeit unbeleuchteter Cockpit-Flaechen 30..100 % (100 = Original) */
    int shade_live_reset; /* FIX(SHADE-FX,W121): 1 = wechselt shading zur Laufzeit, kleine RTs (<= 64², Bloom-Akkumulatoren) leeren (sonst Bloom-Rest des anderen Pfads), 0 = alt */
    int vis_query_live; /* FIX(SHADE-FX,W121): 1 = vis_query-Hooks nachinstallieren, sobald Shading zur Laufzeit (ESC-Menue) eingeschaltet wird (sonst Dauerblendung = Hangar/Welt ueberbelichtet), 0 = alt (nur beim Start) */
    int world_focus;    /* SHADE-FX (W121/2, nur shading=1): Rand-/Fokus-Unschaerfe der Monitor-Komposition 0..100 % (Maske der 200²-Unschaerfe-RT, 100 = Original, 0 = ueberall scharf) */
    int world_exposure; /* SHADE-FX (W121/2, nur shading=1): Belichtung der Aussenwelt 25..400 % (100 = Original), vor der Klemmung */
    int tonemap;        /* SHADE-FX (W121/2, nur shading=1): Tonemapping Aussenwelt 0 = aus (Original), 1 = Reinhard, 2 = ACES-aehnlich */
    int bloom_threshold; /* SHADE-FX (W121/2, nur shading=1): Bloom-Schwelle 0..90 % (0 = Original) beim Lesen der Bloom-RTs */
    int light_world;    /* SHADE-FX (W121/2, nur shading=1): Licht-Intensitaet Aussenwelt 0..200 % (beleuchtete Farbe R0, 100 = Original) */
    int light_cockpit;  /* SHADE-FX (W121/2, nur shading=1): Licht-Intensitaet Cockpit 0..200 % (3D-Draws in den BB ohne Nebel, ohne Monitor-RT, 100 = Original) */
    /* W131 SHADER-OPT2 (Standard = Original/aus) */
    int aa_mode;        /* SHADER-OPT2 (W131): Kantenglaettung 0 = aus (Original), 1 = FXAA Endbild (Mission), 2 = FXAA Welt-RT (nur shading=1, beim Lesen), 3 = beides */
    int ssao;           /* SHADER-OPT2 (W131): Umgebungsverdeckung 0..100 % (0 = aus/Original), Bildraum aus der Backbuffer-Tiefe (Cockpit/3D im BB, nur Mission) */
    int color_sat;      /* SHADER-OPT2 (W131): Saettigung 0..200 % (100 = Original), Endbild Mission */
    int color_temp;     /* SHADER-OPT2 (W131): Farbtemperatur -100..100 (0 = Original, + = waermer) */
    int color_lift;     /* SHADER-OPT2 (W131): Lift (Schatten) -50..50 (0 = Original) */
    int color_gamma;    /* SHADER-OPT2 (W131): Gamma (Mitten) 50..200 % (100 = Original) */
    int color_gain;     /* SHADER-OPT2 (W131): Gain (Lichter) 50..200 % (100 = Original) */
    int shadow_soft;    /* SHADER-OPT2 (W131, nur shading=1;shade_shadow=1): 1 = 3x3-Gauss-PCF (Hypothese NV2A GAUSSIANCUBIC, weichere Kante), 0 = 2x2-PCF (bisher) */
    int hud_label_avoid; /* HUD-LABEL-OPT (W153): 1 = Ziel-Etikett weicht RELOAD/MAIN aus (nur bei Ueberlappung, sonst Originalstelle; wirkt ab Neustart), 0 = Original (kein Patch) */
    int vis_query_async; /* SHADER-OPT2 (W131, Leistung): 1 = Sonnentest liefert das letzte fertige Ergebnis statt blockierend zu warten (1 Bild Latenz), erstes Ergebnis blockierend; 0 = blockierend (bisher) */
    int bb_depth_srv;   /* SHADER-OPT2 (W131): 1 = Backbuffer-Tiefe typlos mit SRV anlegen (fuer ssao, sonst automatisch bei ssao>0); 0 = nur bei Bedarf */
    int ssao_view;      /* DIAG(SHADER-OPT2,W131): 1 = Endbild zeigt die SSAO-Verdeckung (Grauwert) statt des Bildes */
    int shfx_ab;        /* DIAG(SHADE-FX,W121): 1 = Screenshot-Bild mit Original-Werten, Partnerbild (+1) mit den Optionen, dazu <shot>_fx (gleiches Bild + Cockpit-Nachbearbeitung) */
    int shot_mission;   /* DIAG(SHADE-FX,W121): 1 = screenshot_frame zaehlt ab Beginn der Kampagnen-Mission (Modus 4) */
    int shfx_diag;      /* DIAG(SHADE-FX,W121): 1 = SHFX-Zeilen (Optionen, gekennzeichnete Stufen) alle 600 Missionsbilder */
    int mfx_diag; /* DIAG(MONITOR-FX,W105): 1 = MFX-Zeilen (Wash/Zeilen-Draws, Stoerpegel 1x/300 Bilder) */
    int mfx_ab; /* DIAG(MONITOR-FX,W105): 1 = Screenshot-Bild mit altem Pfad, Partnerbild (+1) mit monitor_fx (Bildpaare aus einem Lauf, wie shade_ab) */
    int mfx_shots; /* DIAG(MONITOR-FX,W105): N > 0 = bis zu N Ausloesungen in Wash-/Zeilen-Bildern (Zeilen-Bild 2/40/120, jedes 400. Wash-Bild ab 30): ohne mfx_ab 3 Folgebilder, mit mfx_ab Paar alt (p+1) / neu (p+2) */
    int shade_texskip; /* FIX(SHADE-BLOOM,W112, nur shading=1): 1 = Tiefenformate (0x2A..0x31, Schattenpuffer) auf Stufe 1..3 nicht als Farbtextur laden (Fehlversuche verbrauchten das Upload-Budget, Bodentextur fehlte); 0 = alt */
    int vis_query;     /* FIX(SHADE-M2,W117): NV2A-Sichtbarkeitstest (Begin/EndVisibilityTest, GetVisibilityTestResult) als D3D11-Occlusion-Query; 1 = nur mit shading=1 (Standard, shading=0 bitgleich), 2 = immer, 0 = alt (Ergebnis immer 1 = "sichtbar") */
    int vis_query_all; /* W117 vis_query: 0 = nur Sonnentests 0x3FFE/0x3FFF echt (Standard), 1 = alle Indizes (Objekt-Occlusion-Culling FUN_00025580, Licht-Flares FUN_0001B900) */
    int vis_diag;      /* DIAG(SHADE-M2,W117): 1 = VISQ-Zeilen (Ergebnis je Index, erste 64 + je 2000 Abfragen) */
    int bloom_diag; /* DIAG(SHADE-BLOOM,W112): 1 = Bloom-Kette in Diag-Bildern der Kampagnen-Mission (Modus 4) Draw fuer Draw protokollieren + RTs roh dumpen (BLOOM-Zeilen, shots/bl_*.bin), 2 = jede Mission (auch Titel-Demo) */
    int bloom_frame; /* DIAG(SHADE-BLOOM,W112): Diag-Bilder erst ab dieser Present-Nr. (Screenshot-Bilder, mit shade_ab auch +1) */
    int bloom_max; /* DIAG(SHADE-BLOOM,W112): hoechstens N Diag-Bilder (Standard 4) */
    int bloom_every; /* DIAG(SHADE-BLOOM,W112): Mindestabstand der Diag-Bilder in Bildern (0 = jedes Screenshot-Bild) */
    int fx_diag;
    int fx_shots; /* DIAG(FX-ORIG,W90): mit fx_diag>=1: Client-Bild in bis zu N Frames mit Effekt-PS (V0+C0)*T0 (Abstand >= 15 Frames), 0 = aus */ /* DIAG(FX-ORIG,W90): 1 = Effekt-Draws (geblendet ohne Z-Schreiben/additiv/Alpha-Test/Punkte) je Zustand + NV-RenderStates (FXKEY/FXSUM/FXNV), 2 = alle Missions-Draws */
    int drawdrop_diag; /* DIAG(COCKPIT,W60): 1 = verworfene VB-Draws (vor dem Zensus) + SetVertexShader(0) zaehlen/loggen (DRAWDROP/DDSUM/DRAWCALL-Zeilen) */
    int label_diag; /* DIAG(HUD-LABEL,W153): 1 = LABEL-Zeilen (Etikett-Zeile, Text, Pos, Breite, Ausweich-Entscheid) in Screenshot-Bildern */
    int mon_dummy_nodepth; /* FIX(MONITORVIEW,W36): 1 = Tiefenpass auf Dummy-Surface (Data=0) schreibt keine BB-Tiefe (keine schwarzen Loecher) */
    int mon_color_default; /* FIX(MONITORVIEW,W36): 1 = IM-Farbkanal ohne Color-Call + Rohwert>1.0 -> Weiss (statt stale Gelb) */
    int vsh_dump; /* DIAG(VSH2D,W38): 1 = Vertex-Shader-Mikrocode je Handle einmal dumpen (VSH-Zeilen) */
    int vsh_emul; /* FIX(VSH2D,W38): 1 = oPos per VSH-Emulation (Kompositions-Draws: Welt auf Monitor), 0 = alter Pfad */
    int xbox_transform;
    int vsconst_mirror; /* W25: 1 = VSConst-Hooks replizieren CPU-Spiegel 0x21BCA8 (Xbox-Semantik), 0 = No-Op (alt) */
    int m3d_vs_mvp; /* W25: 1 = 3D-MVP aus VS-Konstanten c98-c101 (pro-Draw-WVP des Spiels) + Hor+ */ /* W25-TEST: 1 = 3D-World=Identitaet (Spiel setzt World nur via VS-Konstanten) */ /* W25: 1 = Xbox-Transform-States (VIEW=0/PROJ=1/WORLD=6-9), 0 = PC-Enum (alt, traf nie) */
    /* [watchdog] */
    int stall_loadpause; /* W32: 1 = bekannte Audio-Poll-Ladepause als LOAD-PAUSE statt STALL melden, 0 = alt (jedes 5-s-Fenster ohne Frames = STALL) */
    int rs_colormask; /* W33: 1 = NV2A COLOR_MASK (0x358) -> RenderTargetWriteMask, 0 = immer ALL (alt) */
    int rs_stencil;   /* W33: 1 = NV2A Stencil-States -> D3D11 DepthStencil-State, 0 = alt (aus/fest) */
    /* [remaster] (W103 REM-FRAME): alles Standard AUS = Original bitgenau */
    int remaster;      /* Hauptschalter: 0 = Original (alle Remaster-Bereiche aus), 1 = Einzelschalter gelten */
    int hd_textures;   /* mit remaster=1: Ersatz-Texturen remaster/textures/x<hd_scale>/<hash>.dds laden */
    int hd_scale;      /* Paket-Stufe 2 oder 4 (textures/x2 bzw. x4), Freigaben aus remaster/selection.json gelten immer (W114); 0 = alt (W111) = 4 */
    int fx_enhanced;   /* Platzhalter (noch ohne Wirkung) */
    int rem_ultrawide; /* cfg-Key 'ultrawide': Platzhalter (noch ohne Wirkung; Mission-Hor+ laeuft unabhaengig) */
    int postfx;        /* Platzhalter (noch ohne Wirkung) */
    int hd_cache_mb;   /* Grenze Ersatz-Textur-Cache in MB (VRAM), Default 1024 */
    int hd_cpu_nohash; /* PERF(FILM-FPS,W123): 1 = per CPU beschriebene Texturen (Film) ohne Inhalts-Hash (nie ersetzt/gedumpt), 0 = alt */
    int hd_manual;     /* W113: 1 = textures/manual/<hash>.dds (Text-Rekonstruktion) hat Vorrang vor x2/x4/Auswahl (ausser Wahl "orig"); 0 = ignorieren */
    int tex_dump;      /* DIAG(REM-FRAME,W103): 1 = jede neue Textur als PNG + index.json nach <remaster_dir>/dump */
    char remaster_dir[512]; /* leer = auto (<Repo-Wurzel mit poc-native>/remaster) */
    int pack_loader;     /* W140 PACK-FORMAT: 1 = Ersatz-Texturen aus .sbpack-Paketen (sb_pack.c), 0 = Ordner-Lader (Rueckfall). W148: Standard = 1 (ohne packs.json: Original) */
    char pack_list[512]; /* W140: packs.json (leer = keine Pakete; relativ = zu remaster_dir). W148: Standard = "packs/packs.json" */
    int pack_fx;         /* W151 EFFECT-PACK: 1 = Effekt-Parameter aktiver Effect-Packs lesen und auf die cfg-Werte legen (Paket gewinnt), 0 = aus */
    char lang[8];                 /* W142: [ui] lang ("en" | "de", Spielertexte) */
    /* [overlay] (W118 REM-OVERLAY, sb_overlay.c) */
    int overlay;                /* 1 = ESC oeffnet das Einstellungs-Overlay (Spieler-Standard), 0 = aus (ESC wie bisher) */
    char overlay_test[1024];    /* TEST(W118): Skript "<bild>:<aktion>,..." -> PostMessage an das eigene Fenster (leer = aus) */
    char overlay_save_path[512]; /* "Als Standard speichern": Ziel (leer = user.cfg neben sb_native.cfg bzw. SB_USER_CFG) */
    /* W188 GAME-MENU (sb_overlay.c, sb_pause.c, sb_feedback.c) */
    int game_menu;              /* 1 = ESC oeffnet zuerst das Spielmenue (Weiter/Einstellungen/Fotomodus/Feedback/Beenden), 0 = ESC direkt in die Einstellungen (W118) */
    int menu_pause;             /* 1 = offenes ESC-Menue pausiert das Spiel (Spieler-Standard), 0 = Spiel laeuft dahinter weiter (Live-Vorschau) */
    int pause_live;             /* W193 PAUSE-LIVE: 1 = Standbild der Pause aus der FPS-INTERP-Aufzeichnung mit den aktuellen Einstellungen neu zeichnen, 0 = W188 (Kopie) */
    char menu_peek_key[16];     /* W193: Taste "Menue kurz ausblenden" (gedrueckt halten), Tastenname wie in Profilen; leer/OFF = aus */
    int alpha_welcome;          /* W222: 1 = Alpha-Hinweis einmal je Programmstart (Spiel pausiert, beliebige Taste/Klick schliesst); Standard Community 1, Private 0 */
    /* W195 PHOTO-MODE ([photo]): Fotomodus (F12 bzw. Profil-Aktion "photo", ESC-Menue -> Fotomodus) */
    int photo_mode;             /* 1 = Fotomodus verfuegbar (Standard), 0 = aus (Menue-Eintrag ausgegraut, Taste ohne Wirkung) */
    char photo_dir[400];        /* Fotoordner (UTF-8); leer = <Nutzerdaten>\Screenshots (legacy/Tests: <Ausgabe>\Screenshots) */
    int photo_scale;            /* Standard-Aufnahmeaufloesung: 1, 2, 4 oder 8 x Fensteraufloesung */
    int photo_jpg;              /* 0 = PNG (Standard), 1 = JPG */
    int photo_aspect;           /* Bildformat ohne Cockpit: 0 = Bildschirm (Ausschnitt), 1 = Bildschirm (weit, Standard; Raender evtl. leer), 2 = Kamera (wie Monitor, ~4:3) */
    int photo_cam_yaw;          /* freie Kamera: max. Drehung links/rechts (Grad, Standard 35) */
    int photo_cam_pitch;        /* freie Kamera: max. Neigung (Grad, Standard 25) */
    int photo_cam_dist;         /* freie Kamera: max. Weg von der Spielkamera (Spiel-Einheiten ~ m, Standard 4) */
    int photo_rt_max;           /* W233: Kante der skalierten Monitor-/Welt-RTs einer Cockpit-Aufnahme xN (Standard 8192, max 16384, 0 = nativ) */
    int photo_rt_fail_test;     /* TEST(W233): so viele Anlegeversuche der Zwischenziele scheitern lassen (Standard 0) */
    char feedback_repo[128];    /* [feedback] repo: GitHub-Repo fuer "Neues Issue" (Owner/Name), Standard SB_FEEDBACK_REPO_DEFAULT (sb_feedback.h) */
    /* W122 PERF-HUD (sb_perfhud.c) */
    int perf_hud;               /* 1 = Leistungs-Overlay sichtbar (Taste schaltet und speichert in user.cfg), 0 = aus (Standard) */
    int perf_hud_pos;           /* Ecke: 0 = oben rechts, 1 = oben links, 2 = unten links, 3 = unten rechts */
    char perf_hud_key[16];      /* Taste: F1..F12 (ohne F10), VK<hex>; 0/aus = keine Taste. Standard F2 */
    int watermark;              /* W219 WATERMARK: 1 = Alpha-Wasserzeichen (Version) unten rechts, 0 = aus (Code-/Test-Standard) */
    int perf_log;               /* DIAG(PERF-HUD,W122): alle N s Zeile PERFHUD + PERFSPIKE-Zeilen (0 = aus) */
    int perf_spike_ms;          /* DIAG(PERF-HUD,W122): PERFSPIKE ab dieser Bildzeit (0 = 2x Zielbildzeit) */
    int perf_prof;              /* DIAG(PERF-HUD,W122): 1 = Stichproben-Profiler des Spiel-Threads (Zeile PERFPROF, mit perf_log) */
    int perf_prof_heavy;
    int swap_match_unlock;      /* PERF(PERF-TICK,W168): 1 = Interp-Zuordnung am Swap ohne ctx_lock (Present-Takt wartet nicht) */
    int krnl_reg_cache;         /* PERF(PERF-M2,W174): 1 = Ergebnis-Cache (je Thread) fuer krnl_phys_to_virt/krnl_memreg_find (linearer Register-Scan je Textur-/RT-Zugriff); gleiche Antworten */
    int ip_match_compact;       /* PERF(PERF-M2,W174): 1 = Interp-Zuordnung (ip_inst_best) liest Signatur/Hash/Bezugspunkte aus einem dicht gepackten Feld; gleiche Zuordnung */
    int ip_cb_ring;             /* PERF(PERF-M2,W174): 1 = Zwischenbild-Wiedergabe schreibt alle Draw-Konstanten (b0/b1) einmal je Wiedergabe in einen Ring-Puffer und bindet sie per Versatz (*SetConstantBuffers1) statt Map(DISCARD) je Draw; gleiche Bytes. Standard 1 (W174) */
    int ip_vb_gpu;              /* PERF(PERF-M2,W174): Bits 1 = Tick-VB am Swap in einen GPU-Puffer (DEFAULT) kopieren und die Wiedergabe daraus lesen; 2 = ebenso Stream 2/Roh-Eingaenge (rqx) des Shading-Pfads. Standard 3 (W174), 0 = alt */
    int ip_gdiag;               /* DIAG(PERF-M2,W174): N > 0 = jede N-te Zwischenbild-Wiedergabe GPU-Zeit (Timestamps um Wiedergabe/Command-List) + Zaehler je Wiedergabe -> runs/<tag>/ipgpu.csv, Zeile IPGPU */
    int ip_ablate;              /* DIAG(PERF-M2,W174): Varianten im Wechsel je Wiedergabe (Bild veraendert!): 1 Viewport 1x1, 2 max. 3 Vertices je Draw, 4 keine Konstanten-Maps, 8 keine Draws, 16 Ring (wie ip_cb_ring), 32 GPU-VB (braucht ip_vb_gpu), 64 Ring+GPU-VB */
    int pd_gw_nopri;            /* PERF(PERF-TICK,W168): 1 = GPU-Warten des Takt-Threads pollt die Sperre ohne Vortritt (Spiel-Thread weicht nicht alle 0,25 ms) */
    int ctx_wait_diag;          /* DIAG(PERF-TICK,W168): 1 = Zeile CTXWAIT (Spiel-Thread wartet auf ctx_lock, Haltezeit Takt-Thread) */        /* DIAG(PERF-TICK,W168): perf_prof=2 - Schwelle schwerer Tick (ms, 0 = 28) fuer PERFPROF3 */
    int perf_blk;               /* DIAG(TICK-PERF,W125): 1 = Zeitmesser um Blockier-Kandidaten (Zeile TICKBLK, mit perf_log) */
    int perf_trace;             /* DIAG(PERF-BENCH,W137): 1 = Bild-/Tick-Spur <SB_OUT_DIR>/perf_trace.csv (scripts/perf_bench.py), misst wie perf_log */
    int perf_gpass;             /* DIAG(GPU-COST,W146): N > 0 = jede N-te Zwischenbild-Wiedergabe GPU-Zeit je Pass, asynchron (Zeile GPUPASS, Panel "GPU Pass") */
    int perf_gsplit;            /* DIAG(TICK-PERF,W125): N > 0 = jede N-te Zwischenbild-Wiedergabe GPU-Zeit je Ziel (Zeile GPUSPLIT, synchron) */
    /* [vr] (W134 VR-PROTO, vr.c): Stereo-Cockpit aus der Interp-Aufzeichnung. Keys flach mit Praefix vr_ (Sektionen sind egal). */
    int vr_enable;              /* 0 = aus (Standard, bitgleich), 1 = VR-Prototyp (braucht interp_fps>0) */
    char vr_preview[16];        /* Desktop-Vorschau: "sbs" (beide Augen nebeneinander), "left"/"right" (ein Auge), "off"/"xr" (OpenXR, Fenster normal); Standard "sbs" */
    char vr_runtime_json[512];  /* OpenXR-Runtime-Manifest (leer = XR_RUNTIME_JSON bzw. Registry ActiveRuntime); Tests: Mock-JSON */
    int vr_seat_height;         /* Sitzhoehe-Versatz in mm (+ = Kopf hoeher), Standard 0 */
    int vr_world_scale;         /* Massstab in % (Tracking-Meter -> Spiel-Einheiten), Standard 100 */
    int vr_ipd;                 /* Augenabstand in 1/10 mm (Vorschau; OpenXR liefert eigene Augenposen), Standard 640 */
    int vr_fov;                 /* Vorschau: vertikales Sichtfeld je Auge in Grad (0 = wie Spielkamera), Standard 0 */
    int vr_monitor_depth;       /* Abstand der 2D-Ebene (HUD/Monitorbild) und Basis des Cockpit-Reliefs in mm (0 = 800), Standard 0 */
    int vr_relief;              /* Cockpit-Relief: halbe Tiefenspanne der P2-Cockpit-Draws in mm (0 = flach), Standard 150 */
    char vr_sim[64];            /* Vorschau-Kopfpose: "keys" (Tasten/Maus, Standard), "script" (Drehen/Neigen/Recentering, Tests), "still" */
    char vr_recenter_key[16];   /* Recentering-Taste (F1..F12, VK<hex>), Standard F8 */
    int vr_diag;                /* DIAG(VR-PROTO,W134): 1 = VR-Zeilen (Pose, Bildrate je Auge), 2 = + Draw-Klassen je Wiedergabe */
    int vr_shots;               /* DIAG(VR-PROTO,W134): N > 0 = alle N VR-Bilder ein Client-Bild (vr_<n>_sbs bzw. Augen), max 24 */
    /* W164 VR-FIX2 */
    int vr_canvas;              /* 1 = Menues/Filme/2D-Bildschirme (ausserhalb der Mission) als virtuelle Leinwand in der Brille (Standard 1, nur mit vr_enable=1) */
    int vr_canvas_dist;         /* Leinwand-Abstand in mm (Standard 2500) */
    int vr_canvas_size;         /* Leinwand-Hoehe in mm (Breite = Hoehe x Seitenverhaeltnis des Bildes, nie gestreckt), Standard 1500 */
    int vr_preview_clean;       /* OpenXR-Vorschau im Fenster ohne Leistungs-Overlay (F2) (Standard 1; ESC-Menue bleibt sichtbar) */
    int vr_world_depth;         /* W164 VR-FIX2: 1 = 3D-Pfad im BB (Cockpit-Mesh, echte Perspektive) mit affiner Tiefe aus der Augenprojektion statt Original-z/w x w_Auge (Standard), 0 = W134 */
    int vr_inst_depth;          /* W164 VR-FIX2: 1 = P2-Draws mit perspektivischem z/w (> 0,2: Rundinstrumente, Anzeigen) auf ihre metrische Tiefe statt ins Relief (Standard), 0 = W134 */
    int vr_diag_skip;           /* DIAG(VR-FIX2,W164): Bitmaske Draw-Klassen im Augenbild weglassen (1 = 2D, 2 = 3D-Pfad, 4 = P2-Cockpit), Standard 0 */
    int vr_pose_shots;          /* DIAG(VR-FIX2,W164): N > 0 = je neue ruhige Kopfpose ein Augenbild vr_<n>_pose_*, max N */
    /* W183 VR-FIX3 (alle nur mit vr_enable=1 wirksam) */
    int vr_render_scale;        /* Render-Aufloesung je Auge in % der Runtime-Empfehlung (30..150), Standard 80 (Preset Ausgewogen); live per ESC bis zur angelegten Swapchain-Groesse */
    int vr_refresh_hz;          /* Bildrate der Brille (XR_FB_display_refresh_rate, z. B. 72/80/90/120), 0 = Runtime-Standard */
    int vr_seat_forward;        /* Sitz-Versatz nach vorne in mm (+ = Augenpunkt naeher an den Monitoren), Standard 300 */
    int vr_recenter_start;      /* 1 = beim ersten Bild im Fokus automatisch zentrieren (Startposition = Sitzposition), Standard 1 */
    int vr_mirror;              /* 1 = Fenster zeigt in der Mission den VR-Spiegel (Augenbild), 0 = Fenster schwarz (spart Leistung), Standard 1 */
    int vr_mirror_fit;          /* 0 = Fenster fuellen durch Zuschnitt (Standard), 1 = ganzes Augenbild mit schwarzen Balken; nie gestreckt */
    int vr_mirror_res;          /* Aufloesung des Spiegels in % der Fenstergroesse (25..100), Standard 100 */
    int vr_world_tick;          /* 1 = Aussenwelt/Monitor-RTs im Spieltakt (Tick-Endstand), Cockpit/HUD je VR-Bild (Standard); 0 = alles je VR-Bild (W134) */
    int vr_shading;             /* 1 = Shading-Draws des Cockpits im Augenbild (Standard), 0 = IM-PS-Ersatz (billiger) */
    int vr_due_ahead;           /* 0 = Spielzeit der VR-Wiedergabe = jetzt - Latenz wie das Fenster (Standard, kein Halten); 1 = W134 (jetzt + Bildperiode) */
    int vr_idle_unfocused;      /* 1 = keine VR-Bilder, solange die Session nicht FOCUSED ist (Brille ab/Systemmenue), Standard 1 */
    int vr_rt_scale;            /* Welt-/Monitor-RT-Faktor im VR-Betrieb (1..4), 0 = wie rt_scale, Standard 2 (Preset Ausgewogen) */
    int vr_preset;              /* Leistungs-Preset (nur Anzeige/Speichern; Werte stehen einzeln): 0 eigene, 1 Leistung, 2 Ausgewogen, 3 Qualitaet */
    int vr_frame_log;           /* DIAG(VR-FIX3,W183): N > 0 = die ersten N VR-Bilder in der Mission je eine Zeile "VR bild" (Pose, Paar, t, Halten, Draws, Sperre) */
    /* W197 VR-MENU (nur mit vr_enable=1 und laufender OpenXR-Session wirksam) */
    int vr_menu;                /* 1 = ESC-Menue als weltfeste Flaeche vor dem Spieler, F2 als kopffeste Flaeche (Standard); 0 = wie W183 (nur Fenster/Leinwand) */
    int vr_menu_dist;           /* Abstand der Menue-Flaeche in mm (beim Oeffnen vor der Blickrichtung), Standard 1750 */
    int vr_menu_size;           /* Groesse: Hoehe der 4:3-Bezugsbox (540 Panel-Einheiten wie am Desktop) in mm, Standard 1800 */
    int vr_menu_px;             /* Pixeldichte: Hoehe der Bezugsbox in Texeln der eigenen Menue-Textur (Faktor = Wert/540), Standard 1620 */
    int vr_menu_dim;            /* Welt hinter dem offenen Menue abdunkeln in % (0 = aus), Standard 35 */
    int vr_hud_dist;            /* F2-Flaeche: Abstand in mm (kopffest), Standard 1000 */
    int vr_hud_size;            /* F2-Flaeche: Breite in mm, Standard 340 */
    int vr_hud_yaw;             /* F2-Flaeche: Richtung rechts der Blickachse in Grad, Standard 26 */
    int vr_hud_pitch;           /* F2-Flaeche: Richtung ueber der Blickachse in Grad, Standard 19 */
    int vr_menu_shots;          /* DIAG(VR-MENU,W197): N > 0 = je neue ruhige Kopfpose/Menue-Zustand bei offenem Menue oder F2 ein Augenbild vr_<n>_menue_* (max N, hoechstens 64) */
} hcfg_t;

extern hcfg_t g_cfg;

void hcfg_defaults(void);
int hcfg_load(const char *path); /* 0 = ok, Datei fehlt ist ok (Defaults) */
/* Ausgabeordner pro Lauf: Umgebungsvariable SB_OUT_DIR (run_once.ps1 setzt
 * runs/<tag>), sonst "out". Legt <dir> und <dir>/shots an. */
const char *hcfg_out_dir(void);
/* W118: Pfad der Spieler-cfg (user.cfg neben der cfg bzw. SB_USER_CFG);
 * auch gesetzt, wenn SB_NO_USERCFG das Laden verhindert. */
const char *hcfg_user_cfg_path(void);
/* W159: "key=wert;..."-Kette auf die cfg legen (Start-Parameter --set bzw.
 * SB_CFG_SET; tag = Log-Herkunft). Gewinnt ueber allen Dateien. */
void hcfg_apply_set(const char *set, const char *tag);

#ifdef __cplusplus
}
#endif
