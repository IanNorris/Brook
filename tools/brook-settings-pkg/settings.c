#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "brook_theme.h"
#include "brook_gtk_appearance.h"

static GtkWidget *scale_choice,*preview,*status,*swatches[3];
static GtkWidget *dark_switch,*opacity_slider,*opacity_value,*blur_choice,*performance;
static BrookGtkAppearance *appearance_style;
static uint32_t selected_appearance=BROOK_APPEARANCE_DEFAULT;
static char *settings_path;
static unsigned selected;
static unsigned selected_scale=100;
static const unsigned scales[]={100,150,200};
static void color(cairo_t *cr,uint32_t rgb) {
    cairo_set_source_rgb(cr,((rgb>>16)&255)/255.,((rgb>>8)&255)/255.,(rgb&255)/255.);
}
static void rect(cairo_t *cr,int x,int y,int w,int h,uint32_t rgb) {
    color(cr,rgb);cairo_rectangle(cr,x,y,w,h);cairo_fill(cr);
}
static void label(cairo_t *cr,int x,int y,const char *text,uint32_t rgb,int size) {
    PangoLayout *layout=pango_cairo_create_layout(cr);
    PangoFontDescription *font=pango_font_description_new();
    pango_font_description_set_family(font,"sans-serif");
    pango_font_description_set_absolute_size(font,size*PANGO_SCALE);
    pango_layout_set_font_description(layout,font);pango_layout_set_text(layout,text,-1);
    color(cr,rgb);cairo_move_to(cr,x,y);pango_cairo_show_layout(cr,layout);
    pango_font_description_free(font);g_object_unref(layout);
}
static gboolean draw_preview(GtkWidget *widget,cairo_t *cr,gpointer unused) {
    (void)unused;const BrookThemePalette *p=brook_theme_palette(selected);
    int w=gtk_widget_get_allocated_width(widget),h=gtk_widget_get_allocated_height(widget);
    cairo_pattern_t *bg=cairo_pattern_create_linear(0,0,w,h);
    cairo_pattern_add_color_stop_rgb(bg,0,.55,.66,.82);
    cairo_pattern_add_color_stop_rgb(bg,.5,.70,.83,.96);
    cairo_pattern_add_color_stop_rgb(bg,1,.36,.50,.69);
    cairo_set_source(cr,bg);cairo_paint(cr);cairo_pattern_destroy(bg);
    int x=26,y=15,ww=w-52;
    rect(cr,x,y,ww,1,p->borderActive);
    rect(cr,x,y,1,h-y,p->borderActive);rect(cr,x+ww-1,y,1,h-y,p->borderActive);
    const int perf=(selected_appearance&BROOK_APPEARANCE_PERFORMANCE)!=0;
    const double alpha=perf?1.0:brook_appearance_opacity(selected_appearance)/100.;
    cairo_set_source_rgba(cr,((p->titleActive>>16)&255)/255.,((p->titleActive>>8)&255)/255.,(p->titleActive&255)/255.,alpha);
    cairo_rectangle(cr,x+1,y+3,ww-2,21);cairo_fill(cr);
    label(cr,x+9,y+6,"•  Window preview",p->text,9);
    label(cr,x+ww-43,y+6,"−  □  ×",p->text,9);
    rect(cr,x+1,y+24,ww-2,h-y-24,brook_surface_palette(selected_appearance&BROOK_APPEARANCE_DARK)->content);
    for(int i=0;i<3;++i)rect(cr,x+9+i*34,y+31,27,17,p->taskButton);
    return FALSE;
}
static gboolean draw_swatch(GtkWidget *widget,cairo_t *cr,gpointer data) {
    unsigned id=GPOINTER_TO_UINT(data);const BrookThemePalette *p=brook_theme_palette(id);
    int w=gtk_widget_get_allocated_width(widget),h=gtk_widget_get_allocated_height(widget);
    cairo_pattern_t *gradient=cairo_pattern_create_linear(0,0,w,h);
    uint32_t a=p->titleActive,b=id==BROOK_THEME_DAWN?0xEFF5FC:id==BROOK_THEME_MIDNIGHT?0x7087A8:0x9DC3EF;
    cairo_pattern_add_color_stop_rgb(gradient,0,((a>>16)&255)/255.,((a>>8)&255)/255.,(a&255)/255.);
    cairo_pattern_add_color_stop_rgb(gradient,1,((b>>16)&255)/255.,((b>>8)&255)/255.,(b&255)/255.);
    cairo_set_source(cr,gradient);cairo_paint(cr);cairo_pattern_destroy(gradient);return FALSE;
}
static void select_theme(GtkButton *button,gpointer data) {
    (void)button;selected=GPOINTER_TO_UINT(data);
    for(unsigned i=0;i<BROOK_THEME_COUNT;++i) {
        GtkStyleContext *context=gtk_widget_get_style_context(swatches[i]);
        if(i==selected)gtk_style_context_add_class(context,"selected");
        else gtk_style_context_remove_class(context,"selected");
    }
    gtk_widget_queue_draw(preview);
}
static int save_appearance(unsigned theme,unsigned scale,uint32_t appearance,GError **error) {
    char *directory=g_path_get_dirname(settings_path);
    if(g_mkdir_with_parents(directory,0700)<0) {
        g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),"Cannot create settings directory: %s",strerror(errno));g_free(directory);return 0;
    }
    g_free(directory);
    GKeyFile *settings=g_key_file_new();
    /* Retain settings owned by other panels; do not replace malformed files. */
    GError *load_error=NULL;
    if(!g_key_file_load_from_file(settings,settings_path,G_KEY_FILE_KEEP_COMMENTS,&load_error)&&
       !g_error_matches(load_error,G_FILE_ERROR,G_FILE_ERROR_NOENT)) {
        g_propagate_error(error,load_error);g_key_file_unref(settings);return 0;
    }
    g_clear_error(&load_error);g_key_file_set_integer(settings,"Desktop","theme",(int)theme);
    g_key_file_set_integer(settings,"Desktop","scale",(int)scale);
    g_key_file_set_integer(settings,"Desktop","dark",(appearance&BROOK_APPEARANCE_DARK)!=0);
    g_key_file_set_integer(settings,"Desktop","performance",(appearance&BROOK_APPEARANCE_PERFORMANCE)!=0);
    g_key_file_set_integer(settings,"Desktop","opacity",brook_appearance_opacity(appearance));
    g_key_file_set_integer(settings,"Desktop","blur",brook_appearance_blur(appearance));
    gsize length=0;char *contents=g_key_file_to_data(settings,&length,error);
    gboolean ok=contents&&g_file_set_contents_full(settings_path,contents,(gssize)length,
                    G_FILE_SET_CONTENTS_CONSISTENT|G_FILE_SET_CONTENTS_DURABLE,0600,error);
    g_free(contents);g_key_file_unref(settings);return ok;
}
static void scale_changed(GtkComboBox *box,gpointer unused) {
    (void)unused;int index=gtk_combo_box_get_active(box);
    if(index>=0&&index<3)selected_scale=scales[index];
    gtk_widget_queue_draw(preview);
}
static void controls_changed(GtkWidget *widget,gpointer unused){
    (void)widget;(void)unused;
    selected_appearance=brook_appearance_pack(gtk_switch_get_active(GTK_SWITCH(dark_switch)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(performance)),
        (unsigned)(gtk_range_get_value(GTK_RANGE(opacity_slider))+.5),
        (unsigned)gtk_combo_box_get_active(GTK_COMBO_BOX(blur_choice)));
    char value[16];snprintf(value,sizeof(value),"%u%%",brook_appearance_opacity(selected_appearance));
    gtk_label_set_text(GTK_LABEL(opacity_value),value);
    const int enabled=!(selected_appearance&BROOK_APPEARANCE_PERFORMANCE);
    gtk_widget_set_sensitive(opacity_slider,enabled);gtk_widget_set_sensitive(blur_choice,enabled);
    brook_gtk_appearance_mode(appearance_style,(selected_appearance&BROOK_APPEARANCE_DARK)!=0);
    gtk_widget_queue_draw(preview);
}
static void dark_changed(GObject *object,GParamSpec *property,gpointer unused){
    (void)object;(void)property;controls_changed(NULL,unused);
}
static int saved_matches_selection(void){
    GKeyFile *file=g_key_file_new();int matches=g_key_file_load_from_file(file,settings_path,0,NULL);
    const char* names[]={"theme","scale","dark","performance","opacity","blur"};
    const int values[]={(int)selected,(int)selected_scale,(selected_appearance&1)!=0,(selected_appearance&2)!=0,
        (int)brook_appearance_opacity(selected_appearance),(int)brook_appearance_blur(selected_appearance)};
    for(unsigned i=0;matches && i<6;++i){GError *error=NULL;int value=g_key_file_get_integer(file,"Desktop",names[i],&error);
        matches=!error && value==values[i];g_clear_error(&error);}
    g_key_file_unref(file);return matches;
}
static void apply(GtkButton *button,gpointer unused) {
    (void)button;(void)unused;
    long previous=syscall(0xB02L,0L,0L),previous_scale=syscall(0xB04L,0L,0L);
    long previous_appearance=syscall(BROOK_SYS_WM_APPEARANCE,0L,0L);
    if(previous<0||previous_scale<0||previous_appearance<0){gtk_label_set_text(GTK_LABEL(status),"Appearance controls need a matching Brook kernel.");return;}
    int applied=syscall(0xB02L,1L,(long)selected)==0 && syscall(0xB04L,1L,(long)selected_scale)==0 &&
        syscall(BROOK_SYS_WM_APPEARANCE,1L,(long)selected_appearance)==0;
    GError *error=NULL;
    if(applied && save_appearance(selected,selected_scale,selected_appearance,&error)) {
        gtk_label_set_text(GTK_LABEL(status),"Appearance applied and saved.");
        printf("BROOK_SETTINGS_SAVED theme=%u scale=%u appearance=%u\n",selected,selected_scale,selected_appearance);fflush(stdout);return;
    }
    if(applied && saved_matches_selection()) {
        gtk_label_set_text(GTK_LABEL(status),"Appearance applied; file updated, but save durability is unconfirmed.");g_clear_error(&error);return;
    }
    int restored=syscall(0xB02L,1L,previous)==0;
    restored=(syscall(0xB04L,1L,previous_scale)==0)&&restored;
    restored=(syscall(BROOK_SYS_WM_APPEARANCE,1L,previous_appearance)==0)&&restored;
    char *message=g_strdup_printf(restored?"Could not save; previous settings restored: %s":"Could not save or fully restore settings: %s",error?error->message:"apply failed");
    gtk_label_set_text(GTK_LABEL(status),message);g_free(message);g_clear_error(&error);
}

