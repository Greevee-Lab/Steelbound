/* loc_live_hle.h - W479 LIVE-LOGON: XOnline-HLE (Xbox-Live-Modus, nur loc_live=1). */
#pragma once
#include "loc_tun.h"

/* Lenkt die XOnline-Einstiege um (Bytevergleich, sonst exit 7). 0 = loc_live aus (nichts geaendert). */
int loc_live_install(loctun *tun);
int loc_live_active(void);
/* W501 ONLINE-BOTH: Spiel bindet den System-Link-Suchport 1201 (nur im System-Link-Weg, nie in Xbox Live/Live-Match)
 * waehrend des Live-Flusses -> Xbox Live gilt als verlassen (Rueckfall, falls das Spiel den Logon-Task nicht schliesst). */
void loc_live_syslink_seen(void);

/* W485 LIVE-QOS: Aufrufzaehler der Nebendienste (Presence, Friends, Mutelist, Feedback, Namenspruefung, QoS) -
 * nur Zahlen, nie Inhalte. loc_live_log_counts: eine Zeile mit allen Zaehlern > 0, nur wenn sich etwas geaendert
 * hat (final = 1: immer, beim Beenden). Ohne Live-Modus tun alle nichts. */
enum {
    LLC_PRESENCE_SET, LLC_NOTIFICATION_GET, LLC_INVITE_GET, LLC_FRIENDS_STARTUP, LLC_FRIENDS_ENUM,
    LLC_FRIENDS_ENUM_FINISH, LLC_FRIENDS_LATEST, LLC_FRIENDS_TITLENAME, LLC_FRIENDS_REMOVE, LLC_FRIENDS_REQUEST,
    LLC_FRIENDS_INVITE, LLC_FRIENDS_REVOKE, LLC_FRIENDS_ANSWER_REQ, LLC_FRIENDS_ANSWER_INV, LLC_FRIENDS_JOIN,
    LLC_MUTE_STARTUP, LLC_MUTE_GET, LLC_MUTE_ADD, LLC_MUTE_REMOVE, LLC_FEEDBACK, LLC_VERIFY_NICK, LLC_TITLE_UPDATE,
    LLC_SAME_TITLE, LLC_QOS_LOOKUP, LLC_QOS_LISTEN, LLC_QOS_RELEASE, LLC_N
};
void loc_live_count(int which);
void loc_live_log_counts(int final);
/* Zaehlerstand lesen (Selbsttest/Diagnose); -1 = ungueltig. */
long loc_live_count_get(int which);
/* QoS-Bereitschaft des Hosts (XNetQosListen) bzw. Werte eines Lookups fuer das Log (nur Zahlen, erste 6 und
 * danach jede 50.). */
void loc_live_qos_listen(uint32_t flags, uint32_t bps);
void loc_live_qos_note(uint32_t cxna, uint32_t cina, uint32_t probes, const loclive_qos_val *v, unsigned n, long seq);
