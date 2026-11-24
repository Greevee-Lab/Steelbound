/* loc_tun_stub.c - W324 LOC-NETPLAY: Platzhalter des UDP-Transports fuer die Community-Fassung.
 *
 * Der eigene Transport (loc_tun.c) spricht mit einem Internet-Treffpunkt (DNS, oeffentliche Relays) und ist
 * vorerst nur in der privaten Fassung (Community: Netz-HLE nur Loopback/LAN, kein DNS - editions/clean_allow.tsv).
 * Mit loc_transport=udp meldet die Community-Fassung das im Log und bleibt beim Original-Transport. */
#include "loc_tun.h"
#include <string.h>

void loctun_cfg_default(loctun_cfg *c)
{
    memset(c, 0, sizeof *c);
}

int loctun_available(void) { return 0; }

loctun *loctun_start(const loctun_cfg *c, struct locnet *n, struct locnetsim *sim, loctun_log_fn log)
{
    (void)c; (void)n; (void)sim;
    if (log) log("LOCTUN: eigener Transport nur in der privaten Fassung - loc_transport=tcp");
    return NULL;
}

void loctun_stop(loctun *t) { (void)t; }
void loctun_bye_all(loctun *t) { (void)t; }
int loctun_dgram(loctun *t, uint32_t dst_vip, uint16_t src_gp, uint16_t dst_gp, const char *buf, int len)
{
    (void)t; (void)dst_vip; (void)src_gp; (void)dst_gp; (void)buf; (void)len;
    return -1;
}
int loctun_is_peer(loctun *t, uint32_t vip) { (void)t; (void)vip; return 0; }
void loctun_set_app(loctun *t, uint16_t gp, loctun_app_fn fn) { (void)t; (void)gp; (void)fn; } /* W338 */
int loctun_version_state(loctun *t, char *need, int ncap, char *peer, int pcap) /* W420 */
{
    (void)t;
    if (need && ncap > 0) need[0] = 0;
    if (peer && pcap > 0) peer[0] = 0;
    return 0;
}
void loctun_set_app2(loctun *t, uint16_t gp, loctun_app_fn fn) { (void)t; (void)gp; (void)fn; } /* W420 */
int loctun_lan_state(loctun *t, int *peers, int *up) /* W454 */
{
    (void)t;
    if (peers) *peers = 0;
    if (up) *up = 0;
    return 0;
}
void loctun_set_chat(loctun *t, uint16_t gp, loctun_chat_fn fn) { (void)t; (void)gp; (void)fn; } /* W437 */
void loctun_set_snchat(loctun *t, loctun_snchat_fn fn, int want) { (void)t; (void)fn; (void)want; } /* W446 */
int loctun_snchat_state(loctun *t) { (void)t; return 0; } /* W446 */
int loctun_snchat_send(loctun *t, const char *json, int len) { (void)t; (void)json; (void)len; return 0; } /* W446 */
int loctun_my_pilot(loctun *t, char *buf, int cap) /* W437 */
{
    (void)t;
    if (buf && cap > 0) buf[0] = 0;
    return 0;
}
int loctun_peer_pilot(loctun *t, uint32_t vip, char *buf, int cap) /* W437 */
{
    (void)t; (void)vip;
    if (buf && cap > 0) buf[0] = 0;
    return 0;
}
int loctun_links_up(loctun *t) { (void)t; return 0; }
int loctun_link_info(loctun *t, int i, int *path, double *srtt_ms, int *red, uint32_t *vip, char *relay, int rlen)
{
    (void)t; (void)i; (void)path; (void)srtt_ms; (void)red; (void)vip; (void)relay; (void)rlen;
    return 0;
}
const loctun_stats *loctun_get_stats(loctun *t)
{
    static loctun_stats z;
    (void)t;
    return &z;
}
uint16_t loctun_port(loctun *t) { (void)t; return 0; }
void loctun_log_stats(loctun *t, int waited_s) { (void)t; (void)waited_s; }
int loctun_lobby_state(loctun *t) { (void)t; return 0; }
int loctun_play_mode(loctun *t) { (void)t; return 0; } /* W440 */
int loctun_auth_state(loctun *t) { (void)t; return 0; }
int loctun_lobby_code(loctun *t, char *buf, int cap)
{
    (void)t;
    if (buf && cap > 0) buf[0] = 0;
    return 0;
}
int loctun_sec_state(loctun *t) { (void)t; return 0; }
int loctun_test_line(loctun *t, uint32_t my_cid, const char *line, char *out, int cap) /* W373 */
{
    (void)t; (void)my_cid; (void)line;
    if (out && cap > 0) out[0] = 0;
    return -1;
}
int loctun_pub_games(loctun *t) { (void)t; return 0; } /* W373 */
int loctun_pub_lookup(loctun *t, const int *lid, int n, int *mode, int *est) /* W443 */
{
    int i;
    (void)t; (void)lid;
    for (i = 0; mode && est && i < n; i++) {
        mode[i] = 0;
        est[i] = -1;
    }
    return 0;
}
void loctun_pub_watch(loctun *t, int on) { (void)t; (void)on; } /* W460 */
int loctun_pub_list(loctun *t, loctun_pub_entry *out, int cap, int *total) /* W460 */
{
    (void)t; (void)out; (void)cap;
    if (total) *total = 0;
    return LOCTUN_PL_OFF;
}
int loctun_peer_hud_get(loctun *t, int i, loctun_peer_hud *o) /* W374 */
{
    (void)t; (void)i; (void)o;
    return 0;
}

int loctun_diag(loctun *t, char *buf, int cap) /* W353 */
{
    (void)t;
    if (buf && cap > 0) buf[0] = 0;
    return 0;
}
