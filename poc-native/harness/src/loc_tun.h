/* loc_tun.h - W324 LOC-NETPLAY: eigener UDP-Transport unter dem LoC-Netz-HLE (loc_transport=udp).
 *
 * Das Spiel behaelt seine echten Sockets (Loopback). Jeder Mitspieler wird lokal durch eine virtuelle
 * Adresse (vip, 127/8) vertreten; der Transport haelt dafuer Stellvertreter-Sockets auf
 * vip:proxy_base+(Spielport-1000) und traegt die Daten ueber EINE eigene UDP-Verbindung je Mitspieler:
 *  - TCP des Spiels (Port 1000, Lockstep 0x34 und Lobby-Rahmen bis 0x100C): Bytestrom unveraendert,
 *    geordnete Zustellung, jede Nachricht wird in bis zu N Paketen mitgeschickt (Redundanz, N adaptiv
 *    nach gemeldetem Verlust), Wiederholung nach RTO, kumulative Bestaetigung.
 *  - UDP des Spiels (Suche 1201, Beitritt 1202, VDP 1203): als Datagramm, in N Paketen mitgeschickt,
 *    beim Empfaenger entdoppelt (keine Wiederholung darueber hinaus, wie UDP).
 *  - W334: Protokoll v2 (net/SECURITY.md): Steuerkanal TLS mit SPKI-Pin, Spielverkehr Ende-zu-Ende (SBT2),
 *    Gastfilter vor jeder Uebergabe ans Spiel (loc_guard).
 *  - Wege: direkt (NAT-Durchstich als SBT2-PROBE) oder ueber ein steelnet-Relay
 *    (SBN1 RELAY mit link_id, Bindung per Ticket des Treffpunkts). Die Wahl trifft der Treffpunkt
 *    (route) anhand der gemessenen Pings (kleinste maximale paarweise Latenz).
 *  - Treffpunkt (loc_lobby=host:port): steelnet-Steuerkanal (JSON-Zeilen) im selben Thread.
 * Keine Spiellogik, keine Inhalte im Log (nur Zaehler, Zeiten, Wege). Testbar ohne Spielspeicher
 * (loc_net_selftest). */
#pragma once
#include <winsock2.h>
#include <stdint.h>
#include "loc_live.h" /* W482 LIVE-MATCH: loclive_sess */

struct locnet;
struct locnetsim;

typedef void (*loctun_log_fn)(const char *msg);

