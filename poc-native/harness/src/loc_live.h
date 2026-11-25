/* loc_live.h - W479 LIVE-LOGON: Kern des Xbox-Live-Modus fuer Line of Contact (ohne Spielspeicher, testbar).
 *
 * steelnet uebernimmt die Rolle von Xbox Live. Dieser Kern legt fest, was die XOnline-HLE dem Spiel meldet:
 * Logon-/Task-Rueckgaben, Dienststatus, Gamertag (aus dem steelnet-Pilotnamen) und Pseudo-XUID (aus der
 * steelnet-Konto-ID). Die Hooks selbst stehen in loc_live_hle.c. Doku: docs/loc/LOC_LIVE.md. */
#ifndef LOC_LIVE_H
#define LOC_LIVE_H

#include <stddef.h>
#include <stdint.h>

/* ---- HRESULTs (XDK 5788, Werte aus dem Spielcode, LOC_RE1 §2.1/§3.3) ---- */
#define LOCLIVE_S_OK 0x00000000u
#define LOCLIVE_S_FALSE 0x00000001u
#define LOCLIVE_TASK_RUNNING 0x00000000u      /* XONLINETASK_S_RUNNING: Spiel pollt weiter */
#define LOCLIVE_TASK_SUCCESS 0x001500F0u      /* XONLINETASK_S_SUCCESS (auch: Name gueltig) */
#define LOCLIVE_TASK_RESULTS 0x001500F1u      /* XONLINETASK_S_RESULTS_AVAIL (Friends-Liste da) */
#define LOCLIVE_LOGON_OK 0x001510F0u          /* XONLINE_S_LOGON_CONNECTION_ESTABLISHED */
#define LOCLIVE_E_NO_NETWORK 0x80151000u      /* Spiel: Meldung 5 */
#define LOCLIVE_E_NO_SERVICE 0x80151001u      /* Logon fehlgeschlagen (Spiel: allgemeine Meldung 3) */
#define LOCLIVE_E_SVC_UNAVAILABLE 0x80151102u /* Dienst voruebergehend nicht verfuegbar -> "Campaign" grau */
#define LOCLIVE_E_FAIL 0x80004005u
#define LOCLIVE_E_INVALIDARG 0x80070057u

/* ---- Dienste (XOnlineLogon des Spiels, FUN_00119990) ---- */
#define LOCLIVE_SVC_MATCH 6u
#define LOCLIVE_SVC_FEEDBACK 8u
#define LOCLIVE_SVC_ACCOUNT 9u
#define LOCLIVE_SVC_CAMPAIGN 0x43430009u

/* Rueckgabe von XOnlineGetServiceInfo je Dienst: 6/8/9 -> S_OK, Kampagne (und alles andere) -> 0x80151102.
 * Nie "Update noetig" oder "nicht berechtigt" (0x80151101 startet im Spiel den TEKKI-Automaten). */
uint32_t loclive_service_hr(uint32_t svc);

/* ---- XONLINE_USER (0x70 Byte, Spielcode: Schritt 0x70, hr bei +0x6C, Gamertag +0x0C) ---- */
#define LOCLIVE_USER_SIZE 0x70
#define LOCLIVE_USER_OFF_XUID 0x00   /* u64 qwUserID */
#define LOCLIVE_USER_OFF_XFLAGS 0x08 /* u32 dwUserFlags (Gast-Bits 0..1) */
#define LOCLIVE_USER_OFF_TAG 0x0C    /* char[16] */
#define LOCLIVE_USER_OFF_HR 0x6C
#define LOCLIVE_MAX_LOGON 4

void loclive_fill_user(uint8_t u[LOCLIVE_USER_SIZE], uint64_t xuid, const char *tag, uint32_t hr);

/* ---- Pseudo-XUID ----
 * qwUserID = 0x53424C01_xxxxxxxx: oberes Wort "SBL" + Version 0x01, unteres Wort = steelnet-Konto-ID (> 0).
 * Bijektiv, fest, nur aus der Konto-ID (nie aus dem Namen). Echte Formen: 0x0009... (Live-Konten),
 * 0xE000... (Offline), 0 (kein Konto) - keine davon beginnt mit 0x5342. 0 = kein Konto. */
#define LOCLIVE_XUID_PREFIX 0x53424C01u
uint64_t loclive_xuid(uint32_t account_id);
int loclive_xuid_is_pseudo(uint64_t xuid);
uint32_t loclive_xuid_account(uint64_t xuid); /* 0 = keine Pseudo-XUID */

/* ---- Gamertag (W487 LIVE-GAMERTAG) ----
 * Der Gamertag ist ein Merkmal des steelnet-Kontos (in steelcore gefuehrt, im Launcher gewaehlt) und kommt im
 * welcome des Treffpunkts; er wird nicht mehr aus dem Pilotnamen abgeleitet (LOC_LIVE.md §4.2).
 * Regel (wie steelcore): 1-15 Zeichen, A-Z a-z 0-9 und einzelne Leerzeichen, vorn ein Buchstabe, hinten kein
 * Leerzeichen. 1 = gueltig. */
