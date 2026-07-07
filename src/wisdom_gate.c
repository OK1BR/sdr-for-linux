/*
 * wisdom_gate.c — first-run FFTW wisdom builder with a progress window.
 *
 * See wisdom_gate.h for the why. WDSPwisdom(dir) imports <dir>/wdspWisdom00 if
 * present (a few ms) or, absent, PATIENT-plans every FFT size WDSP uses (c2c
 * 64..262144 + r2c 64..262144) and writes the cache — tens of seconds of
 * one-time work. It updates a global status string (wisdom_get_status()) as it
 * goes, which we poll to drive the bar. We run the build on a worker thread and
 * pump a private GMainLoop so the window animates; the app boots afterwards.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <adwaita.h>
#include <glib.h>
#include <math.h>
#include <string.h>

#include "wisdom_gate.h"
#include "wdsp.h"      /* WDSPwisdom(), wisdom_get_status() */

/* Cache lives next to config.ini, in ~/.config/sdr-for-linux/. */
#define WISDOM_FILE "wdspWisdom00"

typedef struct {
  char           *dir;      /* directory passed to WDSPwisdom (trailing sep)   */
  GMainLoop      *loop;
  GtkProgressBar *bar;
  GtkSpinner     *spinner;
  GtkLabel       *status;
  gint            done;     /* atomic: worker finished                         */
} WisdomJob;

/* The heavy, one-time PATIENT planning — off the UI thread. */
static gpointer wisdom_worker(gpointer data) {
  WisdomJob *j = data;
  WDSPwisdom(j->dir);
  g_atomic_int_set(&j->done, 1);
  return NULL;
}

/* Plan time grows ~ N·log2(N); weight by that so the bar tracks real time. */
static double size_weight(double n) { return n * log2(n); }

/* Σ size·log2(size) over the ladder 64,128,…,262144 (WDSP's fixed set). */
static double ladder_sum(void) {
  double s = 0.0;
  for (long n = 64; n <= 262144; n <<= 1) { s += size_weight((double)n); }
  return s;
}

/* Fraction complete, from the live WDSP status line. WDSPwisdom plans, in order:
 * phase A = c2c (3 plans/size) for 64..262144, then phase B = r2c (1/size) for
 * 64..262144. We credit sizes already finished; the current (in-flight) size
 * sits until it completes — the spinner carries liveness through the big ones. */
static double wisdom_fraction(const char *st) {
  const double SUM = ladder_sum();
  const double A = 3.0 * SUM, TOTAL = 4.0 * SUM;   /* phase A: 3 plans, B: 1 */
  if (!st || !*st) { return 0.0; }
  if (strstr(st, "complete")) { return 1.0; }

  /* trailing integer in the status = the size being planned */
  const char *end = st + strlen(st);
  while (end > st && !g_ascii_isdigit(end[-1])) { end--; }
  const char *beg = end;
  while (beg > st && g_ascii_isdigit(beg[-1])) { beg--; }
  if (beg == end) { return 0.0; }
  long sz = strtol(beg, NULL, 10);

  long base = 64;                       /* largest ladder size <= sz (handles size+1) */
  while ((base << 1) <= sz && (base << 1) <= 262144) { base <<= 1; }

  double below = 0.0;                   /* Σ weight of sizes fully below the current */
  for (long n = 64; n < base; n <<= 1) { below += size_weight((double)n); }

  double frac = strstr(st, "REAL") ? (A + below) / TOTAL   /* phase B */
                                   : (3.0 * below) / TOTAL; /* phase A */
  return CLAMP(frac, 0.0, 1.0);
}

/* 100 ms tick: refresh bar + status text; quit the loop once the worker is done. */
static gboolean wisdom_tick(gpointer data) {
  WisdomJob *j = data;
  if (g_atomic_int_get(&j->done)) {
    gtk_progress_bar_set_fraction(j->bar, 1.0);
    g_main_loop_quit(j->loop);
    return G_SOURCE_CONTINUE;   /* run_first_run_ui removes the source after the loop */
  }
  const char *st = wisdom_get_status();
  double f = wisdom_fraction(st);
  if (f > gtk_progress_bar_get_fraction(j->bar)) {   /* monotonic */
    gtk_progress_bar_set_fraction(j->bar, f);
  }
  /* trim the trailing newline WDSP puts in the status */
  char *line = g_strchomp(g_strdup(st ? st : ""));
  gtk_label_set_text(j->status, line);
  g_free(line);
  return G_SOURCE_CONTINUE;
}

/* Build + run the first-run window; returns when the cache is written. */
static void run_first_run_ui(const char *dir) {
  GtkWidget *win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(win), "SDR for Linux");
  gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(win), 480, -1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);

  GtkWidget *title = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(title),
      "<span size='large' weight='bold'>Preparing FFT plans</span>");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0);

  GtkWidget *sub = gtk_label_new(
      "First run only — this can take a few minutes. The result is cached, "
      "so it won't happen again.");
  gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
  gtk_label_set_wrap(GTK_LABEL(sub), TRUE);
  gtk_widget_add_css_class(sub, "dim-label");

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *spinner = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(spinner));
  GtkWidget *bar = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);
  gtk_widget_set_hexpand(bar, TRUE);
  gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), spinner);
  gtk_box_append(GTK_BOX(row), bar);

  GtkWidget *status = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(status), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(status, "dim-label");

  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), sub);
  gtk_box_append(GTK_BOX(box), row);
  gtk_box_append(GTK_BOX(box), status);
  gtk_window_set_child(GTK_WINDOW(win), box);

  WisdomJob job = {
    .dir     = g_strdup(dir),
    .loop    = g_main_loop_new(NULL, FALSE),
    .bar     = GTK_PROGRESS_BAR(bar),
    .spinner = GTK_SPINNER(spinner),
    .status  = GTK_LABEL(status),
    .done    = 0,
  };

  gtk_window_present(GTK_WINDOW(win));
  GThread *worker = g_thread_new("fftw-wisdom", wisdom_worker, &job);
  guint tick = g_timeout_add(100, wisdom_tick, &job);

  g_main_loop_run(job.loop);

  g_source_remove(tick);
  g_thread_join(worker);
  gtk_window_destroy(GTK_WINDOW(win));
  g_main_loop_unref(job.loop);
  g_free(job.dir);
}

void wisdom_ensure(void) {
  /* SDRFL_WISDOM_DIR overrides the cache location (used by sdrfl-wisdom-test to
   * exercise a fresh build in a throwaway dir). Default: next to config.ini. */
  const char *env = g_getenv("SDRFL_WISDOM_DIR");
  char *dir  = (env && *env) ? g_strdup(env)
                             : g_build_filename(g_get_user_config_dir(), "sdr-for-linux", NULL);
  g_mkdir_with_parents(dir, 0755);
  char *file = g_build_filename(dir, WISDOM_FILE, NULL);

  /* WDSPwisdom() does strcat(dir, "wdspWisdom00") with no separator → the
   * directory string it gets must already end in one. */
  char *dir_sep = g_strconcat(dir, G_DIR_SEPARATOR_S, NULL);

  if (g_file_test(file, G_FILE_TEST_EXISTS)) {
    WDSPwisdom(dir_sep);                 /* fast import; no window */
  } else {
    if (!gtk_is_initialized()) { gtk_init(); }   /* only GTK widgets used here */
    run_first_run_ui(dir_sep);
    if (!g_file_test(file, G_FILE_TEST_EXISTS)) {
      g_warning("wisdom: cache not written to %s — will rebuild next start", file);
    }
  }

  g_free(dir_sep);
  g_free(file);
  g_free(dir);
}
