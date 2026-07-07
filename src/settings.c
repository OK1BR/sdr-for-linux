/*
 * settings.c — persistent app state via GKeyFile. See settings.h.
 *
 * A plain, human-editable INI at ~/.config/sdr-for-linux/config.ini. No GSettings
 * schema to compile/install; GLib is already linked. Keys are grouped [radio]/[rx].
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>

#include "settings.h"

#define GROUP_RADIO   "radio"
#define GROUP_RX      "rx"
#define GROUP_DISPLAY "display"

static char cfg_path[1024];

const char *settings_path(void) {
  if (!cfg_path[0]) {
    char *p = g_build_filename(g_get_user_config_dir(), "sdr-for-linux", "config.ini", NULL);
    g_strlcpy(cfg_path, p, sizeof(cfg_path));
    g_free(p);
  }
  return cfg_path;
}

int settings_load(Settings *s) {
  GKeyFile *kf = g_key_file_new();
  int ok = 0;
  if (g_key_file_load_from_file(kf, settings_path(), G_KEY_FILE_NONE, NULL)) {
    ok = 1;
    if (g_key_file_has_key(kf, GROUP_RADIO, "ip", NULL)) {
      char *ip = g_key_file_get_string(kf, GROUP_RADIO, "ip", NULL);
      if (ip) { g_strlcpy(s->ip, ip, sizeof(s->ip)); g_free(ip); }
    }
    if (g_key_file_has_key(kf, GROUP_RADIO, "freq", NULL))
      s->freq = g_key_file_get_int64(kf, GROUP_RADIO, "freq", NULL);
    if (g_key_file_has_key(kf, GROUP_RADIO, "rate", NULL))
      s->rate = g_key_file_get_integer(kf, GROUP_RADIO, "rate", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "mode", NULL))
      s->mode = g_key_file_get_integer(kf, GROUP_RX, "mode", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "volume", NULL))
      s->volume = g_key_file_get_double(kf, GROUP_RX, "volume", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "gain", NULL))
      s->gain = g_key_file_get_double(kf, GROUP_RX, "gain", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "latency", NULL))
      s->latency = g_key_file_get_integer(kf, GROUP_RX, "latency", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "fps", NULL))
      s->fps = g_key_file_get_integer(kf, GROUP_DISPLAY, "fps", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "step", NULL))
      s->step = g_key_file_get_integer(kf, GROUP_RX, "step", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "zoom", NULL))
      s->zoom = g_key_file_get_double(kf, GROUP_DISPLAY, "zoom", NULL);
  }
  g_key_file_free(kf);
  return ok;
}

int settings_save(const Settings *s) {
  char *dir = g_path_get_dirname(settings_path());
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);

  GKeyFile *kf = g_key_file_new();
  g_key_file_set_string (kf, GROUP_RADIO, "ip",     s->ip);
  g_key_file_set_int64  (kf, GROUP_RADIO, "freq",   s->freq);
  g_key_file_set_integer(kf, GROUP_RADIO, "rate",   s->rate);
  g_key_file_set_integer(kf, GROUP_RX,    "mode",   s->mode);
  g_key_file_set_double (kf, GROUP_RX,    "volume",  s->volume);
  g_key_file_set_double (kf, GROUP_RX,    "gain",    s->gain);
  g_key_file_set_integer(kf, GROUP_RX,    "latency", s->latency);
  g_key_file_set_integer(kf, GROUP_RX,    "step",    s->step);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "fps",   s->fps);
  g_key_file_set_double (kf, GROUP_DISPLAY, "zoom",  s->zoom);

  GError *e = NULL;
  int rc = g_key_file_save_to_file(kf, settings_path(), &e) ? 0 : -1;
  if (e) { g_warning("settings_save: %s", e->message); g_error_free(e); }
  g_key_file_free(kf);
  return rc;
}
