/*
 * Startup radio picker — see picker.h.
 *
 * Discovery (vendored piHPSDR p2_discovery) is blocking, so it runs in a
 * worker thread; results land in the discovered[]/devices globals and the
 * list is rebuilt via g_idle_add. An empty ipaddr_radio broadcasts on every
 * up interface; a manual IP does a directed probe (radios beyond the local
 * broadcast domain).
 */
#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>
#include <arpa/inet.h>

#include "app_identity.h"
#include "discovered.h"
#include "discovery.h"
#include "picker.h"
#include "radio_support.h"

typedef struct {
  GMainLoop *loop;
  GtkWindow *win;
  GtkWidget *list;        /* GtkListBox of radio rows                        */
  GtkWidget *status;      /* "Searching…" / "N found" line                   */
  GtkWidget *spinner;
  GtkWidget *rescan_btn;
  GtkWidget *probe_btn;
  GtkWidget *ip_entry;    /* manual IP for a directed probe                  */
  char       last_ip[64]; /* preselect this row (previously used radio)      */
  char       chosen[64];
  int        picked;      /* 1 = a radio was chosen                          */
  int        discovering; /* worker thread in flight — buttons insensitive   */
} Picker;

static const char *dev_ip(const DISCOVERED *d) {
  return inet_ntoa(d->network.address.sin_addr);
}

/* ---- selection ------------------------------------------------------------ */

static void pick_row(Picker *p, GtkListBoxRow *row) {
  if (!row) { return; }
  const char *ip = g_object_get_data(G_OBJECT(row), "ip");
  if (!ip) { return; }
  g_strlcpy(p->chosen, ip, sizeof(p->chosen));
  p->picked = 1;
  gtk_window_destroy(p->win);   /* programmatic destroy does NOT emit close-request */
  g_main_loop_quit(p->loop);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer data) {
  (void)lb;
  pick_row((Picker *)data, row);
}

static void on_start_clicked(GtkButton *b, gpointer data) {
  (void)b;
  Picker *p = (Picker *)data;
  pick_row(p, gtk_list_box_get_selected_row(GTK_LIST_BOX(p->list)));
}

/* ---- list (re)build -------------------------------------------------------- */

static void set_busy(Picker *p, int busy, const char *msg) {
  p->discovering = busy;
  gtk_widget_set_visible(p->spinner, busy);
  gtk_widget_set_sensitive(p->rescan_btn, !busy);
  gtk_widget_set_sensitive(p->probe_btn, !busy);
  gtk_label_set_text(GTK_LABEL(p->status), msg);
}

static gboolean fill_list(gpointer data) {
  Picker *p = (Picker *)data;

  GtkListBoxRow *row;
  while ((row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->list), 0))) {
    gtk_list_box_remove(GTK_LIST_BOX(p->list), GTK_WIDGET(row));
  }

  int shown = 0;
  GtkListBoxRow *preselect = NULL;
  for (int i = 0; i < devices; i++) {
    const DISCOVERED *d = &discovered[i];
    char ip[64];                       /* COPY: dev_ip() = inet_ntoa = ONE static
                                          buffer — comparing its pointer against a
                                          later call compares it with itself, which
                                          marked every radio after the first as a
                                          duplicate (contest note #10) */
    g_strlcpy(ip, dev_ip(d), sizeof ip);
    int dup = 0;                       /* directed probes can re-find a radio */
    for (int j = 0; j < i && !dup; j++) { dup = strcmp(ip, dev_ip(&discovered[j])) == 0; }
    if (dup) { continue; }

    char name[64];
    g_strlcpy(name, d->name, sizeof(name));
    g_strstrip(name);
    char sub[160];
    const unsigned char *m = d->network.mac_address;
    snprintf(sub, sizeof(sub), "%s · %02X:%02X:%02X:%02X:%02X:%02X · fw %d.%d · %s",
             ip, m[0], m[1], m[2], m[3], m[4], m[5],
             d->software_version / 10, d->software_version % 10,
             d->protocol == NEW_PROTOCOL ? "Protocol 2" : "Protocol 1");

    /* ⛔ Whitelist: untested models are shown but cannot be started — their
     * Alex/PA bytes are unverified and could damage hardware (radio_support.h). */
    int ok = radio_supported(d);
    int rxonly = ok && !radio_tx_supported(d);   /* connectable, TX not brought up */
    GtkWidget *r = g_object_new(ADW_TYPE_ACTION_ROW, "title", name[0] ? name : "HPSDR radio",
                                "subtitle", sub, "activatable", ok ? TRUE : FALSE, NULL);
    GtkWidget *st = gtk_label_new(!ok ? "Not supported yet"
                                : d->status == STATE_SENDING   ? "In use"
                                : d->status != STATE_AVAILABLE ? "Incompatible"
                                : rxonly                       ? "RX only" : "Available");
    gtk_widget_add_css_class(st, !ok ? "error"
                               : d->status == STATE_SENDING   ? "warning"
                               : d->status != STATE_AVAILABLE ? "error"
                               : rxonly                       ? "accent" : "success");
    adw_action_row_add_suffix(ADW_ACTION_ROW(r), st);
    if (ok) { g_object_set_data_full(G_OBJECT(r), "ip", g_strdup(ip), g_free); }
    else    { gtk_widget_set_sensitive(r, FALSE); }   /* no "ip" data → pick_row no-op */
    gtk_list_box_append(GTK_LIST_BOX(p->list), r);
    shown++;
    if (ok && p->last_ip[0] && strcmp(ip, p->last_ip) == 0) { preselect = GTK_LIST_BOX_ROW(r); }
  }

  char msg[64];
  if (shown) { snprintf(msg, sizeof(msg), "%d radio%s found", shown, shown == 1 ? "" : "s"); }
  else       { snprintf(msg, sizeof(msg), "No radio found — check power and network"); }
  set_busy(p, 0, msg);

  /* Preselect the previously used radio (or the only one found) so a plain
   * Enter starts it. */
  if (!preselect && shown == 1) {
    preselect = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->list), 0);
  }
  if (preselect) {
    gtk_list_box_select_row(GTK_LIST_BOX(p->list), preselect);
    gtk_widget_grab_focus(GTK_WIDGET(preselect));
  }
  return G_SOURCE_REMOVE;
}

