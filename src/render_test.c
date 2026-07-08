/*
 * render_test.c — headless render check for the panadapter.
 *
 * Connects to a live server, grabs one decoded frame, renders it with
 * panadapter_draw() into a Cairo image surface and writes a PNG. No GTK main
 * loop and no display server needed — used to verify the rendering visually
 * without opening a window on the operator's desktop.
 *
 * Usage:  RENDER_OUT=/path.png pihpsdr-render-test [host] [port] [password]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "panadapter.h"
#include "waterfall.h"

int main(int argc, char **argv) {
  const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
  int         port = (argc > 2) ? atoi(argv[2]) : 50000;
  const char *pwd  = (argc > 3) ? argv[3] : getenv("PIHPSDR_PWD");
  if (!pwd) {
    pwd = "";
  }
  const char *out = getenv("RENDER_OUT");
  if (!out) {
    out = "panadapter.png";
  }
  const int W = 1300, H = 680;

  Waterfall *wf = waterfall_new();
  Client *c = client_new(host, port, pwd);
  const char *colenv = getenv("PIHPSDR_COLUMNS");
  if (colenv && atoi(colenv) > 0) {
    client_set_columns(c, atoi(colenv));
  }
  int rc = client_connect(c);

  ClientFrame f;
  memset(&f, 0, sizeof(f));
  const char *status = NULL;

  if (rc != CLIENT_OK) {
    status = client_strerror(rc);
    fprintf(stderr, "connect: %s\n", status);
  }

  /* Accumulate a time-average over several frames (same EMA as the GUI) so the
   * PNG reflects the smoothed trace, not a single noisy frame. */
  static float ema[SPECTRUM_DATA_SIZE];
  int ema_w = 0;
  int frames = 0;

  if (rc == CLIENT_OK) {
    client_start(c);
    uint64_t seq = 0;
    for (int i = 0; i < 120 && frames < 30; i++) { /* up to ~6s */
      if (client_latest(c, &f, &seq)) {
        if (ema_w != f.width) {
          for (int j = 0; j < f.width; j++) { ema[j] = (float)f.dbm[j] - 200.0f; }
          ema_w = f.width;
        } else {
          for (int j = 0; j < f.width; j++) {
            ema[j] += 0.35f * ((float)f.dbm[j] - 200.0f - ema[j]);
          }
        }
        waterfall_push(wf, f.dbm, f.width);
        frames++;
      } else {
        usleep(50000);
      }
    }
    if (frames == 0) {
      status = "connected — no spectrum";
    } else {
      fprintf(stderr, "averaged %d frames: width=%d vfoA=%lld Hz S=%.1f dBm\n",
              frames, f.width, (long long)f.vfo_a_freq, f.s_dbm);
    }
  }

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cairo_t *cr = cairo_create(surf);
  if (status) {
    panadapter_draw(cr, W, H, NULL, NULL, 0, 1, status, NULL);
  } else {
    double low, span;
    waterfall_range(wf, &low, &span);
    int ph = H / 2;
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, W, ph);
    cairo_clip(cr);
    panadapter_draw(cr, W, ph, &f, ema, low, span, NULL, NULL);
    cairo_restore(cr);
    waterfall_draw(wf, cr, 0, ph, W, H - ph);
  }
  cairo_surface_flush(surf);
  cairo_status_t st = cairo_surface_write_to_png(surf, out);
  fprintf(stderr, "wrote %s (%s)\n", out, cairo_status_to_string(st));

  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  client_free(c);
  waterfall_free(wf);
  return (st == CAIRO_STATUS_SUCCESS) ? 0 : 1;
}