#define LOCLIVE_TAG_MAX 15
int loclive_tag_valid(const char *tag);

/* ---- Merkdatei <hdd_root>\steelbound_live.txt: Zeilen "a<Konto-ID> <Ordner-Tag>" ----
 * Ordner-Tag = Gamertag, unter dem die Piloten des Kontos zuletzt lagen (T:\<Tag>CAMP, T:\<Tag>FREE), bzw. ein
 * geparkter Name "_a<Konto-ID>" (nie ein gueltiger Gamertag). Reine Funktionen auf dem Dateitext: */
#define LOCLIVE_PIN_MAX 32768
int loclive_pin_get(const char *text, uint32_t account_id, char out[16]);              /* 1 = Eintrag */
uint32_t loclive_pin_owner(const char *text, const char *tag, uint32_t except_acct);  /* anderes Konto, ohne G/K */
/* neuer Dateitext: Zeile des Kontos ersetzt bzw. angehaengt (kaputte Zeilen fallen weg); 0 = ok, -1 = Fehler */
int loclive_pin_put(const char *text, uint32_t account_id, const char *tag, char *out, size_t cap);
void loclive_park_tag(uint32_t account_id, char out[16]);                              /* "_a<Konto-ID>" */
/* Gamertag-Wechsel: welche Ordner umbenannt werden muessen, damit die Piloten erhalten bleiben.
 * park_*: ein anderes Konto hat auf diesem PC noch einen Eintrag mit dem neuen Tag (ohne Gross/Klein; der Tag ist
 * weltweit eindeutig, der Eintrag also veraltet) -> dessen Ordner auf "_a<Konto>" parken (beim naechsten Logon
 * jenes Kontos zieht er auf dessen aktuellen Tag um). own_*: eigener alter Ordner -> neuer Tag (leer = nichts).
 * Rueckgabe: Zahl der Umbenennungen (0..2), -1 = ungueltig. */
typedef struct {
    uint32_t park_acct;
    char park_from[16], park_to[16];
    char own_from[16], own_to[16];
} loclive_tag_plan_t;
int loclive_tag_plan(const char *text, uint32_t account_id, const char *tag, loclive_tag_plan_t *p);
int loclive_pin_lookup(const char *path, uint32_t account_id, char out[16]); /* Datei lesen + loclive_pin_get */
/* Ordner <tdir>\<from>CAMP und <tdir>\<from>FREE nach <to>CAMP/FREE umbenennen (tdir = Host-Ordner von T:).
 * 1 = erledigt bzw. nichts zu tun (Quelle fehlt), 0 = Ziel existiert schon (nichts verschoben), -1 = Fehler
 * (halb Verschobenes wird zurueckgenommen). Nur Gross/Klein anders: wird umbenannt. */
int loclive_move_folders(const char *tdir, const char *from, const char *to);

/* ---- Tasks (XONLINETASK_HANDLE) ----
 * Jede Task hat eine Art und einen Zaehler; loclive_task_step liefert, was XOnlineTaskContinue melden soll.
 * Logon: laeuft, bis die steelnet-Anmeldung entschieden ist (auth: 2 = angenommen, -1 = abgelehnt,
 * 0 = kein Konto/Gast, 1 = laeuft), hoechstens timeout_ms; dann LOGON_OK bzw. E_NO_SERVICE (bleibt so). */
enum {
    LLT_NONE = 0,
    LLT_LOGON,
    LLT_FRIENDS,      /* XOnlineFriendsStartup: laeuft dauerhaft */
    LLT_FRIENDS_ENUM, /* XOnlineFriendsEnumerate: einmal RESULTS (leere Liste), dann laeuft; nach Finish SUCCESS */
    LLT_MUTE,         /* XOnlineMutelistStartup: laeuft dauerhaft */
    LLT_DONE,         /* sofort SUCCESS (Mutelist-Get, Namenspruefung, Feedback, Friends-Aktionen) */
    LLT_NET,          /* W482: steelnet-Anfrage (Session/Suche): laeuft bis result gesetzt (HLE pollt den Transport) */
    LLT_KINDS
};
typedef struct loclive_task {
    uint32_t magic; /* LOCLIVE_TASK_MAGIC solange offen */
    int kind;
    uint32_t steps;
    uint32_t start_ms;
    uint32_t result; /* Logon/Netz: Endergebnis (0 = offen) */
    int finished;    /* Friends-Enum: Finish gerufen */
    /* W482 LIVE-MATCH (nur LLT_NET) */
    int op;          /* LLO_* */
    int req;         /* Anfrage-Nr. beim Transport (0 = keine) */
    uint8_t sid[8], key[16]; /* Create: XNKID/XNKEY vom Dienst */
    void *res_mem;   /* Suche/Find: Ergebnisblock (Zeigerliste + XMATCH_SEARCHRESULTs), frei bei TaskClose */
    uint32_t res_len, nres;
} loclive_task;
enum { LLO_NONE = 0, LLO_CREATE, LLO_UPDATE, LLO_DELETE, LLO_SEARCH, LLO_FIND };
#define LOCLIVE_TASK_MAGIC 0x4C4C544Bu
#define LOCLIVE_LOGON_TIMEOUT_MS 30000u

