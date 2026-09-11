/*
 * sdrfl-discovery-test — offline gate for discovery_dedup() (BACKLOG SDR-15).
 *
 * The discovery rounds record one discovered[] entry per interface address a
 * radio answered on (plus the app's directed probe); every selection site takes
 * the first entry with the radio's IP. On a host with a link-local 169.254.x.x
 * address next to its LAN address that bound the P2 data socket to the
 * link-local one. discovery_dedup() must collapse the table to one entry per
 * radio, keeping the best-ranked interface — in-subnet > routed > off-subnet >
 * link-local — regardless of input order, and must never merge two different
 * radios or the P1 and P2 answers of one radio. Synthetic entries, no socket.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "discovered.h"
#include "discovery.h"

static int g_checks = 0, g_fail = 0;

static void check(int ok, const char *what) {
  g_checks++;
  if (!ok) { g_fail++; }
  printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

static in_addr_t ip(const char *dotted) {
  struct in_addr a;
  if (inet_aton(dotted, &a) == 0) { fprintf(stderr, "bad ip %s\n", dotted); return 0; }
  return a.s_addr;
}

/* protocol + last MAC byte identify the radio; iface/mask describe the
 * interface the answer came in on; routing = a directed-probe entry. */
static DISCOVERED mk(int protocol, unsigned char mac5, const char *radio,
                     const char *iface, const char *mask, int routing) {
  DISCOVERED d;
  memset(&d, 0, sizeof d);
  d.protocol = protocol;
  d.device   = protocol == NEW_PROTOCOL ? NEW_DEVICE_G1 : DEVICE_HERMES_LITE2;
  snprintf(d.name, sizeof d.name, "%s", protocol == NEW_PROTOCOL ? "ANAN G2E" : "HermesLite V2");
  const unsigned char m[6] = { 0x40, 0x84, 0x32, 0xB0, 0x7C, mac5 };
  memcpy(d.network.mac_address, m, 6);
  d.network.address.sin_family = AF_INET;
  d.network.address.sin_addr.s_addr = ip(radio);
  d.network.address_length = sizeof(struct sockaddr_in);
  d.network.interface_address.sin_family = AF_INET;
  d.network.interface_address.sin_addr.s_addr = ip(iface);
  d.network.interface_netmask.sin_addr.s_addr = ip(mask);
  d.network.interface_length = sizeof(struct sockaddr_in);
  snprintf(d.network.interface_name, sizeof d.network.interface_name, "%s", routing ? "UDP" : "enp0");
  d.use_routing = routing;
  return d;
}

static void load(const DISCOVERED *e, int n) {
  devices = 0;
  for (int i = 0; i < n; i++) { discovered[devices++] = e[i]; }
}

static int iface_is(int i, const char *dotted) {
  return i < devices && discovered[i].network.interface_address.sin_addr.s_addr == ip(dotted);
}

/* The four interface classes for one G2E at 192.168.1.247 (Richard's LAN:
 * enp134s0f1 = 192.168.1.18/24 + 169.254.198.250/16, SDR-15). */
#define RADIO "192.168.1.247"
static DISCOVERED ll, sub, off, routed;

