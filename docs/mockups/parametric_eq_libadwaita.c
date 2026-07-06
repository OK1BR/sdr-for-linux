/* Mockup — parametric EQ with a drawn, draggable response curve INSIDE a
 * libadwaita window. Proves custom Cairo graphics + gestures live happily in an
 * Adwaita app (libadwaita is built on GTK4; plain widgets work unchanged). */
#include <adwaita.h>
#include <math.h>

#define NB 5
#define FMIN 20.0
#define FMAX 20000.0
#define GMAX 15.0
#define GMIN -15.0

typedef struct {
  double freq[NB], gain[NB], q[NB];
  int drag;            /* index of node being dragged, -1 = none */
  GtkWidget *area;
} Eq;

static double lmin, lmax;
static double f2x(double f, int w) { return (log10(f) - lmin) / (lmax - lmin) * w; }
static double x2f(double x, int w) { return pow(10, lmin + (x / w) * (lmax - lmin)); }
static double g2y(double g, int h) { return (GMAX - g) / (GMAX - GMIN) * h; }
static double y2g(double y, int h) { return GMAX - (y / h) * (GMAX - GMIN); }

/* one peaking band's contribution (dB), Gaussian bell in log-freq — visual approx */
static double band_db(Eq *e, int i, double f) {
  double d = (log10(f) - log10(e->freq[i])) * e->q[i];
  return e->gain[i] * exp(-0.5 * d * d);
}
static double total_db(Eq *e, double f) {
  double s = 0; for (int i = 0; i < NB; i++) s += band_db(e, i, f); return s;
}

