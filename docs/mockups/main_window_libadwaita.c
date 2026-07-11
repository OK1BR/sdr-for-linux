/* Mockup B — libadwaita: AdwApplicationWindow + AdwHeaderBar + AdwToolbarView,
 * same control strip + spectrum. Shows the adwaita chrome/styling. */
#include <adwaita.h>
#include "mock_common.h"

static void activate(GtkApplication *app, gpointer d) {
  (void)d;
  mock_css();
  GtkWidget *win = adw_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(win), "SDR for Linux — ANAN G2E  [libadwaita]");
  gtk_window_set_default_size(GTK_WINDOW(win), 1320, 760);

  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), mock_vfo());
  GtkWidget *menu = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
  GtkWidget *rec = gtk_button_new_from_icon_name("media-record-symbolic");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), rec);
  GtkWidget *dot = gtk_label_new("●  ANAN G2E");
  gtk_widget_add_css_class(dot, "dim");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), dot);
  GtkWidget *badge = gtk_label_new("LIBADWAITA");
  gtk_widget_add_css_class(badge, "badge-adw");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), badge);

  GtkWidget *tv = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(v), mock_controls());
  gtk_box_append(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(v), mock_spectrum());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), v);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), tv);
  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  AdwApplication *app = adw_application_new("cz.ok1br.mock.adw", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  int s = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return s;
}