typedef struct loctun_cfg {
    uint16_t port;        /* eigener UDP-Port des Transports (0 = frei waehlen) */
    uint16_t proxy_base;  /* Stellvertreter der Mitspieler: vip:proxy_base+(Spielport-1000) (Standard 21000) */
    int red_fixed;        /* 0 = Redundanz N adaptiv (2..red_max), sonst fest */
    int red_max;          /* obere Grenze fuer N (Standard 8) */
    int gap_ms;           /* Mindestabstand zweier Kopien derselben Nachricht (Standard 15) */
    int hold_ms;          /* Jitter-Puffer: Mindest-Haltezeit vor der Zustellung (Standard 0) */
    int qos_reserve_ms;   /* QoS-RTT = SRTT + 4*RTTVAR + Reserve (Standard 100) */
    int nodirect;         /* TEST: kein direkter Weg (Durchstich aus) -> Relay */
    int allow_public;     /* 1 = Mitspieler-/Relay-Adressen ausserhalb Loopback/privat erlaubt (nur mit Treffpunkt) */
    char peers[256];      /* TEST ohne Treffpunkt: "ip:port,..." (nur Loopback/privat) */
    char lobby[128];      /* Treffpunkt host:port (leer = keiner) */
    char room[33];        /* Lobby-Name beim Treffpunkt (Standard "loc") */
    char pilot[17];       /* Pilotname beim Treffpunkt (Standard "Pilot") */
    char role[8];         /* "auto" (beitreten, sonst anlegen), "host", "join" */
    char relays[256];     /* zusaetzliche Relays "name=host:port,..." (der Treffpunkt nennt seine selbst) */
    char status_path[260];/* Statusdatei fuer den Launcher (leer = keine) */
    char auth[100];       /* W328: Konto "account:exp:G-hex64" (leer = aus Umgebung SB_LOC_AUTH lesen, dann Gast) */
    char code[16];        /* W334: Raumcode zum Beitreten (leer = eigene Lobby mit Raumcode anlegen, SECURITY.md §3.5) */
    char spki[65];        /* W334: Pin (SPKI-SHA-256, hex64) fuer einen anderen Treffpunkt als SB_STEELNET_HOST (§3.6) */
    char netcut_file[260];/* W366 TEST: solange diese Datei existiert, gehen alle UDP-Pakete verloren (Netz kurz weg) */
    char vis[8];          /* W373: "public" = oeffentliche Lobbys (ohne Raumcode; Code geht immer privat), sonst Raumcode-Lobby */
    int route_auto;       /* W395: 1 = Weg je Mitspieler selbst waehlen und wechseln (direkt/alle Relays), 0 = wie bisher
                           * dem Treffpunkt folgen (Standard 1) */
    int open_hold_ms;     /* W409: OPEN eines Mitspielers halten, solange das eigene Spiel TCP 1000 nicht annimmt (wie das
                           * Original, das einen abgelehnten connect wiederholt), 0 = sofort S_FIN (Standard 3000) */
    int test_refuse_ms;   /* W409 TEST: eigenes Spiel gilt die ersten N ms nach jedem OPEN als "lauscht nicht" */
    int ver_check;        /* W420 TEST: Versionspruefung im HELLO auch ohne Treffpunkt (Standard 0: nur steelnet) */
    char ver_test[16];    /* W420 TEST: eigene Version statt SB_NET_VERSION ("-" = HELLO ohne Versionsfeld wie aeltere Clients) */
    char play_mode[8];    /* W440 LOC-LOBBY-MODES: Modus eigener Lobbys beim Anlegen ("sim" = Standard, "arcade") */
    int host_mig;         /* W520 HOSTMIG: eigene Lobbys mit host_migration:true anlegen (Experimental-Schalter) */
    int auto_host;        /* W520: eigene Lobbys mit auto_host:true (nur mit host_mig) */
    int join_delay_ms;    /* W446 TEST: Lobby erst N ms nach dem welcome betreten bzw. anlegen (steelnet-Chat ausserhalb
                           * einer Lobby nachweisen), 0 = sofort (Standard) */
    /* W454 LOC-LAN-MODE: LAN ohne steelnet (nur ohne Treffpunkt; Entdeckung loc_tun_lan.c, SECURITY.md §36) */
    int lan;              /* 1 = LAN-Modus */
    uint16_t lan_port;    /* Entdeckungs-Port (0 = LLAN_PORT 47210) */
    char lan_room[17];    /* Raumname (leer = "LAN"), nur druckbares ASCII */
    char lan_pass[64];    /* Raumpasswort (leer = aus Umgebung SB_LAN_PASS, sonst ohne); wird nach dem Ableiten geloescht */
    char lan_targets[128];/* TEST/Notweg: zusaetzliche Unicast-Ziele "ip:port,..." (nur privat); nur Loopback = kein Broadcast */
    int live;             /* W482 LIVE-MATCH: Xbox-Live-Modus (loc_live=1, nur mit Treffpunkt): keine Lobby beim Start;
                           * Sessions ueber loctun_live_send, Beitritt zu gefundenen Hosts per live_join */
    char route_sim[256];  /* W395 TEST: je Weg eigene Verzoegerung/Ausfall, "name:SPEC/name:SPEC" mit name = direct oder
                           * Relayname, SPEC wie loc_netsim plus cut:A-B (Sekunden ab Transportstart, B leer = bis Ende) */
    /* W496 NET-TELEMETRY Stufe 2 (loc_tun_netrep.c): Netzbericht an steelnet, nur feste Codes und Zahlen */
    int net_report;       /* 1 = Zustimmung des Spielers (Launcher net_report=1); 0 = nie senden, Warteschlange loeschen */
    char netrep_dir[260]; /* Ordner fuer Warteschlange (net_report_q.txt) und Laufmarke (net_report_run.txt), leer = keine */
    unsigned gpu_vendor;  /* PCI-Hersteller-ID der GPU (nur die ID, 0 = unbekannt) */
    /* W500 LOBBY-RESUME: Abriss der Treffpunkt-Verbindung -> Sitzung fortsetzen statt alle Mitspieler zu trennen */
    int resume;           /* 1 = fortsetzen (Standard), 0 = altes Verhalten (sauberer Neuaufbau; Gegenprobe) */
    int cut_s;            /* TEST: Steuerverbindung N s nach jedem (Wieder-)Aufbau abreissen (0 = aus) */
    int cut_mode;         /* TEST: 0 = harter Reset (RST, Dienst merkt es), 1 = still liegenlassen (Dienst merkt nichts) */
    /* W521 NETPATHS (#119): direkter Weg oefter - IPv6-Kandidaten zuerst, Rueck-Stanzen an die gesehene Adresse,
     * langsame Phase und Hintergrund-Wiederholung (Wechsel Relay -> direkt im Spiel) */
    int ipv6;             /* 1 = eigene globale IPv6-Adresse als Kandidat melden und IPv6 zuerst stanzen (Standard), 0 = aus */
    int punch_legacy;     /* TEST/Gegenprobe: 1 = Durchstich wie vor W521 (3 s, kein Rueck-Stanzen, keine Wiederholung, kein IPv6) */
    int v6_loop;          /* TEST: ::1 statt einer globalen Adresse als eigener IPv6-Kandidat (Labor ohne IPv6-Anschluss) */
    int nat_filter;       /* TEST NAT-Nachbildung: 1 = direkte Pakete nur von Adressen, an die wir in den letzten 60 s
                           * gesendet haben (port-restricted Filter bzw. IPv6-Firewall) */
    int cand_skew;        /* TEST NAT-Nachbildung: Port-Versatz fuer alle IPv4-Kandidaten der Mitspieler (Partner hinter
                           * symmetrischem NAT: die gemeldete Zuordnung stimmt fuer uns nicht), 0 = aus */
    int punch_delay_ms;   /* TEST: Durchstich erst N ms nach dem peer_info beginnen (spaet ankommendes peer_info) */
} loctun_cfg;

