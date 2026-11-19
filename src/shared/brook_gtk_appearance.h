#ifndef BROOK_GTK_APPEARANCE_H
#define BROOK_GTK_APPEARANCE_H
#include <gtk/gtk.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "brook_appearance.h"
/* GTK apps follow the desktop's app content colours (Clearwater stage 2.3):
 * the hue and the content mode in use (Mixed glass has light apps). They are
 * written as @define-color lines (brook_bg, brook_raised, brook_sunken,
 * brook_line, brook_ink, brook_dim, brook_accent, brook_accent_hover,
 * brook_on_accent, brook_selection) followed by the rules that use them, and
 * reloaded when the theme changes. */
typedef struct BrookGtkAppearance {
    GtkCssProvider *provider;
    int dark;
    guint timer;
    long selection;              /* 0xB02 op 2, or negative on an older kernel */
    BrookSurfacePalette palette; /* applied */
    int applied;
} BrookGtkAppearance;
static inline uint32_t brook_appearance_read(void){
    long value=syscall(BROOK_SYS_WM_APPEARANCE,0L,0L);
    return value>=0 && value<=UINT32_MAX && brook_appearance_valid((uint32_t)value)?(uint32_t)value:BROOK_APPEARANCE_DEFAULT;
}
/* The content colours in use, from the kernel (0xB02 op 5); the Blue set for
 * the mode on an older kernel. */
static inline BrookSurfacePalette brook_content_palette_read(int dark){
    BrookSurfacePalette p=*brook_surface_palette(dark);
    unsigned i;
    for(i=0;i<BROOK_CONTENT_COUNT;++i){
        long value=syscall(0xB02L,(long)BROOK_THEME_OP_CONTENT,(long)i);
        if(value<0 || value>0xffffff)return *brook_surface_palette(dark);
        *brook_surface_palette_field(&p,i)=(uint32_t)value;
    }
    return p;
}
static inline void brook_gtk_appearance_apply(BrookGtkAppearance *a,const BrookSurfacePalette *p,int dark){
    if(a->applied && a->dark==dark && !memcmp(&a->palette,p,sizeof(*p)))return;
    a->dark=dark;a->palette=*p;a->applied=1;
    g_object_set(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",dark?TRUE:FALSE,NULL);
    char *css=g_strdup_printf(
        "@define-color brook_bg #%06x; @define-color brook_raised #%06x; @define-color brook_sunken #%06x;"
        "@define-color brook_line #%06x; @define-color brook_ink #%06x; @define-color brook_dim #%06x;"
        "@define-color brook_accent #%06x; @define-color brook_accent_hover #%06x;"
        "@define-color brook_on_accent #%06x; @define-color brook_selection #%06x;"
        "window, dialog { background-color: @brook_bg; color: @brook_ink; }"
        "label, entry, textview, treeview { color: @brook_ink; }"
        ".heading { color: @brook_ink; } .muted, .eyebrow { color: @brook_dim; }"
        "headerbar, toolbar, button, combobox, entry { background: @brook_raised; color: @brook_ink; border-color: @brook_line; }"
        "treeview, textview, viewport { background-color: @brook_bg; color: @brook_ink; }"
        "button:hover, treeview:selected { background: @brook_selection; }"
        "button.swatch { background: transparent; } button.swatch label { color: @brook_ink; }"
        "button:checked { background: @brook_selection; } button:checked, button.swatch.selected { border-color: @brook_accent; }"
        "button:disabled label, label:disabled { color: alpha(@brook_dim, 0.6); }"
        "button.primary, switch:checked { background: @brook_accent; color: @brook_on_accent; }"
        "button.primary label { color: @brook_on_accent; } button.primary:hover { background: @brook_accent_hover; }"
        "scale highlight, progressbar progress { background: @brook_accent; }"
        "separator { background: @brook_line; }",
        p->content,p->raised,p->sunken,p->line,p->ink,p->muted,p->accent,p->accentHover,p->onAccent,p->selection);
    gtk_css_provider_load_from_data(a->provider,css,-1,NULL);g_free(css);
}
/* Follow the desktop: the content mode from the appearance word, the colours
 * from the kernel. */
static inline void brook_gtk_appearance_mode(BrookGtkAppearance *a,int dark){
    const BrookSurfacePalette p=brook_content_palette_read(dark);
    brook_gtk_appearance_apply(a,&p,dark);
}
static gboolean brook_gtk_appearance_tick(gpointer data){
    BrookGtkAppearance *a=data;
    const int dark=(brook_appearance_read()&BROOK_APPEARANCE_DARK)!=0;
    const long selection=syscall(0xB02L,2L,0L);
    if(dark!=a->dark || selection!=a->selection){a->selection=selection;brook_gtk_appearance_mode(a,dark);}
    return G_SOURCE_CONTINUE;
}
static inline BrookGtkAppearance* brook_gtk_appearance_new(int watch){
    BrookGtkAppearance *a=g_new0(BrookGtkAppearance,1);a->dark=-1;a->provider=gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(a->provider),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION+1);
    a->selection=syscall(0xB02L,2L,0L);
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
