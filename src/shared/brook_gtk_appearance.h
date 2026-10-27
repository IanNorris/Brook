#ifndef BROOK_GTK_APPEARANCE_H
#define BROOK_GTK_APPEARANCE_H
#include <gtk/gtk.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "brook_appearance.h"
typedef struct BrookGtkAppearance {GtkCssProvider *provider;int dark;guint timer;} BrookGtkAppearance;
static inline uint32_t brook_appearance_read(void){
    long value=syscall(BROOK_SYS_WM_APPEARANCE,0L,0L);
    return value>=0 && value<=UINT32_MAX && brook_appearance_valid((uint32_t)value)?(uint32_t)value:BROOK_APPEARANCE_DEFAULT;
}
static inline void brook_gtk_appearance_mode(BrookGtkAppearance *a,int dark){
    if(a->dark==dark)return;
    a->dark=dark;
    const BrookSurfacePalette *p=brook_surface_palette(dark);
    g_object_set(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",dark?TRUE:FALSE,NULL);
    char *css=g_strdup_printf(
        "window, dialog { background-color: #%06x; color: #%06x; }"
        "label, entry, textview, treeview { color: #%06x; }"
        ".heading { color: #%06x; } .muted, .eyebrow { color: #%06x; }"
        "headerbar, toolbar, button, combobox, entry { background: #%06x; color: #%06x; border-color: #%06x; }"
        "treeview, textview, viewport { background-color: #%06x; color: #%06x; }"
        "button:hover, treeview:selected { background: #%06x; }"
        "button.swatch { background: transparent; } button.swatch label { color: #%06x; }"
        "button.primary, switch:checked { background: #326bb2; color: #ffffff; } button.primary label { color: #ffffff; }"
        "separator { background: #%06x; }",
        p->content,p->ink,p->ink,p->ink,p->muted,p->raised,p->ink,p->line,
        p->content,p->ink,p->selection,p->ink,p->line);
    gtk_css_provider_load_from_data(a->provider,css,-1,NULL);g_free(css);
}
static gboolean brook_gtk_appearance_tick(gpointer data){
    brook_gtk_appearance_mode(data,(brook_appearance_read()&BROOK_APPEARANCE_DARK)!=0);return G_SOURCE_CONTINUE;
}
static inline BrookGtkAppearance* brook_gtk_appearance_new(int watch){
    BrookGtkAppearance *a=g_new0(BrookGtkAppearance,1);a->dark=-1;a->provider=gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(a->provider),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION+1);
    brook_gtk_appearance_mode(a,(brook_appearance_read()&BROOK_APPEARANCE_DARK)!=0);
    if(watch)a->timer=g_timeout_add(500,brook_gtk_appearance_tick,a);
    return a;
}
static inline void brook_gtk_appearance_free(BrookGtkAppearance *a){
    if(a->timer)g_source_remove(a->timer);
    gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(a->provider));
    g_object_unref(a->provider);g_free(a);
}
#endif