typedef struct loctun_stats {
    volatile LONG pkt_tx, pkt_rx, msg_new, msg_copy, msg_rto, msg_rx, msg_dup, msg_saved, dg_tx, dg_rx, dg_dup,
        dg_drop, streams, bad, guard /* W334: Gastfilter-Verstoesse */, e2e_bad /* W334: SBT2 verworfen */;
} loctun_stats;

struct loctun;
typedef struct loctun loctun;

void loctun_cfg_default(loctun_cfg *c);
/* 1 = Transport vorhanden (private Fassung), 0 = Platzhalter (Community, loc_tun_stub.c). */
int loctun_available(void);
/* Startet Socket + Thread. n = Netz-Kern (fuer eigene vip/Basis und Mitspielerliste), sim = optionale
 * Verzoegerungsstufe fuer die AUSGEHENDEN Transportpakete (W322-Pruefstand). NULL bei Fehler. */
loctun *loctun_start(const loctun_cfg *c, struct locnet *n, struct locnetsim *sim, loctun_log_fn log);
void loctun_stop(loctun *t);
/* W366 LOC-NET-EDGES: beim Beenden des Clients (Fenster zu, --run-seconds) allen verbundenen Mitspielern ein BYE
 * senden (authentisch, zweimal), damit sie den Abgang sofort sehen statt nach der Stillegrenze. Wartet hoechstens
 * 200 ms auf den Transport, baut nichts ab (der Prozess endet danach). */
void loctun_bye_all(loctun *t);
void loctun_bye_crash(loctun *t); /* W519: wie bye_all beim Absturz (ohne Netzbericht-Abschluss) */

/* Spiel-UDP (aus dem Spiel-Thread): dst_vip = virtuelle Adresse des Mitspielers oder INADDR_BROADCAST
 * (an alle verbundenen), Ports als Spielports. Rueckgabe len (wie UDP: angenommen) oder -1 (kein Weg). */
int loctun_dgram(loctun *t, uint32_t dst_vip, uint16_t src_gp, uint16_t dst_gp, const char *buf, int len);
/* 1 = vip gehoert einem verbundenen Mitspieler. */
int loctun_is_peer(loctun *t, uint32_t vip);
/* W519 STALLHOLD: Zustand des Mitspielers vip fuer das Halten eines Stillstands im Spielstrom (LNH_PEER_* aus
 * loc_net_core.h): 0 keine Verbindung (beendet/blockiert/unbekannt), 1 Transport hoert ihn (< 3 s), 2 still ohne
 * Abgangsmeldung, 3 Abgang gemeldet (gone_hint). rx_age_ms (optional): ms seit dem letzten Paket. */
