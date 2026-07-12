/* Copyright (C)
*  2015 - John Melton, G0ORX/N6LYT
*  2025 - Christoph van Wüllen, DL1YCF
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

//
// sdr-for-linux: HPSDR Protocol 1 (METIS) discovery, adapted from piHPSDR
// src/old_discovery.c @ 974acba (dl1ycf) the same way discovery_p2.c adapts
// new_discovery.c: GLib-only (headless engine), results into the shared
// discovered[]/devices globals (engine_state.c). Byte layout verified against
// old_discovery.c by first-hand audit 2026-07-12 (docs/P1-SCOPE.md):
//
//   request : UDP port 1024, 63 bytes, EF FE 02 + zeros (broadcast/directed)
//   reply   : [0..1] EF FE, [2] status (2 idle / 3 in use), [3..8] MAC,
//             [9] gateware version, [10] board id
//   HL split: board id 6 → version = 10*buf[9] + buf[21] (HL2 minor byte);
//             < 400 = Hermes Lite V1, else DEVICE_HERMES_LITE2 (synthetic
//             id 506 — the wire id is always 6)  (old_discovery.c:417-448)
//
// The TCP fallback (1032-byte probe) and the HL2 fixed-IP/altered-MAC reply
// flags (buf[11]) are deliberately not implemented — LAN UDP only for now.
//
#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if_arp.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <string.h>
#include <errno.h>

#include "discovered.h"
#include "discovery.h"
#include "message.h"

static char interface_name[64];
static struct sockaddr_in interface_addr = {0};
static struct sockaddr_in interface_netmask = {0};

#define DISCOVERY_PORT 1024
static int discovery_socket;

static void p1_discover(struct ifaddrs *iface, int discflag);
static gpointer p1_discover_receive_thread(gpointer data);

void p1_discovery(void) {
  struct ifaddrs *addrs, *ifa;
  //
  // Directed probe to the fixed IP first; if it answers, that is the radio
  // the operator wants — skip the broadcast round (mirrors p2_discovery).
  //
  int previous_devices = devices;

  if (ipaddr_radio[0]) { p1_discover(NULL, 2); }

  if (devices <= previous_devices) {
    getifaddrs(&addrs);
    ifa = addrs;

    while (ifa) {
      g_main_context_iteration(NULL, 0);

      if (ifa->ifa_addr) {
        if (
          ifa->ifa_addr->sa_family == AF_INET
          && (ifa->ifa_flags & IFF_UP) == IFF_UP
          && (ifa->ifa_flags & IFF_RUNNING) == IFF_RUNNING
          && (ifa->ifa_flags & IFF_LOOPBACK) != IFF_LOOPBACK
          && strncmp("veth", ifa->ifa_name, 4)
          && strncmp("dock", ifa->ifa_name, 4)
          && strncmp("hass", ifa->ifa_name, 4)
        ) {
          p1_discover(ifa, 1);   // send UDP broadcast packet to interface
        }
      }

      ifa = ifa->ifa_next;
    }

    freeifaddrs(addrs);
  }
}

//
// discflag = 1: send UDP broadcast packet
// discflag = 2: send UDP packet to specified IP address (ipaddr_radio)
//
static void p1_discover(struct ifaddrs *iface, int discflag) {
  int rc;
  unsigned char buffer[63];
  struct sockaddr_in to_addr = {0};

  switch (discflag) {
  case 1:
    snprintf(interface_name, sizeof(interface_name), "%s", iface->ifa_name);
    t_print("%s: looking for HPSDR (P1) devices on %s\n", __func__, interface_name);
    discovery_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (discovery_socket < 0) {
      t_perror("socket() failed for p1 discovery broadcast socket");
      return;
    }

    memcpy(&interface_addr, iface->ifa_addr, sizeof(interface_addr));
    memcpy(&interface_netmask, iface->ifa_netmask, sizeof(interface_netmask));
    interface_addr.sin_family = AF_INET;
    interface_addr.sin_port = htons(0);   // system assigned port

    if (bind(discovery_socket, (struct sockaddr *)&interface_addr, sizeof(interface_addr)) < 0) {
      t_perror("bind() failed for p1 discovery broadcast socket\n");
      close(discovery_socket);
      return;
    }

    {
      int on = 1;
      rc = setsockopt(discovery_socket, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

      if (rc != 0) {
        t_print("%s: cannot set SO_BROADCAST: rc=%d\n", __func__, rc);
        close(discovery_socket);
        return;
      }
    }

    to_addr.sin_family = AF_INET;
    to_addr.sin_port = htons(DISCOVERY_PORT);
    to_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    break;

  case 2:
    interface_addr.sin_family = AF_INET;
    interface_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    memset(&to_addr, 0, sizeof(to_addr));
    to_addr.sin_family = AF_INET;
    to_addr.sin_port = htons(DISCOVERY_PORT);

    if (inet_aton(ipaddr_radio, &to_addr.sin_addr) == 0) {
      struct hostent *entry = gethostbyname(ipaddr_radio);

      if (entry != NULL && entry->h_addr_list[0] != 0) {
        memcpy(&to_addr.sin_addr, entry->h_addr_list[0], sizeof(struct in_addr));
      } else {
        return;
      }
    }

    t_print("%s: trying UDP connection with IP %s\n", __func__, inet_ntoa(to_addr.sin_addr));
    discovery_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (discovery_socket < 0) {
      t_perror("socket() failed for p1 discovery UDP socket");
      return;
    }

    break;

  default:
    return;
  }

  {
    int optval = 1;
    setsockopt(discovery_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(discovery_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
  }
  rc = devices;
  GThread *thr = g_thread_new("p1 discover receive", p1_discover_receive_thread,
                              GINT_TO_POINTER(discflag));
  // METIS discovery packet: EF FE 02 + zero padding to 63 bytes
  memset(buffer, 0, sizeof(buffer));
  buffer[0] = 0xEF;
  buffer[1] = 0xFE;
  buffer[2] = 0x02;

  if (sendto(discovery_socket, buffer, sizeof(buffer), 0,
             (struct sockaddr *)&to_addr, sizeof(to_addr)) < 0) {
    t_perror("sendto() failed for p1 discovery socket");
    // fall through: the receive thread still times out and must be joined
  }

  g_thread_join(thr);
  close(discovery_socket);

  switch (discflag) {
  case 1:
    t_print("%s: exiting for %s\n", __func__, iface->ifa_name);
    break;

  case 2:
    t_print("%s: exiting UDP discover for IP %s\n", __func__, ipaddr_radio);

    if (devices == rc + 1) {
      // directed probe answered: pin the address we actually probed
      memcpy((void *)&discovered[rc].network.address, (void *)&to_addr, sizeof(to_addr));
      discovered[rc].network.address_length = sizeof(to_addr);
      snprintf(discovered[rc].network.interface_name,
               sizeof(discovered[rc].network.interface_name), "UDP");
      discovered[rc].use_routing = 1;
    }

    break;
  }
}

/* Board-id → display name (old_discovery.c naming; HL split by version). */
static const char *p1_board_name(int device) {
  switch (device) {
  case DEVICE_METIS:       return "Metis";
  case DEVICE_HERMES:      return "Hermes";
  case DEVICE_HERMES2:     return "Hermes (Anan-10E/100B)";
  case DEVICE_ANGELIA:     return "Angelia";
  case DEVICE_ORION:       return "Orion";
  case DEVICE_HERMES_LITE: return "Hermes Lite V1";
  case DEVICE_HERMES_LITE2:return "Hermes Lite 2";
  case DEVICE_ORION2:      return "Orion MK2";
  default:                 return "unknown (P1)";
  }
}

