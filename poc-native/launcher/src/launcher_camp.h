#pragma once
/* launcher_camp.h - W491 CAMP-CLIENT (Launcher): Status der LoC-Kampagne im Xbox-Live-Modus.
 *
 * Kampagne "verfuegbar" nur, wenn ALLE gelten: steelnet-Konto angemeldet (letzter status), Gamertag gesetzt,
 * keine Mindestversion verletzt (W421, /v1/info) und Steelbound-Zugang des Kampagnenservers erreichbar. Die ersten
 * drei Punkte kommen aus den vorhandenen Quellen (acc_live_state, loc_ver_gate); die Erreichbarkeit prueft
 * lcamp_probe: TCP-Verbindung zu SB_STEELNET_HOST:47100 (Saison) bzw. :47103 (Testkampagne), danach schliessen -
 * KEIN TLS, kein Vorspann, keine Anwendungsdaten, keine Adresse ins Log (W534 AVPRE; vorher TLS-Handshake mit Pin).
 * Kampagne "aus", wenn kein SPKI-Pin (SB_CAMPAIGN_SPKI1/2, steelnet_pins.h) eingebaut ist. lcamp_probe_tls (TCP + TLS
 * + Pin, harness/src/sb_tls.c, derselbe SChannel-Client wie die Kampagnenbruecke des Spiels) nur fuer die
 * Entwickler-CLI; die Konto-API nutzt WinHTTP und sendet immer HTTP-Kopfzeilen, das waere fuer den Zugang ein
 * ungueltiger Vorspann.
 * Test-Ueberschreibung (nur Loopback): SB_CAMPAIGN_ADDR=127.0.0.1:<port>, SB_CAMPAIGN_SPKI=<hex64>. ASCII. */
#include <stddef.h>

/* Ergebnis der Erreichbarkeitspruefung */
enum { LCAMP_R_UNKNOWN = 0, LCAMP_R_OK = 1, LCAMP_R_FAIL = 2 };

/* Kampagnenstatus (Reihenfolge der Pruefung = Prioritaet des Hinweises) */
enum {
    LCAMP_OK = 0,   /* verfuegbar */
    LCAMP_OFF,      /* steelnet in dieser Ausgabe nicht eingerichtet */
    LCAMP_NOACC,    /* kein steelnet-Konto */
    LCAMP_UNK,      /* Kontostatus unbekannt (steelnet nicht erreicht) */
    LCAMP_GTOFF,    /* Gamertags auf steelnet gerade nicht verfuegbar */
    LCAMP_NOGT,     /* kein Gamertag */
    LCAMP_OLD,      /* Launcher aelter als min_client_version */
    LCAMP_CHECK,    /* Zugang wird geprueft (noch kein Ergebnis) */
    LCAMP_UNREACH,  /* Zugang nicht erreichbar bzw. Pin passt nicht */
    LCAMP_N
};

/* Rein (Selbsttest): acc = ACC_LIVE_* (launcher_gui_int.h: OK, OFF, NOACC, UNK, NOGT, GTOFF), outdated = 1 bei
 * verletzter Mindestversion, reach = LCAMP_R_*. */
int lcamp_state(int acc, int outdated, int reach);

/* Ziel der Pruefung: Host, Port, Pins (normiert 64 Hex, leer = keiner). 0 = kein Pin (Kampagne aus). */
typedef struct {
    char host[128];
    int port;
    char pin1[65], pin2[65];
    int test; /* Loopback-Ueberschreibung aktiv */
} lcamp_target_t;
int lcamp_target(int test_campaign, lcamp_target_t *t);

/* Blockierend (nur im Hintergrund-Thread bzw. CLI). W534 AVPRE: lcamp_probe (Oberflaeche) = nur TCP-Verbindung,
 * dann schliessen; kein TLS im Launcher (kein SChannel/Secur32 im ausgelieferten Launcher), den Pin prueft der
 * Spielprozess beim echten Verbinden. lcamp_probe_tls = TCP + TLS-Handshake + Pin (nur `--steelnet campaign-probe`,
 * Entwickler-Builds; im Release ohne Aufrufer und vom Linker entfernt).
 * 0 = erreichbar; sonst 1 und err = "off" (kein Pin), "net" (keine TCP-Verbindung), "pin" (Pin passt nicht, nur _tls),
 * "tls" (Handshake abgelehnt/abgebrochen, nur _tls), "timeout". Nie Adresse/IP in err. */
int lcamp_probe(int test_campaign, int timeout_ms, char *err, size_t ecap);
int lcamp_probe_tls(int test_campaign, int timeout_ms, char *err, size_t ecap);
/* W534 Selbsttest (Loopback): why leer = bestanden */
void lcamp_selftest_tcp(char *why, size_t cap);