int loctun_peer_stall(loctun *t, uint32_t vip, int *rx_age_ms);
/* W338 LOC-ROLLBACK-R2: Datagramme mit Quell- UND Zielspielport gp gehen an den Client (fn) statt an das Spiel -
 * eigener Eingabekanal ueber SBT2 (Senden mit loctun_dgram(t, vip, gp, gp, ...)). Nie an den Gast; fn laeuft im
 * Transport-Thread (kurz halten), vip = Mitspieler; Inhaltspruefung beim Empfaenger (net/SECURITY.md §18 Ae1).
 * Ratenbremse je Mitspieler wie fuer Spiel-Datagramme. fn = NULL schaltet ab. */
typedef void (*loctun_app_fn)(uint32_t vip, const uint8_t *d, int len);
void loctun_set_app(loctun *t, uint16_t gp, loctun_app_fn fn);
/* W420 LOC-CLIPDUMP: zweiter Kanal gleicher Art (Match-Regel "SBRU", loc_rule.c) auf eigenem Spielport, eigene
 * Ratenbremse (20/s). Aeltere Clients kennen den Port nicht und verwerfen das Datagramm (nicht gebunden). */
void loctun_set_app2(loctun *t, uint16_t gp, loctun_app_fn fn);
/* W437 LOC-CHAT: dritter Kanal gleicher Art (Textchat "SBCH", loc_chat.c) auf eigenem Spielport, eigene Ratenbremse
 * (4/s, Burst 8; die Nachrichtengrenze 3 je 5 s prueft der Chat). fn bekommt zusaetzlich den Pilotnamen des
 * Mitspielers aus der Sitzung (peer_info, an die Schluessel gebunden; leer ohne Treffpunkt). Nie an den Gast; der
 * entschluesselte Puffer wird nach der Zustellung geloescht. fn = NULL schaltet ab. */
typedef void (*loctun_chat_fn)(uint32_t vip, const char *pilot, const uint8_t *d, int len);
void loctun_set_chat(loctun *t, uint16_t gp, loctun_chat_fn fn);
/* W446 STEELNET-CHAT: steelnet-weiter Chat ueber den TLS-Steuerkanal des Treffpunkts (kein neuer Kanal, PROTOKOLL.md
 * W446). fn laeuft im Transport-Thread unter der Transport-Sperre (kurz halten, keine loctun_*-Aufrufe darin):
 * kind 0 = Nachricht, 1 = Verlaufszeile (beim Abonnieren), 2 = Statuscode des Dienstes (from = NULL, text = Code,
 * z. B. "rate", "muted", "account_required"). Der Klartextpuffer wird nach dem Aufruf geloescht. want = abonnieren
 * (Schalter loc_snchat); ohne want sendet der Dienst nichts. fn = NULL schaltet ab. */
typedef void (*loctun_snchat_fn)(int kind, const char *from, const uint8_t *text, int len);
void loctun_set_snchat(loctun *t, loctun_snchat_fn fn, int want);
/* W446: 0 = kein Treffpunkt bzw. Dienst ohne Chat, 1 = Dienst bietet Chat (welcome), 2 = abonniert (Tab verfuegbar). */
int loctun_snchat_state(loctun *t);
/* W446: eine fertige chat-Zeile (lch_sn_tx, ohne Zeilenumbruch) zum Senden einreihen (UI-Thread). 1 = eingereiht,
 * 0 = nicht abonniert bzw. Warteschlange voll. Die Kopie wird nach dem Senden geloescht. */
