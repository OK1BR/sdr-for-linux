/*
 * sdr-for-linux — engine global state.
 *
 * Definitions of the discovery globals that piHPSDR keeps in discovered.c /
 * discovery.c. Kept in one small TU so both the discovery code and any front-end
 * link against a single owner. As more engine is imported, radio/receiver state
 * will join it here (or split into focused TUs).
 */
#include "discovered.h"
#include "discovery.h"

int devices = 0;
int selected_device = 0;
DISCOVERED discovered[MAX_DEVICES];
char ipaddr_radio[128] = { 0 };
