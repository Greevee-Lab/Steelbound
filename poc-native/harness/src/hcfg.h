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
    int ntsc_u;           /* W230 [expert]: 1 = USA-Fassung (NTSC-U) mit US-Profil starten (Standard), 0 = Notbremse (abgelehnt wie unbekannte Fassung) */
    char hdd_root_us[512]; /* W230: Spielstandordner der US-Fassung; leer = <hdd_root>_US (gleiche Title-ID wie EU, nie mischen) */
    int loc_boot;         /* W275 [expert]: 1 = Line of Contact (World-Disc) mit LoC-Profil starten (experimentell); 0 = abgelehnt wie fremdes Spiel (Standard) */
    char hdd_root_loc[512]; /* W275: Spielstandordner fuer LoC; leer = <hdd_root>_LOC (nie mit SB mischen) */
    char loc_replay[1024]; /* W277 TEST: Tastenfolge "frame:vk,len;..." nur im LoC-Profil (wie replay=); leer = aus */
    int loc_net;          /* W286 LOC-SYSLINK: 1 = Netz-HLE fuer System Link (nur loc_boot=1, nur Loopback/LAN; Standard), 0 = "Netz nicht bereit" wie W275 */
    char loc_net_addr[64]; /* W286: eigene Adresse ("auto" = erste freie aus 127.0.0.2-127.0.0.9; sonst Loopback- oder private LAN-IPv4) */
    int loc_net_port;     /* W286: Portbasis (Standard 1000 = Original: TCP 1000, UDP 1201/1202, VDP 1203 -> Basis+0/+201/+202/+203) */
    char loc_net_peers[512]; /* W286: Mitspieler "ip[:basis],..." ("auto" = 127.0.0.2-127.0.0.9 ohne sich selbst); nur Loopback/privat */
    int loc_net_rtt;      /* W286: gemeldete QoS-Rundlaufzeit in ms (XNetQosLookup, Standard 10) */
    int gpu_pref; /* W370: 0 = Standardadapter (wie bisher), 1 = leistungsstaerkster Grafikchip (DXGI 1.6, Laptops mit zwei GPUs) */
    int loc_net_leave_reset; /* W350: 1 = Stromende (recv 0) eines Mitspielers meldet das Netz-HLE dem Spiel als WSAECONNRESET (Abgang wie XNet, Platz frei; Standard), 0 = recv 0 durchreichen (Geisterplatz) */
    int loc_net_nodelay;  /* W322: 1 = TCP_NODELAY auf den LoC-Spielverbindungen (kein Nagle-Stau ueber Strecken mit Laufzeit; Standard), 0 = Host-Standard */
    char loc_netsim[256]; /* W322 DIAG/TEST: Verzoegerungsstufe im Netz-HLE, z. B. "dsl", "eu-jp,nagle:0", "rtt:300,jitter:20" (leer = aus, Standard) */
    char loc_transport[8]; /* W324 LOC-NETPLAY: "tcp" = Original (Standard), "udp" = eigener UDP-Transport mit Redundanz unter dem Spiel */
    int loc_transport_port;   /* W324: eigener UDP-Port des Transports (0 = frei) */
    char loc_transport_peers[256]; /* W324 TEST: feste Mitspieler "ip:port,..." ohne Treffpunkt (nur Loopback/privat) */
    int loc_transport_proxy;  /* W324: Portbasis der lokalen Stellvertreter der Mitspieler (Standard 21000) */
    int loc_transport_red;    /* W324: Redundanz N (0 = adaptiv 2..8, Standard) */
    char loc_test_netcut[260]; /* W366 TEST: Marker-Datei - solange sie existiert, verwirft der Transport alle UDP-Pakete */
    int loc_transport_gap;    /* W324: Abstand der Kopien in ms (Standard 15) */
    int loc_transport_hold;   /* W324: Jitter-Puffer, Mindest-Haltezeit in ms (Standard 0) */
    int loc_transport_qos;    /* W324: Reserve fuer die gemeldete QoS-RTT in ms (Standard 100) */
    int loc_transport_nodirect; /* W324 TEST: 1 = kein direkter Weg (Relay erzwingen) */
    int loc_route;              /* W395: 1 = Weg je Mitspieler selbst waehlen/wechseln (direkt, alle Relays), 0 = Treffpunkt folgen */
    char loc_route_sim[256];    /* W395 TEST: je Weg Verzoegerung/Ausfall (loctun_cfg.route_sim), leer = aus */
    int loc_transport_open_hold; /* W409: ms, OPEN eines Mitspielers halten, solange das eigene Spiel TCP 1000 nicht annimmt (0 = sofort S_FIN) */
    int loc_test_open_refuse;   /* W409 TEST: ms, eigenes Spiel gilt nach jedem OPEN so lange als "lauscht nicht" (0 = aus) */
    int guest_wx;       /* W339 SEC-A3: W^X fuer das Gastabbild: -1 automatisch (LoC 1, SB 0), 0 aus, 1 strikt, 2 Diagnose */
    int loc_crash_test; /* W339 TEST: N > 0 = N s nach Beginn der Netzsitzung Ausnahme im Gastkontext (Absturz = Ende pruefen); Modus loc_crash_test_mode */
    int loc_crash_test_mode; /* W339 TEST: 0 = Schreibfehler in privatem Code, 1 = Sprung in eine Gast-Datenseite (W^X) */
    char loc_lobby[128];      /* W324: Treffpunkt steelnet "host:port" (leer = System Link wie bisher, Standard) */
    char loc_lobby_room[33];  /* W324: Lobby-Name beim Treffpunkt (Standard "loc") */
    char loc_lobby_pilot[17]; /* W324: Pilotname beim Treffpunkt (Standard "Pilot") */
    char loc_lobby_role[8];   /* W324: "auto" (beitreten, sonst anlegen), "host", "join" */
    char loc_lobby_code[16];  /* W334: Raumcode zum Beitreten (leer = eigene Lobby mit Raumcode anlegen) */
    char loc_lobby_vis[8];    /* W373: "public" = oeffentliche Lobbys (Search Match zeigt alle offenen Spiele); Code geht vor */
    char loc_lobby_spki[65];  /* W334: SPKI-Pin (hex64) eines anderen Treffpunkts als relay.steelbound.dev */
    char loc_relays[256];     /* W324: zusaetzliche Relays "name=host:port,..." (der Treffpunkt nennt seine selbst) */
    int loc_net_diag;     /* W286 DIAG: 1 = alle 10 s Spiel-Threads abtasten (EIP + LoC-Ruecksprungadressen), Standard 0 */
    int loc_sync;         /* W297 DIAG: 1 = Lockstep-Zustandshash je 0x34-Rahmen loggen (LOCSYNC, nur LoC + Netz), Standard 0 */
    char loc_replay_tick[1024]; /* W297 TEST: Tastenfolge "rahmen:vk,len;..." an Lockstep-Rahmen gekoppelt (braucht loc_sync=1), leer = aus */
    char loc_steps[4096]; /* W375: 2048 -> 4096 (drei Runden); W366: 1024 -> 2048 (zweite Runde); W297 TEST: zustandsgesteuerte Testtasten "acc>=1+400>98,8;rx:0xF+150>100,10;sync>=60>32,8;f+50>..." (braucht loc_sync=1) */
    int loc_sync_every;   /* W297: LOCSYNC-Logzeile alle n Rahmen (Standard 150) */
    int loc_pace_diag;    /* W320 DIAG: 1 = Missionstakt FUN_00033050 messen (LOCPACE alle 10 s), Standard 0 */
    int loc_direct_present; /* W320 LOC-PERF: 1 = LoC-Profil praesentiert direkt (interp_fps=0, keine Wiederhol-Presents), Standard 1 */
    int loc_widescreen;   /* W326 LOC-WIDE: Bildformat des Spiels: 0 = 4:3 (Original, Standard), 1 = 16:9 (Spiel meldet WIDESCREEN, 720x480 anamorph), 2 = 720p (1280x720 progressiv, HDTV-AV-Pack) - nur LoC, wirkt beim Start */
    int loc_interp;       /* W323 LOC-INTERP: 1 = Zwischenbilder in der LoC-Mission (20-Hz-Takt, interp_fps bleibt), 0 = wie W320 direkt */
    int loc_interp_rzalt; /* W349 LOC-FRAMES-2: 1 = rawchg-Draws (neue Eingangsdaten) mit dem zur Kamera passenden Partner gleicher Signatur lerpen (Mehrpass-Himmel, Billboards) und reject/rawchg knapp ueber der Matrix-Schwelle per Geometrie-Pruefung (li_camfit) retten, 0 = Stand W340 */
    int loc_ui_diag; /* DIAG(W349): 1 = Farbe/Aufrufkette der LoC-UI-Rahmen loggen (LOCUI rahmen) */
    int loc_interp_dbg; /* DIAG(W349 LOC-FRAMES-2): Bit 1 = exakte Welt-Draws in Zwischenbildern weglassen (Bildfolge zeigt Loecher), Bit 2 = exakte Welt-Draws je Paar loggen */
    int loc_interp_rejcam; /* W340 LOC-FRAMES: 1 = reject-Draws lerpen, wenn M_A^-1*M_B zur Kamera passt (Objekt ruht in der Welt), 0 = exakt */
    int loc_interp_copy;   /* W340 LOC-FRAMES: 1 = BB-Kopie (Backbuffer als Textur) als Interp-Op -> Welt in Zwischenbildern bewegt, 0 = alt (Welt 20 Hz) */
    int loc_interp_anchor; /* W340 LOC-FRAMES: 1 = Zustandszeit am Feld des Tick-Anfangs (erstes Op), 0 = Feld am Swap (W323) */
    int loc_rb_spike;     /* W325 DIAG: Rollback-Machbarkeit messen (1 = Zustand je Tick, 2 = Determinismus-Zyklen), Standard 0 */
    char loc_rb_opts[128]; /* W325 DIAG: Optionen zu loc_rb_spike, z. B. "k=1,2,4,8;render=0;warm=300;max=0;census=1" */
    int loc_rb;           /* W330 LOC-ROLLBACK-R1: 1 = Zustandsring je Tick (inkrementell, Ruhe aller Threads), nur LoC-Profil, Standard 0 */
    int loc_menu_hz; /* W377 LOC-MENU-FPS: LoC-Menues (Lockstep aus, keine Zwischenbilder) mit der Bildrate (interp_fps, gedeckelt durch fps_limit) zeigen: Takt-Thread wiederholt das letzte Spielbild, phasenrichtig zum Spiel-Present, nur bei freier GPU; 1 = an (Standard), 0 = alt (nur Spiel-Presents) */
    int loc_menu_diag; /* DIAG(W377): 1 = Zeile LOCMENU alle 300 Spiel-Presents in LoC-Menues (Anteile in sb_tex_get, Wiederholbilder, ausgelassene) */
    int gpu_burn_menu; /* DIAG(W377): 1 = gpu_burn auch vor dem Present des Spiels (Menues/Ladebilder; langsame GPU nachbilden) */
    int loc_vq_cache; /* PERF(W377): Texturauffrischung (Adressabbildung, Inhalts-Hash): VirtualQuery-Ergebnisse festgeschriebener Regionen je Thread innerhalb desselben Spielbilds wiederverwenden (Adresse in bekannter Region = gleiches Ergebnis); 1 = an (Standard, nur LoC), 0 = alt, 2 = DIAG zusaetzlich direkt abfragen und Abweichungen zaehlen */
    int loc_text_cache; /* PERF(W377): LoC-Textebene (XFONT, CPU-geschrieben): Neu-Upload nur bei geaendertem Inhalt (voller Hash) und in die vorhandene Textur (UpdateSubresource) statt neuer Textur; 1 = an (Standard), 0 = alt */
    int loc_rb_input;     /* W338 LOC-ROLLBACK-R2: 1 = eigene Eingabeschicht (Verzoegerung, Vorhersage, Rollback, Hash-Pruefung) statt
                           * Spiel-Lockstep; braucht den Ring (schaltet loc_rb ein). W346: je Mission ausgehandelt - nur wenn alle
                           * Clients es koennen und wollen, sonst Original-Lockstep. Standard: intern 1, Community 0 */
    int loc_ls_arbiter;   /* LOC-LOCKSTEP-ARBITER: Pufferregler im Original-Lockstep (Rueckfall ohne Rollback): 1 = neu (Vorlauf
                           * nach RTT+Jitter und Stau, Hysterese, kein Rauswurf nach Warteschlange > 45; Standard), 0 = Original.
                           * Es zaehlt der Schalter des Taktgebers (niedrigster aktiver Platz, meist der Host) */
    int im_lock_split;    /* LOC-IM-LOCK: 1 = reine CPU-/Device-Arbeit ohne ctx_lock (Standard): Textur-Quellen der Tick-Liste vor dem
                           * Swap einordnen, im Snap alle 64 Ops dem wartenden Present-Takt die Sperre geben (sb_ctx_yield) und Inhalts-Hashes der
                           * Texturpruefung (Refresh, Lockwatch, Textebene) bei abgegebener Sperre wie der Upload (W138) -> der
                           * Present-Takt wartet seltener; 0 = alt (alles unter der Sperre). Bild unveraendert */
    int imlock_diag;      /* LOC-IM-LOCK DIAG: 1 = Zeile IMLOCK je 10 s (Wartezeit des Present-Takts auf ctx_lock, Halter je
                           * Aufrufstelle mit CPU-Anteil, Teilzeiten langer sb_render_im_end) */
    int interp_p2rej;     /* OPTION(W406 M3-ROAD-JITTER): 1 = vorab transformierte Welt-Draws (P2) mit gleichen Eingangsdaten, die nur wegen eines Bezugs-Vertex nahe der Augenebene an ip_plausible scheitern, im Zwischenbild lerpen (Bezugs-Vertices mit |ndc| <= 2); 0 = alt (exakt aus Tick N) */
    int loc_interp_rawrzb; /* W410 LOC-GFX-JITTER: "rawlerp" (gleiche Signatur, neue Eingangsdaten, Vertex i <-> i) eines in der Welt
                            * ruhenden Draws (M_A^-1*M_B passt zur Kamera des Ziels) als Matrix-Lerp der Tick-N-Vertices zeichnen
                            * (rzb): neu gebaute Gelaende-Netze (~9000 Vertices) ordnen Vertex i je Tick anderen Weltpunkten zu ->
                            * Keile/Loecher. 1 = nur LoC (Standard), 2 = auch SB, 0 = aus */
    int interp_p2hom;     /* FIX(W429 M3-ROAD-FIX): 1 = P2 im RT (Strom schon durch w geteilt) im Zwischenbild homogen lerpen
                           * (echte Clip-w aus dem W237-Nebenpuffer, interp_hlerp-Modus 3 wenn interp_hlerp=0) mit derselben
                           * Regel je Vertex wie der 3D-Pass (nur W224-w-Wechsel), und Welt-Draws mit GLEICHEN Eingangsdaten,
                           * die nur an Bezugs-Vertices ausserhalb des Bildes bzw. hinter der Kamera scheitern (reject), lerpen;
                           * nur SB (LoC unveraendert); 0 = alt */
    int seam_diag;        /* DIAG(W406 M3-ROAD-JITTER): N = Fugen-Messung zwischen Nachbar-Draws im Zwischenbild (SEAM/SEAMW, erste N Ticks mit Fund; SEAMSUM je 200 Ticks, nur sichtbare Ecken), braucht interp_diag>=3; 0 = aus */
    int loc_wlag_fix;     /* W412 LOC-WORLD-LAG: 1 = Himmel (Welt-Draw mit eigener Tiefe) mit derselben vorhergesagten Kamera wie die Welt (nur LoC, nur mit Kamera-Versatz W385/W390), 0 = aus (alt) */
    int loc_wlag_diag;    /* DIAG(W412 LOC-WORLD-LAG): Bit 1 = Zeile LOCWLAG alle 10 s (Kamera je Draw-Klasse, Versatz in Grad), Bit 2 = Matrizen nicht-affiner Draws (LOCWLAG naff), Bit 4 = W435 Zeile LOCWLAG sicht (Himmel/nicht-affine Draws in Wiedergaben mit Sicht-Korrektur), 0 = aus */
    int loc_sky_fix;      /* W435 LOC-SKY-LAG-REG: 1 = Himmel (Weltkoordinaten, eigene Tiefe) bekommt auch die Sicht-Korrektur je Bild (W376/W394, o->law 1) wie die Welt (nur LoC), 0 = aus (alt) */
    int loc_ret_fix;      /* W439 LOC-RETICLE-JUMP: 1 = Fadenkreuz-Draws, die im Zwischenbild exakt aus Tick N gezeichnet werden, bekommen die Verschiebung Soll - Mitte(B) statt Soll - lerp(A, B, t) (sonst je Tick um (1 - t) * Tickschritt daneben), nur LoC mit Zwischenbildern; 0 = alt */
    int ctrl_despike;     /* W439 LOC-RETICLE-JUMP (#78), Standard 1 (Nutzerentscheid 22.11., bewusste Ausnahme von "Abweichung = Experimental"): 1 = Ausreisser-Filter fuer analoge Achsen des Original-Controllers (SBC/USB) und von Joysticks (input_despike.h: Einzelwert mit Sprung > Schwelle wird verworfen, wenn der naechste Report zurueckkehrt; normale Bewegung unverzoegert; Gamepad/Maus ungefiltert), 0 = Original */
    int loc_ret_diag;     /* DIAG(W439 LOC-RETICLE-JUMP): Bit 1 = je Wiedergabe Fadenkreuz (Soll/gezeichnet), Sicht, Ereignisse nach locret_rep.bin (nur Testfolge), Bit 2 = Zeile LOCRET alle 10 s, Bit 4 = je Liste Fadenkreuz-Kandidaten nach locret_cand.txt (W444, nur Testfolge); 0 = aus (Sprung-Zaehler fuer F11 laeuft immer, nur Zaehler) */
    int loc_ret_mark;     /* W444 LOC-RETICLE-MARK (#78, Ursache 2): 1 = Fadenkreuz-Teile ueber Form-Identitaet zur Vorliste erkennen (loc_ret_mark.h; sonst verwarf die Bewegungs-Pruefung gegen die Formel-Mitte beim Drehen Teile, die dann ohne Sicht-Korrektur versetzt erschienen), nur LoC mit Zwischenbildern; 0 = alt (W376/W439) */
    int loc_tree_fix;     /* W506 ISSUE-105: 1 = in Zwischenbildern kleine texturierte Welt-Draws mit eigener Matrix (Baeume, <= 64 Vertices), die exakt blieben oder deren Partner nicht zur Kamera der Welt passt, mit der Kamera der Welt bewegen (rzm, nur LoC); 0 = alt (Baeume stehen im Tick-Takt bzw. gleiten vom falschen Partner) */
    int loc_stat_fix;     /* W415 LOC-STATIC-LAG: 1 = in Zwischenbildern die Kamera der Welt (haeufigste gleiche Matrix im Weltziel, bestaetigt durch gelerpte Draws) fuer mit der Kamera bewegte Gelaende-Netze (W410 rzm) und fuer partnerlose Draws in Weltkoordinaten (grosses Bodennetz, nomatch) nehmen (nur LoC); 0 = alt (W410-Kamera bzw. exakt) */
    int memreg_keep;      /* W417 SB-BRIEF-BLACK (#66): 1 = Eintraege, die der volle Register-Ring (8192) verdraengt, als Bereiche behalten; Adressabfragen (Texturkopf-Pruefung) finden sie weiter, solange der Speicher zugesagt ist (vorher nach langen Laeufen Film im Briefing schwarz, Grund 1); 0 = alt */
    int loc_rule_clipdump_block; /* W420 LOC-CLIPDUMP: Host-Regel "Clip dumping unterbinden" (1 = Magazinwechsel setzt die Feuerpause der Hauptwaffe nicht zurueck; Standard 1, gilt im Match nach der Einstellung des Hosts, loc_rule.c) */
    int loc_chat;         /* W437 LOC-CHAT: Textchat in LoC online (Launcher "Chat", Standard 1; 0 = weder Anzeige noch Senden) */
    int loc_chat_pos;     /* W508 CHATBAR: Lage des Chats - 0 = auto (Standard: in den schwarzen Seitenbalken, wenn breit genug), 1 = immer ueber dem Bild (unten links wie bis 0.6.0) */
    int loc_chat_direct;  /* W437 TEST: Chat auch im eigenen Transport ohne Treffpunkt (loc_transport_peers, Testschluessel) */
    char loc_chat_test[1024]; /* W437 TEST: Chat-Testfolge "ms:O;ms:T<Text>;ms:E" (Nachrichten an das eigene Fenster) */
    int loc_play_mode;       /* W440 LOC-LOBBY-MODES: Modus eigener steelnet-Lobbys (Host): 1 = sim (Standard), 2 = arcade
                               * (LAS_*, loc_assist.h); Launcher "Lobby mode" -> loc_play_mode=sim|arcade */
    int loc_snchat;          /* W446 STEELNET-CHAT: steelnet-weiter Chat (Launcher "steelnet chat", Standard 1, getrennt von loc_chat) */
    char loc_snchat_test[1024]; /* W446 TEST: Testfolge ab steelnet-Chat verfuegbar (Syntax wie loc_chat_test, zusaetzlich K = Tab) */
    int loc_chat_mission_sn;    /* W474: steelnet-Chat waehrend des Matchs (Launcher W473, Standard 0: kein Leuchten/Zaehler im Spiel) */
    int loc_chat_mission_lobby; /* W474: Lobby-Chat waehrend des Matchs (Standard 1) */
    int loc_chat_mission_team;  /* W474: Team-Chat waehrend des Matchs (Standard 1; ohne Team-Reiter wirkungslos) */
    int loc_lobby_join_delay; /* W446 TEST: Lobby erst N ms nach dem welcome betreten/anlegen (0 = sofort) */
    int loc_lobby_resume;     /* W500: Abriss der Treffpunkt-Verbindung -> Sitzung fortsetzen (1 = Standard, 0 = alter Neuaufbau) */
    int loc_ipv6;             /* W521: eigene globale IPv6-Adresse als Durchstich-Kandidat, IPv6 zuerst (1 = Standard, 0 = nur IPv4) */
    int loc_punch_legacy;     /* W521 TEST/Gegenprobe: 1 = Durchstich wie vor W521 */
    int loc_host_migration;   /* W520 HOSTMIG (Experimental, Standard 1): eigene Lobbys erlauben den Host-Wechsel; Neuaufbau der
                               * Spiel-Session nach host_changed. 0 = Lobby endet mit dem Host wie bisher */
    int loc_auto_host;        /* W520: eigene Lobbys mit Auto-Host (Uebergabe an den stabilsten Spieler vor dem Match; Standard 0;
                               * W523: wirkt nur mit loc_hostmig_rebuild=1) */
    int loc_hostmig_rebuild;  /* W523 HOSTMIG-SCOPE TEST/0.7.0 (Standard 0): Neuaufbau der Spiel-Session ueber die Menues nach
                               * einem Host-Wechsel in der Lobby bzw. nach dem Match und Auto-Host. 0 = 0.6.3: nur Hinweis, das
                               * Match laeuft weiter (Host-Wechsel im Match), Lobby bleibt bei steelnet, Neuaufbau von Hand */
    int loc_hostmig_test;     /* W520 TEST: Bilder aus den Ablaeufen (Taste *) */
    char loc_hostmig_prog_host[400]; /* W520 TEST: anderer Ablauf fuer den neuen Host ("" = eingebaut) */
    char loc_hostmig_prog_join[400]; /* W520 TEST: anderer Ablauf fuer die Mitspieler */
    char loc_hostmig_cont_host[700]; /* W520 TEST: Testfolge (loc_steps) nach gelungenem Neuaufbau als neuer Host */
    char loc_hostmig_cont_join[700]; /* W520 TEST: Testfolge nach gelungenem Neuaufbau als Mitspieler */
    int loc_lobby_cut;        /* W500 TEST: Steuerverbindung N s nach jedem (Wieder-)Aufbau abreissen (0 = aus) */
    int loc_lobby_cut_mode;   /* W500 TEST: 0 = harter Reset, 1 = still liegenlassen (Dienst merkt nichts) */
    int loc_live;             /* W479 LIVE-LOGON: 1 = LoC im Xbox-Live-Modus (XOnline-HLE, steelnet als Live-Dienst, braucht Treffpunkt + Konto; docs/loc/LOC_LIVE.md), 0 = System Link wie bisher (Standard) */
    int loc_camp;             /* W491 CAMP-CLIENT: 1 = Kampagne im Xbox-Live-Modus anbieten (Standard; Ticket + Steelbound-Zugang, docs/loc/LOC_CAMPAIGN.md), 0 = Campaign bleibt grau */
    char loc_camp_id[33];     /* W491: "" bzw. "season" = Produktivsaison steelbound-s1 (47100), "test" = Testkampagne steelbound-test (47103) */
    char loc_camp_host[128];  /* W491: Zugang (Hostname), "" = Treffpunkt-Host (relay.steelbound.dev) */
    int loc_camp_port;        /* W491: Zugangsport, 0 = nach Kampagne */
    char loc_camp_spki[65];   /* W491: zusaetzlicher Pin (hex64) NUR fuer einen lokalen Test-Zugang (Loopback) */
    int loc_camp_faction_swap;/* W515 FACTIONSWAP (#110): 1 = Enlistment Office im Kampagnen-HQ in jedem Zug offen (Seitenwechsel jederzeit; Original nur Zug 3-6, FUN_000EF6B0 @0xEF73B), 0 = Original; nur loc_live=1 + loc_camp=1 */
    int loc_camp_min_side;    /* W492: Mindestzahl Piloten je Seite (1..3); 0 = nicht gesetzt -> Beta-Standard LOC_CAMP_BETA_MIN_SIDE (1); -1 = Original (Schluessel 0: Kampagne 3, sonst 1; FUN_000FC4B0); nur loc_live=1 */
    int loc_lan;              /* W454 LOC-LAN-MODE: 1 = LAN-Modus ohne steelnet (Entdeckung im lokalen Netz, mit loc_transport=udp) */
    char loc_lan_room[17];    /* W454: Raumname (leer = "LAN") */
    char loc_lan_pass[64];    /* W454: Raumpasswort (besser Umgebung SB_LAN_PASS, wie der Launcher; leer = ohne) */
    int loc_lan_port;         /* W454: Entdeckungs-Port (0 = 47210) */
    char loc_lan_targets[128];/* W454 TEST/Notweg: Unicast-Ziele "ip:port,..." (nur privat) */
    char loc_census_ticks[128]; /* W448 DIAG: Draw-Zensus (m3d_census + loc_census_from) an diesen Lockstep-Rahmen ("2000,2030") */
    int loc_shot_tick_from;     /* W448 TEST: loc_shot_tick_every erst ab diesem Spieltick (0 = ab Missionsbeginn) */
    int loc_interp_eyeblind;    /* FIX(W448 LOC-CHAFF-LOOK, #83): 1 = Netz (> 8 Vertices, gleiche Eingangsdaten), dessen 4 Bezugs-
                                 * Vertices in beiden Ticks hinter der Kamera liegen, im Zwischenbild exakt statt Matrix-Lerp
                                 * (Chaff-Huelsen an der Augenebene -> grosse harte Vierecke); nur LoC, Standard 1, 0 = alt */
    int loc_play_mode_test;  /* W440 TEST: Modus ohne Treffpunkt (System Link/loc_transport_peers): 0 = aus (Sim), 1 sim, 2 arcade */
    int loc_lobby_list;      /* W443 LOC-LOBBY-LIST: 1 = Marke SIM/ARCADE je Zeile der Suchliste (Search Match) und Modus/Ping-Schaetzung im Detailfeld (nur LoC, oeffentliche steelnet-Lobbys; Standard 1), 0 = aus */
    int loc_lobby_list_test; /* W443 TEST: 1 = Bild (mit Anzeige) und Logzeilen je stabiler Listenansicht (hoechstens 6) */
    int loc_aa_diag;         /* DIAG(W442 LOC-AIMASSIST): 1 = Zeile LOCAA-Diag je 5 Ticks (Sicht/Arm-Zustand, nur Testfolge); 0 = aus */
    int loc_net_auto;        /* W447 LOC-NET-AUTO: 1 = Netzmodus Auto (Standard): Profil Direkt/Rollback je Mission nach Ping
                               * (loc_rbn.h), nur wenn alle Auto wollen; 0 = Rollback fest (W386). Lockstep = loc_rb_input=0.
                               * Kurzform loc_net_mode=auto|rollback|lockstep */
    int loc_net_panel;       /* W447: 1 = kleines Panel in der Lobby (Netzmodus, Profil, Verzoegerung, Ping je Spieler, Modus) */
    int loc_lobby_widget;      /* W460 LOC-LOBBY-WIDGET (#95): 1 = Widget offene Lobbys im Hauptmenue/MatchMaking (rechts) */
    int loc_lobby_widget_test; /* W460 TEST: 1 = Bilder des Widgets, 2 = Widget ohne Menueerkennung (Lage pruefen) */
    int loc_lobby_widget_diag; /* W460 DIAG: 1 = Szene/Menue-Woerter bei Aenderung loggen (Seitenerkennung) */
    int loc_net_panel_test;  /* W447 TEST: 1 = Bilder des Panels (W453: hoechstens 6, alle 10 s solange sichtbar), 2 = Panel auch ohne Mitspieler (Lage pruefen) */
    int loc_replay_help;     /* W457: 1 = Bedienhilfe im LoC-Replay-Viewer beim Start sichtbar (F1/replay_help schaltet) */
    int loc_replay_ctrl_test;/* W457 TEST: 1 = Titel-Demo als bedienbarer Replay-Viewer (Demo-Flag aus) + Bilder bei Zustandswechseln */
    int loc_clip_diag;    /* W420 DIAG: Zeile LOCCLIP je Sekunde (Schuesse, Waffe, Magazine, Feuerpause des eigenen VT) */
    int loc_insignia_rt;  /* W422 LOC-INSIGNIA-PEN: 1 = Stift-RTs des Insignien-Editors (64x64 [[0x57C6B8]+0x694], 384x16 +0x6BC) teilen den Speicher mit der CPU wie auf der Xbox: vor jeder Session Xbox-Speicher -> RT, danach RT -> Xbox-Speicher bis zum BlockOnFence; dazu CPU-beschriebene Editor-Texturen (Flaeche [obj+0x4D0], Muster, Datei-Vorschauen) je Bild voll auf Aenderung pruefen (Standard); 0 = alt (Stift setzt nichts, Cursor-Spur, Zeilen 16-47 der Flaeche nie sichtbar); nur LoC-Profil */
    int sb_allied_squads; /* W424 SB-ALLIED-SQUADS: 1 = verbuendete Trupps (und Feinde in ihrer Naehe) werden auch fern vom Spieler simuliert (Thunk an 0x5CED9; W426: erst nachdem das Original die verbuendete Einheit einmal aktiv geschaltet hat, danach bis Missionsende; wirkt ab der naechsten Mission; Standard 1), 0 = Original (eingefroren ausserhalb der Spielerzone, kein Patch); nur SB-Profil */
    int sb_ally_diag;     /* DIAG(W424): Zeile SBALLY alle 300 Spielbilder (Einheiten, Original aktiv, Zusatz verbuendet/Feind) und Verbuendeten-Lage */
    int loc_ins_diag;     /* DIAG(W422): Zeile LOCINS alle 30 Bilder im Insignien-Editor (Zustand, Werkzeug, Cursor, Stift, Pixel in Flaeche/Ebene/RT-Speicher); 0 = aus */
    int loc_zcache_alt;   /* W423 LOC-XSB-LOOP (#71): 1 = ist der Datei-Cache Z: (<hdd_root_loc>\ZCACHE) nicht beschreibbar, weicht Z: auf einen Ersatzordner aus (%LOCALAPPDATA%\Steelbound\loc_zcache bzw. %TEMP%\Steelbound_loc_zcache, Kurzpfad); sonst kopiert LoC media\sndeff endlos und haengt am Dolby-Logo (Standard); 0 = alt; nur LoC-Profil */
    int loc_boot_nopause; /* W423 LOC-XSB-LOOP: 1 = ESC-Menue, Alpha-Hinweis und Pause-Taste halten das Spiel in LoC erst an, wenn der Start-Kopiervorgang nach Z: fertig ist (Boot laeuft weiter, Standard); 0 = alt; nur LoC-Profil */
    int loc_save_alt;     /* W434 LOC-PILOT-LOST (#73): 1 = ist der LoC-Spielstand-Ordner (<hdd_root_loc>: TDATA/UDATA/ZCACHE) nicht beschreibbar, laufen alle Spielstaende ueber den festen Ersatzordner %LOCALAPPDATA%\Steelbound\loc_save (Kurzpfad, nie %TEMP%; einmalige Uebernahme vorhandener Staende, bleibt aktiv solange seine Merkdatei auf diesen Ordner zeigt); sonst schreibt LoC Piloten still ins Leere (Standard); 0 = alt (nur Meldung); nur LoC-Profil */
    int loc_save_selftest; /* DIAG(W434): Selbsttest Spielstand-Ordner (beschreibbar unveraendert, nicht beschreibbar -> Ersatzordner uebersteht Neustart, beide gesperrt -> klare Meldung), synthetische Ordner unter hdd_root, vor dem Spiel */
    int loc_boot_diag;    /* DIAG(W423): LOCBOOTDIAG-Zeilen: Rueckgabewerte von Oeffnen/Lesen/Seek fuer media\sndeff und Z:, CreateSoundBank, Wiederholzaehler, Pause-Zustand in der Startphase (je Art begrenzt); 0 = aus */
    int memreg_diag;      /* DIAG(W417 SB-BRIEF-BLACK): 1 = Speicherregister messen (MEMREG-Zeilen: Fuellstand je Art alle 1024 Eintraege, Ring voll, verdraengte grosse Eintraege) und bei Textur-Ablehnung Grund 1 Lage der Adresse (VirtualQuery, Register-Nachbarn, verdraengter Eintrag); 0 = aus */
    int loc_stat_diag;    /* DIAG(W415 LOC-STATIC-LAG): Bit 1 = Zeile LOCSTAT alle 10 s (Versatz jedes Welt-Draws im Zwischenbild gegen die Kamera der Welt an derselben Stelle, Pixel je Klasse), Bit 2 = groesste Einzel-Draws (LOCSTAT top), Bit 4 = Bezug Mehrheits-D, Bit 8 = exakte Draws mit Versatz (LOCSTAT ex), Bit 16 = Tickweg je Kamera-Kandidat (LOCSTAT paar); 0 = aus */
    int loc_sndloop_fix;  /* W413 LOC-SOUND-LOOP (#65): 1 = nach jedem Neurechnen Toene stoppen, die die korrigierte Zeitlinie nicht mehr gestartet bzw. schon gestoppt hat (nachgeholte Waisen, Index-/Quellen-Stopps, Sicherheitsnetz), nur LoC-Rollback; 0 = alt (nur W375-Waisen) */
    int loc_sndloop_grace; /* W413: Sicherheitsnetz - Schleifen-Cue aus einer Neurechnung endet nach N Live-Ticks ohne Besitzer (Handle-Ablage des Spiels); 0 = Netz aus. Standard 30 */
    int loc_audio_loopcount; /* W413 LOC-SOUND-LOOP (#65): 1 = Schleife einer Spur nach der XSB-Schleifenzahl (direkte Eintraege und Spuren mit 0 einmal, 0xFFFF endlos, n endlich), 0 = alt (Schleifen-Flag der Welle; MG-Ausklang lief endlos) */
    int loc_audio_3d_xsb; /* W456 BOOTSND (#93): 1 = Min-/Max-Abstand der 3D-Cues aus der XSB-3D-Tabelle (Satz je Eintrag, Bank.xsb: Min 100/Max 100000 bzw. Min 1) wie die XACT-Bibliothek, 0 = alt (fest 8/1000 - ferne Toene anderer VTs bis ~22 dB zu leise) */
    int loc_audio_eg; /* W456 BOOTSND (#93): 1 = Huellkurven-Ereignisse der Bank.xsb (Typ 0x0A = SetEG Amplitude) anwenden und Stop ohne Flag 2 mit Release ausklingen lassen wie die XACT-Bibliothek, 0 = alt (ohne Huellkurve, Stop sofort) */
    int loc_sndloop_diag; /* DIAG(W413): 1 = Zeilen LOCSNDLOOP (je verwaistem Cue, je Index-/Quellen-Stopp-Luecke, Netz, alle 10 s aktive Schleifen), 0 = aus */
    int loc_lampdiag;     /* DIAG(W405 LOC-LAMPS): 1 = Cockpit-Lampen im LoC-Atlas beobachten (LAMPD), 2 = + Knoten/VT (loc_lampdiag.c), 0 = aus */
    char loc_ls_opts[128]; /* LOC-LOCKSTEP-ARBITER DIAG: Reglerwerte "lmin=2,drop_rmin=2,..." (loc_lsarb.h), leer = Standard */
    int loc_rb_delay;     /* W338: Eingabeverzoegerung in Ticks mit loc_rb_input (1..8); W346: 0 = adaptiv nach Umlaufzeit (Standard) */
    int loc_fields;       /* W320 TEST: Felder je Missionsbild statt 3 (aendert Spielgeschwindigkeit, nur Messung), 0 = Original */
    int loc_pad_keys;     /* W286: 1 = Tastatur auch auf das virtuelle Menue-Gamepad (Ziffernblock 8/2/4/6, SPACE/0, Backspace, Enter; Standard), 0 = Pad nur anwesend (neutral) */
    int loc_mouse;        /* W341 LOC-MOUSE: 1 = Maus zielt in der LoC-Mission wie in SB (Fangen, sobald der eigene VT im Lockstep abgesetzt ist; Standard), 0 = altes Verhalten (Maus nie gefangen, nur Tasten) */
    int loc_mouse_test;   /* W341 TEST: 1 = (mit input_test_hook=1) synthetische Maus ueber den echten Fenster-/Rohdatenpfad auch bei Testfolge; Pseudo-Tasten 330-333 = Bewegung +x/-x/+y/-y, 334/335 = links/rechts, 336 = Bild */
    int loc_local_aim;    /* W376 LOC-LOCAL-AIM: 1 = eigene, noch nicht simulierte Eingabe (Verzoegerung D) sofort auf Kamera/HUD beim Zeichnen anwenden (nur Netzspiel mit Rollback-Eingabeschicht, Spielzustand unberuehrt), 0 = aus (Standard) */
    int loc_aim_diag;     /* W376 DIAG: Bit 1 Spielerblock je Tick, Bit 2 Kamera-Matrix je aufgezeichnetem Bild, Bit 4 .data-Dump (Slots loc_aim_diag_from..+n), Bit 8 Draw-Zensus, Bit 16 Logzeilen der Vorhersage */
    int loc_rb_smooth;    /* W385 LOC-SMOOTH (Standard 1): 1 = Rollback-Korrekturen anderer VTs (und der eigenen Kamera, wenn sie selbst springt) im Zwischenbild-Pfad abklingend ueberblenden (nur Anzeige, Simulation/Hash unberuehrt), 0 = sofort springen */
    int loc_rb_smooth_ms; /* W385: Dauer des Abklingens in ms (Standard 150, 50..500; je 50 ms eine Liste) */
    int loc_rb_smooth_r;  /* W385: Radius Draw -> VT in Spieleinheiten (Standard siehe hcfg.c) */
    int loc_rb_smooth_max; /* W385: groessere Spruenge (Spieleinheiten) springen sofort (Neustart, Teleport) */
    int loc_rb_smooth_diag; /* W385 DIAG: Bit 1 Zeile je Rollback (Lagen-Spruenge, nur mit Testfolge), Bit 2 Render-Zensus der verschobenen Draws */
    int loc_smooth_opp;     /* W387 LOC-SMOOTH-OPP (Standard 1): andere VTs leicht in der Vergangenheit zeigen (nur Anzeige): 0 aus, 1 auto (0..loc_smooth_opp_max Ticks nach sichtbaren Korrekturen), 2 fest 1 Tick */
    int loc_smooth_opp_max; /* W387: Obergrenze auto in Ticks (Standard 2, 1..2) */
    int loc_smooth_opp_vis; /* W387: Sprung ab dem eine Korrektur sichtbar zaehlt (Spieleinheiten ~ mm bei 10 m Abstand, weiter weg proportional; Standard 20) */
    int loc_own_predict;    /* W390 LOC-OWN-PREDICT: eigene VT-Lage per spekulativem Vorlauf D+1 Ticks voraus zeigen (nur Anzeige, Simulation unveraendert): 0 aus, 1 an (Standard) */
    int loc_own_predict_ticks;  /* W390 TEST: Vorlauf-Ticks fest (0 = D + 1) */
    int loc_own_predict_max_ms; /* W390: Kostengrenze je Vorlauf (p95, ms; Standard 5) - darueber 30 s Pause, nach 3 Mal aus fuer die Mission */
    int loc_own_predict_prep;   /* W390 TEST: Render-Vorbereitung im Vorlauf ausfuehren (Standard 0: ausgelassen, ohne Belang fuer die Lage) */
    int loc_own_predict_diag;   /* W390 DIAG: Bit 1 Schatten-Pruefung nach jedem 50. Zuruecksetzen, Bit 2 Lagen-Datei je Tick (nur mit Testfolge), Bit 4 Zeile je Korrektur */
    int loc_pd_spec;        /* W393 LOC-PACING: 1 = Present-Takt laeuft waehrend des spekulativen Vorlaufs weiter (nie warten, Standard), 0 = alt (W390: Ruhe bis 4 ms abwarten, sonst Bild auslassen) */
    int interp_pace_clock;  /* W393: stetige Bilduhr - Latenz L nur weich nachgefuehrt statt Spruengen (1 = LoC, Standard; 2 = auch SB; 0 = alt) */
    int pace_vsync;         /* W393: mit present_interval=1 Zwischenbild-Takt an den VBlank des Monitors koppeln (1 = LoC, Standard; 2 = auch SB; 3 = LoC auch mit VSync aus im Fenster; 0 = nur Timer) */
    int pace_diag;          /* W393 DIAG frame_pace_diag (Schluessel; nicht verwechseln mit loc_pace_diag): 1 = Zeile PACE je 10 s (Bildabstand p50/p99, ausserhalb +-1 ms, Auslassungen, doppelte Phase, Bilduhr) */
    int frame_pacing;       /* W468 PERF-PACING (Launcher, neutral): 0 = Timer (alt, Zwischenbilder nach QPC ohne Monitorbezug), 1 = Monitor (Standard: SB im Fenster/randlos mit VSync aus Takt auf das VBlank-Raster, wenn interp_fps die Monitorrate ganzzahlig teilt), 2 = VRR (Flip-Modell + Tearing erlaubt, fuer G-Sync/FreeSync; Timer) */
    int pace_log;           /* W468 DIAG: je gezeigtem Zwischenbild QPC + gezeigte Spielzeit in pace_log.csv (Logordner), hoechstens N Eintraege (0 aus) */
    int pace_sim_mhz;       /* W468 TEST pace_sim_hz: VBlank-Thread nutzt statt des Monitors einen kuenstlichen Takt (milli-Hz, z. B. 165000; 0 = echter Monitor) */
    int swap_flip;          /* W468: 1 = Flip-Modell-Swapchain (FLIP_DISCARD, 2 Puffer) mit dauerhaftem Zwischenziel; von frame_pacing=2 gesetzt; 0 = Blit (SEQUENTIAL, bisher) */
    int interp_pace_up;     /* W393 TEST: Bilduhr - hoechste Verlangsamung in Promille, wenn L steigt (Standard 200) */
    int interp_pace_hold;   /* W393 TEST: 1 = elastisches Halten (Bilduhr dehnt sich beim Halten; Messung: mehr gehaltene Bilder, Standard 0) */
    int interp_pace_dn;     /* W393 TEST: hoechste Beschleunigung in Promille, wenn L faellt (Standard 20) */
    int loc_hud_predict;    /* W394 LOC-HUD-VIEW: HUD-Marken mit Weltbezug (Zielmarken, Lock-On-Rahmen, Kompass, Radar) zur vorgezeichneten eigenen Kamera zeichnen (nur Anzeige): 0 aus, 1 an (Standard) */
    int loc_view_perframe;  /* W394: Sicht/Fadenkreuz je Bild aus Maus bzw. Hebel vorzeichnen (nur Anzeige, Simulation tastet weiter je Tick ab): 0 aus, 1 an */
    int loc_vpf_repeat;     /* W505 ISSUE-106: mit Eingabeprofil (keine Legacy-Belegung) Vorschau-Rahmen = letzter eigener Satz, damit die Sicht je Bild weiterlaeuft statt in Tick-Stufen: 0 aus (alt), 1 an (Standard) */
    int loc_vpf_test_profile; /* W505 TEST (nur mit Testfolge): Vorschau wie mit Eingabeprofil (Legacy-Akku-Vorschau aus), Vergleich alt/neu mit Legacy-Testfolgen */
    int loc_hud_diag;       /* W394 DIAG: Bit 1 HUD-Zensus mit Gegner im Bild (nur mit Testfolge), Bit 2 Zeilen LOCHUD/LOCVPF, Bit 4 Bild-Datei je Wiedergabe (nur mit Testfolge) */
    int loc_vpf_test;       /* W394 TEST: Pseudo-Taste 337 -> Mausschritte (Counts je 5 ms) ab zufaelliger Tickphase (nur input_test_hook + loc_mouse_test + Testfolge) */
    int loc_vpf_test_alt;   /* W394 TEST: 1 = loc_view_perframe je Gruppe von 4 Testschritten abwechselnd aus/an (Vergleich im selben Lauf) */
    int loc_dodge_diag;   /* W380 DIAG: Seitenschritt-Zaehler LOCDODGE (Bit 1 Zaehler, Bit 2 je Versuch), nur mit Testfolge */
    int loc_aim_diag_from, loc_aim_diag_n; /* W376 DIAG: Slot-Fenster fuer Bit 4/8 */
    int iso_selftest;     /* W150 DIAG: 1 = D:-Selbsttest (Lesen, Schreibschutz) vor dem Spielstart */
    int save_check;       /* W187 KIA-LOAD: Spielstaende (T:) beim Start pruefen: 1 = reparieren (mit Sicherung), 2 = nur melden, 0 = aus */
    int save_lock;        /* W190 SAVE-LOCK: 1 = Spielstaende schuetzen (T:/U: in eine Wegwerf-Arbeitskopie <hdd>\_savelock, beim naechsten Start verworfen), 0 = Original (Standard) */
    int savelock_selftest; /* W190 DIAG: 1 = Selbsttest Pilot-Loeschen/death00 ueber die Nt*File-Wege vor dem Spiel */
    int host_dir_enum;    /* W241 REPLAY-LIST: 1 = NtQueryDirectoryFile listet Host-Ordner (Replays U:), 0 = alt (M1: immer NO_MORE_FILES) */
    int directory_selftest; /* W213/W241 DIAG: Selbsttest Ordner-Auflistung (synthetisch) */
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
    /* [log] */
    int level;
    int syscall_trace; /* 1 = jeden Kernel-Call loggen (Diagnose, langsam) */
    int hle_arity_check; /* DIAG(ARITY,W162): 1 = HLE-Arity-Selbsttest (Ord 87/203 durch echte Thunk-Slots, ESP-Pruefung) + Aufruf-Zaehler */
    int hlog_keep;     /* 1 = zweites hlog_init trunkiert nicht (cfg-Zeilen bleiben), 0 = alt */
    int diag_states; /* DIAG(STATES): Modus/Szene/Missionsflag bei Aenderung + Heartbeat (A3) */
    int state_diag;  /* DIAG(STATE): kompakte Missions-Zustandszeile 1x/s (A11c) */
    int state_every; /* DIAG(STATE)-Intervall in ms (<=0 = 1000) */
    int startup_diag; /* DIAG(STARTUP): Balken/Linie/Stufe/Engine pro Intervall (W21-R2) */
    int combat_diag; /* DIAG(COMBAT): Entity-HP/Schaden pro Intervall (W35) */
    int cam_diag; /* DIAG(CAMD,W269): Kamera/View/Proj gegen eigene VT-Lage alle N Missions-Frames, 0 = aus */
    int campaign_diag; /* DIAG(CAMPAIGN): Missionsziel/Phase/Ergebnis/Tutorial-Regie (W63) */
    int gear_diag; /* DIAG(GEAR): Block-Gang/Out+0xB/Mapper-Latch/Position pro Intervall (W141) */
    int aimat_antiwindup; /* W63: aimat-Schirmregler mit Anti-Windup (1) bzw. alt (0) */
    int poll_census; /* DIAG(POLLCENSUS,W59): 48CF0-Aufrufer-Zensus (RA/Port/Gruppe/Button, Treffer != 0) */
    int nullcall_diag; /* DIAG(NULLCALL): Sprung auf 0: Aufrufer+SEH je einmal, 0 = alter VEH-Dump */
    int contig_fast; /* PERF(PERF-HUD,W122): 1 = low_alloc (MmAllocateContiguousMemory) ueberspringt belegte Allokationen am Stueck (gleiche Adresse, viel weniger VirtualQuery), 2 = Pruefmodus (alte Suche vergleicht, Zeile LOWALLOC), 3 = EXPERIMENT Merkpunkt (nicht exakt), 0 = alt */
    int veh_oob_cmp; /* FIX(OOB-CMP,W321): 1 = Lese-AV in Spielcode, deren Seite BULKFIX nicht anlegen kann: reiner Vergleich cmp [mem],imm wird mit Speicherwert 0 ausgefuehrt (= Ergebnis einer frisch angelegten Seite; M07-Skript 0x4AD6E, GitHub #35), 0 = alt (Absturz) */
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
    int host_hi_addr;      /* FIX(TEX-WHITE-AMD,W315): 1 = Host-Adressen ab 2 GB (Prozess /LARGEADDRESSAWARE, Contig-HIGH-Bloecke) fuer Texturkoepfe/-daten und Pixelshader-Definitionen gueltig (bis 0xFFFE0000); 0 = alt (ab 0x80000000 verworfen -> Textur nie hochgeladen, Flaeche weiss/Vertexfarbe) */
    int contig_hi_place;   /* FIX(ISSUE-TEX, Steelbound#37/#42/#43): 1 = scheitert die gezielte Contig-HIGH-Suche unter 2 GB, gezielt oberhalb 2 GB suchen (Low28-Bild frei, braucht host_hi_addr=1) statt VirtualAlloc(NULL) mit ueberlappendem Bild (fremde/leere Texturdaten); 0 = alt */
    int contig_img0_guard; /* FIX(ISSUE-TEX): 1 = Low28-Bild 0..0xFFFF fuer Contig-Bloecke gesperrt (kein Block an einem 256-MB-Fensteranfang -> nie Phys/Data 0 = "keine Daten"); 0 = alt */
    int contig_lowq_test;  /* TEST(ISSUE-TEX): 1 = gezielte Contig-HIGH-Suche unter 2 GB auslassen (AMD-Tester-Lage: Adressraum unter 2 GB vom Treiber belegt); 0 = aus */
    int contig_hi_test;    /* TEST(W315): 1 = Contig-Bloecke (MmAllocateContiguousMemory[Ex]) zuerst oberhalb 2 GB platzieren (Low28-Bild frei) -> Lage bei AMD-Testern nachstellen; 0 = aus */
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
    int rt_init_clear;     /* FIX(HD-PACK-ART,W262): 1 = neu angelegte Spiel-RTs (rtt_ensure) einmal auf Null-Bytes setzen (Farbe 0, Tiefe 0, Stencil 0) statt undefiniertem VRAM-Inhalt, 0 = alt, 2 = DIAG: Farbe magenta (zeigt nie beschriebene RT-Bereiche) */
    int tex_fmt_first; /* FIX(LOC-GH-DATA,W383): 1 = Upload, der ohnehin am Format scheitert, belegt keinen Platz in der Drossel (12/Bild), 0 = alt */
    int tex_throttle_retry; /* FIX(TEX-WHITE-UI,W307): 1 = an der Upload-Drossel gescheiterter Erst-Upload beim naechsten Get erneut (statt erst beim 16.), 0 = alt */
    int tex_lw_drop_test;   /* TEST(HUDFREEZE,W345): >0 = in der Mission alle N Bilder die Anzeige-Texturen aus der Lockwatch-Beobachtung nehmen (Verlust nachstellen), 0 = aus */
    int tex_hud_guard;      /* FIX(HUDFREEZE,W345): 1 = Cockpit-Anzeige-Texturen (CPU-gerastert, dauerhaft gesperrt) je Bild in der Lockwatch-Beobachtung halten, 0 = alt */
    int tex_up_budget;      /* DIAG(TEX-WHITE-UI,W307): Uploads je Bild (Drossel), Standard 12; kleiner = Last/langsame Maschine nachstellen */
    int tex_missing_mark;   /* DIAG(TEX-WHITE-AMD,W315): 1 = Draw erwartet Textur, hat aber keine SRV -> Farbe nach Grund statt weiss/Vertexfarbe: gelb = noch nicht hochgeladen/gedrosselt, cyan = Anlegen abgelehnt (D3D), magenta = sonst; 0 = aus (bitgleich) */
    int d3d_debug;         /* DIAG(AMD-ARTIFACTS,W302): 1 = D3D11-Debug-Layer, InfoQueue-Meldungen gruppiert ins Log (D3DDBG), 2 = zusaetzlich je Binden eines Render-Ziels abholen (Ziel im Log), 0 = aus */
    int d3d_warp;          /* DIAG(AMD-ARTIFACTS,W302): 1 = Software-Rasterizer WARP statt Grafikkarte (nur Test, langsam; Prozess auf 4 Kerne ab d3d_warp_core), 0 = aus */
    int d3d_no_threadopt;  /* DIAG(AMD-ARTIFACTS,W302): 1 = Geraet mit PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (Command-List-Emulation wie AMD/WARP erzwingen), 0 = aus */
    int d3d_warp_core;     /* DIAG(AMD-ARTIFACTS,W302): erster Kern der WARP-Affinitaet (4 Kerne), Standard 26 */
    int shader_nan_mark;   /* DIAG(AMD-ARTIFACTS,W302): 1 = NaN/Inf-Pixel der Remaster-/Post-Shader magenta faerben + zaehlen (NANMARK), 0 = aus */
    int hd_mark;           /* DIAG(AMD-ARTIFACTS,W302): 1 = per HD-Pack ersetzte Texturen mit Farbrand markieren (gruen), 2 = zusaetzlich Cache-Treffer nach Neubelegung gelb, 0 = aus */
    int rt_hazard_unbind;  /* FIX(AMD-ARTIFACTS,W302): 1 = Spiel-RT vor dem Binden als Ziel aus den PS-Texturplaetzen loesen (statt Laufzeit-Zwang, Debug-Layer id 9/7), 0 = alt */
    int post_sanitize;     /* FIX(AMD-ARTIFACTS,W302): 1 = NaN/Inf am Eingang der Remaster-Nachbearbeitung auf 0 setzen bzw. klemmen, 0 = alt */
    int tex_reuse_inval;   /* FIX(WASH-TEX,W244): 1 = Cache-Eintraege in wiederverwendeten Contig-Bloecken (contig_reuse) beim naechsten Get neu laden, 0 = alt */
    int tex_upload_guard;  /* 1 = Upload-Quelle voll per VirtualQuery pruefen (TEXLIFE) */
    /* [input] */
    char replay[4096];   /* "pkt:vk,len;..." (M2b-Test, 0 = aus) */
    /* A4: zustandsgesteuerte Schritte "trigger>vk,len;..." (s. BUILD.md):
     * f+N | file:NAME[+N] | mem:0xADDR=0xVAL[+N]. Leer = aus. */
    char replay_steps[4096];
    char replay_autoplay[40]; /* W254 REPLAY-LAUNCHER (experimentell): Replay-Ordner-ID unter UDATA/43430002 direkt abspielen, leer = aus */
    int replay_camera;        /* W254: Ansicht der Wiedergabe fest ([0x3B8CF4]: 0 Cockpit, 1 Vollbild, 2..6 Aussenkameras), -1 = frei */
    int replay_mouse_exit;     /* W299 REPLAY: 1 = Maustasten (Hauptwaffe) beenden Spiel-Replays wie im Original, 0 = nicht (Standard; Abbruch per Aktion replay_exit) */
    int focus_click_eat;       /* W299 REPLAY: 1 = Klick, der das Fenster aktiviert (Alt-Tab zurueck), nicht als Spieleingabe (Standard), 0 = wie bisher */
    int replay_autoplay_shots; /* W254 DIAG: Bild bei jedem Phasenwechsel des Replay-Starts */
    int mission_autostart;     /* W272 ALPHA-TEST: Teststart direkt in Mission mno 0..23 (Launcher "Alpha Test"), -1 = aus */
    int mission_vt;            /* W272: Spieler-VT-Modell 1..22 (ENGDATnn, ohne 12), -1 = Missionsstandard */
    int mission_difficulty;    /* W272: Schwierigkeit 0..5 (ROOKIE..ARMAGEDDON), -1 = wie im Spiel gewaehlt */
    int mission_skip_intro;    /* W272: 1 = Einfuehrungsfilm (Training) per Lock-On ueberspringen */
    char mission_nav_name[160]; /* W272 DIAG: Tastenfolge der Namenseingabe (OK,B,U,D,L,R,bN), leer = Standard */
    int bloom_vsc; /* DIAG(ISSUE-LIGHT): 1 = mit bloom_diag je Welt-Draw Licht-Konstanten + Vertex 0 (BLOOM vsc) */
    int test_god; /* TEST(ISSUE-LIGHT): 1 = HP des eigenen VT (Entity [0x356E2C]) nie unter den Missionsstartwert fallen lassen (Beleglaeufe in spaeten Missionen), 0 = aus (Standard) */
    int mission_autostart_steps; /* TEST(W315): 1 = replay_steps bleiben beim Teststart an (Startsequenz per Kette ohne Titel-Demos, z. B. WARP), 0 = abgeschaltet (Standard) */
    int mission_autostart_shots; /* W272 DIAG: Bild bei jeder Taste/jedem Phasenwechsel des Teststarts */
    int campaign_chain;        /* W303 CAMPAIGN-CHAIN (Testwerkzeug): N Siege in Folge per Missionsende-Testhook, 0 = aus */
    int campaign_chain_branch; /* W303: Auswahl nach mno 15 (0 = mno 16, 1 = mno 18) */
    int campaign_chain_hold;   /* W303: Frames in der Mission bis zum Testhook (Standard 150) */
    int campaign_chain_load;   /* W303: 1 = Ladetest vom Titel (LISTED PILOT -> gespeicherte Mission) */
    int campaign_chain_chapter; /* W303: Ladetest Kapitelwahl 0 = 2080 (Vorwahl), 1 = 2082 */
    int campaign_chain_shots;  /* W303 DIAG: Bild bei jedem Szenenwechsel/jeder Menuetaste der Kette (Standard 1) */
    int campaign_chain_exit;   /* W303: Testlauf (--run-seconds) n Sekunden nach Kettenende beenden, 0 = weiterlaufen */
    /* [video] W255 VIDEO-EXPORT (experimentell): Replay offline als MP4 (H.264/AAC, Media Foundation) */
    char video_export[520];   /* Zieldatei .mp4 (UTF-8), "auto" = <Fotoordner>\Videos\Steelbound_Replay_<id>_<Zeit>.mp4; leer = aus */
    unsigned video_width, video_height; /* Ausgabegroesse (Standard 1920x1080; gerade, 320..4096 x 240..2304) */
    int video_fps;            /* 30 oder 60 (60: Zwischenbilder, video_interp) */
    int video_kbps;           /* H.264-Zielrate in kbit/s, 0 = auto (0,15 bit/Pixel: 1080p30 ~9,3 Mbit/s, 2160p60 ~75 Mbit/s) */
    int video_audio;          /* 1 = Spielton offline mitschreiben (AAC 192 kbit/s), 0 = stumm */
    int video_hw;             /* 1 = Hardware-Encoder erlauben (Rueckfall Software), 0 = nur Software */
    int video_interp;         /* 1 = bei 60 FPS Zwischenbilder rendern (interp_fps=60), 0 = Tick-Bild doppelt */
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
    int input_rec_log; /* Datenschutz 16.11.: 1 = Entwickler-Aufnahme "XIN record" (Tastenwechsel ins Log), 0 = aus (Standard, keine Eingaben loggen) */
    int input_focus_only; /* W79: 1 = echte Tastatur/Maus nur bei Fokus des Client-Fensters, 0 = alt (global) */
    int input_source;     /* W79: 1 = echte Tasten/Maustasten aus Fenster-Nachrichten (nur mit Fokus), 0 = alt (GetAsyncKeyState, global) */
    int input_test_hook;  /* W79 (Test): 1 = WM_APP+0x7A/0x7B (Fokus/Mausdelta per PostMessage) annehmen, 0 = aus */
    int key_toggles;      /* W79: 1 = Tasten 1-5 rasten die 5 Kippschalter (bi34-38) ein/aus (nur ohne Replay), 0 = alt */
    int mouse_mode;       /* W87: 1 = absolut (Fadenkreuz folgt der Maus ohne Spielglaettung, Vorsteuerung auf [0x356D08]/[0x3521BC]), 0 = relativ (alt) */
    int startseq_shots;   /* W87 DIAG(STARTSEQ): 1 = Client-Bild bei jedem Startsequenz-Wechsel + alle 300 Frames bis VT online (braucht input_diag=1), 0 = aus */
    int slide_assist;     /* W380 LOC-DODGE: Ausweich-Hilfe - digitale Slide-Step-Belegung haelt Pedal links + Drehrichtung des Drucks so viele Polls (0 = aus) */
    int toggle_assist;    /* W87: 1 = Druck 1-5 in der Schalterphase auf einen schon eingerasteten, vom Spiel nicht erkannten Kippschalter erzeugt die Flanke neu (bleibt AN), nur mit key_toggles=1; 0 = reine Rast (W79). W451: in LoC ebenso (Schalterpruefung des LoC-Cockpits, Zustand 4 = blinkt rot, z. B. nach Eject) */
    int toggle_test;      /* TEST(W451): 1 = Rastschalter (key_toggles) auch mit Testfolge/Replay, damit Pseudo-Tasten 290..294 bzw. '1'..'5' der Testfolge wie Tastendruecke rasten; 0 = Standard (Rast nur ohne Replay) */
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
    int demo_shot_from, demo_shot_to, demo_shot_step; /* DIAG(SB-SUN-WHITEOUT,W511): Bilder relativ zum Beginn jeder Titel-Demo/jedes Replays (Szene 0x2C, Bild from..to, alle step; Name demoK_OFFSET) + Zeile DEMOSHOT (Blendung, C0); step 0 = aus */
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
    int ign_toggle_fix; /* W52/W404: 1 = Gate Bit 2 in Mission setzen (Kniff IGNITION+START immer frei), 0 = Original (Standard seit W404, Freischaltung durch das Spiel) */
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
    int stick_aim;       /* W271: Pad-/Joystick-Quellen auf dem Zielhebel: 0 = Position (Original, Standard), 1 = relativ ("wie eine Maus", Loslassen haelt); ":mode=" je Quelle hat Vorrang */
    int stick_aim_sens;  /* W271: Empfindlichkeit relativer Stick 1..16 (Default 4 = Mitte->Monitorrand ~0,5 s bei vollem Ausschlag) */
    int stick_aim_accel; /* W271: Beschleunigung bei gehaltenem Vollausschlag 0..4 (Default 0 = aus) */
    int auto_gear;       /* 1 = automatischer Gangwechsel (Gas: hoch/runter nach Geschwindigkeit, Bremse im Stand = R) */
    int audio_ring_overwrite; /* W83: 1 = Ring-Ueberlapp: spaetere Schreibung ersetzt noch nicht gespielte FIFO-Frames (Xbox-Ring), 0 = alt (Clip: erste gewinnt) */
    int audio_ithis_fix;   /* W89: 1 = Puffer-Stop/GetStatus an den I-Wrappern 0x21F53C/0x21F598 hooken (diese rufen C-Ebene mit this-0x1C -> Stimme nie erkannt: Stop wirkungslos, Status immer STOPPED = Stimmenklau), 0 = alt */
    int audio_loop_fix;    /* W89: 1 = SetLoopRegion legt nur die Region fest (Schleife nur mit Play(LOOPING)), Region/Position in Daten-Frames (Resampling), Einmal-Ton bis Datenende; 0 = alt (jede Region loopt, Quell-Frames) */
    int audio_pitch_range; /* W89: 1 = SetPitch-Klemme weit (-98304..32767; Spiel nutzt -8576 = 11,3 kHz), 0 = alt (-8192..8192) */
    int audio_freq_xdk;    /* W343 ISSUE-46: 1 = Frequenz-Setter nach XDK-Grenzen (SetFrequency 188..191983 Hz geklemmt statt verworfen, 0 = Original-Rate, SetPitch -32768..8191, Schritt ohne 1/16..16-Klemme), 0 = alt (unter 4000 Hz verworfen) */
    int audio_pitch_diag;  /* W343 DIAG(PITCH): 1 = alle 10 s je Stimme Frequenz-Setter (Anzahl, Pitch/Hz min..max, verworfen, geklemmt), 0 = aus */
    int audio_api_diag;    /* W348 DIAG(DSAPI): 1 = Aufrufzaehler je DirectSound-Einstieg (alle 60 s) + DIAG(I3DL2)/DIAG(EG)/DIAG(VEL)-Wertezeilen, 0 = aus */
    int audio_eg;          /* W348: 1 = Hüllkurven der Stimmen (SetEG: Amplitude + Multi-EG mit Pitch-Skala) wie MCPX, 0 = alt (ignoriert) */
    int audio_reverb;      /* W348: 1 = I3DL2-Hall im Mischer (Send je 3D-Stimme nach lRoom/lRoomHF), 0 = alt (trocken) */
    int audio_doppler;     /* W348: 1 = Doppler aus DS3D-Geschwindigkeiten, 0 = aus */
    int audio_headroom_xdk; /* W348: 1 = DirectSound-Grundheadroom 600 mB (-6 dB) fuer 2D-Stimmen wie die XDK-Bibliothek, 2 = zusaetzlich Submix-Headroom 1 (-6 dB, Hypothese), 0 = alt (0 dB) */
    int audio_reverb_test; /* W348 TEST: LoC-Hallpreset erzwingen (0..24 = Index der Spieltabelle 0x273FB0, z. B. 9 = Hoehle der Untergrundkarten), -1 = aus */
    int audio_limiter;     /* W348: 1 = Spitzenbegrenzer am Mischerausgang (-1 dBFS, Rueckkehr 150 ms) statt harter Begrenzung, 0 = alt */
    int audio_occlusion;   /* W356: 1 = I3DL2-Quelle vollstaendig (lDirect/lDirectHF, Obstruction, Occlusion, Raum-Rolloff), 0 = alt (nur lRoom/lRoomHF) */
    int audio_hrtf;        /* W356: 1 = Kopfhoerer-HRTF fuer positionierte 3D-Stimmen (Kugelkopf-Modell, Original: DirectSoundUseFullHRTF), 0 = Stereo-Balance */
    int audio_surround;    /* W356: 1 = echte 5.1/7.1-Ausgabe bei Geraet >= 4 Kanaele (WASAPI), 0 = immer Stereo; 2 = TEST: Surround-Mix erzwingen + <out>/audio51.wav */
    int audio_downmix;     /* W356: 1 = 5.1-Faltung ITU (C/Surround -3 dB, LFE -6 dB) normiert auf Vollaussteuerung samt 2D-Headroom, 0 = alt (ungenormt) */
    int audio_xact_sends;  /* W356 LoC: 1 = XACT je Cue wie die Bibliothek (Hall-Send Eintrag +0xF x -2,56 dB nur 3D, LFE-Pegel, Lautstaerke 9 Bit x 0,16 dB, Occlusion je Quelle), 0 = W348 */
    int audio_adpcm_lerp;  /* W89: 1 = ADPCM-Resampling linear interpoliert, 0 = alt (naechster Nachbar) */
    int audio_3d;          /* W89: 1 = DS3D-Distanzdaempfung (min/max/rolloff) + Balance aus Listener-Raum fuer positionierte Stimmen, 0 = alt (3D-Setter ignoriert, alles voll + mittig) */
    int audio_oneshot_fix; /* W95: 1 = Loop-Region-Wrap im Mixer nur bei Schleife (Play LOOPING), Einmal-Ton endet am Datenende; 0 = alt (jede gesetzte Loop-Region wrappt -> Einmal-Toene enden nie) */
    int audio_filter;      /* W102: 1 = Voice-Filter DLS2 (SetFilter Mode 1: resonanter 2-Pol-Tiefpass, Chamberlin-SVF wie MCPX) im Mixer, 0 = aus (alt: SetFilter ignoriert) */
    int audio_level_diag;  /* W102: 1 = DIAG(LVL) Pegel je Stimme und Sekunde -> <out>/voice_levels.csv + DIAG(LVLT)-Rangliste (Laufzeit x Pegel) alle 30 s, 0 = aus */
    int loc_audio;         /* W298 LOC-AUDIO: 1 = XACT-HLE fuer Line of Contact (nur LoC-Profil, mit audio=1; Standard), 2 = plus Cue-Protokoll LOCAUDIO cue, 0 = stumm wie W275 */
    int loc_wma;           /* W367 LOC-WMA: 1 = WMA-Soundtrack (IXACTWmaPlayList) ueber Windows Media Foundation im eigenen Thread (Standard; nur mit loc_audio), 0 = Playlist-Stub stumm wie W298 */
    int loc_wma_demo;      /* W367 LOC-WMA Test/Experimental: 1 = leeren Einsatz-Soundtrack mit den beiden Spiel-Liedern vorbelegen (wie im Optionsmenue eingestellt, nur Spielspeicher dieses Rechners), 0 = aus (Standard, Original) */
    int loc_wma_fail_test; /* W367 DIAG: 1 = Lieddateien gelten als nicht lesbar (Fehlerfall-Beleg: stumm, Logzeile, Cue endet), 0 = aus */
    int volume_master;     /* W303 AUDIO-VOLUME: Gesamtlautstaerke 0..100 % (100 = Original, bitgleich; linear) */
    int volume_music;      /* W303: Musik (Streams mehrkanalig, LoC Soundtrack/BGM) 0..100 */
    int volume_sfx;        /* W303: Effekte (Puffer, Menuetoene, LoC Ambience/Motoren/Rest) 0..100 */
    int volume_voice;      /* W303: Sprache/Funk (Streams mono, LoC Hinweise) 0..100; Filme nur Gesamt */
    int audio_menu_sfx;    /* W258: 1 = Menue-/Namenseingabe-Toene (DirectMusic snk_000..009) per HLE ueber den Mischer, 0 = stumm (alt) */
    int audio_voice_diag;  /* W95: DIAG(SFXN) aktive Puffer-Stimmen je Sekunde (Zahl/Loop/stumm/Alter + aelteste), 0 = aus */
    int audio_sfx_diag;    /* W89: DIAG(SFX) Ereignis-Spur der Puffer-Stimmen (ohne Bink), Wert = max. Zeilen, 0 = aus */
    int audio_wrap_diag;   /* W83: DIAG(BINKWRAP) Ring-Wrap-Unlocks (Lock-Puffer vs. SetBufferData-Speicher), 0 = aus */
    int audio_sync_diag;   /* W205: 1=ring/frame/device trace, 2=also mute device only, 0=off */
    int audio_bink_sync;   /* W205: overwrite the initial Bink ring before Play */
    int audio_ring_underrun; /* ISSUE-10: 1 = Bink-Ringstimmen bei Unterlauf still statt alter Ring-Momentaufnahme, Cursor-Reset verwirft ungespielte FIFO-Frames, 5 ms Ein-/Ausblenden; 0 = alt */
    int audio_test_stall_ms; /* ISSUE-10 TEST: Bink-Tonfuetterung einmal so lange blockieren (ms), 0 = aus */
    int audio_test_stall_at; /* ISSUE-10 TEST: nach so vielen Bink-Locks (alle Stimmen zusammen) */
    int audio_bink_delay;  /* W205: align the movie start with device output */
    int audio_low_latency; /* W205: WASAPI short queue, default 1 */
    int audio_process_async; /* W34: 1 = Process async (PENDING + Echtzeit-Vollzug, Xbox), 0 = alt (sync + Sleep(10)x50-Backpressure -> 5,1-s-Ladepause) */
    int tex_swz06; /* FIX(W38): swizzled 32-Bit (Code 0x06, z.B. 128x128 Wash-Textur) per dokumentiertem NV2A-Morton-Swizzle laden (0=aus, 1=an, 2=an+Einmal-Dump) */
    int lin_tex_rect; /* A2: Lineartextur = Texel-Koords, /(w,h) auch ohne RHW */
    int lin_rhw_range; /* W2B-FIX: linearer VB-Quad/Strip in Pixel-Range = RHW (kein 3D-MVP) */
    int film_tex_keep; /* W261 FILM-ALTTAB: Film-/UI-Quad ohne gueltigen Textur-Upload: Lage aus dem Texturkopf (RHW), Draw auslassen statt weiss; 0 = alt */
    int edge_mode; /* W107 EDGE-MODE: Bildraender ausserhalb der 4:3-Box im Breitbild: 0 = aus (volles Hor+), 1 = harter Schnitt (schwarz), 2 = Abdunklung (Verlauf ins Schwarze) */
    int edge_fade_width; /* W107: Verlaufsbreite in % der 4:3-Boxbreite (edge_mode=2), 0 = harte Kante */
    int edge_fade_strength; /* W107: Endabdunklung in % (100 = schwarz, edge_mode=2) */
    int edge_ab; /* DIAG(EDGE-MODE,W107): 1 = zu jedem Screenshot Varianten _e1/_e2 aus demselben Bild */
    int cockpit_persp;      /* W191 UW-PERSP: Cockpit-Perspektive (nur Desktop, Cockpit-Ansicht der Mission, in VR aus), 0 = Original (aus), 1..100 = Staerke in % von cockpit_persp_dist; Dolly-Zoom um die Ebene cockpit_persp_ref, nie gestreckt */
    int cockpit_panini;     /* W191 UW-PERSP: Panini-Projektion der Cockpit-Ansicht (Nachbearbeitung, nur Desktop), 0 = aus, 1..100 = Staerke in % von cockpit_panini_d; Mitte formtreu vergroessert, Raender waagerecht verdichtet */
    int cockpit_panini_d;   /* W191: groesstes Panini-d (x100); mit cockpit_panini_frame=1 begrenzt der Rahmen frueher */
    int cockpit_panini_frame; /* W191: 1 = Radar oben und Frontview unten bleiben voll im Bild (100 % = Grenze ohne Anschnitt), 0 = d = cockpit_panini % x cockpit_panini_d (Experiment) */
    int fx_wireframe;       /* W240 FX-EXPERIMENTAL: 1 = Welt/Mechs als Drahtgitter (Mission, nicht VR), 0 = Original */
    int fx_vaporwave;       /* W240 FX-EXPERIMENTAL: 1 = Vaporwave-Nachbearbeitung des Missionsbilds (Farbverlauf, Scanlines, Farbsaum; nicht VR), 0 = Original */
    int cockpit_width;      /* W235 COCKPIT-WIDTH: leichte waagerechte Streckung der Cockpit-Ansicht um die Bildmitte (Nachbearbeitung im Panini-Pass, nur Desktop, in VR aus), 0 = Original (aus), 1..10 = Breite + n %; Nutzer-Option, sonst gilt: nie strecken */
    int replay_fx_live;     /* W299 REPLAY-FX (experimentell): Replay-Bildfilter des Spiels (EFFECT #1..#8) auch in der Live-Mission, nur in der Vollbild-Ansicht (Cockpit aus); 0 = aus (Standard, bitgleich) */
    int monitor_view;       /* W319 MONITOR-VIEW: 1 = Monitoransicht (Hauptmonitor bildfuellend wie im Cockpit, ohne Cockpit; experimentell), 2 = + Radar/FRONT VIEW als kleine Felder; nicht in VR/Replay; hat Vorrang vor cockpit_hide; 0 = Original (Standard, bitgleich) */
    int monitor_view_diag;  /* DIAG(W319): 1 = MONVIEW-Zeilen (weggelassene Draws, Abbildung, Zaehler je 600 Bilder) */
    int ent_dump;           /* DIAG(ISSUE-GEO): N > 0 = Entity-Tabelle einmal N Bilder nach Missionsbeginn ausgeben (ENT-Zeilen); 0 = aus (Standard) */
    int heli_shots;         /* DIAG(ISSUE-GEO): mit heli_call: bei Ankunft (Zustand 2) N Bilder je Spiel-Bild (bzw. N Ticks Zwischenbild-Folge mit interp_fps) */
    int heli_call;          /* DIAG(ISSUE-GEO): N > 0 = N Bilder nach Missionsbeginn (mloop 1) einmal den Nachschub-Hubschrauber rufen (Spiel-Funktion FUN_00119930 wie das Monitor-Menue), dazu HELI-Zeilen; 0 = aus (Standard) */
    int cockpit_hide;       /* W268 COCKPIT-HIDE: 1 = in der Live-Mission Cockpit aus wie der Spiel-Kniff IGNITION+START (Vollbild-Flag [0x3516DC]=1, ohne Freischaltung); nicht in VR, Replay, Titel-Demo; 0 = Original (Standard, bitgleich) */
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
    int rtt_lock_stale; /* FIX(CAMPAIGN-END,W310): 1 = sperrt das Spiel eine Textur per LockRect zum Schreiben (CPU-Inhalt, z. B. Bink-Film) und liegt ein RT-Eintrag auf derselben Datenadresse, der im aktuellen/vorigen Bild kein Ziel war, gilt dieser RT als veraltet (Speicher wiederverwendet; ED01_00.bik nach Mission 10 zeigte das Ladebild-RT); 0 = alt */
    int rtt_stale; /* FIX(MAP-RETRY,W124): 1 = veraltete RT-Eintraege (Textur freigegeben, neue RT auf derselben Datenadresse, Surface mit neuer Datenadresse) beim Sampeln ueberspringen, unter mehreren Treffern den zuletzt gerenderten nehmen, volle Tabelle verdraengt zuerst veraltete; 0 = alt (erster Daten-Treffer, nach Missions-Neustart sampelten Texturen alte RT-Inhalte) */
    int rtt_selfcopy; /* FIX(FX-HOLE,W106): 1 = Draws, die ihr eigenes RT sampeln, zeichnen ins RT und lesen eine Kopie (Xbox: Lesen+Schreiben derselben Flaeche); 0 = alt (Umbinden auf den Backbuffer, Rauch-Hitzeflimmern stanzt Loecher ins Cockpit) */
    int rtt_selfclip; /* FIX(SIDESMEAR,W45): 1 = Draws, die ihr eigenes RT sampeln, per Scissor auf Aspect-Box clippen (Seiten schwarz, Mitte exakt wie bisher), 0 = alt (Vollbild-Schmier) */
    int m3d_census; /* DIAG(MISSION3D,W25): Missions-Zensus alle N Frames (Szene 0x2C, mloop=1), 0 = aus */
    int sbar_diag; /* DIAG(STARTSEQ-BARS,W98): 1 = im Zensus-Frame jede neue Stage-0-Textur als shots/rqtex_<xptr>_{rgb,a}.bmp (max 64/Lauf) */
    unsigned sbar_skip_xp; /* DIAG(STARTSEQ-BARS,W98): != 0 = Draws mit dieser Stage-0-Textur (xptr) in der Mission verwerfen (A/B-Bild) */
    int sbar_probe[4]; /* DIAG(STARTSEQ-BARS,W98): BB-Pixel x1:y1:x2:y2, im Zensus-Frame nach jedem BB-Draw lesen, Aenderung loggen (SBAR px) */
    int m3d_census_skip; /* DIAG(STARTSEQ-BARS,W98): erste N Draws des Zensus-Frames nur zaehlen, nicht loggen (Draws hinter der 1600er-Kappe sehen) */
    int m3d_census_gate; /* DIAG(MONITORVIEW,W36): 1 = Missions-Zensus erst ab VT-online ([0x53831C]==1, nach START), 2 = nur Modus 4 (W101), 0 = ab 1. Missions-Frame */
    int loc_census_from; /* W285 DIAG(LOC-RENDER): nur LoC-Profil: Zensus (m3d_census = Abstand) ab diesem Frame statt SB-Szene 0x2C; 0 = aus */
    int loc_bbtex; /* W285 LOC-RENDER: 1 = Backbuffer-Surface als Textur (LoC-Glow-Kette) aus einer Kopie der 4:3-Box (Standard), 0 = alt (weiss); nur LoC-Profil */
    int loc_hatch_lights; /* W495 HATCH-LIGHTS (#102): 1 = Lichtteile der Cockpit-Lichtgruppe mit bewegtem Elternteil fahren in Endlage mit (Standard), 0 = Original (Monitorlichter haengen bei offener Luke im Himmel); nur LoC */
    int loc_visq; /* W297 LOC-MISSION: Sichtbarkeitstests 1 = echt (Occlusion-Query), 2 = immer sichtbar, 0 = alt (1 Pixel, Standard); nur LoC, A12 ohne sichtbaren Unterschied */
    int loc_text_clear; /* W297 LOC-MISSION: 1 = D3D-Clear der Textebene loescht auch den CPU-Speicher (Standard), 0 = alt; nur LoC */
    int loc_vpreset; /* W297 LOC-MISSION: 1 = SetRenderTarget setzt den Viewport auf die volle Zielgroesse (wie D3D; Standard), 0 = alt; nur LoC-Profil */
    int loc_vismap; /* W311 LOC-MISSION-RENDER: 1 = Sichtkarte (256x256-RT [0x300FF4], Dauer-Lock pBits [0x300D70]) im selben Bild per GPU-Rueckschrieb in den Xbox-Speicher (Gelaende-Zellen/Objektlisten lesen sie, Standard), 0 = alt (Karte leer -> nur Himmel); nur LoC-Profil */
    int loc_short3; /* W311 LOC-MISSION-RENDER: 1 = VB-Position SHORT3 (0x35, roh, w = 1) wie NV2A annehmen (LoC-Modelle: VTs, Cockpit, Objekte; Standard), 0 = alt (Draw verworfen, nodecl); nur LoC-Profil */
    int loc_vdata_s; /* W342 LOC-DISPLAY: 1 = SetVertexData2s/4s in den IM-Pfad (UI-Rahmen, Felder, Kartenraster; Standard), 0 = alt (verworfen); nur LoC-Profil */
    int loc_nodummy; /* W297 LOC-MISSION: 1 = Draws in die Dummy-Farbflaeche (Schatten-Tiefenpass, Data=0) verwerfen statt in den Backbuffer (Standard), 0 = alt; nur LoC-Profil */
    int loc_bumpenv; /* W285 LOC-RENDER: 1 = PS-Texturmodus BUMPENVMAP wie PROJECT2D abtasten (Gelaende sichtbar, ohne Bump-Verschiebung; Standard), 0 = alt (Fallback, Vertexfarbe); nur LoC-Profil */
    int loc_psdump; /* W285 DIAG(LOC-RENDER): 1 = je LoC-Pixelshader Def-Woerter + erzeugtes HLSL nach <out>/locps_<handle>.txt (nur LoC-Profil) */
    int loc_shading; /* W285 LOC-RENDER: 1 = Shading-Pfad (Combiner-Emulation) im LoC-Profil ueberall wie shade_scope=1 (Standard; sonst nie, weil die Missions-Erkennung SB-Adressen liest), 0 = alt */
    int loc_rtuv; /* W285 LOC-RENDER: 1 = Shading-Pfad skaliert RT-Texturen nur bei Texel-UV (> 1.5) auf 1/Groesse, sonst normiert (Standard); 0 = immer 1/Groesse wie SB; nur LoC-Profil */
    unsigned loc_texdump_xp; /* DIAG(W368 LOC-COCKPIT): Lineartextur mit diesem Kopf (xptr) ab Bild loc_texdump_from beim naechsten Upload als RGB+Alpha-JPG nach <out>/shots (bis 4x), 0 = aus */
    int loc_texdump_from; /* DIAG(W368): erstes Bild fuer loc_texdump_xp */
    int loc_dbg_skip; /* DIAG(W368): 1 = Draws mit Stufe-0-Textur loc_dbg_xp auslassen */
    unsigned loc_dbg_xp; /* DIAG(W368): Shading-Pfad-Ausgabe T0/V0/C0/R0 (je 100 Bilder wechselnd) fuer Draws mit dieser Stufe-0-Textur, 0 = aus; nur LoC */
    int loc_lookstat; /* DIAG(W358 LOC-LOOK): alle N Bilder Zustandszaehler je Draw (LOOKSTAT-Zeilen, max. 8 Bilder), 0 = aus; nur LoC */
    int loc_texfilt; /* W358 LOC-LOOK: 1 = Shading-Pfad-Sampler mit LOD-Bias des Spiels (aufloesungskompensiert), MAXMIPLEVEL, MIN/MAG, Mips linear; 3 = volle Emulation inkl. MIP POINT/NONE (Standard seit W363), 2 = wie 3 mit Roh-Bias (DIAG), 0 = alt (immer trilinear, kein Bias); nur LoC */
    int loc_xact_rpc; /* W363 LOC-ORIGINAL: 1 = Laufzeit-Parameter (IXACTEngine_SetParameterControl, z. B. Motordrehzahl RPM_VT01) mit den Kurven aus Bank.xsb wie die XACT-Bibliothek (Tonhoehe, Lautstaerke, Tiefpass); 0 = ignoriert (alt) */
    int loc_lintexel; /* W368 LOC-COCKPIT: 1 = lineare Texturen auch bei vom Vertexprogramm gerechneter UV (tsrc < 0) mit Texel-Koordinaten (NV2A), Cockpit-Anzeigen; 0 = alt; nur LoC */
    int loc_swznorm; /* W363 LOC-ORIGINAL: 1 = swizzled Texturen im Shading-Pfad immer normiert (NV2A: Texel-Koordinaten nur LIN_*), auch bei vortransformierten Draws; 0 = alt (Texel-Skala bei UV > 1,5); nur LoC */
    int loc_texfilt_gate; /* DIAG(LOC-ORIGINAL,W363): loc_texfilt nur fuer Teilmengen (Bit0 nur RT-Ziel, Bit1 nur Backbuffer, Bit2 nur Stufe 0, Bit3 nur Stufe 1-3, Bit4 nur Texturen mit Mips, Bit5 nur ohne Mips, Bit6 nur MIP NONE, Bit7 nur MIP POINT/LINEAR); 0 = alle */
    int loc_vsod0; /* W358 LOC-LOOK: 1 = Shading-Pfad nimmt bei P2-Vertexprogrammen (kind 2) oD0/oD1 aus dem Programm (Vertex-Licht der LoC-Modelle), 0 = alt (Eingangsfarbe); nur LoC-Profil */
    int loc_glow; /* W285 LOC-RENDER: 1 = Glow-Draws mit Backbuffer-Textur ueber den Shading-Pfad (Original-Komposition), 0 = alter Pfad (nur Szene); nur LoC-Profil; W459 Standard 1 */
    int loc_sblook; /* W459 LOC-FX-LIGHT: SB look (Experimental, Standard 0): Bloom-Pfade des Spiels in allen Ansichten + SB-Bloom-Regler auf den LoC-Bloom (render_sblook.h); nur LoC */
    int loc_sblook_mask; /* W459: Unterschalter fuer loc_sblook (Bit 1 Bloom Hauptbild, Bit 2 Bloom Lock-On-Kamera, Bit 4 Bloom-Verstaerkung; Standard 7) */
    int loc_sblook_bloom; /* W459: Bloom-Staerke in % bei SB look (Bit 4), 0..200 wie bloom_strength; Standard 150 */
    int gfx_loc_rev; /* W465 LOC-ENHANCED: [render] gfx_loc_rev in user_loc.cfg (0/fehlt = vor Enhanced: LoC-Spieler-cfg bekommt beim Laden die Enhanced-Optik, sb_gfxpreset.h SB_GP_LOC_REV) */
    int shfx_flash_diag; /* DIAG(W465): Blitzlicht-Zustand zuruecklesen und loggen (SHFXFLASH), Standard 0 */
    char shfx_var[2048]; /* TEST(W465): Varianten der Nachbearbeitung je LoC-Testbild (render_shfx.c sb_shfx_var_shots) */
    int shadow_sun; /* W466: LoC Sonnen-/Schattenkontrast 0..200 % (nur LoC, nur mit shade_shadow), Standard 140 */
    int loc_shadow_diag; /* DIAG(W466): Koepfe der LoC-Schattenpass-Ziele loggen (Anzahl), Standard 0 */
    int loc_shadow_sel; /* W466: LoC-Schattenpuffer-Auswahl (Bit 1 >= 1024 Texel, Bit 2 kleiner), Standard 3 */
    int loc_horp_comp; /* W465: LoC 4:3 + volles Hor+: Spiel-Komposition (Replay) auf volle Breite, Standard 1 */
    int shfx_loc_lc; /* W465: LoC-Leuchten Schwelle relativ zum lokalen Mittel in % (130) */
    int shfx_loc_th; /* W465: LoC-Leuchten absolute Schwelle max(R,G,B) in % (60) */
    int shfx_flash_gain; /* W465: LoC-Blitzlicht Verstaerkung (150) */
    int shfx_gpu_diag; /* DIAG(W465): GPU-Zeit je Stufe der Endbild-Nachbearbeitung (SHFXGPU), Standard 0 */
    int loc_census_draws; /* W285 DIAG(LOC-RENDER): ... und nur, wenn das Vorbild mindestens so viele Draws hatte (Standard 300 = 3D-Szene) */
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
    int gfx_preset_rev; /* W296 PRESET-ORIG: [render] gfx_preset_rev aus user.cfg (0 = vor W296: altes Original 120 wird beim Laden auto); ESC-Speichern schreibt SB_GP_REV */
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
    int dlg_text_rtsel; /* FIX(DLG-TEXTOUT,W316): 1 = CPU-Text-Upload in den RT-Slot, den der Draw sampelt (nicht veraltet, eigene Surface, zuletzt gebunden), 0 = alt (erster Datentreffer); wirkt nur mit dlg_text_upload=1 */
    int dlg_scene_diag; /* DIAG(DLG-TEXTOUT,W316): Szene N (z. B. 0x17 = 23): je TextOut-Aufruf (DLGSCENE, max. 400) und Upload (DLGTEXT up, max. 200) in dieser Szene loggen; 0 = aus */
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
    int render_thread_emul; /* FIX(AMD-ARTIFACTS,W302): 1 = render_thread aus, wenn das Geraet keine treibereigenen Command Lists hat (AMD/WARP: Laufzeit-Emulation, weisse Flaechen in Zwischenbildern), 0 = alt */
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
    int shade_fast_first; /* PERF(LOC-PERF,W371): kalter Shader-Cache -> erst schnelle Uebersetzung (shade_fast_level), volle im Hilfs-Thread in den Cache; 1 = nur LoC (Standard), 2 = immer, 0 = aus */
    int shade_fast_level; /* PERF(LOC-PERF,W371): 0 = SKIP_OPTIMIZATION (Standard), 1 = OPTIMIZATION_LEVEL0, 2 = LEVEL1 */
    int shade_par_compile; /* PERF(PERF-SPIKE,W318): zweite VS-Variante parallel uebersetzen (kalter Shader-Cache), 0 = nacheinander */
    int interp_bsets_imm; /* PERF(PERF-SPIKE,W318): Reserve-Saetze ohne Render-Thread (render_thread=0), 0 = wie interp_bsets */
    int interp_gpu_tight_imm; /* PERF(PERF-SPIKE,W318): interp_gpu_tight ohne Render-Thread, wenn interp_gpu_tight=0; 0 = aus */
    int gpu_burn; /* DIAG(PERF-SPIKE,W318): N Warm-GPU-Einheiten vor jedem Interp-Present (langsame GPU nachbilden), 0 = aus */
    int warm_vbpre;   /* PERF(PERF-WARM,W139): 1 = alle Interp-Tick-Puffersaetze beim ersten Present mit Offscreen-BB anlegen + beruehren (nicht erst beim ersten Missionsstart) */
    int warm_diag;    /* DIAG(PERF-WARM,W139): 1 = D3D-Context-Aufrufe > warm_diag_ms loggen (WARM-DIAG lang) */
    int warm_diag_ms; /* DIAG(PERF-WARM,W139): Schwelle in ms (0 = 2) */
    int interp_shots; /* DIAG(FPS-INTERP): ab Interp-Paar N (Zaehler ab Missionsbeginn) alle praesentierten Bilder von interp_shot_ticks Ticks speichern (ip_<f>_<k>), 0 = aus */
    int interp_shots_mrel; /* DIAG(W302): >0 = interp_shots beim Beginn der Spielermission auf Paare+N setzen (Bildfolge relativ zum Missionsbeginn) */
    unsigned interp_shots_scene[4]; /* DIAG(TEX-WHITE-AMD,W315): bis 4 Szenen (Liste 0x1B,0x2F): bei jedem Eintritt interp_shots auf Paare+interp_shots_sdelay setzen (Bildfolgen in Menues/Briefings), 0 = aus */
    int interp_shots_sdelay; /* DIAG(W315): Paare nach dem Szeneneintritt bis zur ersten Bildfolge (Standard 45) */
    int interp_shot_ticks; /* Anzahl Ticks je Bildfolge (Default 6) */
    int interp_shot_every; /* Bildfolge alle N Interp-Paare wiederholen (0 = einmal) */
    int interp_shot_max; /* Deckel Bildfolgen (Default 8) */
    int interp_inst; /* FIX(POPIN,W91): 1 = mehrfach gezeichnete Modelle (gleiche Signatur + Eingangsdaten, z. B. Haeuser) dem Vortick-Draw mit kleinstem Bildsprung zuordnen (gegenseitig naechster, eindeutig; sonst exakt), 0 = alt (Reihenfolge -> falsche Instanz, Haeuser poppen in Zwischenbildern) */
    int interp_zcons; /* FIX(W251 INTERP-GROUND): 1 = exakter Welt-Draw mit Vortick-Partner gleicher Objektraum-Vertices wird per Matrix gelerpt, wenn M_A^-1*M_B zur Kamerabewegung des Ziels passt (gleiche Zwischenkamera, keine Tiefen-Konkurrenz exakt gegen gelerpt); 2 = Versuch Ziel ganz exakt; 0 = alt */
    int interp_rawzc; /* FIX(W284 INTERP-RAWCHG): exakte Welt-Draws zwischen gelerpten Nachbarn per Kamera-Pruefung lerpen: 1 = "rawchg" (gleiche Signatur, neue Eingangsdaten): passt M_A^-1*M_B zur Kamera (Objekt ruht, Gitter verschoben) -> Matrix-Lerp der Tick-N-Vertices, passt M_A^-1*(T(d)*M_B) (fester Versatz d) -> Vertex-Lerp; 2 = zusaetzlich "nomatch" mit Kandidat gleicher Signatur ohne Vertexzahl (Welt-Test); 0 = alt */
    int interp_streak; /* FIX(W294 LINE-STREAK): Bits 1 = Kamerabewegung fuer interp_rawzc/interp_zcons nur aus gelerpten Draws mit verschiedenen, nicht affinen Matrizen (P2-/gleiche Matrizen tragen keine Kamera, D = I); 2 = interp_rawzc auch fuer Welt-Draws ohne Tiefenpuffer (Kamera je Renderziel); 4 = "rawlerp" mit Gitter-Versatz (x/z einheitlich >= 0,5, Hoehe frei; Boden-/Meeresgitter neu gelegt) und Welt-Test -> Matrix-Lerp der Tick-N-Vertices wie der rawchg-Nachbar; Standard 7, 0 = alt */
    int interp_pair; /* W317 INTERP-PAIR: Bits 1 = robuste Zuordnung der Welt-Draws (Kamera-korrigierte Kosten, global eindeutig; bisher exakte ohne Partner mit der Kamera bewegen), 2 = Mehrpass-Gruppen folgen ihrem Leiter (gleiche Matrix N-1); Standard 0 (W317: nicht klar besser, Option) */
    int interp_selfcopy; /* W347 GHOST: 1 = Selbstkopie eines Render-Ziels (W106-Ring, z. B. Hauptmonitor-RT 512^2) als Interp-Op -> Zwischenbilder sampeln den gelerpten Stand statt der Tick-Kopie (sonst Geisterbild beim Drehen), 0 = alt; nur SB (LoC: loc_interp_copy) */
    int interp_bbcam; /* W347 GHOST: 1 = exakt gebliebene rawchg-Billboards (Baeume, je Tick zur Kamera gedreht) per Kamera-Drehpunkt paaren und vertex-lerpen (sonst stehen sie beim Drehen im Tick-Takt = Doppelkanten), 0 = alt; nur SB */
    int interp_pair_diag; /* W317 DIAG: Zensus der Fehlpaarungen (Zeile INTERP pairz je INTERP stat; 2 = auch Einzelzeilen INTERP pzd) */
    float interp_pair_thr; /* W317: Kostengrenze Welt-Partner (NDC, Kamera-korrigiert; Standard 0,004) */
    float interp_pair_mov; /* W317: Kostengrenze bewegter Partner (NDC, nur eindeutig; Standard 0,1) */
    int interp_mode; /* W295 WORLD-REPROJ (Kolben): 0 = Zwischenbilder durch Neuzeichnen aller Draws (Standard), 1 = "reproj": Welt-RT per Tiefe + Bewegung je Draw reprojizieren, Cockpit/HUD wie bisher */
    float reproj_maxobj; /* W295: Matrix-Bewegung eines Draws, die um mehr als N (NDC je Tick) von der Kamera abweicht, gilt als falsche Instanz -> Kamera (0 = aus) */
    float reproj_tear; /* W295: Grenze (Pixel bei 512er Welt-RT) fuer gerissene Gitterdreiecke (Verschiebungsunterschied der Ecken) -> Hintergrund-Fuellung */
    int reproj_opt; /* W295 DIAG: Bit 0 = vorab transformierte 3D-Draws mit eigener Matrix statt Kamera, Bit 1 = sie auch fuer die Kamera-Abstimmung, Bit 2 = Warp mit t = 0 (Vorhersage Tick N-1, Pruefmass), Bit 3 = Vertex-Lerp-Draws nur Kamera, Bit 4 = alle Zwischenbilder mit t = 0,001 (Pruefmass auch fuer das Neuzeichnen), Bit 6 = nur Kamerabewegung (ohne Objektbewegung, ATW-artig), Bit 7 = Kamera ohne Mehrheitsschwelle, Bit 8 = bildfeste weit reichende Matrix-Lerp-Draws bei Kamerabewegung mit Kamera, Bit 9 = dasselbe fuer unveraenderte Draws */
    int reproj_dframe; /* W295 DIAG: Spiel-Bild-Nr. ab Missionsbeginn (wie pf_from), ab der 6 Ticks lang REPROJ d-Zeilen kommen (0 = Stichprobe bei Kamerabewegung) */
    int reproj_diag; /* W295 DIAG: 1 = Zaehler je Statistikfenster, 2 = zusaetzlich Bewegungsbild statt Farbe */
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
    int interp_hlerp;     /* FIX(W237 LINE-FLICKER2): echte Clip-w je P2-Vertex im RT (Strom dort schon durch w geteilt, w = 1) in einem Nebenpuffer; 1 = W224-Pruefung (w-Wechsel N-1/N -> Tick-N-Lage) mit der echten w, Lerp sonst unveraendert; 2 = zusaetzlich homogener Lerp im Clip-Raum (Versuch, nicht Standard); 0 = alt */
    int interp_guard;     /* FIX(W265 LINE-GUARD): Dreieck-Waechter im Zwischenbild (Geometry Shader je gelerptem Dreieck, Lagen N-1/N/t): Bit 1 = Flaeche/Kante/Umklappen unplausibel -> ganzes Dreieck in Tick-N-Lage, Bit 2 = ganzes Dreieck bei w-Wechsel eines Vertex in Tick-N-Lage (Versuch), Bit 4 = auch projektiv konsistente Matrix-Lerp-Draws; 0 = aus (Standard, Nutzer-Gegenprobe offen) */
    int interp_guard_ka;  /* W265: Flaechen-Faktor k_A (Default 4): |A_t| > k_A*max(|A_N-1|,|A_N|) + A_min */
    int interp_guard_ke;  /* W265: Kanten-Faktor k_E in Zehnteln (Default 20 = 2,0): L_t > k_E*max(L_N-1,L_N) + L_min */
    int interp_pass_exact; /* FIX(W273 INTERP-RACE): 1 = interp_pass vergleicht Instanzen voll (Ziel, Geometrie, Matrix) statt nur ueber die 16-Bit-Maske mit RTV-Zeiger (Fehltreffer, je Lauf andere Draws exakt); 0 = alt */
    int interp_rhash; /* DIAG(W273 INTERP-RACE): je Zwischenbild-Wiedergabe Pruefsummen der Eingangsdaten loggen (INTERP rhash ...), Bild unveraendert */
    int interp_guard_diag; /* DIAG(W265): Regel-Maske (1 w-Wechsel, 2 Flaeche, 4 Umklappen, 8 Kante; 15 = alle) - Treffer per Pipeline-Statistik zaehlen (INTERP guard ...), Bild unveraendert */
    int interp_ddrel;     /* DIAG(W251 INTERP-GROUND): R > 0 (mit pf_ring und interp_diag>=3) = Ticks R..R+2 nach Missionsbeginn: INTERP dd-Zeilen + Draw-ID-Bilder des Welt-RT je Zwischenbild (shots/ipid_<f>_<k>.bmp) */
    char interp_ddfile[260]; /* DIAG(W284 INTERP-RAWCHG): Datei mit Ticks (relativ zum Missionsbeginn, je Zeile/Komma) fuer dd-Zeilen + Draw-ID-Bilder wie interp_ddrel (mit pf_ring und interp_diag>=3); leer = aus */
    int interp_eyediag;   /* DIAG(W224): N = hoechstens N Zeilen INTERP eyex (zwischengerechnete Draws mit w-Wechsel einzelner Vertices) + Summen in INTERP stat2; CPU-Kopie der Clip-w je Vertex */
    float interp_maxjump; /* Plausibilitaet: max. NDC-Sprung eines Draws zwischen zwei Ticks (Default 0.5) */
    int im_fast; /* PERF(IM-PERF,W99): 1 = VB-Draws kompakt dekodieren (40 B statt 256-B-Slots je Vertex), Primitiv-Expansion direkt in den gemappten Puffer (IM-Ring/Tick-VB), keine Zwischenkopien; Bild bitgleich. 0 = alt. Diagnosen (rq_diag/fx_diag/Zensus/sbar_diag/interp_diag>=3) nehmen immer den alten Weg */
    int rtt_lookup_filter; /* W204/W253 DRAW-OPT: konservativer Negativfilter der RT-Datenadressen vor der Tabellensuche; 2 = Pruefmodus (jede Ablehnung gegen die Tabelle) */
    int draw_tick_diag; /* W204/W253 DIAG: D-Zeilen je Tick in perf_trace.csv (mit perf_trace=1): Draws, Tick-Last, Submit, Zaehler Set-Aufrufe, Map, Konstanten */
    int draw_state_cache; /* W253 DRAW-OPT: redundante Set-Aufrufe am Immediate-Context entfallen (Schatten in render_dsc.h); 0 = alle durchreichen */
    int im_strip_ib; /* W264 DRAW-OPT2: Strip/Fan/Quad-Draws nur mit Quell-Vertices hochladen und ueber festen Indexpuffer (Strip->Liste-Muster) zeichnen; Bit 0 = Draws ohne, Bit 1 = mit Shading-Pfad (3 = alle); 0 = CPU-Expansion zur Liste */
    int im_cb_ring; /* W253 DRAW-OPT: Draw-Konstanten (MVP + has_tex) der Uebergabe in einen Ring (NO_OVERWRITE, Bindung per Versatz), gleiche Folgesaetze ohne Map; 0 = Map DISCARD je Draw */
    int blend_obj_cache; /* W253 DRAW-OPT: Blend-Objekte je Beschreibung in eigener Tabelle (kein CreateBlendState je Blend-Wechsel); 0 = alt */
    int im_end_reset; /* FIX(EDGE,W64): 1 = End() schliesst den IM-Batch (kein Geister-Wiederholdraw beim naechsten VB-Draw), 0 = alt */
    int fx_blend_eq; /* FIX(FX-ORIG,W90): 1 = NV2A BLEND_EQUATION (0x350: ADD/SUB/REVSUB/MIN/MAX) + BLEND_COLOR (0x34C) abbilden, 0 = alt (immer ADD, Faktor 1) */
    int clear_xbflags; /* FIX(CLEAR-XB,W101/W177): 1 = Clear-Flags nach Xbox-D3D8 (TARGET 0xF0, Z 0x1, STENCIL 0x2) inkl. Textebenen-Anpassung (W177), 2 = TEST: Xbox-Flags ohne Textebenen-Anpassung (Stand W101, Hilfetext fehlt), 0 = alt (PC-Werte 1/2/4) */
    int fx_shot_gap; /* DIAG(FX-HOLE,W106): Mindestabstand der fx_shots-Bilder in Frames (0 = 15) */
    int fx_census; /* DIAG(FX-HOLE,W106): mit m3d_census>0: die ersten N FXSHOT-Bilder loesen einen Zensus im Folgeframe aus */
    int fx_zfunc; /* FIX(FX-ORIG,W90; Standard 1 seit W101 FRONT-HANGAR): 1 = NV2A DEPTH_FUNC (0x354, GL-Enum) als D3D11-DepthFunc, 0 = alt (immer LESS) */
    int tex_lockwatch; /* FIX(STARTSEQ-BARS,W98): 1 = per LockRect gesperrte Texturen einmal je Frame voll hashen + bei Aenderung neu laden (Spiel schreibt ueber behaltenen Lock-Zeiger), 0 = alt */
    int fx_alphakill; /* FIX(STARTSEQ-BARS,W98): 1 = Xbox D3DTSS_ALPHAKILL Stufe 0 (Deferred-TSS 0x21A8DC Bit 2, NV2A TEXTURE_CONTROL0.ALPHAKILLEN): Texel-Alpha 0 -> discard (nicht fuer RT-Texturen), 0 = alt */
    int fx_alphatest; /* FIX(FX-ORIG,W90): 1 = NV2A Alpha-Test (0x300/0x33C/0x340) im IM-Pixelshader (discard), 0 = alt (kein Alpha-Test) */
    int psconst_ab; /* DIAG(ISSUE-LIGHT): 1 = im Screenshot-Bild psconst_store wirkungslos (Ablage 0 wie vorher), Partnerbild +1 wird mitgeschossen -> A/B-Paar aus einem Lauf */
    int psconst_store; /* FIX(ISSUE-LIGHT): 1 = SetPixelShaderConstant legt den Wert wie das XDK-Original (0x2103B5) als ARGB im Geraet ab ([[0x21A8A0]+0x4AC+4*r]), damit D3DDevice_GetPixelShaderConstant (0x20FCD0, nicht gehookt) ihn liefert - Sonder-Material FUN_00023D70/00024410 sichert und stellt C0 (Umgebungslicht) damit wieder her; 0 = alt (Get lieferte 0 -> Umgebungslicht 0 bis zum naechsten Material-Setup) */
    int ps_mul_c0; /* FIX(LOAD-VT-COLOR,W257): 1 = Standardpfad: Combiner-Muster Farbe = V0 * C0 (1 Stufe, ohne Textur; Drahtgitter-VT im Ladebild/Nachschub-Menue) -> Vertexfarbe mit dem explizit gesetzten C0 (SetPixelShaderConstant roh, unabhaengig von shading, bzw. PSCONSTANT0_0) multiplizieren, 0 = alt (nur V0 -> weiss) */
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
    int vis_query_fresh; /* FIX(SB-SUN-WHITEOUT,W511): 1 = Query-Objekte erst wiederverwenden, wenn die GPU ihr letztes Ergebnis fertig hat (sonst lieferten langsame GPUs/WARP fremde Pixelzahlen fuer den Sonnentest -> Blendung, weisser Hauptmonitor), 0 = alt (sofort wiederverwenden) */
    int bb_depth_srv;   /* SHADER-OPT2 (W131): 1 = Backbuffer-Tiefe typlos mit SRV anlegen (fuer ssao, sonst automatisch bei ssao>0); 0 = nur bei Bedarf */
    int ssao_view;      /* DIAG(SHADER-OPT2,W131): 1 = Endbild zeigt die SSAO-Verdeckung (Grauwert) statt des Bildes */
    int shfx_ab;        /* DIAG(SHADE-FX,W121): 1 = Screenshot-Bild mit Original-Werten, Partnerbild (+1) mit den Optionen, dazu <shot>_fx (gleiches Bild + Cockpit-Nachbearbeitung) */
    int shot_mission;   /* DIAG(SHADE-FX,W121): 1 = screenshot_frame zaehlt ab Beginn der Kampagnen-Mission (Modus 4); 2 = ab dem ersten Missionsbild inkl. Titel-Demo (W313) */
    int shfx_diag;      /* DIAG(SHADE-FX,W121): 1 = SHFX-Zeilen (Optionen, gekennzeichnete Stufen) alle 600 Missionsbilder */
    int mfx_diag; /* DIAG(MONITOR-FX,W105): 1 = MFX-Zeilen (Wash/Zeilen-Draws, Stoerpegel 1x/300 Bilder) */
    int mfx_ab; /* DIAG(MONITOR-FX,W105): 1 = Screenshot-Bild mit altem Pfad, Partnerbild (+1) mit monitor_fx (Bildpaare aus einem Lauf, wie shade_ab) */
    int mfx_shots; /* DIAG(MONITOR-FX,W105): N > 0 = bis zu N Ausloesungen in Wash-/Zeilen-Bildern (Zeilen-Bild 2/40/120, jedes 400. Wash-Bild ab 30): ohne mfx_ab 3 Folgebilder, mit mfx_ab Paar alt (p+1) / neu (p+2) */
    int shade_texskip; /* FIX(SHADE-BLOOM,W112, nur shading=1): 1 = Tiefenformate (0x2A..0x31, Schattenpuffer) auf Stufe 1..3 nicht als Farbtextur laden (Fehlversuche verbrauchten das Upload-Budget, Bodentextur fehlte); 0 = alt */
    int vis_query;     /* FIX(SHADE-M2,W117): NV2A-Sichtbarkeitstest (Begin/EndVisibilityTest, GetVisibilityTestResult) als D3D11-Occlusion-Query; 1 = nur mit shading=1 (Standard, shading=0 bitgleich), 2 = immer, 0 = alt (Ergebnis immer 1 = "sichtbar") */
    int vis_query_all; /* W117 vis_query: 0 = nur Sonnentests 0x3FFE/0x3FFF echt, 2 = Sonne + Licht-Flares 0x3EFE..0x3FFD (W304 FLARE-OCCL, Standard), 1 = alle Indizes (zusaetzlich Objekt-Occlusion-Culling FUN_00025580/00025840) */
    int vis_zfar;      /* W477 SB-SUN-WHITEOUT: 1 = vortransformierter Testpunkt (P2-Punkt im Sichtbarkeitstest, Tiefentest LESS/LEQUAL/EQUAL) mit z > 1 wird nicht gezeichnet -> Ergebnis 0 wie NV2A (Standard); 0 = alt (D3D11-Tiefenclipping, AMD-Treiber zaehlen ihn sichtbar); 2 = TEST: z auf 1 klemmen (AMD-Verhalten nachstellen) */
    int vis_ztest;     /* W304 FLARE-OCCL: 1 = Punkt-Draws im Sichtbarkeitstest (Sonne, Flares; P2-Pixel ins Welt-RT) mit dem Tiefentest des Spiels wie NV2A (Standard), 0 = alt (RHW ohne Tiefentest -> verdeckt zaehlt als sichtbar) */
    int p2_ztest;      /* W313 P2-ZTEST: 1 = P2-Draws (VS-Programm Clip/Tail, Bildschirm-Pixel rhw=1) in Spiel-RTs mit Tiefenflaeche bekommen den Tiefenzustand des Spiels (ZENABLE-Spiegel, ZFUNC, Tiefenmaske) wie die NV2A; 0 = alt (RHW ohne Tiefentest) */
    int line_scale;    /* FIX(ISSUE-EMP,W352): 1 = Linien-Primitive in Spiel-RTs mit Render-Skalierung (rt_scale/auto >= 1,5) per Geometry Shader 1 Xbox-Pixel breit wie die NV2A (Funken-/Fallen-/EMP-Linien, Monitor-Stoerzeilen); 0 = alt (1 physisches Pixel, bei Faktor N nur ~1/N Deckung) */
    int loc_line_bb;   /* FIX(W381,#59): 1 = LoC-Linien im Backbuffer (Welt-Effekte: Funken/Explosionen) bei Bildhoehe >= 1,5 x Framebuffer per Band-GS 1 Xbox-Pixel breit wie die NV2A; 0 = alt (1 physisches Pixel). Nur LoC-Profil */
    int specmat_diag;  /* DIAG(ISSUE36): 1 = C0-Wiederherstellung nach Sonder-Material-Draws (Aufrufer 0x241C7) je Bild zaehlen (SPECMAT-Zeilen), Belegbild beim ersten Treffer */
    int proj_diag;     /* DIAG(ISSUE49): 1 = Geschoss-Listen des Spiels ([0x6E4480]/[0x6E4484], 0xE0 je Eintrag) je Bild: Geburt/Ende einzeln (PROJ new/end), aktive Eintraege alle 30 Bilder (PROJ act) */
    int line_diag;     /* DIAG(ISSUE-EMP,W352): N = Linien-/Punkt-Draws (Begin LINELIST/LOOP/STRIP/POINTLIST) in der Kampagnen-Mission: erste N einzeln (LINED d), Summen je 300 Bilder (LINED sum) mit Laenge und Pixeln nach Tiefentest */
    int p2z_diag;      /* DIAG(P2-ZTEST,W313): 1 = Zensus der Draws in Spiel-RTs je Zustand (P2Z-Zeilen: VS/PS/Textur/Ziel/zen/zf/dm/Blend, z-Bereich, Pixel nach Tiefentest) */
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
    int hd_zero_skip;  /* W288 PACK-ZERO-TEX: 1 = Textur, deren Mip 0 nur Null-Bytes enthaelt, nie per Pack ersetzen und nie dumpen; 0 = alt */
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
    char input_hold_test[64];   /* TEST(W423): ab Bild 0 gehaltene synthetische Eingabe: "esc" | "pause" (Tastatur, Wiederholmeldungen ans eigene Fenster) bzw. "pad:<hex>" (Test-Pad, input_test_hook); leer = aus */
    char overlay_save_path[512]; /* "Als Standard speichern": Ziel (leer = user.cfg neben sb_native.cfg bzw. SB_USER_CFG) */
    /* W188 GAME-MENU (sb_overlay.c, sb_pause.c, sb_feedback.c) */
    int game_menu;              /* 1 = ESC oeffnet zuerst das Spielmenue (Weiter/Einstellungen/Fotomodus/Feedback/Beenden), 0 = ESC direkt in die Einstellungen (W118) */
    int pause_net;              /* W357 LOC-PAUSE-NET: 0 = aus (Pause wie bisher), 1 = im LoC-Netzspiel kein Anhalten (Standard), 2 = TEST: immer wie im Netzspiel */
    int menu_pause;             /* 1 = offenes ESC-Menue pausiert das Spiel (Spieler-Standard), 0 = Spiel laeuft dahinter weiter (Live-Vorschau) */
    int keep_display_on;        /* #113: 1 = SetThreadExecutionState(ES_CONTINUOUS|ES_DISPLAY_REQUIRED|ES_SYSTEM_REQUIRED) solange der Client laeuft (nicht bei unsichtbaren Testlaeufen), 0 = aus */
    int esc_debug;              /* W291 ESC-REWORK: 1 = ESC-Gruppe "Debug" (Drahtgitter, Vaporwave, Sonnentest, Vergleich B) zeigen; der Launcher setzt es je Start aus alpha_tools, Standard 0 */
    int pause_live;             /* W193 PAUSE-LIVE: 1 = Standbild der Pause aus der FPS-INTERP-Aufzeichnung mit den aktuellen Einstellungen neu zeichnen, 0 = W188 (Kopie) */
    char menu_peek_key[16];     /* W193: Taste "Menue kurz ausblenden" (gedrueckt halten), Tastenname wie in Profilen; leer/OFF = aus */
    int orig_hint;              /* W278 ORIG-FLAG: 1 = beim Start kurz einblenden, wenn experimentelle Grafik aktiv ist (Spieler: Launcher/sb_player.cfg), 0 = aus (Code-/Test-Standard) */
    int orig_once;              /* W278: 1 = Einmal-Start im Original-Modus (Launcher --set, nur Einblendung) */
    char orig_key[24];          /* W278: Taste "Original <-> eigene Einstellungen" live, z. B. SHIFT+F8, CTRL+F7, F6; leer/OFF = aus. Standard SHIFT+F8 */
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
    int photo_aa;               /* W249: Kantenglaettung gekachelter Fotos: intern 2- bzw. 4-fach rendern und mitteln (Standard 2; 1 = aus) */
    int photo_diag;             /* DIAG(W249): Foto-Zensus misst je Welt-Draw die Raster-Lage der Eckpunkte (Standard 0) */
    int photo_view;             /* W290 QUICK-SHOT: Ansicht der Aufnahme: 0 = wie im Spiel (Standard), 1 = ohne Cockpit (Vollbild-Ansicht des Spiels), 2 = nur Aussenwelt (ohne HUD/Monitor) */
    int photo_size;             /* W290: Groesse: 1/2/4/8 = x Fenster, -4 = 4K, -8 = 8K, 0 = nicht gesetzt (photo_scale > 1 bzw. 4K) */
    int photo_hud;              /* W290: 1 = HUD im Foto (Standard), 0 = HUD des Hauptmonitors weglassen (Monitor-Effekt bleibt; braucht Tick-Aufzeichnung) */
    int photo_cine_self;        /* W428 PHOTO-CINEMATIC (photo_view=3): Welt-Draws, deren Modell-Ursprung naeher als N/10 Spiel-Einheiten (~m) an der Spielkamera liegt, gelten als eigener VT und fehlen im Foto. Standard 40; 0 = nichts auslassen */
    int photo_cine_diag;        /* DIAG(W428): Bit 1 = Zeile je ausgelassenem Draw (begrenzt), Bit 2 = zusaetzlich alle P2-Draws (Vertex-Programm) im Welt-RT auslassen (Test); 0 = aus */
    int save_delete_fix;        /* W433 REPLAY-DELETE: 1 = NtCreateFile/NtOpenFile beachten RootDirectory (Ordner-Handle + relativer Name, XAPI XDeleteSaveGame), Loeschen beim Schliessen auch fuer Ordner, nur im Spiel-Laufwerk (Standard); 0 = alt */
    int save_delete_selftest;   /* DIAG(W433): Selbsttest Spielstand-Ordner loeschen wie XDeleteSaveGame (synthetischer Ordner unter UDATA, vor dem Spiel) */
    int replay_label;           /* W290: 1 = REPLAY-Schriftzug der Wiedergabe zeigen (Original, Standard), 0 = weglassen (live, Foto, Video) */
    int replay_logo;            /* W290: 1 = Spiel-Logo der Wiedergabe zeigen (Original, Standard), 0 = weglassen */
    int watermark_media;        /* W290: 1 = Wasserzeichen auch im Video-Export (Standard), 0 = nicht im Video-Export */
    char feedback_repo[128];    /* [feedback] repo: GitHub-Repo fuer "Neues Issue" (Owner/Name), Standard SB_FEEDBACK_REPO_DEFAULT (sb_feedback.h) */
    /* W122 PERF-HUD (sb_perfhud.c) */
    int perf_hud;               /* 1 = Leistungs-Overlay sichtbar (Taste schaltet und speichert in user.cfg), 0 = aus (Standard) */
    int perf_hud_pos;           /* Ecke: 0 = oben rechts, 1 = oben links, 2 = unten links, 3 = unten rechts */
    char perf_hud_key[16];      /* Taste: F1..F12 (ohne F10), VK<hex>; 0/aus = keine Taste. Standard F2 */
    /* W374 LOC-NET-OVERLAY (sb_nethud.c); Taste = Profil-Aktion net_hud (Standard F3) */
    int net_hud;                /* 1 = Netz-Overlay sichtbar (Taste schaltet und speichert in user.cfg), 0 = aus (Standard) */
    int net_hud_pos;            /* Ecke wie perf_hud_pos; Standard 1 = oben links. Gleiche Ecke wie F2: darunter/darueber */
    int watermark;              /* W219 WATERMARK: 1 = Alpha-Wasserzeichen (Version) unten rechts, 0 = aus (Code-/Test-Standard) */
    int perf_log;               /* DIAG(PERF-HUD,W122): alle N s Zeile PERFHUD + PERFSPIKE-Zeilen (0 = aus) */
    int perf_spike_ms;          /* DIAG(PERF-HUD,W122): PERFSPIKE ab dieser Bildzeit (0 = 2x Zielbildzeit) */
    int perf_prof;              /* DIAG(PERF-HUD,W122): 1 = Stichproben-Profiler des Spiel-Threads (Zeile PERFPROF, mit perf_log) */
    int perf_prof_heavy;
    int perf_prof_follow;       /* DIAG(W391): 1 = Profiler folgt dem praesentierenden Thread (LoC-Mission: Render-Thread) */
    int loc_thr_diag;           /* DIAG(W391): 1 = PERFTHR-Liste bei neuen Xbox-Threads neu (PERFTHR-XB), CPU je Tick (LOCTHR, LOCTHR-LANG) */
    int loc_xb_prio;            /* PERF(W391): LoC-Xbox-Threads ueber normal (1 = ABOVE_NORMAL ab 4 Kernen, 2 = HIGHEST, 3 = TEST ohne Kern-Schutz, 0 = normal) */
    int loc_yield_sleep;        /* PERF(W391): Xbox-Warteschleifen (NtYieldExecution) nach 1 ms in 0,25-ms-Schritten schlafen (nur LoC) */
    int test_render_slow_ms;
    int loc_shot_tick_every;    /* TEST(W391): LoC-Mission je N Spielticks ein Bild shots/loct_<Tick>.jpg (0 = aus) */    /* TEST(W391): Render-Thread des Spiels rechnet je Spielbild N ms zusaetzlich (nur LoC, 0 = aus) */
    int perf_prof_stack;        /* DIAG(W391): 1 = PERFPROF-STK (Aufrufketten der Systemmodul-Proben in schweren Ticks, perf_prof=2) */
    int loc_delay_diag;         /* DIAG(W391): 1 = Zeile LOCDELAY (KeDelayExecutionThread gewollt/ist je Aufrufer) */
    unsigned test_cpu_mask;     /* TEST(W391): Prozess-Affinitaet (Bitmaske, 0 = aus) - "schwacher PC" mit wenigen Kernen */
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
    /* W242 VR-SHELL (nur mit vr_enable=1, nur Augenbild der Cockpit-Wiedergabe) */
    int vr_shell;               /* 1 = dunkle, schlichte Huelle um den Sitz fuellt leere Bildteile (Standard), 0 = schwarz wie bisher */
    int vr_vignette;            /* Rand-Vignette 0..100 %: Deckung der weltfesten Blende jenseits des modellierten Cockpits (Standard 80, 0 = aus) */
    int vr_shell_diag;          /* DIAG(VR-SHELL,W242): 1 = leere Pixel magenta statt Huelle, ohne Vignette (Messung der Abdeckung) */
    /* W493 LOC-MEMLEAK (#101), nur LoC-Profil */
    int loc_mem_reclaim;        /* FIX: 1 = VB/IB aus CreateVertex/IndexBuffer2 mit Xbox-Refcount (Common 0x01000001/0x01010001); Release auf 0 gibt die Daten nach der Quarantaene an LOCMEM zurueck, freie Bloecke nach Groessenklassen wiederverwendet (Standard); 0 = alt (nie frei, Bereich lief nach ~1 h voll) */
    int loc_mem_quar;           /* Quarantaene freigegebener LOCMEM-Bloecke in Bildern (Inhalt bleibt so lange stehen, Standard 300 = 10 s bei 30 Bildern/s; 0 = sofort) */
    /* W496 NET-TELEMETRY: Netzbericht an steelnet (nur LoC mit Treffpunkt), Launcher-Frage "anonyme Netzstatistik" */
    int net_report;             /* 1 = Zustimmung: feste Codes/Zahlen am Lobby-/Match-Ende an steelnet (loc_tun_netrep.c); 0 = nie (Standard, bis gefragt) */
    /* W507 LOC-BLOOM-LAG (#107), nur LoC */
    int loc_bloom_diag;         /* DIAG: 1 = LoC-Bloom-Ziele (<= 32^2) beim Lesen zuruecklesen, Zeile LOCBLOOM rt (Mittel/Max je Kanal) */
    int loc_bloom_fast;         /* FIX: Nachlauf des spieleigenen Bloom k-fach schneller bei gleicher Helligkeit (render_bloomlag.h), nur mit Enhanced-Bloom (bloom_strength != 100 oder bloom_threshold > 0); 0/1 = aus (Original-Zeitverhalten), Standard 4 */
    /* W512 ISSUE-109, nur LoC */
    int loc_shadow_border;      /* FIX: Schattenvergleich ausserhalb der Schattenkarte mit der Randfarbe des Spiels (ADDRESS BORDER, LoC: 0xFFFFFFFF = groesste Tiefe = beleuchtet) statt Randtexel; 0 = alt (Randtexel), Standard 1 */
    /* W518 ISSUE-111, nur LoC */
    int loc_ui2d;               /* FIX: Draws mit LoC-UI-Programm (einziger oPos-Schrieb MAD v0*cS+cO, keine Matrix) immer 2D, auch ausserhalb -64..704 x -64..544 (gezoomte Einsatzkarte im Menue); 0 = alt (Bereichs-Heuristik), Standard 1 */
    int loc_menu_bg;            /* W518b MENUBG: 1 = bildfuellende ui2d-Hintergrundebene (x auf beiden Seiten ueber -64..704, Verbindungs-/Pilotenmenue) sichtbar (Standard); 0 = alte Bereichsgrenze (unsichtbar wie vor W518); die gezoomte Karte bleibt immer 2D */

    /* W519 STALLHOLD (#122), nur LoC online (Treffpunkt + eigener Transport) */
    int loc_stall_hold;         /* FIX: Stillstand im Spielstrom eines Mitspielers bis zu N s halten, solange der Transport nur einen Stau sieht (kein Abgang gemeldet), statt ihn nach der Original-Grenze (~10 s) zu entfernen; 0 = aus (Original), Standard 60, Bereich 15..120 */
} hcfg_t;

extern hcfg_t g_cfg;
/* FIX(TEX-WHITE-AMD,W315) host_hi_addr: Obergrenze gueltiger Host-Adressen im 32-Bit-Prozess
 * (/LARGEADDRESSAWARE: Nutzeradressraum bis 4 GB; vorher pauschal 2 GB). */
#define SB_HOST_LIM() (g_cfg.host_hi_addr ? 0xFFFE0000u : 0x80000000u)


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