int loctun_snchat_send(loctun *t, const char *json, int len);
/* W437: eigener wirksamer Pilotname (welcome, Transkript) in buf; 1 = vorhanden (nur mit Treffpunkt). */
int loctun_my_pilot(loctun *t, char *buf, int cap);
/* W487 LIVE-GAMERTAG: Gamertag des angemeldeten Kontos (welcome, aus steelcore) in buf; 1 = vorhanden. */
int loctun_my_gamertag(loctun *t, char *buf, int cap);
/* W437: Pilotname des Mitspielers mit virtueller Adresse vip (leer = unbekannt). 1 = gefunden. */
int loctun_peer_pilot(loctun *t, uint32_t vip, char *buf, int cap);
/* W420 (Nutzerentscheid 22.11.): steelnet nur mit gleicher Client-Version (poc-native/VERSION). Rueckgabe 0 = keine
 * Ablehnung, 1 = Treffpunkt verlangt need (Fehler version_required), 2 = Mitspieler mit Version peer ("" = Feld fehlt,
 * aelterer Client) abgelehnt; need = die neuere der beiden Versionen. */
int loctun_version_state(loctun *t, char *need, int ncap, char *peer, int pcap);
/* W454: Rueckgabe 3 = LAN-Mitspieler mit anderer Version nicht verbunden (need/peer wie 2). */
/* W454 LOC-LAN-MODE: 1 = LAN-Modus aktiv (Entdeckung laeuft); peers = gefundene Mitspieler im Raum, up = verbunden. */
int loctun_lan_state(loctun *t, int *peers, int *up);

/* Zustand fuer Log/Launcher/Selbsttest. */
int loctun_links_up(loctun *t);
/* Weg und RTT des i-ten Mitspielers: path 0 = keiner, 1 = direkt, 2 = Relay. 0 = kein solcher Eintrag. */
int loctun_link_info(loctun *t, int i, int *path, double *srtt_ms, int *red, uint32_t *vip, char *relay, int rlen);
/* W521 NETPATHS: Zustand des direkten Wegs des i-ten Mitspielers (Labor/Selbsttest): direct_ok, v6 = ueber IPv6,
 * late = erst nach gescheitertem Durchstich (Wechsel Relay -> direkt), failed = Durchstich einmal gescheitert,
 * seen = ein PROBE des Partners kam an. 0 = kein solcher Eintrag. */
int loctun_link_direct(loctun *t, int i, int *direct_ok, int *v6, int *late, int *failed, int *seen);
/* W521: 1 = eigener IPv6-Kandidat gebunden und gemeldet */
int loctun_has_v6(loctun *t);
/* W494: an das Spiel gemeldete QoS-RTT (ms) aus der gemessenen; online (Treffpunkt) hoechstens 598 ms, weil das
 * Spiel ab RttMed 600 (Ping 300, Klasse 1) den Beitritt abbricht. Ohne Treffpunkt (LAN) unveraendert. */
int loctun_qos_game(int rtt_ms, int online);
const loctun_stats *loctun_get_stats(loctun *t);
uint16_t loctun_port(loctun *t);
/* Logzeilen LOCTUN (je Mitspieler) und Statusdatei; aus loc_net_tick alle 10 s. */
void loctun_log_stats(loctun *t, int waited_s);
/* Treffpunkt-Zustand: 0 aus, 1 verbinde, 2 angemeldet, 3 in Lobby. */
int loctun_lobby_state(loctun *t);
/* W517 HOSTRESUME: Fortsetzung fuer den Hinweis im Spiel: 0 nichts, 1 verbinde neu (seit >= 3 s), 2 wieder verbunden,
 * 3 Fortsetzung gescheitert (Neuaufbau). Bleibt bis zum naechsten Wechsel stehen. */
int loctun_relink_state(loctun *t);
/* W520 HOSTMIG: Host-Wechsel der steelnet-Lobby (host_changed). Liefert ein Ereignis genau einmal (1), sonst 0.
 * me_new = 1: dieser Client ist der neue Host; me_old = 1: dieser Client war der Host (Auto-Host). reason 1 = Host
 * gegangen, 2 = Host verloren, 3 = Auto-Host. Namen druckbares ASCII (Anzeige), nie ins Log. */
typedef struct { int me_new, me_old, reason; uint32_t host_cid, old_cid; char host[24], old[24]; } loctun_hostmig_ev;
int loctun_hostmig_poll(loctun *t, loctun_hostmig_ev *ev);
/* W520: Lobby-Eigenschaften fuer das Infopanel: *mig/*autoh = Lobby darf migrieren / Auto-Host, *sugg_me = 1 der
 * Dienst schlaegt diesen Client als Host vor, *am_host = dieser Client ist Host. Rueckgabe 1 = in einer Lobby. */
