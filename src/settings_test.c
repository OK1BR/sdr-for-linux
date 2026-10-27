/*
 * sdrfl-settings-test — offline gate for the per-band Drive / TUNE memory
 * (issue #47) and the per-radio TX-calibration groups it lives in.
 *
 * Pins, against a scratch config.ini (XDG_CONFIG_HOME is pointed at a temp
 * dir BEFORE the first settings call — settings_path() caches it):
 *   1. the "key=W;..." table helpers: defaults, clamp, unknown/malformed tokens
 *      skipped, format→parse exact round-trip, truncation by whole tokens;
 *   2. migration: a file written before #47 (scalar drive_w / tune_w only)
 *      seeds EVERY band from the scalar — no version marker, the absent key
 *      is the marker;
 *   3. a per-band edit does not leak: edit 20 m, save, reload → 40 m untouched;
 *   4. a band missing from a saved table falls back to the scalar;
 *   5. ⛔ per-radio isolation: the [tx-hermes2] table never shows up in [tx]
 *      and vice versa (settings_load reads [tx], settings_load_txdev the group).
 *
 * GLib only — no GTK, no WDSP, no radio. Exit 0 = pass.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"

#define NB 11
static const char *const KEYS[NB] = { "160m", "80m", "60m", "40m", "30m", "20m",
                                      "17m", "15m", "12m", "10m", "6m" };
enum { B160, B80, B60, B40, B30, B20, B17, B15, B12, B10, B6 };

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int eq(double a, double b) { return fabs(a - b) < 1e-9; }

static void write_file(const char *path, const char *text) {
  GError *e = NULL;
  if (!g_file_set_contents(path, text, -1, &e)) {
    printf("FAIL cannot write %s: %s\n", path, e ? e->message : "?"); fails++;
    if (e) { g_error_free(e); }
  }
}

static Settings fresh(void) {
  Settings s; memset(&s, 0, sizeof s);
  s.tx_drive = 25.0; s.tx_tune = 10.0; s.tx_digi_max = 100.0;
  return s;
}

int main(void) {
  /* ---- 1. pure table helpers ------------------------------------------ */
  double v[NB];
  settings_band_table_parse(NULL, KEYS, NB, 25.0, 0.0, 100.0, v);
  for (int i = 0; i < NB; i++) { CHECK(eq(v[i], 25.0), "NULL table: band %d = %g", i, v[i]); }
  settings_band_table_parse("", KEYS, NB, 7.5, 0.0, 100.0, v);
  for (int i = 0; i < NB; i++) { CHECK(eq(v[i], 7.5), "empty table: band %d = %g", i, v[i]); }

  settings_band_table_parse("20m=50;40m=30.5;15m=999;17m=abc;bogus=3;=4;10m=-2;80m",
                            KEYS, NB, 25.0, 0.0, 100.0, v);
  CHECK(eq(v[B20], 50.0),  "20m = %g (want 50)", v[B20]);
  CHECK(eq(v[B40], 30.5),  "40m = %g (want 30.5)", v[B40]);
  CHECK(eq(v[B15], 100.0), "15m = %g (want clamp 100)", v[B15]);
  CHECK(eq(v[B17], 25.0),  "17m = %g (malformed number must keep dflt 25)", v[B17]);
  CHECK(eq(v[B10], 0.0),   "10m = %g (want clamp 0)", v[B10]);
  CHECK(eq(v[B80], 25.0),  "80m = %g (token without '=' must keep dflt)", v[B80]);
  CHECK(eq(v[B160], 25.0) && eq(v[B6], 25.0), "untouched bands keep dflt");

  /* format → parse exact round-trip (one decimal) */
  double in[NB], out[NB]; char buf[256];
  for (int i = 0; i < NB; i++) { in[i] = 1.5 * i + 0.5; }
  settings_band_table_format(buf, sizeof buf, KEYS, NB, in);
  CHECK(strncmp(buf, "160m=0.5;80m=2.0;60m=3.5;", 25) == 0, "format prefix: %s", buf);
  settings_band_table_parse(buf, KEYS, NB, -1.0, -10.0, 1000.0, out);
  for (int i = 0; i < NB; i++) { CHECK(eq(in[i], out[i]), "round-trip band %d: %g vs %g", i, in[i], out[i]); }
  /* truncation drops whole tokens, never half a token */
  char small[20];
  settings_band_table_format(small, sizeof small, KEYS, NB, in);
  CHECK(strcmp(small, "160m=0.5;80m=2.0") == 0, "truncated format: '%s'", small);
  settings_band_table_format(small, 1, KEYS, NB, in);
  CHECK(small[0] == '\0', "len 1 → empty");

  /* ---- scratch config dir --------------------------------------------- */
  GError *e = NULL;
  char *tmp = g_dir_make_tmp("sdrfl-settings-XXXXXX", &e);
  if (!tmp) { printf("FAIL g_dir_make_tmp: %s\n", e ? e->message : "?"); return 2; }
  g_setenv("XDG_CONFIG_HOME", tmp, TRUE);        /* before the first settings call */
  char *dir = g_build_filename(tmp, "sdr-for-linux", NULL);
  g_mkdir_with_parents(dir, 0755);
  const char *path = settings_path();
  CHECK(g_str_has_prefix(path, tmp), "settings_path '%s' not under '%s'", path, tmp);
  printf("scratch config: %s\n", path);

  /* ---- 2. migration: pre-#47 file, scalar only ------------------------ */
  write_file(path, "[radio]\nip=192.0.2.1\n[tx]\npa_enable=1\ndrive_w=40\ntune_w=15\n");
  Settings s = fresh();
  CHECK(settings_load(&s) == 1, "settings_load parsed the pre-#47 file");
  CHECK(eq(s.tx_drive, 40.0) && eq(s.tx_tune, 15.0), "scalars read: %g / %g", s.tx_drive, s.tx_tune);
  CHECK(s.drive_w_band[0] == '\0' && s.tune_w_band[0] == '\0', "no table in a pre-#47 file");
  double bd[NB], bt[NB];
  settings_band_table_parse(s.drive_w_band, KEYS, NB, s.tx_drive, 0.0, 100.0, bd);
  settings_band_table_parse(s.tune_w_band,  KEYS, NB, s.tx_tune,  0.0, 100.0, bt);
  for (int i = 0; i < NB; i++) {
    CHECK(eq(bd[i], 40.0) && eq(bt[i], 15.0), "migration seed band %d: %g / %g", i, bd[i], bt[i]);
  }

  /* ---- 3. per-band edit does not leak -------------------------------- */
  bd[B20] = 60.0; bt[B20] = 5.0;                 /* operator on 20 m moves both sliders */
  s.tx_drive = 60.0; s.tx_tune = 5.0;            /* live = current band's */
  settings_band_table_format(s.drive_w_band, sizeof s.drive_w_band, KEYS, NB, bd);
  settings_band_table_format(s.tune_w_band,  sizeof s.tune_w_band,  KEYS, NB, bt);
  g_strlcpy(s.ip, "192.0.2.1", sizeof s.ip);
  CHECK(settings_save(&s) == 0, "settings_save (G2E, [tx])");
  Settings r = fresh();
  CHECK(settings_load(&r) == 1, "reload");
  settings_band_table_parse(r.drive_w_band, KEYS, NB, r.tx_drive, 0.0, 100.0, bd);
  settings_band_table_parse(r.tune_w_band,  KEYS, NB, r.tx_tune,  0.0, 100.0, bt);
  CHECK(eq(bd[B20], 60.0) && eq(bt[B20], 5.0),   "20 m after reload: %g / %g", bd[B20], bt[B20]);
  CHECK(eq(bd[B40], 40.0) && eq(bt[B40], 15.0),  "40 m untouched after a 20 m edit: %g / %g", bd[B40], bt[B40]);
  CHECK(eq(bd[B160], 40.0) && eq(bd[B6], 40.0),  "160 m / 6 m untouched");
  CHECK(eq(r.tx_drive, 60.0) && eq(r.tx_tune, 5.0), "scalar fallback = the live values: %g / %g", r.tx_drive, r.tx_tune);
  /* the file carries the keys where a human expects them */
  char *text = NULL;
  g_file_get_contents(path, &text, NULL, NULL);
  CHECK(text && strstr(text, "drive_w_band=160m=40.0;80m=40.0;60m=40.0;40m=40.0;30m=40.0;20m=60.0;"),
        "drive_w_band in [tx]: %s", text ? text : "(none)");
  CHECK(text && strstr(text, "tune_w_band=160m=15.0;"), "tune_w_band in [tx]");
  g_free(text);

  /* ---- 4. a band missing from the table falls back to the scalar ------ */
  write_file(path, "[tx]\ndrive_w=33\ntune_w=11\ndrive_w_band=20m=80\ntune_w_band=20m=20;40m=7\n");
  s = fresh();
  CHECK(settings_load(&s) == 1, "load hand-edited partial table");
  settings_band_table_parse(s.drive_w_band, KEYS, NB, s.tx_drive, 0.0, 100.0, bd);
  settings_band_table_parse(s.tune_w_band,  KEYS, NB, s.tx_tune,  0.0, 100.0, bt);
  CHECK(eq(bd[B20], 80.0) && eq(bt[B20], 20.0), "partial: 20 m from the table");
  CHECK(eq(bd[B40], 33.0) && eq(bt[B40], 7.0),  "partial: 40 m drive from scalar, tune from table: %g / %g", bd[B40], bt[B40]);
  CHECK(eq(bd[B15], 33.0) && eq(bt[B15], 11.0), "partial: 15 m both from scalar");
  /* clamp to a 10 W radio's rating on parse — the per-radio re-clamp path */
  settings_band_table_parse(s.drive_w_band, KEYS, NB, 1.0, 0.0, 10.0, bd);
  CHECK(eq(bd[B20], 10.0) && eq(bd[B40], 1.0), "10 W rating: 80 → 10, missing → 1 W seed");

  /* ---- 5. per-radio isolation ----------------------------------------- */
  Settings h = fresh();
  g_strlcpy(h.tx_group, "tx-hermes2", sizeof h.tx_group);
  h.tx_drive = 3.0; h.tx_tune = 1.0;
  double hd[NB], ht[NB];
  for (int i = 0; i < NB; i++) { hd[i] = 1.0; ht[i] = 1.0; }
  hd[B20] = 3.0; hd[B40] = 2.0;
  settings_band_table_format(h.drive_w_band, sizeof h.drive_w_band, KEYS, NB, hd);
  settings_band_table_format(h.tune_w_band,  sizeof h.tune_w_band,  KEYS, NB, ht);
  CHECK(settings_save(&h) == 0, "settings_save (10E, [tx-hermes2])");
  /* [tx] (G2E) must still carry the hand-edited partial table from step 4 */
  s = fresh();
  CHECK(settings_load(&s) == 1, "reload after the 10E save");
  CHECK(strcmp(s.drive_w_band, "20m=80") == 0, "[tx] drive_w_band survived a [tx-hermes2] save: '%s'", s.drive_w_band);
  CHECK(strcmp(s.tune_w_band, "20m=20;40m=7") == 0, "[tx] tune_w_band survived: '%s'", s.tune_w_band);
  CHECK(eq(s.tx_drive, 33.0), "[tx] drive_w survived: %g", s.tx_drive);
  /* the 10E's group reads back its own table and nothing of the G2E's */
  Settings t = fresh();
  t.tx_drive = 1.0; t.tx_tune = 1.0;             /* the radio's SAFE defaults */
  CHECK(settings_load_txdev(&t, "tx-hermes2") == 1, "settings_load_txdev");
  settings_band_table_parse(t.drive_w_band, KEYS, NB, t.tx_drive, 0.0, 10.0, hd);
  CHECK(eq(hd[B20], 3.0) && eq(hd[B40], 2.0) && eq(hd[B15], 1.0), "10E table: %g / %g / %g", hd[B20], hd[B40], hd[B15]);
  /* a group that never saved → tables stay as pre-set (the safe defaults) */
  Settings u = fresh(); u.tx_drive = 1.0;
  CHECK(settings_load_txdev(&u, "tx-saturn") == 1 && u.drive_w_band[0] == '\0' && eq(u.tx_drive, 1.0),
        "unknown group leaves the safe defaults: '%s' %g", u.drive_w_band, u.tx_drive);
  /* and the file has both groups, each with its own table */
  text = NULL;
  g_file_get_contents(path, &text, NULL, NULL);
  CHECK(text && strstr(text, "[tx-hermes2]") && strstr(text, "drive_w_band=160m=1.0;80m=1.0;60m=1.0;40m=2.0;30m=1.0;20m=3.0;"),
        "[tx-hermes2] table in the file");
  CHECK(text && strstr(text, "drive_w_band=20m=80"), "[tx] table still in the file");
  g_free(text);

  /* ---- scratch cleanup (this test's own files only) ------------------- */
  g_remove(path);
  g_rmdir(dir);
  g_rmdir(tmp);
  g_free(dir); g_free(tmp);

  printf("sdrfl-settings-test: %d checks, %d failed — %s\n", checks, fails, fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
