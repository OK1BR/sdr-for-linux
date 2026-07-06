/* Settings mockup B — libadwaita: AdwPreferencesWindow with Adw*Row widgets.
 * The "settings for free" experience. Themed Adwaita (ignores custom GTK theme). */
#include <adwaita.h>

static GtkWidget *combo(const char *title, const char *const *items, guint sel) {
  GtkStringList *m = gtk_string_list_new(items);
  return g_object_new(ADW_TYPE_COMBO_ROW, "title", title,
                      "model", G_LIST_MODEL(m), "selected", sel, NULL);
}
static GtkWidget *sw(const char *title, const char *subtitle, gboolean on) {
  return g_object_new(ADW_TYPE_SWITCH_ROW, "title", title,
                      "subtitle", subtitle, "active", on, NULL);
}
static GtkWidget *spin(const char *title, const char *subtitle, double lo, double hi, double val) {
  GtkAdjustment *a = gtk_adjustment_new(val, lo, hi, 1, 1, 0);
  return g_object_new(ADW_TYPE_SPIN_ROW, "title", title, "subtitle", subtitle,
                      "adjustment", a, "digits", 0, NULL);
}
static GtkWidget *entry(const char *title, const char *text) {
  GtkWidget *r = g_object_new(ADW_TYPE_ENTRY_ROW, "title", title, NULL);
  gtk_editable_set_text(GTK_EDITABLE(r), text);
  return r;
}
static AdwPreferencesPage *page(const char *title, const char *icon) {
  return ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
                              "title", title, "icon-name", icon, NULL));
}
static AdwPreferencesGroup *grp(AdwPreferencesPage *p, const char *title) {
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", title, NULL));
  adw_preferences_page_add(p, g);
  return g;
}

static void activate(GtkApplication *app, gpointer d) {
  (void)d;
  GtkWidget *win = adw_preferences_window_new();
  gtk_application_add_window(app, GTK_WINDOW(win));
  gtk_window_set_title(GTK_WINDOW(win), "Preferences");
  gtk_window_set_default_size(GTK_WINDOW(win), 760, 580);

  const char *rates[] = {"48 kHz","96 kHz","192 kHz","384 kHz","768 kHz", NULL};
  AdwPreferencesPage *p; AdwPreferencesGroup *g;

  /* Radio */
  p = page("Radio", "network-workgroup-symbolic");
  g = grp(p, "Connection");
  adw_preferences_group_add(g, entry("Radio IP address", "192.168.1.247"));
  adw_preferences_group_add(g, combo("Sample rate", rates, 2));
  adw_preferences_group_add(g, sw("Auto-connect on start", NULL, TRUE));
  g = grp(p, "Discovery");
  adw_preferences_group_add(g, sw("Auto-discover on LAN", NULL, TRUE));
  adw_preferences_group_add(g, spin("Discovery timeout", "seconds", 1, 10, 2));
  adw_preferences_window_add(ADW_PREFERENCES_WINDOW(win), p);

  /* Audio */
  p = page("Audio", "audio-speakers-symbolic");
  g = grp(p, "Output");
  adw_preferences_group_add(g, combo("Backend", (const char*[]){"PipeWire","PulseAudio", NULL}, 0));
  adw_preferences_group_add(g, spin("Target latency", "ms", 5, 100, 15));
  adw_preferences_group_add(g, spin("Master volume", "dB", -40, 0, -10));
  g = grp(p, "Processing");
  adw_preferences_group_add(g, spin("Digital master gain", NULL, 1, 32, 8));
  adw_preferences_window_add(ADW_PREFERENCES_WINDOW(win), p);

  /* Display */
  p = page("Display", "video-display-symbolic");
  g = grp(p, "Panadapter");
  adw_preferences_group_add(g, spin("Frame rate", "fps", 5, 60, 15));
  adw_preferences_group_add(g, spin("dB range top", NULL, -60, 0, -40));
  adw_preferences_group_add(g, spin("dB range bottom", NULL, -160, -60, -140));
  adw_preferences_group_add(g, sw("Trace averaging", NULL, TRUE));
  g = grp(p, "Appearance");
  adw_preferences_group_add(g, combo("Waterfall colormap", (const char*[]){"Classic","Viridis","Grayscale","Turbo", NULL}, 0));
  adw_preferences_group_add(g, combo("Theme", (const char*[]){"Follow system","Light","Dark", NULL}, 0));
  adw_preferences_window_add(ADW_PREFERENCES_WINDOW(win), p);

  /* DSP */
  p = page("DSP", "applications-engineering-symbolic");
  g = grp(p, "Defaults");
  adw_preferences_group_add(g, combo("AGC", (const char*[]){"Fast","Med","Slow","Long", NULL}, 1));
  adw_preferences_group_add(g, sw("Noise reduction (NR)", NULL, FALSE));
  adw_preferences_group_add(g, sw("Noise blanker (NB)", NULL, FALSE));
  adw_preferences_group_add(g, sw("Auto notch (ANF)", NULL, FALSE));
  adw_preferences_window_add(ADW_PREFERENCES_WINDOW(win), p);

  /* About */
  p = page("About", "help-about-symbolic");
  g = grp(p, NULL);
  GtkWidget *ar = g_object_new(ADW_TYPE_ACTION_ROW, "title", "SDR for Linux",
                               "subtitle", "v0 · GTK4 · WDSP · GPLv3 · OK1BR", NULL);
  adw_preferences_group_add(g, ar);
  adw_preferences_window_add(ADW_PREFERENCES_WINDOW(win), p);

  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  AdwApplication *app = adw_application_new("cz.ok1br.mock.prefs.adw", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  int s = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return s;
}