int loctun_hostmig_info(loctun *t, int *mig, int *autoh, int *sugg_me, int *am_host);
/* W520: Pilotname des vom Dienst vorgeschlagenen Hosts (Anzeige, druckbares ASCII), "" = keiner/man selbst. */
void loctun_hostmig_sugg_name(loctun *t, char *out, int cap);
/* W520: Mitglieder der steelnet-Lobby (aus lobby_state), 0 = keine Lobby. */
int loctun_hostmig_members(loctun *t);
/* W520: Das Spiel hat den Host verloren (Lobby-Zustand 0x31) -> host_gone an den Dienst (zaehlt nur, solange auch
 * der Dienst den Host verloren hat). W520 Auto-Host: lobby_handover anfragen (nur Host einer Auto-Host-Lobby). */
void loctun_host_gone(loctun *t);
void loctun_handover(loctun *t);
/* W328: Konto-Anmeldung am Treffpunkt: 0 keine (Gast), 1 laeuft, 2 angenommen, -1 abgelehnt. */
int loctun_auth_state(loctun *t);
/* W479 LIVE-LOGON: Konto-ID der Anmeldung (0 = Gast bzw. kein Treffpunkt); nur die Zahl, nie Schluessel. */
uint32_t loctun_account(loctun *t);
/* W482 LIVE-MATCH: Anfrage an steelnet einreihen (json = vollstaendiges Objekt ohne ref; with_vip: eigene vip wird
 * eingesetzt). Gesendet im Transport-Thread. Rueckgabe Anfrage-Nr. > 0, 0 = kein Live-Modus/keine Verbindung/voll. */
/* W514: 1, solange das Spiel im Xbox-Live-Fluss ist (t->live, W501); 0 ohne Transport. */
int loctun_is_live(const loctun *t);
int loctun_live_send(loctun *t, const char *json, int with_vip);
/* W501 ONLINE-BOTH: Spiel betritt (on = 1) bzw. verlaesst (0) den Xbox-Live-Fluss (XOnline-HLE: Kontenauswahl/Logon
 * bzw. Logon-Task geschlossen). Der Transport startet immer im System-Link-Zustand; der Netz-Thread schaltet um
 * (System-Link-Lobby verlassen bzw. Live-Anfragen beenden). Ohne loc_live/Treffpunkt wirkungslos. */
void loctun_live_mode(loctun *t, int on);
/* Ergebnis abholen: 0 laeuft, 1 fertig, -1 Fehler (err = Code des Dienstes bzw. timeout/offline), -2 unbekannt.
 * Bei 1/-1 wird die Anfrage freigegeben; sid/key aus live_session, sess (bis cap) aus live_results. */
int loctun_live_poll(loctun *t, int id, char *err, int errcap, uint8_t sid[8], uint8_t key[16], loclive_sess *sess,
                     int cap, int *nsess);
/* W485 LIVE-QOS: RTT fuer XNetQosLookup im Live-Modus. loctun_qos_target: Mitspieler mit vip (Netzreihenfolge);
 * Rueckgabe 1 = verbunden (src DIRECT/RELAY, RttMed = SRTT + 4*RTTVAR + Reserve wie die System-Link-Messung, RttMin =
 * kleinste RTT), 2 = gefundener Host ohne Verbindung (src EST: Relay-Schaetzung + Reserve), 0 = unbekannt (Vorgabe).
 * loctun_qos_service: RTT zum Treffpunkt (dessen Relay-Ping), 0 = noch nicht gemessen (Vorgabe). */
int loctun_qos_target(loctun *t, uint32_t vip, loclive_qos_val *v);
int loctun_qos_service(loctun *t, loclive_qos_val *v);
/* W491 CAMP-CLIENT: Kampagnen-Ticket ueber den angemeldeten Steuerkanal (nur Live-Modus; Spielschluessel, nie der
 * Geraeteschluessel). Das Ticket bleibt im Transport (nie im Spielspeicher). loctun_camp_want: Kampagnen-ID setzen und
 * ein Ticket anfordern (force = vorhandenes verwerfen, z. B. nach Ablehnung durch den Zugang). Der Transport erneuert
 * es danach selbst vor Ablauf. loctun_camp_ticket: > 0 = Ticketlaenge (Kopie nach out, exp = gueltig bis, Unix-s),
 * 0 = noch keins (laeuft), -1 = abgelehnt/aus/offline (err = Code des Dienstes bzw. timeout/offline/bad_ticket). */
