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
#define GROUP_WINDOW  "window"
#define GROUP_TX      "tx"

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
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "pan_high", NULL))
      s->pan_high = g_key_file_get_double(kf, GROUP_DISPLAY, "pan_high", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "pan_low", NULL))
      s->pan_low = g_key_file_get_double(kf, GROUP_DISPLAY, "pan_low", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "db_grid", NULL))
      s->db_grid = g_key_file_get_integer(kf, GROUP_DISPLAY, "db_grid", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "db_scale", NULL))
      s->db_scale = g_key_file_get_integer(kf, GROUP_DISPLAY, "db_scale", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "freq_grid", NULL))
      s->freq_grid = g_key_file_get_integer(kf, GROUP_DISPLAY, "freq_grid", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "freq_scale", NULL))
      s->freq_scale = g_key_file_get_integer(kf, GROUP_DISPLAY, "freq_scale", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "filter_wf", NULL))
      s->filter_wf = g_key_file_get_integer(kf, GROUP_DISPLAY, "filter_wf", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "filter_op", NULL))
      s->filter_op = g_key_file_get_integer(kf, GROUP_DISPLAY, "filter_op", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "auto_level", NULL))
      s->auto_level = g_key_file_get_integer(kf, GROUP_DISPLAY, "auto_level", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "avg_spec", NULL))
      s->avg_spec = g_key_file_get_integer(kf, GROUP_DISPLAY, "avg_spec", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "avg_wf", NULL))
      s->avg_wf = g_key_file_get_integer(kf, GROUP_DISPLAY, "avg_wf", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "palette", NULL))
      s->palette = g_key_file_get_integer(kf, GROUP_DISPLAY, "palette", NULL);
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "band_edges", NULL))
      s->band_edges = g_key_file_get_integer(kf, GROUP_DISPLAY, "band_edges", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "region", NULL)) {
      char *r = g_key_file_get_string(kf, GROUP_RX, "region", NULL);
      if (r) { g_strlcpy(s->region, r, sizeof(s->region)); g_free(r); }
    }
    if (g_key_file_has_key(kf, GROUP_RX, "country", NULL)) {
      char *c = g_key_file_get_string(kf, GROUP_RX, "country", NULL);
      if (c) { g_strlcpy(s->country, c, sizeof(s->country)); g_free(c); }
    }
    if (g_key_file_has_key(kf, GROUP_DISPLAY, "band_levels", NULL)) {
      char *bl = g_key_file_get_string(kf, GROUP_DISPLAY, "band_levels", NULL);
      if (bl) { g_strlcpy(s->band_levels, bl, sizeof(s->band_levels)); g_free(bl); }
    }
    if (g_key_file_has_key(kf, GROUP_RX, "atten", NULL))
      s->atten = g_key_file_get_integer(kf, GROUP_RX, "atten", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "agc", NULL))
      s->agc = g_key_file_get_integer(kf, GROUP_RX, "agc", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "agc_gain", NULL))
      s->agc_gain = g_key_file_get_double(kf, GROUP_RX, "agc_gain", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "filter", NULL))
      s->filter = g_key_file_get_integer(kf, GROUP_RX, "filter", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "nr", NULL))
      s->nr = g_key_file_get_integer(kf, GROUP_RX, "nr", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "nb", NULL))
      s->nb = g_key_file_get_integer(kf, GROUP_RX, "nb", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "anf", NULL))
      s->anf = g_key_file_get_integer(kf, GROUP_RX, "anf", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "binaural", NULL))
      s->binaural = g_key_file_get_integer(kf, GROUP_RX, "binaural", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "mode_filters", NULL)) {
      char *mf = g_key_file_get_string(kf, GROUP_RX, "mode_filters", NULL);
      if (mf) { g_strlcpy(s->mode_filt, mf, sizeof(s->mode_filt)); g_free(mf); }
    }
    if (g_key_file_has_key(kf, GROUP_RX, "var_filters", NULL)) {
      char *vf = g_key_file_get_string(kf, GROUP_RX, "var_filters", NULL);
      if (vf) { g_strlcpy(s->var_filt, vf, sizeof(s->var_filt)); g_free(vf); }
    }
    if (g_key_file_has_key(kf, GROUP_TX, "pa_enable", NULL))
      s->tx_pa = g_key_file_get_integer(kf, GROUP_TX, "pa_enable", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "antenna", NULL))
      s->tx_ant = g_key_file_get_integer(kf, GROUP_TX, "antenna", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "drive_w", NULL))
      s->tx_drive = g_key_file_get_double(kf, GROUP_TX, "drive_w", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "tune_w", NULL))
      s->tx_tune = g_key_file_get_double(kf, GROUP_TX, "tune_w", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "swr_alarm", NULL))
      s->tx_swr = g_key_file_get_double(kf, GROUP_TX, "swr_alarm", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "mic_gain", NULL))
      s->mic_gain = g_key_file_get_double(kf, GROUP_TX, "mic_gain", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "comp", NULL))
      s->tx_comp = g_key_file_get_integer(kf, GROUP_TX, "comp", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "comp_db", NULL))
      s->tx_comp_db = g_key_file_get_double(kf, GROUP_TX, "comp_db", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "gate", NULL))
      s->tx_gate = g_key_file_get_integer(kf, GROUP_TX, "gate", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "gate_db", NULL))
      s->tx_gate_db = g_key_file_get_double(kf, GROUP_TX, "gate_db", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "monitor", NULL))
      s->tx_mon = g_key_file_get_integer(kf, GROUP_TX, "monitor", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "monitor_db", NULL))
      s->tx_mon_db = g_key_file_get_double(kf, GROUP_TX, "monitor_db", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "filt_lo", NULL))
      s->tx_flo = g_key_file_get_double(kf, GROUP_TX, "filt_lo", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "filt_hi", NULL))
      s->tx_fhi = g_key_file_get_double(kf, GROUP_TX, "filt_hi", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "pan_high", NULL))
      s->tx_pan_high = g_key_file_get_double(kf, GROUP_TX, "pan_high", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "pan_low", NULL))
      s->tx_pan_low = g_key_file_get_double(kf, GROUP_TX, "pan_low", NULL);
    if (g_key_file_has_key(kf, GROUP_TX, "pa_cal", NULL)) {
      char *pc = g_key_file_get_string(kf, GROUP_TX, "pa_cal", NULL);
      if (pc) { g_strlcpy(s->pa_cal, pc, sizeof(s->pa_cal)); g_free(pc); }
    }
    if (g_key_file_has_key(kf, GROUP_TX, "pa_trim", NULL)) {
      char *pt = g_key_file_get_string(kf, GROUP_TX, "pa_trim", NULL);
      if (pt) { g_strlcpy(s->pa_trim, pt, sizeof(s->pa_trim)); g_free(pt); }
    }
    if (g_key_file_has_key(kf, GROUP_RX, "audio_rate", NULL))
      s->audio_rate = g_key_file_get_integer(kf, GROUP_RX, "audio_rate", NULL);
    if (g_key_file_has_key(kf, GROUP_RX, "audio_device", NULL)) {
      char *ad = g_key_file_get_string(kf, GROUP_RX, "audio_device", NULL);
      if (ad) { g_strlcpy(s->audio_device, ad, sizeof(s->audio_device)); g_free(ad); }
    }
    if (g_key_file_has_key(kf, GROUP_TX, "mic_device", NULL)) {
      char *md = g_key_file_get_string(kf, GROUP_TX, "mic_device", NULL);
      if (md) { g_strlcpy(s->mic_device, md, sizeof(s->mic_device)); g_free(md); }
    }
    if (g_key_file_has_key(kf, GROUP_WINDOW, "width", NULL))
      s->win_w = g_key_file_get_integer(kf, GROUP_WINDOW, "width", NULL);
    if (g_key_file_has_key(kf, GROUP_WINDOW, "height", NULL))
      s->win_h = g_key_file_get_integer(kf, GROUP_WINDOW, "height", NULL);
    if (g_key_file_has_key(kf, GROUP_WINDOW, "maximized", NULL))
      s->win_max = g_key_file_get_integer(kf, GROUP_WINDOW, "maximized", NULL);
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
  g_key_file_set_integer(kf, GROUP_RX,    "atten",   s->atten);
  g_key_file_set_integer(kf, GROUP_RX,    "agc",     s->agc);
  g_key_file_set_double (kf, GROUP_RX,    "agc_gain", s->agc_gain);
  g_key_file_set_integer(kf, GROUP_RX,    "filter",  s->filter);
  g_key_file_set_integer(kf, GROUP_RX,    "nr",      s->nr);
  g_key_file_set_integer(kf, GROUP_RX,    "nb",      s->nb);
  g_key_file_set_integer(kf, GROUP_RX,    "anf",     s->anf);
  g_key_file_set_integer(kf, GROUP_RX,    "binaural", s->binaural);
  g_key_file_set_string (kf, GROUP_RX,    "mode_filters", s->mode_filt);
  g_key_file_set_string (kf, GROUP_RX,    "var_filters",  s->var_filt);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "fps",   s->fps);
  g_key_file_set_double (kf, GROUP_DISPLAY, "zoom",  s->zoom);
  g_key_file_set_double (kf, GROUP_DISPLAY, "pan_high", s->pan_high);
  g_key_file_set_double (kf, GROUP_DISPLAY, "pan_low",  s->pan_low);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "db_grid",    s->db_grid);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "db_scale",   s->db_scale);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "freq_grid",  s->freq_grid);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "freq_scale", s->freq_scale);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "filter_wf",  s->filter_wf);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "filter_op",  s->filter_op);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "auto_level", s->auto_level);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "avg_spec",   s->avg_spec);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "avg_wf",     s->avg_wf);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "palette",    s->palette);
  g_key_file_set_integer(kf, GROUP_DISPLAY, "band_edges", s->band_edges);
  g_key_file_set_string (kf, GROUP_RX,      "region",     s->region);
  g_key_file_set_string (kf, GROUP_RX,      "country",    s->country);
  g_key_file_set_string (kf, GROUP_DISPLAY, "band_levels", s->band_levels);
  g_key_file_set_integer(kf, GROUP_TX,     "pa_enable", s->tx_pa);
  g_key_file_set_integer(kf, GROUP_TX,     "antenna",   s->tx_ant);
  g_key_file_set_double (kf, GROUP_TX,     "drive_w",   s->tx_drive);
  g_key_file_set_double (kf, GROUP_TX,     "tune_w",    s->tx_tune);
  g_key_file_set_double (kf, GROUP_TX,     "swr_alarm", s->tx_swr);
  g_key_file_set_double (kf, GROUP_TX,     "mic_gain",  s->mic_gain);
  g_key_file_set_integer(kf, GROUP_TX,     "comp",      s->tx_comp);
  g_key_file_set_double (kf, GROUP_TX,     "comp_db",   s->tx_comp_db);
  g_key_file_set_integer(kf, GROUP_TX,     "gate",      s->tx_gate);
  g_key_file_set_double (kf, GROUP_TX,     "gate_db",   s->tx_gate_db);
  g_key_file_set_integer(kf, GROUP_TX,     "monitor",   s->tx_mon);
  g_key_file_set_double (kf, GROUP_TX,     "monitor_db", s->tx_mon_db);
  g_key_file_set_double (kf, GROUP_TX,     "filt_lo",   s->tx_flo);
  g_key_file_set_double (kf, GROUP_TX,     "filt_hi",   s->tx_fhi);
  g_key_file_set_double (kf, GROUP_TX,     "pan_high",  s->tx_pan_high);
  g_key_file_set_double (kf, GROUP_TX,     "pan_low",   s->tx_pan_low);
  g_key_file_set_string (kf, GROUP_TX,     "pa_cal",    s->pa_cal);
  g_key_file_set_string (kf, GROUP_TX,     "pa_trim",   s->pa_trim);
  g_key_file_set_integer(kf, GROUP_RX,     "audio_rate",   s->audio_rate);
  g_key_file_set_string (kf, GROUP_RX,     "audio_device", s->audio_device);
  g_key_file_set_string (kf, GROUP_TX,     "mic_device",   s->mic_device);
  g_key_file_set_integer(kf, GROUP_WINDOW, "width",     s->win_w);
  g_key_file_set_integer(kf, GROUP_WINDOW, "height",    s->win_h);
  g_key_file_set_integer(kf, GROUP_WINDOW, "maximized", s->win_max);

  GError *e = NULL;
  int rc = g_key_file_save_to_file(kf, settings_path(), &e) ? 0 : -1;
  if (e) { g_warning("settings_save: %s", e->message); g_error_free(e); }
  g_key_file_free(kf);
  return rc;
}
