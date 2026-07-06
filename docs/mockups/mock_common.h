/* Shared mockup bits: fake panadapter/waterfall + a GTK4 control strip.
 * Used by mock_plain.c (pure GTK4) and mock_adw.c (libadwaita). Throwaway. */
#ifndef MOCK_COMMON_H
#define MOCK_COMMON_H
#include <gtk/gtk.h>
#include <math.h>

static inline double mock_hash(int x, int r) {
  unsigned h = (unsigned)(x * 374761393 + r * 668265263);
  h = (h ^ (h >> 13)) * 1274126177u;
  return (double)((h ^ (h >> 16)) & 0xffff) / 65535.0;
}
static inline double mock_gauss(double x, double mu, double sig) {
  double d = (x - mu) / sig; return exp(-0.5 * d * d);
}
/* spectral magnitude in [0,1], a few signals over a noise floor */
static inline double mock_mag(double xf, int row) {
  double m = 0.10 + 0.05 * mock_hash((int)(xf * 400), row);
  m += 0.62 * mock_gauss(xf, 0.500, 0.010);            /* centre carrier   */
  m += 0.40 * mock_gauss(xf, 0.322, 0.006);
  m += 0.30 * mock_gauss(xf, 0.688, 0.008);
  m += 0.22 * mock_gauss(xf, 0.760, 0.004);
  m += 0.18 * mock_gauss(xf, 0.230, 0.005);
  return m > 1.0 ? 1.0 : m;
}
static inline void mock_cmap(double t, double *r, double *g, double *b) {
  if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;
  if (t < 0.45) { double u = t / 0.45; *r = 0.02; *g = 0.05 + 0.35 * u; *b = 0.12 + 0.55 * u; }
  else if (t < 0.8) { double u = (t - 0.45) / 0.35; *r = 0.05 + 0.15 * u; *g = 0.40 + 0.55 * u; *b = 0.67 - 0.25 * u; }
  else { double u = (t - 0.8) / 0.2; *r = 0.20 + 0.75 * u; *g = 0.95; *b = 0.42 + 0.45 * u; }
}

static inline void mock_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer d) {
  (void)area; (void)d;
  int ph = (int)(h * 0.52);

  /* ---- panadapter ---- */
  cairo_set_source_rgb(cr, 0.035, 0.045, 0.06);
  cairo_rectangle(cr, 0, 0, w, ph); cairo_fill(cr);
  cairo_set_line_width(cr, 1.0);
  for (int i = 1; i < 6; i++) {
    double y = ph * i / 6.0;
    cairo_set_source_rgba(cr, 0.4, 0.5, 0.6, 0.10);
    cairo_move_to(cr, 0, y + 0.5); cairo_line_to(cr, w, y + 0.5); cairo_stroke(cr);
  }
  /* passband highlight around centre */
  double cx = w * 0.5;
  cairo_set_source_rgba(cr, 0.15, 0.7, 0.9, 0.10);
  cairo_rectangle(cr, cx + 0.005 * w, 0, 0.035 * w, ph); cairo_fill(cr);
  cairo_set_source_rgba(cr, 0.9, 0.9, 0.95, 0.35);
  cairo_move_to(cr, cx + 0.5, 0); cairo_line_to(cr, cx + 0.5, ph); cairo_stroke(cr);
  /* trace */
  for (int x = 0; x <= w; x++) {
    double xf = (double)x / w;
    double y = ph - mock_mag(xf, 7) * ph * 0.92 - ph * 0.04;
    if (x == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
  }
  cairo_line_to(cr, w, ph); cairo_line_to(cr, 0, ph); cairo_close_path(cr);
  cairo_set_source_rgba(cr, 0.10, 0.80, 0.95, 0.16); cairo_fill(cr);
  for (int x = 0; x <= w; x++) {
    double xf = (double)x / w;
    double y = ph - mock_mag(xf, 7) * ph * 0.92 - ph * 0.04;
    if (x == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
  }
  cairo_set_source_rgba(cr, 0.35, 0.95, 1.0, 0.9); cairo_set_line_width(cr, 1.2); cairo_stroke(cr);

  /* ---- waterfall ---- */
  int step = 2;
  for (int ry = ph; ry < h; ry += step) {
    int row = ry - ph;
    for (int x = 0; x < w; x += step) {
      double xf = (double)x / w;
      double v = mock_mag(xf, row) * (0.7 + 0.3 * mock_hash(x, row));
      double r, g, b; mock_cmap(v, &r, &g, &b);
      cairo_set_source_rgb(cr, r, g, b);
      cairo_rectangle(cr, x, ry, step, step); cairo_fill(cr);
    }
  }
}

/* one linked segmented group of radio-style toggles; first label active */
static inline GtkWidget *mock_segmented(const char *const *labels, const char *css) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(box, "linked");
  GtkWidget *group = NULL;
  for (int i = 0; labels[i]; i++) {
    GtkWidget *b = gtk_toggle_button_new_with_label(labels[i]);
    if (css) gtk_widget_add_css_class(b, css);
    if (!group) { group = b; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE); }
    else gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(group));
    gtk_box_append(GTK_BOX(box), b);
  }
  return box;
}

