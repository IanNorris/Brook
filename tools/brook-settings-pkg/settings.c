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

static GtkWidget *choice,*scale_choice,*preview,*status;
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
    rect(cr,0,0,w,h,p->launcher);rect(cr,24,24,w-48,h-92,p->borderActive);
    rect(cr,25,25,w-50,36,p->titleActive);label(cr,38,32,"Window preview",p->text,15);
    label(cr,w-118,32,"−   □   ×",p->text,16);
    rect(cr,25,61,w-50,h-130,selected==BROOK_THEME_DAWN?0xf5f7fa:0x172335);label(cr,44,84,"Brook desktop preview",selected==BROOK_THEME_DAWN?0x26374b:0xe5edf8,17);
    rect(cr,0,h-44,w,44,p->taskbar);
    for(int i=0;i<4;++i){rect(cr,10+i*44,h-38,34,32,i==1?p->taskActive:p->taskButton);rect(cr,19+i*44,h-30,16,16,p->borderActive);}
    rect(cr,56,h-9,30,2,p->borderActive);label(cr,w-75,h-32,"12:34",p->taskText,14);
    return FALSE;
}
static void changed(GtkComboBox *box,gpointer unused) {
    (void)unused;int active=gtk_combo_box_get_active(box);
    if(active>=0&&(unsigned)active<BROOK_THEME_COUNT)selected=(unsigned)active;
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

int main(int argc,char **argv) {
    const char *configured=g_getenv("BROOK_USER_SETTINGS");
    settings_path=g_strdup(configured&&*configured?configured:"/data/home/.config/brook/settings.ini");
    g_set_prgname("brook-settings");g_set_application_name("Settings");
    gtk_init(&argc,&argv);
    g_object_set(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",TRUE,NULL);
    GtkCssProvider *brook_css=gtk_css_provider_new();
    gtk_css_provider_load_from_data(brook_css,"window, dialog { background-color: #172335; color: #e5edf8; } headerbar, toolbar { background: #243c5d; color: #e5edf8; } button, combobox, entry { background: #243a56; color: #e5edf8; border-color: #3c5270; } button:hover { background: #355987; } treeview, textview, viewport { background-color: #1c314c; color: #e5edf8; }",-1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(brook_css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(brook_css);
    long active=syscall(0xB02L,0L,0L);selected=active>=0&&active<BROOK_THEME_COUNT?(unsigned)active:0;
    long scale=syscall(0xB04L,0L,0L);
    int scale_index=0;for(int i=0;i<3;++i)if(scale==(long)scales[i]){selected_scale=scales[i];scale_index=i;}
    GtkWidget *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"Settings");
    gtk_window_set_default_size(GTK_WINDOW(window),640,540);
    g_signal_connect(window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,16);gtk_container_set_border_width(GTK_CONTAINER(box),24);gtk_container_add(GTK_CONTAINER(window),box);
    GtkWidget *heading=gtk_label_new("Appearance");gtk_widget_set_halign(heading,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(box),heading,FALSE,FALSE,0);
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,16);gtk_box_pack_start(GTK_BOX(box),row,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(row),gtk_label_new("Desktop theme"),FALSE,FALSE,0);
    choice=gtk_combo_box_text_new();for(unsigned i=0;i<BROOK_THEME_COUNT;++i)gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(choice),brook_theme_name(i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(choice),(int)selected);gtk_box_pack_start(GTK_BOX(row),choice,TRUE,TRUE,0);
    GtkWidget *scale_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,16);gtk_box_pack_start(GTK_BOX(box),scale_row,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(scale_row),gtk_label_new("Desktop scale"),FALSE,FALSE,0);
    scale_choice=gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale_choice),"100%");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale_choice),"150%");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale_choice),"200%");
    gtk_combo_box_set_active(GTK_COMBO_BOX(scale_choice),scale_index);
    gtk_box_pack_start(GTK_BOX(scale_row),scale_choice,TRUE,TRUE,0);
    g_signal_connect(scale_choice,"changed",G_CALLBACK(scale_changed),NULL);
    preview=gtk_drawing_area_new();gtk_widget_set_size_request(preview,540,250);gtk_box_pack_start(GTK_BOX(box),preview,TRUE,TRUE,0);
    g_signal_connect(preview,"draw",G_CALLBACK(draw_preview),NULL);g_signal_connect(choice,"changed",G_CALLBACK(changed),NULL);
    GtkWidget *button=gtk_button_new_with_label("Apply appearance");gtk_widget_set_halign(button,GTK_ALIGN_END);gtk_box_pack_start(GTK_BOX(box),button,FALSE,FALSE,0);g_signal_connect(button,"clicked",G_CALLBACK(apply),NULL);
    status=gtk_label_new("Choose a theme and scale for the taskbar, launcher and window chrome.");gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);gtk_box_pack_start(GTK_BOX(box),status,FALSE,FALSE,0);
    gtk_widget_show_all(window);gtk_main();g_free(settings_path);return 0;
}
