/*
 * sdr-for-linux — engine discovery interface.
 *
 * Protocol-2 (HPSDR "new protocol") radio discovery. p2_discovery() first sends
 * a directed UDP probe to ipaddr_radio (if set), and only if that finds nothing
 * falls back to a broadcast on every up/running interface. Results land in the
 * discovered[] table (see discovered.h) with the count in `devices`.
 *
 * The DISCOVERED data contract lives in vendor/pihpsdr/discovered.h (verbatim).
 */
#ifndef SDRFL_ENGINE_DISCOVERY_H
#define SDRFL_ENGINE_DISCOVERY_H

#include "discovered.h" /* DISCOVERED, discovered[], devices, MAX_DEVICES, enums */

/* Fixed radio address tried before broadcasting (empty = broadcast only). */
extern char ipaddr_radio[128];

/* Run Protocol-2 discovery; fills discovered[]/devices. Blocking (~2 s/probe). */
void p2_discovery(void);

#endif /* SDRFL_ENGINE_DISCOVERY_H */
