/*
 * sdr-for-linux — persistent run log for desktop launches.
 *
 * A session started from the app grid has stdout/stderr on /dev/null, so every
 * engine line (tx: KEY/UNKEY, p2: link lost, …) is gone the moment it is
 * needed — the footswitch stuck-TX of 2026-10-24 left no trace at all. When
 * fd 2 IS /dev/null, logfile_init() routes both streams into
 *   $XDG_STATE_HOME/sdr-for-linux/sdr-for-linux.log   (~/.local/state/…)
 * keeps the previous four runs as .1 … .4 (the fifth-oldest is overwritten),
 * and stamps every line with the wall-clock time so the log lines up with the
 * system journal (that is how the 13:01 NIC link drop was found). A terminal
 * or a shell redirect (`2> file`) is left alone — the operator already owns
 * that output. SDRFL_LOG=0 switches the redirect off.
 */
#ifndef SDRFL_LOGFILE_H
#define SDRFL_LOGFILE_H

/* Call first thing in main(), before anything prints. Returns the log path
 * when stdout/stderr were redirected, NULL when they were left alone. */
const char *logfile_init(const char *appname, const char *version);

/* The active log path (NULL = not redirected) — for About → Debug Info. */
const char *logfile_path(void);

#endif /* SDRFL_LOGFILE_H */