uint32_t loclive_task_step(loclive_task *t, int auth, uint32_t now_ms, uint32_t timeout_ms);

/* ==== W482 LIVE-MATCH: Matchmaking ueber steelnet (Sessions, Suche, Find) - Formate ohne Spielspeicher ====
 * XONLINE_ATTRIBUTE (16 B): dwAttributeID, fChanged, 8-B-Wert bzw. LPWSTR (+8). Typ aus der ID: Bit 0x00100000 =
 * String (WCHAR), sonst Integer 64 bit. Suchparameter: dieselbe Struktur, dwAttributeID = Parametertyp (0 Integer,
 * 0x00100000 String, 0x00F00000 NULL = egal); Parameter i gehoert zu Attribut i der Prozedur (LOC_RE1 §2.3/§2.4). */
#define LOCLIVE_ATTR_STRING 0x00100000u
#define LOCLIVE_ATTR_NULL 0x00F00000u
#define LOCLIVE_ATTR_TYPEMASK 0x00F00000u
#define LOCLIVE_ATTR_SIZE 16
#define LOCLIVE_MAX_ATTR 32
#define LOCLIVE_STR_MAX 15            /* WCHAR-Zeichen (+ NUL), wie steelnet */
#define LOCLIVE_MAX_RESULTS 50
#define LOCLIVE_SR_SIZE 0x54          /* XMATCH_SEARCHRESULT */
#define LOCLIVE_XNADDR_SIZE 36
#define LOCLIVE_PROC_MATCH 1u
#define LOCLIVE_PROC_CHAT 2u
#define LOCLIVE_REQ_TIMEOUT_MS 15000u /* steelnet-Antwort; danach E_FAIL (Spiel: zurueck zur Live-Init) */

typedef struct loclive_attr {
    uint32_t id;
    int is_str, is_null;   /* is_null nur in Suchparametern */
    int64_t v;
    uint16_t w[LOCLIVE_STR_MAX + 1]; /* NUL-terminiert */
} loclive_attr;

typedef struct loclive_sess {
    uint8_t sid[8];        /* XNKID */
    uint32_t lobby_id;
    uint32_t vip;          /* Host-vip (Netzreihenfolge), vom Transport geprueft */
    uint32_t slots[4];     /* public open, public filled, private open, private filled */
    int nattr;
    loclive_attr attr[LOCLIVE_MAX_ATTR];
} loclive_sess;

/* Attribut-IDs der Prozeduren in Create-Reihenfolge (Suchparameter i -> ID); n = Anzahl (19 bzw. 13), 0 = unbekannt */
int loclive_proc_ids(uint32_t proc, const uint32_t **ids);
/* Art einer Session aus ihren Attributen: 2 = Chatraum (traegt 0x13 oder 0x1A), sonst 1 = Match */
int loclive_kind(const loclive_attr *a, int n);
/* JSON-Liste [[id,wert],...] (Strings UTF-8, JSON-maskiert; is_null -> null). Laenge oder -1 (zu klein). */
int loclive_attrs_json(const loclive_attr *a, int n, char *out, int cap);
/* XNKEY im Suchergebnis: oeffentlicher Platzhalter "SBLIVEK" + 0 + XNKID (der echte Schluessel geht nur an
 * Eigentuemer/Mitglieder; das XNet-HLE prueft keine Schluessel, Spielverkehr ist SBT2-verschluesselt). */
void loclive_key_placeholder(const uint8_t sid[8], uint8_t key[16]);
/* Groesse eines Ergebnisblocks: Kopf 0x54 + nattr * 16 + Strings (je 32 B). */
uint32_t loclive_result_size(const loclive_sess *s);
/* Ergebnisblock bauen: dst = lokaler Puffer (>= loclive_result_size), va = Adresse, unter der das Spiel ihn sieht
 * (String-Zeiger). dwReserved = Blockgroesse (fuer Parse). */
void loclive_result_build(uint8_t *dst, uint32_t va, const loclive_sess *s, const uint8_t xnaddr[LOCLIVE_XNADDR_SIZE],
                          const uint8_t key[16]);