void loctun_camp_want(loctun *t, const char *campaign, int force);
int loctun_camp_ticket(loctun *t, uint8_t *out, int cap, long long *exp, char *err, int errcap);
/* XNADDR (36 B) fuer eine vip (Netzreihenfolge), wie XNetGetTitleXnAddr/XNetInAddrToXnAddr des Netz-HLE. */
void loctun_xnaddr(loctun *t, uint32_t vip, uint8_t out[36]);
/* W440 LOC-LOBBY-MODES: Spielmodus der aktuellen steelnet-Lobby aus lobby_state.play_mode: 0 = in keiner Lobby
 * (bzw. kein Treffpunkt), 1 = sim (auch: Feld fehlt = aelterer Dienst, unbekannter Wert), 2 = arcade (LAS_*). */
int loctun_play_mode(loctun *t);
/* W334: Raumcode der eigenen Lobby ("XXXX-XXXX", leer = noch keiner). 1 = vorhanden. */
int loctun_lobby_code(loctun *t, char *buf, int cap);
/* W334: 0 ok, 1 Krypto-Selbsttest fehlgeschlagen, 2 Server-Pruefung (Pin) fehlgeschlagen, 3 kein Pin
 * (Dienst nicht eingerichtet), 4 Treffpunkt verlangt eine neuere Version. */
int loctun_sec_state(loctun *t);
/* W353 LOC-DURCHSTICH: eine Zeile fuer das F11-Paket (Treffpunkt, Weg, Ping, Verlust, sec, err) - nur Zustaende
 * und Zahlen, keine Adressen, kein Raumcode, keine Eingaben. 0 = kein Transport. */
int loctun_diag(loctun *t, char *buf, int cap);
/* W373 LOC-PUBLIC-LOBBY TEST (loc_net_selftest): eine Steuerzeile des Treffpunkts ohne Netz einspeisen (line = NULL:
 * nur Zeitschritt), my_cid > 0 = angemeldet; danach rechnet die oeffentliche Logik einen Schritt. out = seither vom
 * Client gesendete Steuerzeilen ('\n'-getrennt). Rueckgabe 1 = oeffentlicher Modus, 0 = nicht, -1 = Fehler. */
int loctun_test_line(loctun *t, uint32_t my_cid, const char *line, char *out, int cap);
/* W500 LOBBY-RESUME TEST (loc_net_selftest): op 1 = wie nach dem TLS-Aufbau (Schluesselpaar, Konto arg), 2 = Abriss
 * der Steuerverbindung ("eof"), 3 = hello senden, 4 = Zustand als Text, 5 = Frist jetzt ablaufen lassen + lb_poll,
 * 6 = Zeitschritt lb_poll. out = gesendete Steuerzeilen (op 1-3, 5, 6) bzw. Zustand (op 4). 0 = ok, -1 = Fehler. */
int loctun_test_resume(loctun *t, int op, uint32_t arg, char *out, int cap);
/* W373: Zahl der lokal vertretenen offenen oeffentlichen Spiele (Geister). */
int loctun_pub_games(loctun *t);
/* W443 LOC-LOBBY-LIST: Modus (LAS_SIM/LAS_ARCADE, 0 = keine solche Lobby) und Ping-Schaetzung (ms, -1 = unbekannt)
 * der lokal vertretenen oeffentlichen Lobbys lid[0..n-1] (Lobby-Id aus der XNKID der Host-Info). Rueckgabe: gefunden. */
int loctun_pub_lookup(loctun *t, const int *lid, int n, int *mode, int *est);
/* W460 LOC-LOBBY-WIDGET (GitHub #95): offene oeffentliche Lobbys fuer das Widget im Hauptmenue/MatchMaking.
 * loctun_pub_watch(t, 1): solange das Widget sichtbar ist, fragt der Transport die Liste hoechstens alle 12 s beim
 * Treffpunkt an (gleiche Anfrage wie die Suche, "lobby_list"); 0 = aus. loctun_pub_list: hoechstens cap Eintraege
 * (aelteste Lobby zuerst), *total = alle offenen. Rueckgabe: Zahl in out, LOCTUN_PL_OFF = kein oeffentlicher
 * Treffpunkt (System Link, Raumcode), LOCTUN_PL_WAIT = noch nicht angemeldet bzw. Liste fehlt/aelter als 40 s. */
