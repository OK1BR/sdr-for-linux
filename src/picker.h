/*
 * Startup radio picker (Zeus/piHPSDR-style): broadcast-discover the HPSDR
 * radios on the LAN, list them, and return the chosen radio's IP address.
 * Runs BEFORE the main window exists — own toplevel + nested main loop,
 * same pattern as wisdom_gate. Skipped entirely when SDRFL_RADIO_IP pins
 * the radio (scripts/CI) or in --server mode.
 */
#ifndef SDRFL_PICKER_H
#define SDRFL_PICKER_H

/* Show the picker. last_ip ("" = none) preselects the previously used radio.
 * Returns 1 with *ip filled on selection, 0 if the user closed the window. */
int picker_run(const char *last_ip, char *ip, int ip_len);

#endif
