/* app_identity.h — stamp pre-application windows with the app id.
 *
 * Windows created before the AdwApplication exists (the radio picker and
 * the first-run wisdom progress) have no GtkApplication to carry the app
 * id for them, and the shell associates a window with our .desktop entry
 * (and its icon) exactly by that id.  On X11 the id arrives via WM_CLASS
 * from g_set_prgname() in main(), but GTK's Wayland backend as shipped in
 * Ubuntu 24.04 (4.14) ignores the program name and falls back to the
 * literal app id "GTK Application" — the shell then shows a generic gear.
 * Stamping the Wayland app id explicitly is version-independent and a
 * no-op on X11.  Call AFTER gtk_widget_realize()/present (needs surface).
 */
#pragma once
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#define SDRFL_APP_ID "cz.ok1br.sdr_for_linux"

static inline void sdrfl_claim_app_identity(GtkWindow *win) {
#ifdef GDK_WINDOWING_WAYLAND
  GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(win));
  if (s != NULL && GDK_IS_WAYLAND_TOPLEVEL(s)) {
    gdk_wayland_toplevel_set_application_id(GDK_WAYLAND_TOPLEVEL(s),
                                            SDRFL_APP_ID);
  }
#else
  (void)win;
#endif
}