/* XOnlineMatchSearchParse: Attribute des Ergebnisblocks (res, Spieladresse va, len Byte) nach den Specs (je
 * {Typ, Laenge}) in out schreiben: Integer 8 B, String Laenge B (WCHAR, NUL, Rest 0). Rueckgabe S_OK bzw. E_FAIL
 * (Typ passt nicht, zu wenige Attribute, Block kaputt, out zu klein); *used = geschriebene Bytes. */
uint32_t loclive_parse(const uint8_t *res, uint32_t va, uint32_t len, uint32_t nspec, const uint32_t *spec,
                       uint8_t *out, uint32_t cap, uint32_t *used);

/* ---- W485 LIVE-QOS: XNetQosLookup im Live-Modus (LOC_RE1 §2.5, Spezifikation Punkt 7) ----
 * XNQOS = u32 cxnqos, u32 cxnqosPending, XNQOSINFO[cxnqos] je 24 B: u8 bFlags, u8 res, u16 cProbesXmit,
 * u16 cProbesRecv, u16 cbData, u32 pbData, u16 wRttMin, u16 wRttMed, u32 dwUpBitsPerSec, u32 dwDnBitsPerSec.
 * Das Spiel bewertet je Ziel: Klasse aus RttMed/2 (< 100 ms -> 4, < 200 -> 3, < 300 -> 2, sonst 1) und wertet ab,
 * wenn die Bitrate unter der Tabelle 0x2518D4 liegt (bis 205 200 bit/s bei 10 Spielern). Ohne COMPLETE bricht
 * Create ab. Die RTT kommt aus dem echten Transport (loc_tun), die Bitrate ist eine feste, ehrliche Zusage je Weg. */
#define LOCLIVE_QOS_INFO_SIZE 24
#define LOCLIVE_QOS_MAX_TARGETS 64
#define LOCLIVE_QOS_F_COMPLETE 0x01u
#define LOCLIVE_QOS_F_CONTACTED 0x02u
#define LOCLIVE_QOS_MIN_BPS 205200u      /* Tabelle 0x2518D4, 10 Spieler: darunter wertet das Spiel ab */
#define LOCLIVE_QOS_BPS_DIRECT 10000000u /* direkter Weg (wie System Link) */
#define LOCLIVE_QOS_BPS_RELAY 2000000u   /* ueber ein steelnet-Relay (Spielverkehr LoC < 64 kbit/s je Peer) */
#define LOCLIVE_QOS_BPS_SERVICE 2000000u /* Dienst 6 (steelnet) */
#define LOCLIVE_QOS_BPS_EST 1000000u     /* Host ohne Verbindung (Schaetzung ueber das Relay) */
#define LOCLIVE_QOS_RTT_DEFAULT 100      /* nichts gemessen: konservativ (Klasse 4 bleibt erreichbar) */
/* Herkunft eines Werts (Log/Selbsttest) */
#define LOCLIVE_QOS_SRC_NONE 0    /* nichts bekannt -> Vorgabe */
#define LOCLIVE_QOS_SRC_DIRECT 1  /* verbundener Mitspieler, direkter Weg */
#define LOCLIVE_QOS_SRC_RELAY 2   /* verbundener Mitspieler ueber ein Relay */
#define LOCLIVE_QOS_SRC_EST 3     /* gefundener Host (Geist), Schaetzung */
#define LOCLIVE_QOS_SRC_SERVICE 4 /* Dienst (steelnet-Treffpunkt) */
typedef struct loclive_qos_val {
    int src;     /* LOCLIVE_QOS_SRC_* */
    int rtt_min; /* ms */
    int rtt_med; /* ms */
} loclive_qos_val;
/* Bitrate je Herkunft (immer >= LOCLIVE_QOS_MIN_BPS). */
uint32_t loclive_qos_bps(int src);
/* Einen XNQOSINFO-Eintrag fuellen: COMPLETE|CONTACTED, Proben gesendet = empfangen = probes (mind. 1), RTT auf
 * 1..4999 ms begrenzt, min <= med; Bitrate nach Herkunft; kein Listen-Datenblock (cbData 0). */
void loclive_qos_entry(uint8_t e[LOCLIVE_QOS_INFO_SIZE], unsigned probes, const loclive_qos_val *v);
/* Ganzen XNQOS-Block fuer n Ziele (1..64) schreiben (Groesse 8 + 24 n), cxnqosPending = 0. Rueckgabe: Groesse. */
uint32_t loclive_qos_block(uint8_t *q, uint32_t cap, unsigned probes, const loclive_qos_val *v, unsigned n);
/* Klasse, wie das Spiel sie aus einem Eintrag bildet (FUN_00052390, ohne Abwertung): fuer Selbsttest/Log. */
int loclive_qos_class(const uint8_t e[LOCLIVE_QOS_INFO_SIZE]);

#endif