/* ---- discovery worker ------------------------------------------------------ */

static gpointer discover_thread(gpointer data) {
  Picker *p = (Picker *)data;
  p2_discovery();                    /* blocking ~2 s; fills discovered[]     */
  p1_discovery();                    /* METIS round (HL2 & co.); MAC-deduped  */
  g_idle_add(fill_list, p);
  return NULL;
}

static void start_discovery(Picker *p, const char *fixed_ip, int reset) {
  if (p->discovering) { return; }
  if (reset) { devices = 0; }
  snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s", fixed_ip ? fixed_ip : "");
  set_busy(p, 1, fixed_ip && fixed_ip[0] ? "Probing address…" : "Searching the LAN…");
  GThread *t = g_thread_new("sdrfl-picker-disc", discover_thread, p);
  g_thread_unref(t);
}

static void on_rescan(GtkButton *b, gpointer data) {
  (void)b;
  start_discovery((Picker *)data, "", 1);
}

static void on_probe(GtkButton *b, gpointer data) {
  (void)b;
  Picker *p = (Picker *)data;
  const char *ip = gtk_editable_get_text(GTK_EDITABLE(p->ip_entry));
  if (ip && *ip) { start_discovery(p, ip, 0); }
}

/* ---- window ---------------------------------------------------------------- */

static gboolean on_close(GtkWindow *w, gpointer data) {
  (void)w;
  Picker *p = (Picker *)data;
  g_main_loop_quit(p->loop);
  return FALSE;                      /* proceed with destroy */
}

int picker_run(const char *last_ip, char *ip, int ip_len) {
  if (!gtk_is_initialized()) { gtk_init(); }
  adw_init();                        /* Adwaita widgets + style pre-application */

  Picker p;
  memset(&p, 0, sizeof(p));
  p.loop = g_main_loop_new(NULL, FALSE);
  if (last_ip) { g_strlcpy(p.last_ip, last_ip, sizeof(p.last_ip)); }

  p.win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(p.win, "SDR for Linux — select radio");
  gtk_window_set_default_size(p.win, 480, 320);
  g_signal_connect(p.win, "close-request", G_CALLBACK(on_close), &p);

  /* The window's default CSD titlebar already shows the title — no own header. */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(box, 12);   gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);

  GtkWidget *srow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  p.spinner = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(p.spinner));
  p.status = gtk_label_new("");
  gtk_widget_add_css_class(p.status, "dim-label");
  gtk_box_append(GTK_BOX(srow), p.spinner);
  gtk_box_append(GTK_BOX(srow), p.status);
  gtk_box_append(GTK_BOX(box), srow);

  p.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(p.list), GTK_SELECTION_SINGLE);
  /* Single click only SELECTS; double-click / Enter / Start button starts —
   * a stray click must not launch a radio connection. */
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p.list), FALSE);
  gtk_widget_add_css_class(p.list, "boxed-list");
  g_signal_connect(p.list, "row-activated", G_CALLBACK(on_row_activated), &p);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p.list);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(box), scroll);

  /* Bottom bar: manual IP probe (left) + Rediscover / Start (right). */
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  p.ip_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(p.ip_entry), "IP address (other subnet)");
  gtk_widget_set_hexpand(p.ip_entry, TRUE);
  gtk_box_append(GTK_BOX(bar), p.ip_entry);
  p.probe_btn = gtk_button_new_with_label("Add by IP");
  g_signal_connect(p.probe_btn, "clicked", G_CALLBACK(on_probe), &p);
  g_signal_connect(p.ip_entry, "activate", G_CALLBACK(on_probe), &p);
  gtk_box_append(GTK_BOX(bar), p.probe_btn);
  p.rescan_btn = gtk_button_new_with_label("Rediscover");
  g_signal_connect(p.rescan_btn, "clicked", G_CALLBACK(on_rescan), &p);
  gtk_box_append(GTK_BOX(bar), p.rescan_btn);
  GtkWidget *start = gtk_button_new_with_label("Start");
  gtk_widget_add_css_class(start, "suggested-action");
  g_signal_connect(start, "clicked", G_CALLBACK(on_start_clicked), &p);
  gtk_box_append(GTK_BOX(bar), start);
  gtk_box_append(GTK_BOX(box), bar);

  gtk_window_set_child(p.win, box);
  gtk_widget_realize(GTK_WIDGET(p.win));   /* surface must exist for the id */
  sdrfl_claim_app_identity(p.win);
  gtk_window_present(p.win);

  start_discovery(&p, "", 1);        /* broadcast on all interfaces */
  g_main_loop_run(p.loop);
  g_main_loop_unref(p.loop);

  if (p.picked && ip && ip_len > 0) { g_strlcpy(ip, p.chosen, (gsize)ip_len); }
  return p.picked;
}