int main(void) {
  printf("sdrfl-discovery-test: discovery_dedup() ranking + merging\n");
  ll     = mk(NEW_PROTOCOL, 0x60, RADIO, "169.254.198.250", "255.255.0.0",   0);
  sub    = mk(NEW_PROTOCOL, 0x60, RADIO, "192.168.1.18",    "255.255.255.0", 0);
  off    = mk(NEW_PROTOCOL, 0x60, RADIO, "10.8.0.5",        "255.255.255.0", 0);
  routed = mk(NEW_PROTOCOL, 0x60, RADIO, "0.0.0.0",         "0.0.0.0",       1);

  printf("-- rank\n");
  check(discovery_rank(&ll)     == DISCOVERY_RANK_LINK_LOCAL, "169.254.198.250/16 → link-local");
  check(discovery_rank(&sub)    == DISCOVERY_RANK_IN_SUBNET,  "192.168.1.18/24 → in-subnet");
  check(discovery_rank(&off)    == DISCOVERY_RANK_OFF_SUBNET, "10.8.0.5/24 → off-subnet");
  check(discovery_rank(&routed) == DISCOVERY_RANK_ROUTED,     "directed probe (INADDR_ANY, mask 0) → routed");
  {
    /* The static interface_netmask is not reset by a directed probe: a stale
     * mask must not turn INADDR_ANY into "in-subnet" (0 & m == radio & m is
     * false for any real radio, but a zero mask would match) — routed FIRST. */
    DISCOVERED stale = mk(NEW_PROTOCOL, 0x60, RADIO, "0.0.0.0", "255.255.0.0", 0);
    check(discovery_rank(&stale) == DISCOVERY_RANK_ROUTED, "INADDR_ANY + stale mask → still routed");
    DISCOVERED both_ll = mk(NEW_PROTOCOL, 0x60, "169.254.7.7", "169.254.198.250", "255.255.0.0", 0);
    check(discovery_rank(&both_ll) == DISCOVERY_RANK_IN_SUBNET, "radio AND host link-local /16 → in-subnet (piHPSDR case a)");
  }

  printf("-- merge: link-local first (the SDR-15 desk case)\n");
  { DISCOVERED e[] = { ll, sub }; load(e, 2);
    int dropped = discovery_dedup();
    check(dropped == 1 && devices == 1, "two entries → one");
    check(iface_is(0, "192.168.1.18"), "in-subnet interface kept"); }

  printf("-- merge: order-independent\n");
  { DISCOVERED e[] = { sub, ll }; load(e, 2); discovery_dedup();
    check(devices == 1 && iface_is(0, "192.168.1.18"), "in-subnet first → same winner"); }

  printf("-- merge: picker broadcast + app directed probe\n");
  { DISCOVERED e[] = { ll, sub, routed }; load(e, 3); discovery_dedup();
    check(devices == 1 && iface_is(0, "192.168.1.18"), "in-subnet beats routed and link-local"); }
  { DISCOVERED e[] = { routed, sub }; load(e, 2); discovery_dedup();
    check(devices == 1 && iface_is(0, "192.168.1.18"), "routed first → in-subnet still wins"); }

  printf("-- merge: no in-subnet interface\n");
  { DISCOVERED e[] = { ll, routed }; load(e, 2); discovery_dedup();
    check(devices == 1 && iface_is(0, "0.0.0.0"), "routed beats link-local"); }
  { DISCOVERED e[] = { ll, off }; load(e, 2); discovery_dedup();
    check(devices == 1 && iface_is(0, "10.8.0.5"), "off-subnet beats link-local"); }
  { DISCOVERED e[] = { off, routed }; load(e, 2); discovery_dedup();
    check(devices == 1 && iface_is(0, "0.0.0.0"), "routed beats off-subnet"); }

  printf("-- merge: ties keep the earlier entry\n");
  { DISCOVERED a = mk(NEW_PROTOCOL, 0x60, RADIO, "192.168.1.18", "255.255.255.0", 0);
    DISCOVERED b = mk(NEW_PROTOCOL, 0x60, RADIO, "192.168.1.19", "255.255.255.0", 0);
    DISCOVERED e[] = { a, b }; load(e, 2); discovery_dedup();
    check(devices == 1 && iface_is(0, "192.168.1.18"), "equal rank → first kept (stable)"); }

  printf("-- never merge different radios / protocols\n");
  { DISCOVERED other = mk(NEW_PROTOCOL, 0x61, "192.168.1.248", "169.254.198.250", "255.255.0.0", 0);
    DISCOVERED e[] = { ll, other, sub }; load(e, 3); discovery_dedup();
    check(devices == 2, "G2E ×2 + second radio → two entries");
    check(iface_is(0, "192.168.1.18") && discovered[0].network.mac_address[5] == 0x60,
          "G2E collapsed in place (slot 0, in-subnet)");
    check(discovered[1].network.mac_address[5] == 0x61 && iface_is(1, "169.254.198.250"),
          "other radio untouched, order preserved"); }
  { DISCOVERED same_ip = mk(NEW_PROTOCOL, 0x62, RADIO, "192.168.1.18", "255.255.255.0", 0);
    DISCOVERED e[] = { sub, same_ip }; load(e, 2); discovery_dedup();
    check(devices == 2, "same IP, different MAC → not merged (MAC is the identity)"); }
  { DISCOVERED p1 = mk(ORIGINAL_PROTOCOL, 0x60, RADIO, "192.168.1.18", "255.255.255.0", 0);
    DISCOVERED e[] = { sub, p1 }; load(e, 2); discovery_dedup();
    check(devices == 2, "P2 and P1 answers of one MAC → both kept"); }

  printf("-- idempotent / empty\n");
  { DISCOVERED e[] = { sub }; load(e, 1);
    check(discovery_dedup() == 0 && devices == 1, "single entry → unchanged");
    check(discovery_dedup() == 0 && devices == 1, "second pass → still unchanged"); }
  { devices = 0; check(discovery_dedup() == 0 && devices == 0, "empty table → no-op"); }

  printf("\n%d checks, %d failed → %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
