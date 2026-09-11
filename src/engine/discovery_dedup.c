/*
 * sdr-for-linux — one discovered[] entry per radio (BACKLOG SDR-15).
 *
 * Discovery (discovery_p2.c / discovery_p1.c, adapted from piHPSDR) records one
 * entry PER INTERFACE ADDRESS the radio answered on, and a picker round followed
 * by the app's own directed probe adds one more. A host carrying a link-local
 * 169.254.x.x address next to its LAN address therefore sees the same radio two
 * or three times, and every "first entry with this IP" selection (gui, picker,
 * probes) inherited whichever address getifaddrs() listed first — on Richard's
 * desk that was the link-local one, so p2_rx_start() bound the data socket to a
 * transient address (log: `p2: socket N bound to 169.254.198.250:0`) and the
 * stream survived only because the G2E replies to whatever source it saw.
 *
 * discovery_dedup() is the post-pass both discovery rounds end with: entries of
 * the same radio (same protocol + MAC) collapse into the best-ranked one, in
 * this order (piHPSDR's discovery.c:799-810 `can_connect` classes, ranked):
 *
 *   3  in-subnet   interface address and radio address agree under the
 *                  interface netmask (mask != 0) — bind here, deterministic
 *   2  routed      directed probe (use_routing) or an INADDR_ANY interface —
 *                  the kernel routes; always works, less explicit
 *   1  off-subnet  a real address not in the radio's subnet — works only
 *                  because the radio answers the source it saw
 *   0  link-local  169.254/16 and not in the radio's subnet — transient
 *
 * The routed class is tested FIRST: the static interface_netmask in the
 * discovery files is never reset by a directed probe, so a stale or zero mask
 * must not make an INADDR_ANY entry look "in subnet". Ties keep the earlier
 * entry (stable). The pass is order-independent and pinned by
 * sdrfl-discovery-test.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "discovered.h"
#include "discovery.h"
#include "message.h"

static int is_link_local(in_addr_t s_addr_net) {
  return (ntohl(s_addr_net) & 0xFFFF0000u) == 0xA9FE0000u;   /* 169.254.0.0/16 */
}

int discovery_rank(const DISCOVERED *d) {
  const in_addr_t iface = d->network.interface_address.sin_addr.s_addr;
  const in_addr_t radio = d->network.address.sin_addr.s_addr;
  const in_addr_t mask  = d->network.interface_netmask.sin_addr.s_addr;

  if (d->use_routing || iface == htonl(INADDR_ANY)) { return DISCOVERY_RANK_ROUTED; }
  if (mask != 0 && (iface & mask) == (radio & mask)) { return DISCOVERY_RANK_IN_SUBNET; }
  if (!is_link_local(iface)) { return DISCOVERY_RANK_OFF_SUBNET; }
  return DISCOVERY_RANK_LINK_LOCAL;
}

const char *discovery_rank_name(int rank) {
  switch (rank) {
  case DISCOVERY_RANK_IN_SUBNET:  return "in-subnet";
  case DISCOVERY_RANK_ROUTED:     return "routed";
  case DISCOVERY_RANK_OFF_SUBNET: return "off-subnet";
  case DISCOVERY_RANK_LINK_LOCAL: return "link-local";
  default:                        return "?";
  }
}

static int mac_is_zero(const unsigned char *m) {
  for (int i = 0; i < 6; i++) { if (m[i]) { return 0; } }
  return 1;
}

/* Same radio = same protocol and same MAC; entries without a MAC (never seen
 * from a real radio — both protocols carry it in the reply) fall back to the
 * radio's IP so a synthetic entry cannot swallow a real one by accident. */
static int same_radio(const DISCOVERED *a, const DISCOVERED *b) {
  if (a->protocol != b->protocol) { return 0; }
  const unsigned char *ma = a->network.mac_address, *mb = b->network.mac_address;
  if (!mac_is_zero(ma) || !mac_is_zero(mb)) { return memcmp(ma, mb, 6) == 0; }
  return a->network.address.sin_addr.s_addr == b->network.address.sin_addr.s_addr;
}

int discovery_dedup(void) {
  int dropped = 0;

  for (int i = 0; i < devices; i++) {
    for (int j = i + 1; j < devices; ) {
      if (!same_radio(&discovered[i], &discovered[j])) { j++; continue; }

      const int ri = discovery_rank(&discovered[i]);
      const int rj = discovery_rank(&discovered[j]);
      const int keep = (rj > ri) ? j : i;
      const int drop = (keep == j) ? i : j;
      char radio_ip[INET_ADDRSTRLEN], keep_ip[INET_ADDRSTRLEN], drop_ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &discovered[i].network.address.sin_addr, radio_ip, sizeof radio_ip);
      inet_ntop(AF_INET, &discovered[keep].network.interface_address.sin_addr, keep_ip, sizeof keep_ip);
      inet_ntop(AF_INET, &discovered[drop].network.interface_address.sin_addr, drop_ip, sizeof drop_ip);
      t_print("discovery: %s at %s also answered via %s (%s) — keeping %s (%s)\n",
              discovered[i].name, radio_ip,
              drop_ip, discovery_rank_name(keep == j ? ri : rj),
              keep_ip, discovery_rank_name(keep == j ? rj : ri));

      if (keep == j) { discovered[i] = discovered[j]; }
      memmove(&discovered[j], &discovered[j + 1], (size_t)(devices - j - 1) * sizeof(DISCOVERED));
      devices--;
      dropped++;
      /* j now holds the next entry — re-examine it. */
    }
  }

  return dropped;
}