static inline GtkWidget *mock_labeled(const char *text, GtkWidget *w) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "dim");
  gtk_box_append(GTK_BOX(box), l);
  gtk_box_append(GTK_BOX(box), w);
  return box;
}

/* the horizontal control strip shown under the header */
static inline GtkWidget *mock_controls(void) {
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(bar, "controlbar");

  static const char *modes[] = {"LSB","USB","CWL","CWU","AM","FM", NULL};
  static const char *bands[] = {"160","80","40","20","17","15","12","10", NULL};

  /* modes (USB default → make USB active by ordering) */
  static const char *modes2[] = {"USB","LSB","CWL","CWU","AM","FM", NULL};
  gtk_box_append(GTK_BOX(bar), mock_segmented(modes2, "mode"));
  (void)modes;

  GtkWidget *filt = gtk_drop_down_new_from_strings((const char *[]){"2.7 k","2.4 k","1.8 k","500","250", NULL});
  gtk_box_append(GTK_BOX(bar), mock_labeled("Filter", filt));

  GtkWidget *agc = gtk_drop_down_new_from_strings((const char *[]){"Med","Fast","Slow","Long","Off", NULL});
  gtk_box_append(GTK_BOX(bar), mock_labeled("AGC", agc));

  GtkWidget *nrbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(nrbox, "linked");
  const char *nr[] = {"NR","NB","ANF"};
  for (int i = 0; i < 3; i++) { GtkWidget *t = gtk_toggle_button_new_with_label(nr[i]); gtk_box_append(GTK_BOX(nrbox), t); }
  gtk_box_append(GTK_BOX(bar), nrbox);

  GtkWidget *vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -40, 0, 1);
  gtk_range_set_value(GTK_RANGE(vol), -10);
  gtk_widget_set_size_request(vol, 130, -1);
  gtk_box_append(GTK_BOX(bar), mock_labeled("AF", vol));

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bar), spacer);

  gtk_box_append(GTK_BOX(bar), mock_segmented(bands, "band"));
  return bar;
}

/* the big VFO readout used as the header title widget */
static inline GtkWidget *mock_vfo(void) {
  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(v, GTK_ALIGN_CENTER);
  GtkWidget *f = gtk_label_new("14 074 000");
  gtk_widget_add_css_class(f, "vfo");
  GtkWidget *s = gtk_label_new("Hz  ·  VFO A  ·  USB");
  gtk_widget_add_css_class(s, "vfosub");
  gtk_box_append(GTK_BOX(v), f);
  gtk_box_append(GTK_BOX(v), s);
  return v;
}

static inline void mock_css(void) {
  GtkCssProvider *p = gtk_css_provider_new();
  const char *css =
    ".controlbar { padding: 7px 10px; }"
    ".vfo { font-family: monospace; font-size: 22px; font-weight: 800; letter-spacing: 2px; }"
    ".vfosub { font-size: 9px; letter-spacing: 3px; opacity: 0.55; }"
    ".dim { opacity: 0.6; font-size: 11px; }"
    "button.band, button.mode { min-width: 30px; padding-left: 7px; padding-right: 7px; }"
    "button.mode:checked { background: #1d6fa5; color: #fff; }"
    "button.band:checked { background: #b5651d; color: #fff; }"
    ".badge-plain { background: #1d6fa5; color: #fff; font-weight: 800; font-size: 11px; padding: 2px 10px; border-radius: 9px; letter-spacing: 1px; }"
    ".badge-adw { background: #8a3ffc; color: #fff; font-weight: 800; font-size: 11px; padding: 2px 10px; border-radius: 9px; letter-spacing: 1px; }";
  gtk_css_provider_load_from_string(p, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static inline GtkWidget *mock_spectrum(void) {
  GtkWidget *area = gtk_drawing_area_new();
  gtk_widget_set_vexpand(area, TRUE);
  gtk_widget_set_hexpand(area, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), mock_draw, NULL, NULL);
  return area;
}

#endif
