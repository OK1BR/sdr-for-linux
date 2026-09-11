/*
 * sdrfl-discover — headless Protocol-2 discovery probe.
 *
 * Sends HPSDR P2 discovery (directed to $SDRFL_RADIO_IP, default 192.168.1.247,
 * then broadcast only if that finds nothing) and prints the radios that answer.
 * Each radio appears ONCE with the interface address the data socket would
 * bind to and its class (in-subnet/routed/off-subnet/link-local, SDR-15).
 * Read-only: it never opens a data connection, so it is safe to run while
 * another client (piHPSDR) is using the radio — the radio answers discovery
 * even while streaming (status SENDING).
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discovered.h"
#include "discovery.h"

int main(void) {
  const char *ip = getenv("SDRFL_RADIO_IP");
  snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s",
           (ip && *ip) ? ip : "192.168.1.247");

  printf("sdrfl-discover: probing %s (then broadcast) for HPSDR radios (P2 + P1)...\n\n",
         ipaddr_radio);

  p2_discovery();
  p1_discovery();   /* METIS round; dedups by MAC against the P2 results */

  printf("\nfound %d device(s):\n", devices);
  for (int i = 0; i < devices; i++) {
    const DISCOVERED *d = &discovered[i];
    const unsigned char *m = d->network.mac_address;
    char radio_ip[INET_ADDRSTRLEN], iface_ip[INET_ADDRSTRLEN];   /* two addresses
                                          in one printf: inet_ntop, not inet_ntoa's
                                          single static buffer */
    inet_ntop(AF_INET, &d->network.address.sin_addr, radio_ip, sizeof radio_ip);
    inet_ntop(AF_INET, &d->network.interface_address.sin_addr, iface_ip, sizeof iface_ip);
    printf("  [%d] %-22s  P%d  dev=%d  ip=%-15s  mac=%02X:%02X:%02X:%02X:%02X:%02X"
           "  fw=%d  status=%d  %.3f-%.3f MHz  via %s %s (%s)\n",
           i, d->name, (d->protocol == NEW_PROTOCOL) ? 2 : 1,
           (d->protocol == NEW_PROTOCOL) ? d->device - 1000 : d->device,
           radio_ip,
           m[0], m[1], m[2], m[3], m[4], m[5],
           d->software_version, d->status,
           d->frequency_min * 1e-6, d->frequency_max * 1e-6,
           d->network.interface_name, iface_ip,
           discovery_rank_name(discovery_rank(d)));
  }

  return devices > 0 ? 0 : 1;
}
