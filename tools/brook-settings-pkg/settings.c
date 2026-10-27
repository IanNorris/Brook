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

static GtkWidget *scale_choice,*preview,*status,*swatches[3];
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
    rect(cr,x,y,ww,h-y,p->borderActive);
    rect(cr,x+1,y+3,ww-2,21,p->titleActive);
    label(cr,x+9,y+6,"•  Window preview",p->text,9);
    label(cr,x+ww-43,y+6,"−  □  ×",p->text,9);
    rect(cr,x+1,y+24,ww-2,h-y-24,selected==BROOK_THEME_DAWN?0xf5f7fa:0x172335);
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
static int save_appearance(unsigned theme,unsigned scale,GError **error) {
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
static void apply(GtkButton *button,gpointer unused) {
    (void)button;(void)unused;
    long previous=syscall(0xB02L,0L,0L);
    long previous_scale=syscall(0xB04L,0L,0L);
    if(previous<0||previous_scale<0||syscall(0xB02L,1L,(long)selected)<0) {
        gtk_label_set_text(GTK_LABEL(status),"Could not apply appearance settings.");return;
    }
    if(syscall(0xB04L,1L,(long)selected_scale)<0) {
        (void)syscall(0xB02L,1L,previous);
        gtk_label_set_text(GTK_LABEL(status),"Could not apply desktop scale.");return;
    }
    GError *error=NULL;
    if(!save_appearance(selected,selected_scale,&error)) {
        (void)syscall(0xB02L,1L,previous);
        (void)syscall(0xB04L,1L,previous_scale);
        char *message=g_strdup_printf("Could not save settings: %s",error?error->message:"unknown error");
        gtk_label_set_text(GTK_LABEL(status),message);g_free(message);g_clear_error(&error);return;
    }
    gtk_label_set_text(GTK_LABEL(status),"Appearance applied and saved. App content keeps its current scale.");
    printf("BROOK_SETTINGS_SAVED theme=%u scale=%u path=%s\n",selected,selected_scale,settings_path);fflush(stdout);
}

static void first_paint(GdkFrameClock *clock,gpointer data) {
    GtkWindow *window=GTK_WINDOW(data);
    // The first configure has chosen the compact inspector width. Subsequent
    // resizing and maximize retain only a usable minimum size.
    GdkGeometry geometry={0};geometry.min_width=380;geometry.min_height=520;
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
    long active=syscall(0xB02L,0L,0L);selected=active>=0&&active<BROOK_THEME_COUNT?(unsigned)active:0;
    long scale=syscall(0xB04L,0L,0L);
    int scale_index=0;for(int i=0;i<3;++i)if(scale==(long)scales[i]){selected_scale=scales[i];scale_index=i;}
    GtkWidget *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"Settings");
    gtk_window_set_default_size(GTK_WINDOW(window),440,560);
    /* A compact inspector: truthful geometry bounds let the compositor choose
     * its initial width while retaining bounded defaults for other apps. */
    GdkGeometry geometry={0};geometry.min_width=380;geometry.max_width=440;
    geometry.min_height=520;geometry.max_height=560;
    gtk_window_set_geometry_hints(GTK_WINDOW(window),NULL,&geometry,GDK_HINT_MIN_SIZE|GDK_HINT_MAX_SIZE);
    g_signal_connect(window,"realize",G_CALLBACK(realized),NULL);
    g_signal_connect(window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,12);gtk_container_set_border_width(GTK_CONTAINER(box),26);gtk_container_add(GTK_CONTAINER(window),box);
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
    GtkWidget *spacer=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_box_pack_start(GTK_BOX(box),spacer,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(box),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,0);
    GtkWidget *footer=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),footer,FALSE,FALSE,0);
    status=gtk_label_new("Choose a theme and scale.");gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);gtk_label_set_xalign(GTK_LABEL(status),0);
    gtk_style_context_add_class(gtk_widget_get_style_context(status),"muted");gtk_box_pack_start(GTK_BOX(footer),status,TRUE,TRUE,0);
    GtkWidget *button=gtk_button_new_with_label("Apply appearance");gtk_style_context_add_class(gtk_widget_get_style_context(button),"primary");
    gtk_box_pack_end(GTK_BOX(footer),button,FALSE,FALSE,0);g_signal_connect(button,"clicked",G_CALLBACK(apply),NULL);
    gtk_widget_show_all(window);gtk_main();g_free(settings_path);return 0;
}