#define LOCTUN_PL_OFF (-1)
#define LOCTUN_PL_WAIT (-2)
typedef struct loctun_pub_entry {
    int lid, mode, est;  /* Lobby-Id, LAS_SIM/LAS_ARCADE, Ping-Schaetzung ms (-1 = unbekannt) */
    int cur, max;        /* belegte / alle Sitze */
    char pilot[24];      /* Host-Pilot (UTF-8) */
} loctun_pub_entry;
void loctun_pub_watch(loctun *t, int on);
int loctun_pub_list(loctun *t, loctun_pub_entry *out, int cap, int *total);
/* W374 LOC-NET-OVERLAY (F3): Werte des i-ten Mitspielers fuer das Netz-Overlay (nur Zahlen und der Pilotname
 * aus dem Treffpunkt, keine Adressen). 0 = kein solcher Eintrag. */
#define LOCTUN_HUD_ALT 4 /* W395: Wege je Mitspieler im Overlay */
typedef struct loctun_peer_hud {
    int up, path;                  /* path 0 keiner, 1 direkt, 2 Relay */
    double srtt, rtt_min, rttvar;  /* ms */
    int loss_in, loss_peer;        /* Promille: Verlust beim Empfang bzw. vom Mitspieler gemeldet (unsere Pakete) */
    char pilot[24];                /* Pilotname (leer ohne Treffpunkt) */
    /* W395 LOC-ROUTE: aktiver Weg und die gemessenen Alternativen (nur Namen und Zahlen) */
    char route[24];                /* "direct" oder Relayname, leer = keiner */
    int nalt;
    struct { char name[24]; double rtt; int state, active; } alt[LOCTUN_HUD_ALT]; /* rtt < 0: keine Antwort;
                                                                                * state 0 aus, 1 verdaechtig, 2 gut */
    int nswitch, nfail;            /* Wechsel insgesamt / davon wegen Ausfall */
    int rt_on, decided;            /* eigene Wegwahl aktiv / erste Wahl nach dem Messfenster getroffen */
} loctun_peer_hud;
int loctun_peer_hud_get(loctun *t, int i, loctun_peer_hud *o);
/* W496 NET-TELEMETRY: Spielzustand fuer den Netzbericht (aus dem Transport-Thread hoechstens 1x/s gelesen, nur Zahlen):
 * Mission laeuft, Rollbacks gesamt, Desync-Enden gesamt, Mitspieler der Mission (-1 = unbekannt). 0 = nicht lieferbar. */
typedef struct loctun_game_info {
    int in_mission;
    unsigned rb_total, desyncs;
    int npeers;
    unsigned gpu_vendor;  /* PCI-Hersteller-ID der benutzten GPU (0 = unbekannt) */
    /* W519 STALLHOLD (Zaehler seit Prozessstart, der Bericht bildet die Differenz je Match): Luecken im Spielstrom nach
     * Laenge (< 5, < 15, < 30, < 60, >= 60 s), gehaltene Stillstaende, davon fortgesetzt bzw. bis zur Obergrenze */
    unsigned st_gh[5], st_holds, st_ok, st_cap;
} loctun_game_info;
typedef int (*loctun_game_fn)(loctun_game_info *o);
void loctun_set_game_fn(loctun *t, loctun_game_fn fn);
/* W496 TEST (loc_net_selftest): Netzbericht ohne Netz pruefen. op: 1 Spielzustand setzen (a = in_mission, b = Rollbacks,
 * c = Desyncs), 2 Zeitschritt um a ms + Takt (out = gesendete Steuerzeilen), 3 angemeldet (a = welcome.netlog), 4 getrennt
 * (s = Grund), 5 Lobby (a = beigetreten), 6 Warteschlange (Rueckgabe Zahl, out = erste Zeile), 7 Prozessende.
 * Rueckgabe -1 = Netzbericht aus (keine Zustimmung bzw. kein Treffpunkt). */
int loctun_netrep_test(loctun *t, int op, int a, int b, int c, const char *s, char *out, int cap);