static void draw(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer u) {
  (void)a;
  Eq *e = u;
  cairo_set_source_rgb(cr, 0.05, 0.06, 0.08);
  cairo_rectangle(cr, 0, 0, w, h); cairo_fill(cr);

  /* horizontal dB grid */
  cairo_select_font_face(cr, "monospace", 0, 0); cairo_set_font_size(cr, 10);
  for (int db = -12; db <= 12; db += 6) {
    double y = g2y(db, h);
    cairo_set_source_rgba(cr, 0.5, 0.6, 0.7, db == 0 ? 0.30 : 0.12);
    cairo_set_line_width(cr, db == 0 ? 1.4 : 1.0);
    cairo_move_to(cr, 0, y + 0.5); cairo_line_to(cr, w, y + 0.5); cairo_stroke(cr);
    char t[8]; snprintf(t, sizeof t, "%+d", db);
    cairo_set_source_rgba(cr, 0.6, 0.7, 0.8, 0.6); cairo_move_to(cr, 4, y - 3); cairo_show_text(cr, t);
  }
  /* vertical freq grid */
  double marks[] = {50,100,200,500,1000,2000,5000,10000};
  const char *ml[] = {"50","100","200","500","1k","2k","5k","10k"};
  for (int i = 0; i < 8; i++) {
    double x = f2x(marks[i], w);
    cairo_set_source_rgba(cr, 0.5, 0.6, 0.7, 0.10); cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x, 0); cairo_line_to(cr, x, h); cairo_stroke(cr);
    cairo_set_source_rgba(cr, 0.6, 0.7, 0.8, 0.5); cairo_move_to(cr, x + 3, h - 5); cairo_show_text(cr, ml[i]);
  }
  /* faint per-band bells */
  for (int i = 0; i < NB; i++) {
    cairo_set_source_rgba(cr, 0.35, 0.7, 0.9, 0.20); cairo_set_line_width(cr, 1);
    for (int x = 0; x <= w; x += 2) {
      double y = g2y(band_db(e, i, x2f(x, w)), h);
      if (x == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
  }
  /* summed response */
  cairo_set_source_rgb(cr, 0.35, 0.95, 1.0); cairo_set_line_width(cr, 2.2);
  for (int x = 0; x <= w; x++) {
    double y = g2y(total_db(e, x2f(x, w)), h);
    if (x == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
  }
  cairo_stroke(cr);
  /* node handles */
  for (int i = 0; i < NB; i++) {
    double x = f2x(e->freq[i], w), y = g2y(e->gain[i], h);
    cairo_set_source_rgb(cr, 1.0, 0.6, 0.15);
    cairo_arc(cr, x, y, i == e->drag ? 9 : 7, 0, 2 * M_PI); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.05, 0.06, 0.08);
    cairo_arc(cr, x, y, 3, 0, 2 * M_PI); cairo_fill(cr);
    char t[48];
    snprintf(t, sizeof t, "%.0f Hz  %+.1f dB", e->freq[i], e->gain[i]);
    cairo_set_source_rgba(cr, 0.9, 0.95, 1.0, 0.9);
    cairo_move_to(cr, x + 11, y - 8); cairo_show_text(cr, t);
  }
}

static void drag_begin(GtkGestureDrag *g, double x, double y, gpointer u) {
  Eq *e = u;
  int w = gtk_widget_get_width(e->area);
  int best = 0; double bd = 1e9;
  for (int i = 0; i < NB; i++) {
    double dx = f2x(e->freq[i], w) - x; if (fabs(dx) < bd) { bd = fabs(dx); best = i; }
  }
  e->drag = best;
  (void)g; (void)y;
  gtk_widget_queue_draw(e->area);
}
static void drag_update(GtkGestureDrag *g, double ox, double oy, gpointer u) {
  Eq *e = u;
  if (e->drag < 0) return;
  double sx, sy; gtk_gesture_drag_get_start_point(g, &sx, &sy);
  int w = gtk_widget_get_width(e->area), h = gtk_widget_get_height(e->area);
  double f = x2f(sx + ox, w), gg = y2g(sy + oy, h);
  if (f < FMIN) f = FMIN; if (f > FMAX) f = FMAX;
  if (gg > GMAX) gg = GMAX; if (gg < GMIN) gg = GMIN;
  e->freq[e->drag] = f; e->gain[e->drag] = gg;
  gtk_widget_queue_draw(e->area);
}
static void drag_end(GtkGestureDrag *g, double ox, double oy, gpointer u) {
  (void)g; (void)ox; (void)oy; ((Eq *)u)->drag = -1;
  gtk_widget_queue_draw(((Eq *)u)->area);
}

static void activate(GtkApplication *app, gpointer u) {
  Eq *e = u;
  GtkWidget *win = adw_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(win), "Parametric Equalizer");
  gtk_window_set_default_size(GTK_WINDOW(win), 820, 560);

  GtkWidget *header = adw_header_bar_new();
  GtkWidget *badge = gtk_label_new("LIBADWAITA  +  custom Cairo widget");
  gtk_widget_add_css_class(badge, "dim");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), badge);
  GtkWidget *reset = gtk_button_new_with_label("Flat");
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), reset);

  GtkWidget *tv = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

  e->area = gtk_drawing_area_new();
  gtk_widget_set_vexpand(e->area, TRUE); gtk_widget_set_hexpand(e->area, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(e->area), draw, e, NULL);
  GtkGesture *drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-begin",  G_CALLBACK(drag_begin),  e);
  g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), e);
  g_signal_connect(drag, "drag-end",    G_CALLBACK(drag_end),    e);
  gtk_widget_add_controller(e->area, GTK_EVENT_CONTROLLER(drag));

  GtkWidget *hint = gtk_label_new("↳  táhni oranžové uzly — mění se frekvence (X) a zisk (Y); křivka se překresluje živě");
  gtk_widget_add_css_class(hint, "dim");
  gtk_widget_set_margin_top(hint, 8); gtk_widget_set_margin_bottom(hint, 10);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), e->area);
  gtk_box_append(GTK_BOX(box), hint);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), box);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), tv);

  GtkCssProvider *p = gtk_css_provider_new();
  gtk_css_provider_load_from_string(p, ".dim{opacity:.6;font-size:12px;}");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  lmin = log10(FMIN); lmax = log10(FMAX);
  Eq e = { .drag = -1 };
  double f0[NB] = {60, 250, 1000, 4000, 12000};
  double g0[NB] = {4, -3, 2.5, -5, 3};
  double q0[NB] = {1.4, 2.0, 1.6, 2.4, 1.2};
  for (int i = 0; i < NB; i++) { e.freq[i]=f0[i]; e.gain[i]=g0[i]; e.q[i]=q0[i]; }
  AdwApplication *app = adw_application_new("cz.ok1br.mock.eq", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), &e);
  int s = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return s;
}
