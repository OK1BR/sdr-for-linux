/*
 * sdr-for-linux — persistent run log for desktop launches (see logfile.h).
 *
 * Mechanics: the log file is dup2()'d over fds 1 and 2, so even a library
 * that writes to the descriptors directly lands in the file; on glibc the
 * stdout/stderr FILE objects are then replaced by fopencookie() streams that
 * prefix every line with HH:MM:SS.mmm before writing to the same fd —
 * synchronously, no helper thread, so a crash loses nothing that was
 * printed. stderr stays unbuffered, stdout becomes line-buffered (a plain
 * file would make it fully buffered and the renderer/startup lines would sit
 * in the buffer until exit).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE          /* fopencookie */
#endif
#include "logfile.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define LOG_KEEP 5          /* current + .1 … .4 */

static char *s_path;        /* NULL = not redirected */

static void write_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0) { return; }          /* nowhere to report an error to */
    p += (size_t)w; n -= (size_t)w;
  }
}

#ifdef __GLIBC__
typedef struct { int fd; int bol; } ts_stream;

/* Line-start timestamps. The FILE lock serialises calls per stream; a
 * stdout/stderr interleave mid-line can only mis-stamp one line. */
static ssize_t ts_write(void *cookie, const char *buf, size_t n) {
  ts_stream *s = (ts_stream *)cookie;
  size_t i = 0;
  while (i < n) {
    if (s->bol) {
      gint64 us = g_get_real_time();
      time_t sec = (time_t)(us / G_USEC_PER_SEC);
      struct tm tm; localtime_r(&sec, &tm);
      char ts[24];
      int len = snprintf(ts, sizeof ts, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min,
                         tm.tm_sec, (int)((us % G_USEC_PER_SEC) / 1000));
      write_all(s->fd, ts, (size_t)len);
      s->bol = 0;
    }
    const char *nl = memchr(buf + i, '\n', n - i);
    size_t len = nl ? (size_t)(nl - (buf + i)) + 1 : n - i;
    write_all(s->fd, buf + i, len);
    i += len;
    if (nl) { s->bol = 1; }
  }
  return (ssize_t)n;
}

static FILE *ts_open(int fd, int mode) {
  ts_stream *s = g_new0(ts_stream, 1);
  s->fd = fd; s->bol = 1;
  cookie_io_functions_t io = { .write = ts_write };
  FILE *f = fopencookie(s, "w", io);
  if (f) { setvbuf(f, NULL, mode, 0); }
  return f;
}
#endif

const char *logfile_init(const char *appname, const char *version) {
  /* OPT-IN (Richard, 2026-10-24): nobody who just installs the app gets a log
   * file. On = SDRFL_LOG=1 in the environment, or the Preferences switch
   * (config.ini [debug] log=1, read here directly — settings are loaded
   * later). SDRFL_LOG=0 wins over the switch. */
  const char *env = g_getenv("SDRFL_LOG");
  int on = 0;
  if (env && *env) {
    on = strcmp(env, "0") != 0;
  } else {
    char *cfg = g_build_filename(g_get_user_config_dir(), appname, "config.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, cfg, G_KEY_FILE_NONE, NULL)) {
      on = g_key_file_get_integer(kf, "debug", "log", NULL) != 0;
    }
    g_key_file_unref(kf); g_free(cfg);
  }
  if (!on) { return NULL; }

  char *dir = g_build_filename(g_get_user_state_dir(), appname, NULL);
  if (g_mkdir_with_parents(dir, 0755) != 0) { g_free(dir); return NULL; }
  char *base = g_strdup_printf("%s.log", appname);
  char *path = g_build_filename(dir, base, NULL);

  /* Rotate: .3 → .4, …, log → .1 (rename over the oldest). */
  for (int i = LOG_KEEP - 1; i >= 1; i--) {
    char *from = i == 1 ? g_strdup(path) : g_strdup_printf("%s.%d", path, i - 1);
    char *to   = g_strdup_printf("%s.%d", path, i);
    if (g_file_test(from, G_FILE_TEST_EXISTS)) { g_rename(from, to); }
    g_free(from); g_free(to);
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) { g_free(path); g_free(base); g_free(dir); return NULL; }
  dup2(fd, 1);
  dup2(fd, 2);
  close(fd);
#ifdef __GLIBC__
  FILE *out = ts_open(1, _IOLBF);
  FILE *err = ts_open(2, _IONBF);
  if (out) { stdout = out; }
  if (err) { stderr = err; }
#else
  setvbuf(stdout, NULL, _IOLBF, 0);
#endif
  s_path = path;
  g_free(base); g_free(dir);

  GDateTime *now = g_date_time_new_now_local();
  char *stamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S %Z");
  fprintf(stderr, "%s %s — log started %s, PID %d, %s\n",
          appname, version ? version : "", stamp, (int)getpid(), s_path);
  g_free(stamp); g_date_time_unref(now);
  return s_path;
}

const char *logfile_path(void) { return s_path; }