static void first_paint(GdkFrameClock *clock,gpointer data) {
    GtkWindow *window=GTK_WINDOW(data);
    // The first configure has chosen the compact inspector width. Subsequent
    // resizing and maximize retain only a usable minimum size.
    GdkGeometry geometry={0};geometry.min_width=380;geometry.min_height=400;
    gtk_window_set_geometry_hints(window,NULL,&geometry,GDK_HINT_MIN_SIZE);
    g_signal_handlers_disconnect_by_func(clock,G_CALLBACK(first_paint),window);
}
static void realized(GtkWidget *widget,gpointer unused) {
    (void)unused;GdkFrameClock *clock=gtk_widget_get_frame_clock(widget);
    if(clock)g_signal_connect_object(clock,"after-paint",G_CALLBACK(first_paint),widget,0);
}

int main(int argc,char **argv) {
    const char *configured=g_getenv("BROOK_USER_SETTINGS");
    settings_path=g_strdup(configured&&*configured?configured:"/data/home/.config/brook/settings.ini");
    g_set_prgname("brook-settings");g_set_application_name("Settings");
    gtk_init(&argc,&argv);
    g_object_set(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",TRUE,NULL);
    GtkCssProvider *brook_css=gtk_css_provider_new();
    gtk_css_provider_load_from_data(brook_css,
        "window { background-color: #172335; color: #e5edf8; }"
        "label { color: #e5edf8; } .eyebrow { color: #9baec5; font-size: 10px; font-weight: bold; }"
        ".heading { font-size: 27px; font-weight: 500; } .muted { color: #a4b5ca; font-size: 11px; }"
        "button, combobox { background: #243a56; color: #e5edf8; border: 1px solid #3c5270; border-radius: 4px; }"
        "button:hover { background: #355987; } button.swatch { background: transparent; border: 1px solid transparent; padding: 3px; box-shadow: none; }"
        "button.swatch.selected { border-color: #68acff; } button.swatch label { font-size: 10px; }"
        "button.primary { background: #326bb2; border-color: #5b8dc7; color: white; padding: 8px 14px; }"
        "separator { background: #3c5270; min-height: 1px; }",-1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(brook_css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(brook_css);
    selected_appearance=brook_appearance_read();appearance_style=brook_gtk_appearance_new(0);
    long active=syscall(0xB02L,0L,0L);selected=active>=0&&active<BROOK_THEME_COUNT?(unsigned)active:0;
    long scale=syscall(0xB04L,0L,0L);
    int scale_index=0;for(int i=0;i<3;++i)if(scale==(long)scales[i]){selected_scale=scales[i];scale_index=i;}
    GtkWidget *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"Settings");
    GdkRectangle area={0,0,1920,800};
    GdkMonitor *monitor=gdk_display_get_primary_monitor(gdk_display_get_default());
    if(monitor)gdk_monitor_get_workarea(monitor,&area);
    int initial_height=area.height-120;if(initial_height>780)initial_height=780;if(initial_height<400)initial_height=400;
    gtk_window_set_default_size(GTK_WINDOW(window),440,initial_height);
    /* A compact inspector: truthful geometry bounds let the compositor choose
     * its initial width while retaining bounded defaults for other apps. */
    GdkGeometry geometry={0};geometry.min_width=380;geometry.max_width=440;
    geometry.min_height=initial_height;geometry.max_height=initial_height;
    gtk_window_set_geometry_hints(GTK_WINDOW(window),NULL,&geometry,GDK_HINT_MIN_SIZE|GDK_HINT_MAX_SIZE);
    g_signal_connect(window,"realize",G_CALLBACK(realized),NULL);
    g_signal_connect(window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,12);gtk_container_set_border_width(GTK_CONTAINER(box),26);GtkWidget *outer=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_container_add(GTK_CONTAINER(window),outer);
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(outer),scroll,TRUE,TRUE,0);gtk_container_add(GTK_CONTAINER(scroll),box);
    GtkWidget *eyebrow=gtk_label_new("APPEARANCE");gtk_widget_set_halign(eyebrow,GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(eyebrow),"eyebrow");gtk_box_pack_start(GTK_BOX(box),eyebrow,FALSE,FALSE,0);
    GtkWidget *heading=gtk_label_new("Choose your\nmaterial.");gtk_label_set_justify(GTK_LABEL(heading),GTK_JUSTIFY_LEFT);gtk_widget_set_halign(heading,GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading),"heading");gtk_box_pack_start(GTK_BOX(box),heading,FALSE,FALSE,0);
    GtkWidget *helper=gtk_label_new("Preview the frame, launcher and navigation.");gtk_widget_set_halign(helper,GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(helper),"muted");gtk_box_pack_start(GTK_BOX(box),helper,FALSE,FALSE,0);
    preview=gtk_drawing_area_new();gtk_widget_set_size_request(preview,-1,77);gtk_box_pack_start(GTK_BOX(box),preview,FALSE,FALSE,0);
    g_signal_connect(preview,"draw",G_CALLBACK(draw_preview),NULL);
    GtkWidget *swatch_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);gtk_box_pack_start(GTK_BOX(box),swatch_row,FALSE,FALSE,0);
    const unsigned order[]={0,2,1};
    for(unsigned i=0;i<3;++i) {
        unsigned id=order[i];GtkWidget *button=gtk_button_new();swatches[id]=button;
        gtk_style_context_add_class(gtk_widget_get_style_context(button),"swatch");
        if(id==selected)gtk_style_context_add_class(gtk_widget_get_style_context(button),"selected");
        GtkWidget *contents=gtk_box_new(GTK_ORIENTATION_VERTICAL,5);gtk_container_add(GTK_CONTAINER(button),contents);
        GtkWidget *sample=gtk_drawing_area_new();gtk_widget_set_size_request(sample,-1,24);gtk_box_pack_start(GTK_BOX(contents),sample,FALSE,FALSE,0);
        g_signal_connect(sample,"draw",G_CALLBACK(draw_swatch),GUINT_TO_POINTER(id));
        gtk_box_pack_start(GTK_BOX(contents),gtk_label_new(brook_theme_name(id)),FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(swatch_row),button,TRUE,TRUE,0);g_signal_connect(button,"clicked",G_CALLBACK(select_theme),GUINT_TO_POINTER(id));
    }
    GtkWidget *mode_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),mode_row,FALSE,FALSE,0);
    GtkWidget *mode_label=gtk_label_new("Dark mode");gtk_widget_set_halign(mode_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(mode_row),mode_label,TRUE,TRUE,0);
    dark_switch=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(dark_switch),(selected_appearance&BROOK_APPEARANCE_DARK)!=0);gtk_box_pack_end(GTK_BOX(mode_row),dark_switch,FALSE,FALSE,0);
    GtkWidget *opacity_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),opacity_row,FALSE,FALSE,0);
    GtkWidget *opacity_label=gtk_label_new("Glass opacity");gtk_widget_set_halign(opacity_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(opacity_row),opacity_label,TRUE,TRUE,0);
    opacity_value=gtk_label_new("");gtk_box_pack_end(GTK_BOX(opacity_row),opacity_value,FALSE,FALSE,0);
    opacity_slider=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,40,100,1);gtk_scale_set_draw_value(GTK_SCALE(opacity_slider),FALSE);
    gtk_range_set_value(GTK_RANGE(opacity_slider),brook_appearance_opacity(selected_appearance));gtk_box_pack_start(GTK_BOX(box),opacity_slider,FALSE,FALSE,0);
    gtk_widget_set_tooltip_text(opacity_slider,"Frame, taskbar and launcher fills. App content stays opaque.");
    GtkWidget *blur_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),blur_row,FALSE,FALSE,0);
    GtkWidget *blur_label=gtk_label_new("Backdrop blur");gtk_widget_set_halign(blur_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(blur_row),blur_label,TRUE,TRUE,0);
    blur_choice=gtk_combo_box_text_new();const char *levels[]={"None","Light","Moderate","Heavy"};
    for(unsigned i=0;i<4;++i)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(blur_choice),levels[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(blur_choice),brook_appearance_blur(selected_appearance));gtk_box_pack_end(GTK_BOX(blur_row),blur_choice,FALSE,FALSE,0);
    performance=gtk_check_button_new_with_label("Performance mode");gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(performance),(selected_appearance&BROOK_APPEARANCE_PERFORMANCE)!=0);
    gtk_widget_set_tooltip_text(performance,"Flat opaque frames, without blur or shadows. Your glass settings are remembered.");gtk_box_pack_start(GTK_BOX(box),performance,FALSE,FALSE,0);
    g_signal_connect(dark_switch,"notify::active",G_CALLBACK(dark_changed),NULL);
    g_signal_connect(opacity_slider,"value-changed",G_CALLBACK(controls_changed),NULL);
    g_signal_connect(blur_choice,"changed",G_CALLBACK(controls_changed),NULL);
    g_signal_connect(performance,"toggled",G_CALLBACK(controls_changed),NULL);
    GtkWidget *scale_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,16);gtk_box_pack_start(GTK_BOX(box),scale_row,FALSE,FALSE,4);
    GtkWidget *scale_label=gtk_label_new("Desktop scale");gtk_widget_set_halign(scale_label,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(scale_row),scale_label,TRUE,TRUE,0);
    scale_choice=gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale_choice),"100%");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale_choice),"150%");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale_choice),"200%");
    gtk_combo_box_set_active(GTK_COMBO_BOX(scale_choice),scale_index);
    gtk_box_pack_end(GTK_BOX(scale_row),scale_choice,FALSE,FALSE,0);
    g_signal_connect(scale_choice,"changed",G_CALLBACK(scale_changed),NULL);

    gtk_box_pack_start(GTK_BOX(outer),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,0);
    GtkWidget *footer=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_container_set_border_width(GTK_CONTAINER(footer),20);gtk_box_pack_start(GTK_BOX(outer),footer,FALSE,FALSE,0);
    status=gtk_label_new("Choose a theme and scale.");gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);gtk_label_set_xalign(GTK_LABEL(status),0);
    gtk_style_context_add_class(gtk_widget_get_style_context(status),"muted");gtk_box_pack_start(GTK_BOX(footer),status,TRUE,TRUE,0);
    GtkWidget *button=gtk_button_new_with_label("Apply appearance");gtk_style_context_add_class(gtk_widget_get_style_context(button),"primary");
    gtk_box_pack_end(GTK_BOX(footer),button,FALSE,FALSE,0);g_signal_connect(button,"clicked",G_CALLBACK(apply),NULL);
    controls_changed(NULL,NULL);gtk_widget_show_all(window);gtk_main();brook_gtk_appearance_free(appearance_style);g_free(settings_path);return 0;
}
