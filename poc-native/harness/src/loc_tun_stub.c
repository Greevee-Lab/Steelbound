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
int loctun_dgram(loctun *t, uint32_t dst_vip, uint16_t src_gp, uint16_t dst_gp, const char *buf, int len)
{
    (void)t; (void)dst_vip; (void)src_gp; (void)dst_gp; (void)buf; (void)len;
    return -1;
}
int loctun_is_peer(loctun *t, uint32_t vip) { (void)t; (void)vip; return 0; }
void loctun_set_app(loctun *t, uint16_t gp, loctun_app_fn fn) { (void)t; (void)gp; (void)fn; } /* W338 */
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
int loctun_auth_state(loctun *t) { (void)t; return 0; }
int loctun_lobby_code(loctun *t, char *buf, int cap)
{
    (void)t;
    if (buf && cap > 0) buf[0] = 0;
    return 0;
}
int loctun_sec_state(loctun *t) { (void)t; return 0; }
int loctun_diag(loctun *t, char *buf, int cap) /* W353 */
{
    (void)t;
    if (buf && cap > 0) buf[0] = 0;
    return 0;
}