static gpointer p1_discover_receive_thread(gpointer data) {
  struct sockaddr_in addr;
  socklen_t len;
  unsigned char buffer[2048];
  struct timeval tv;
  int flag = GPOINTER_TO_INT(data);
  int oldnumdev = devices;
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  setsockopt(discovery_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(struct timeval));
  len = sizeof(addr);

  while (1) {
    if (flag != 1 && devices > oldnumdev) {
      // directed probe: first valid answer wins
      break;
    }

    int bytes_read = recvfrom(discovery_socket, buffer, sizeof(buffer), 0,
                              (struct sockaddr *)&addr, &len);

    if (bytes_read < 0) {
      break;   // timeout
    }

    if (bytes_read < 22) { continue; }   // need bytes up to [21] (HL2 minor)

    if ((buffer[0] & 0xFF) != 0xEF || (buffer[1] & 0xFF) != 0xFE) { continue; }

    int status = buffer[2] & 0xFF;   // 2 = idle, 3 = radio already streaming

    if (status != 2 && status != 3) { continue; }

    // The same radio answers a directed round AND per-interface broadcasts —
    // deduplicate by MAC against everything already discovered (P1 or P2).
    {
      int dup = 0;

      for (int i = 0; i < devices; i++) {
        if (memcmp(discovered[i].network.mac_address, &buffer[3], 6) == 0) { dup = 1; break; }
      }

      if (dup) { continue; }
    }

    if (devices >= MAX_DEVICES) { break; }

    DISCOVERED *d = &discovered[devices];
    memset(d, 0, sizeof(*d));
    d->protocol = ORIGINAL_PROTOCOL;
    d->device = buffer[10] & 0xFF;
    d->software_version = buffer[9] & 0xFF;
    d->status = status;

    for (int i = 0; i < 6; i++) { d->network.mac_address[i] = buffer[i + 3]; }

    if (d->device == DEVICE_HERMES_LITE) {
      // HL2 official version e.g. 73.2 = 10*buf[9] + buf[21]; >= 400 means
      // a Hermes Lite 2 (old_discovery.c:417-448)
      d->software_version = 10 * (buffer[9] & 0xFF) + (buffer[21] & 0xFF);

      if (d->software_version >= 400) { d->device = DEVICE_HERMES_LITE2; }
    }

    d->frequency_min = 0.0;
    d->frequency_max = (d->device == DEVICE_HERMES_LITE ||
                        d->device == DEVICE_HERMES_LITE2) ? 38400000.0 : 61440000.0;
    d->supported_receivers = 2;   // old_discovery.c:504

    if (d->device == DEVICE_HERMES_LITE2 && buffer[19] >= 1 && buffer[19] <= 8) {
      // HL2 discovery byte 0x13 = gateware receiver count (hermeslite.py;
      // the main gateware reports 4). PureSignal needs RX3/RX4 as the
      // feedback pair — the GUI refuses PS below 4 (P1-TX-SCOPE §6).
      // piHPSDR ignores this byte and hard-codes 2.
      d->supported_receivers = buffer[19];
    }
    snprintf(d->name, sizeof(d->name), "%s", p1_board_name(d->device));
    memcpy((void *)&d->network.address, (void *)&addr, sizeof(addr));
    d->network.address.sin_port = htons(DISCOVERY_PORT);
    d->network.address_length = sizeof(addr);
    memcpy((void *)&d->network.interface_address, (void *)&interface_addr, sizeof(interface_addr));
    memcpy((void *)&d->network.interface_netmask, (void *)&interface_netmask, sizeof(interface_netmask));
    d->network.interface_length = sizeof(interface_addr);
    snprintf(d->network.interface_name, sizeof(d->network.interface_name), "%s", interface_name);
    t_print("%s: P1 name=%s device=%d version=%d status=%d address=%s (%02X:%02X:%02X:%02X:%02X:%02X)\n",
            __func__, d->name, d->device, d->software_version, d->status,
            inet_ntoa(d->network.address.sin_addr),
            d->network.mac_address[0], d->network.mac_address[1],
            d->network.mac_address[2], d->network.mac_address[3],
            d->network.mac_address[4], d->network.mac_address[5]);
    devices++;
  }

  return NULL;
}
