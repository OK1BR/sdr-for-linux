/*
 * sdr-for-linux — engine discovery interface.
 *
 * Protocol-2 (HPSDR "new protocol") radio discovery. p2_discovery() first sends
 * a directed UDP probe to ipaddr_radio (if set), and only if that finds nothing
 * falls back to a broadcast on every up/running interface. Results land in the
 * discovered[] table (see discovered.h) with the count in `devices`.
 *
 * The DISCOVERED data contract lives in vendor/pihpsdr/discovered.h (verbatim).
 * Both rounds end with discovery_dedup() — one entry per radio (SDR-15).
 */
#ifndef SDRFL_ENGINE_DISCOVERY_H
#define SDRFL_ENGINE_DISCOVERY_H

#include "discovered.h" /* DISCOVERED, discovered[], devices, MAX_DEVICES, enums */

/* Fixed radio address tried before broadcasting (empty = broadcast only). */
extern char ipaddr_radio[128];

/* Run Protocol-2 discovery; fills discovered[]/devices. Blocking (~2 s/probe). */
void p2_discovery(void);

/* Run Protocol-1 (METIS) discovery; same contract as p2_discovery. Answers
 * are deduplicated by MAC against everything already in discovered[], so
 * calling both back-to-back is safe. Blocking (~2 s/probe). */
void p1_discovery(void);

/* One entry per radio (discovery_dedup.c, BACKLOG SDR-15). Both discovery
 * rounds end with discovery_dedup(): entries of the same radio (protocol +
 * MAC) collapse into the best-ranked one, so every "first entry with this IP"
 * selection binds the data socket to the right interface address.
 * Returns the number of entries dropped. */
enum {
  DISCOVERY_RANK_LINK_LOCAL = 0,  /* 169.254/16, not the radio's subnet    */
  DISCOVERY_RANK_OFF_SUBNET = 1,  /* real address, not the radio's subnet  */
  DISCOVERY_RANK_ROUTED     = 2,  /* directed probe / INADDR_ANY interface */
  DISCOVERY_RANK_IN_SUBNET  = 3,  /* interface in the radio's subnet       */
};
int         discovery_rank(const DISCOVERED *d);
const char *discovery_rank_name(int rank);
int         discovery_dedup(void);

#endif /* SDRFL_ENGINE_DISCOVERY_H */
