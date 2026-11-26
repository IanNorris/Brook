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
#include "brook_desktop_scale.h"
#include "brook_audio_control.h"
#include "brook_gtk_appearance.h"
#include "brook_taskbar.h"
#include "brook_pointer.h"
#include "input_preferences.h"
#include "brook_user_experience.h"

static GtkWidget *scale_choice,*preview,*status;
static GtkWidget *volume_slider,*mute_check,*audio_status;

/* Sliders never take the mouse wheel: GtkScale's default handler changes the
 * value when a scroll merely passes over it, which stops the page scrolling
 * and can jump the master volume. Stop the scale's handler and return FALSE so
 * the event bubbles to the scrolled window. Drag, click and keys still work. */
static gboolean slider_pass_scroll(GtkWidget *w,GdkEvent *e,gpointer unused){
    (void)e;(void)unused;g_signal_stop_emission_by_name(w,"scroll-event");return FALSE;
}
static void slider_ignore_wheel(GtkWidget *slider){
    g_signal_connect(slider,"scroll-event",G_CALLBACK(slider_pass_scroll),NULL);
}
static GtkWidget *pointer_slider,*pointer_status,*tap_check;
static long pointer_speed=-1;
static GtkWidget *keyboard_choice, *cursor_choice;
static const unsigned cursor_sizes[] = {24, 32, 48, 64};
static long touchpad_tap=-1;
static uint32_t audio_control=BROOK_AUDIO_DEFAULT;
static int audio_supported;
static GtkWidget *opacity_slider,*opacity_value,*blur_choice,*accel_note;
static GtkWidget *material_buttons[BROOK_THEME_COUNT],*mode_buttons[BROOK_MODE_COUNT];
static GtkWidget *hue_buttons[BROOK_HUE_COUNT],*custom_picker,*custom_swatch;
static BrookGtkAppearance *appearance_style;
static uint32_t selected_appearance=BROOK_APPEARANCE_DEFAULT;
static uint64_t selected_theme=BROOK_THEME_SELECTION_DEFAULT; /* selection word */
static int theme_supported,glass_available=1,updating_controls;
static BrookHueTokens hue_tokens[BROOK_HUE_COUNT];
static int hue_derived[BROOK_HUE_COUNT];
static uint32_t selected_metrics=BROOK_TASKBAR_DEFAULT;
static GtkWidget *metric_choices[7];
static GtkWidget *pro_mode_check;
static int pro_mode_supported;
static const char pro_mode_label[]="Pro mode";
static const char pro_mode_tooltip[]="Show technical details where available, including system message diagnostics and detailed CPU and memory views.";
static char *settings_path;
static unsigned selected_scale=BROOK_DESKTOP_SCALE_MIN;
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
static const BrookHueTokens *tokens_for(unsigned hue){
    if(hue==BROOK_HUE_CUSTOM){
        static BrookHueTokens custom;
        static uint32_t rgb;
        static int ready;
        if(!ready || rgb!=brook_selection_rgb(selected_theme)){
            brook_hue_derive_selection(selected_theme,&custom);
            rgb=brook_selection_rgb(selected_theme);ready=1;
        }
        return &custom;
    }
    if(!hue_derived[hue]){brook_hue_derive(hue,&hue_tokens[hue]);hue_derived[hue]=1;}
    return &hue_tokens[hue];
}
static unsigned sel_theme(void){return brook_selection_theme(selected_theme);}
static unsigned sel_mode(void){return brook_selection_mode(selected_theme);}
static unsigned sel_hue(void){return brook_selection_hue(selected_theme);}
static const char *accent_key(uint64_t selection,char hex[8]){
    const unsigned hue=brook_selection_hue(selection);
    if(hue!=BROOK_HUE_CUSTOM)return brook_hue_key(hue);
    brook_custom_accent_key(brook_selection_rgb(selection),hex);
    return hex;
}
/* What the selection shows on this display: Glass, or its stand-in. */
static unsigned resolved_theme(void){return brook_theme_resolve(sel_theme(),glass_available);}
static void rgba(cairo_t *cr,uint32_t rgb,double alpha){
    cairo_set_source_rgba(cr,((rgb>>16)&255)/255.,((rgb>>8)&255)/255.,(rgb&255)/255.,alpha);
}
static gboolean draw_preview(GtkWidget *widget,cairo_t *cr,gpointer unused) {
    (void)unused;
    const unsigned resolved=resolved_theme(),mode=sel_mode();
    const BrookHueTokens *t=tokens_for(sel_hue());
    BrookThemePalette palette;brook_theme_build_palette(t,resolved,brook_theme_chrome_dark(mode),&palette);
    const BrookThemePalette *p=&palette;
    const BrookContentTokens *content=&t->content[brook_theme_content_dark(mode)?1:0];
    int w=gtk_widget_get_allocated_width(widget),h=gtk_widget_get_allocated_height(widget);
    cairo_pattern_t *bg=cairo_pattern_create_linear(0,0,w,h);
    cairo_pattern_add_color_stop_rgb(bg,0,.55,.66,.82);
    cairo_pattern_add_color_stop_rgb(bg,.5,.70,.83,.96);
    cairo_pattern_add_color_stop_rgb(bg,1,.36,.50,.69);
    cairo_set_source(cr,bg);cairo_paint(cr);cairo_pattern_destroy(bg);
    int x=26,y=15,ww=w-52;
    rect(cr,x,y,ww,1,p->borderActive);
    rect(cr,x,y,1,h-y,p->borderActive);rect(cr,x+ww-1,y,1,h-y,p->borderActive);
    const int glass=resolved==BROOK_THEME_GLASS;
    const double alpha=glass?brook_appearance_opacity(selected_appearance)/100.:1.0;
    rgba(cr,p->titleActive,alpha);cairo_rectangle(cr,x+1,y+1,ww-2,23);cairo_fill(cr);
    rect(cr,x+10,y+1,60,2,p->accent);
    label(cr,x+9,y+6,"•  Window preview",p->text,9);
    label(cr,x+ww-43,y+6,"−  □  ×",p->text,9);
    const int bar_h=8;
    rect(cr,x+1,y+24,ww-2,h-y-24-bar_h,content->background);
    for(int i=0;i<3;++i)rect(cr,x+9+i*34,y+31,27,17,i==0?content->accent:content->raised);
    label(cr,x+120,y+31,"Apps",content->ink,9);
    rgba(cr,p->taskbar,alpha);cairo_rectangle(cr,0,h-bar_h,w,bar_h);cairo_fill(cr);
    return FALSE;
}
static gboolean draw_hue(GtkWidget *widget,cairo_t *cr,gpointer data) {
    const unsigned hue=GPOINTER_TO_UINT(data);
    const BrookHueTokens *t=tokens_for(hue);
    const unsigned material=resolved_theme();
    const BrookChromeTokens *c=&t->chrome[brook_theme_chrome_dark(sel_mode())?1:0];
    const uint32_t tint=material==BROOK_THEME_GLASS?c->glass:c->flat;
    const int w=gtk_widget_get_allocated_width(widget),h=gtk_widget_get_allocated_height(widget);
    const double r=(w<h?w:h)/2.-1;
    rgba(cr,tint,1);cairo_arc(cr,w/2.,h/2.,r,0,6.2832);cairo_fill_preserve(cr);
    rgba(cr,c->ink,0.3);cairo_set_line_width(cr,1);cairo_stroke(cr);
    rgba(cr,t->swatch,1);cairo_arc(cr,w/2.,h/2.,r/2.,0,6.2832);cairo_fill(cr);
    return FALSE;
}
static void set_class(GtkWidget *widget,const char *name,int on){
    GtkStyleContext *context=gtk_widget_get_style_context(widget);
    if(on)gtk_style_context_add_class(context,name);else gtk_style_context_remove_class(context,name);
}
/* Selection, availability and the controls agree; the preview and the
 * app colours follow the selection as it would show on this display. */
static void refresh_appearance(void){
    const unsigned theme=sel_theme(),mode=sel_mode(),hue=sel_hue();
    updating_controls=1;
    for(unsigned i=0;i<BROOK_THEME_COUNT;++i){
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(material_buttons[i]),i==theme);
        gtk_widget_set_sensitive(material_buttons[i],theme_supported);
    }
    for(unsigned i=0;i<BROOK_MODE_COUNT;++i){
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mode_buttons[i]),i==mode);
        gtk_widget_set_sensitive(mode_buttons[i],theme_supported);
    }
    for(unsigned i=0;i<BROOK_HUE_COUNT;++i){
        set_class(hue_buttons[i],"selected",i==hue);
        gtk_widget_set_sensitive(hue_buttons[i],theme_supported);
        gtk_widget_queue_draw(hue_buttons[i]);
    }
    const uint32_t rgb=tokens_for(hue)->swatch;
    const GdkRGBA rgba_value={((rgb>>16)&255)/255.,((rgb>>8)&255)/255.,(rgb&255)/255.,1};
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(custom_picker),&rgba_value);
    gtk_widget_set_sensitive(custom_picker,theme_supported);
    set_class(custom_picker,"selected",hue==BROOK_HUE_CUSTOM);
    gtk_widget_set_visible(custom_swatch,hue==BROOK_HUE_CUSTOM);
    gtk_widget_queue_draw(custom_swatch);
    const int glass=theme_supported && theme==BROOK_THEME_GLASS;
    gtk_widget_set_sensitive(opacity_slider,glass);gtk_widget_set_sensitive(blur_choice,glass);
    gtk_label_set_text(GTK_LABEL(accel_note),"Opaque is in use until backdrop blur is available.");
    gtk_widget_set_visible(accel_note,glass && !glass_available);
    updating_controls=0;
    /* This window's own colours preview the selection's app content. */
    const BrookContentTokens *c=&tokens_for(hue)->content[brook_theme_content_dark(mode)?1:0];
    BrookSurfacePalette app={c->background,c->ink,c->dim,c->line,c->raised,brook_tint_over(c->selection,c->background),
                             c->sunken,c->accent,c->accentHover,c->onAccent};
    brook_gtk_appearance_apply(appearance_style,&app,brook_theme_content_dark(mode));
    gtk_widget_queue_draw(preview);
}
static void select_material(GtkToggleButton *button,gpointer data) {
    if(updating_controls || !gtk_toggle_button_get_active(button))return;
    selected_theme=brook_selection_make(GPOINTER_TO_UINT(data),sel_mode(),sel_hue(),brook_selection_rgb(selected_theme));
    refresh_appearance();
}
static void select_mode(GtkToggleButton *button,gpointer data) {
    if(updating_controls || !gtk_toggle_button_get_active(button))return;
    selected_theme=brook_selection_make(sel_theme(),GPOINTER_TO_UINT(data),sel_hue(),brook_selection_rgb(selected_theme));
    refresh_appearance();
}
static void select_custom(GtkColorButton *button,gpointer unused) {
    (void)unused;
    GdkRGBA value;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button),&value);
    const uint32_t rgb=(uint32_t)(value.red*255+0.5)<<16 |
                       (uint32_t)(value.green*255+0.5)<<8 |
                       (uint32_t)(value.blue*255+0.5);
    selected_theme=brook_selection_make(sel_theme(),sel_mode(),BROOK_HUE_CUSTOM,rgb);
    refresh_appearance();
}
static void select_hue(GtkButton *button,gpointer data) {
    (void)button;
    selected_theme=BROOK_THEME_SELECTION(sel_theme(),sel_mode(),GPOINTER_TO_UINT(data));refresh_appearance();
}
/* Acceleration can come and go (renderer start or exit, a GPU fault). */
static gboolean follow_status(gpointer unused){
    (void)unused;
    const long status=syscall(0xB02L,(long)BROOK_THEME_OP_STATUS,0L);
    const int available=status<0?1:(status&BROOK_THEME_STATUS_GLASS_AVAILABLE)!=0;
    if(available!=glass_available){glass_available=available;refresh_appearance();}
    return G_SOURCE_CONTINUE;
}
static int save_appearance(uint64_t theme,unsigned scale,uint32_t appearance,
                           gboolean pro_mode,GError **error) {
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
    g_clear_error(&load_error);
    g_key_file_set_string(settings,"Desktop","theme",brook_theme_key(brook_selection_theme(theme)));
    g_key_file_set_string(settings,"Desktop","mode",brook_mode_key(brook_selection_mode(theme)));
    char hex[8];
    g_key_file_set_string(settings,"Desktop","accent",accent_key(theme,hex));
    g_key_file_remove_key(settings,"Desktop","palette",NULL);
    g_key_file_set_integer(settings,"Desktop","scale",(int)scale);
    /* For an older kernel, which reads only these: dark apps, and an Opaque
     * theme as Performance mode. A Clearwater kernel prefers the names. */
    g_key_file_set_integer(settings,"Desktop","dark",brook_selection_mode(theme)==BROOK_MODE_DARK);
    g_key_file_set_integer(settings,"Desktop","performance",brook_selection_theme(theme)!=BROOK_THEME_GLASS);
    g_key_file_set_integer(settings,"Desktop","opacity",brook_appearance_opacity(appearance));
    g_key_file_set_integer(settings,"Desktop","blur",brook_appearance_blur(appearance));
    g_key_file_set_integer(settings,"Desktop","taskbar_metrics",selected_metrics);
    if(pointer_speed>0)g_key_file_set_integer(settings,"Desktop","pointer_speed",(int)pointer_speed);
    g_key_file_set_string(settings,"Desktop","keyboard_layout",
                          brook_keyboard_layout_name(gtk_combo_box_get_active(GTK_COMBO_BOX(keyboard_choice))));
    g_key_file_set_integer(settings,"Desktop","cursor_size",
                           cursor_sizes[gtk_combo_box_get_active(GTK_COMBO_BOX(cursor_choice))]);
    if(touchpad_tap>=0)g_key_file_set_integer(settings,"Desktop","touchpad_tap",(int)touchpad_tap);
    if(audio_supported)g_key_file_set_integer(settings,"Audio","output",audio_control);
    g_key_file_set_boolean(settings,BROOK_EXPERIENCE_GROUP,BROOK_EXPERIENCE_DETAILS_KEY,pro_mode);
    gsize length=0;char *contents=g_key_file_to_data(settings,&length,error);
    gboolean ok=contents&&g_file_set_contents_full(settings_path,contents,(gssize)length,
                    G_FILE_SET_CONTENTS_CONSISTENT|G_FILE_SET_CONTENTS_DURABLE,0600,error);
    g_free(contents);g_key_file_unref(settings);return ok;
}
static void scale_changed(GtkComboBox *box,gpointer unused) {
    (void)unused;int index=gtk_combo_box_get_active(box);
    if(index>=0 && (unsigned)index<BROOK_DESKTOP_SCALE_COUNT)
        selected_scale=BROOK_DESKTOP_SCALE_MIN + (unsigned)index*BROOK_DESKTOP_SCALE_STEP;
    gtk_widget_queue_draw(preview);
}
/* Opacity and blur; the dark and performance bits of the appearance word
 * are the kernel's views of the selection and are set on Apply. */
static void controls_changed(GtkWidget *widget,gpointer unused){
    (void)widget;(void)unused;
    selected_appearance=brook_appearance_pack(0,0,(unsigned)(gtk_range_get_value(GTK_RANGE(opacity_slider))+.5),
        (unsigned)gtk_combo_box_get_active(GTK_COMBO_BOX(blur_choice)));
    char value[16];snprintf(value,sizeof(value),"%u%%",brook_appearance_opacity(selected_appearance));
    gtk_label_set_text(GTK_LABEL(opacity_value),value);
    gtk_widget_queue_draw(preview);
}
static void audio_changed(GtkWidget *widget,gpointer unused){
    (void)widget;(void)unused;
    const uint32_t value=(unsigned)(gtk_range_get_value(GTK_RANGE(volume_slider))+.5)|
        (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mute_check))?BROOK_AUDIO_MUTED:0);
    if(syscall(BROOK_SYS_AUDIO_CONTROL,1L,(long)value)<0){
        gtk_label_set_text(GTK_LABEL(audio_status),"Could not change output level.");return;
    }
    audio_control=value;
    gtk_label_set_text(GTK_LABEL(audio_status),
        syscall(BROOK_SYS_AUDIO_CONTROL,2L,0L)>0?
        ((value&BROOK_AUDIO_MUTED)?"Muted. Apply settings to remember this level.":"Output level changed. Apply settings to remember it."):
        "No audio device available. Level is ready for a future device.");
}
/* Pointer speed applies live; Apply saves it with the other settings. */
static void pointer_changed(GtkWidget *widget,gpointer unused){
    (void)widget;(void)unused;
    const long value=(long)(gtk_range_get_value(GTK_RANGE(pointer_slider))+.5);
    if(syscall(BROOK_SYS_POINTER_SPEED,1L,value)<0){
        gtk_label_set_text(GTK_LABEL(pointer_status),"Could not change pointer speed.");return;
    }
    pointer_speed=value;
    gtk_label_set_text(GTK_LABEL(pointer_status),"Pointer speed changed. Apply settings to remember it.");
}
/* Tap to click applies live; Apply saves it with the other settings. */
static void tap_changed(GtkWidget *widget,gpointer unused){
    (void)widget;(void)unused;
    const long value=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tap_check))?1:0;
    if(syscall(BROOK_SYS_TOUCHPAD_TAP,1L,value)<0){
        gtk_label_set_text(GTK_LABEL(pointer_status),"Could not change tap to click.");return;
    }
    touchpad_tap=value;
    gtk_label_set_text(GTK_LABEL(pointer_status),value?
        "Tap to click on: tap for left click, two-finger tap for right click, tap then touch and move to drag. Apply settings to remember it.":
        "Tap to click off. Apply settings to remember it.");
}
/* The taskbar volume control changes the same kernel value; follow it while
 * open so Apply saves what the user last chose from either place. */
static gboolean audio_follow(gpointer unused){
    (void)unused;
    long output=syscall(BROOK_SYS_AUDIO_CONTROL,0L,0L);
    if(output<0 || !brook_audio_control_valid((uint32_t)output) || (uint32_t)output==audio_control)return G_SOURCE_CONTINUE;
    audio_control=(uint32_t)output;
    g_signal_handlers_block_by_func(volume_slider,G_CALLBACK(audio_changed),NULL);
    g_signal_handlers_block_by_func(mute_check,G_CALLBACK(audio_changed),NULL);
    gtk_range_set_value(GTK_RANGE(volume_slider),brook_audio_volume(audio_control));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mute_check),(audio_control&BROOK_AUDIO_MUTED)!=0);
    g_signal_handlers_unblock_by_func(mute_check,G_CALLBACK(audio_changed),NULL);
    g_signal_handlers_unblock_by_func(volume_slider,G_CALLBACK(audio_changed),NULL);
    return G_SOURCE_CONTINUE;
}
static int saved_matches_selection(void){
    GKeyFile *file=g_key_file_new();int matches=g_key_file_load_from_file(file,settings_path,0,NULL);
    const char* names[]={"scale","opacity","blur","taskbar_metrics"};
    const int values[]={(int)selected_scale,(int)brook_appearance_opacity(selected_appearance),
        (int)brook_appearance_blur(selected_appearance),(int)selected_metrics};
    for(unsigned i=0;matches && i<4;++i){GError *error=NULL;int value=g_key_file_get_integer(file,"Desktop",names[i],&error);
        matches=!error && value==values[i];g_clear_error(&error);}
    const char* keys[]={"theme","mode","accent"};
    char hex[8];
    const char* words[]={brook_theme_key(sel_theme()),brook_mode_key(sel_mode()),accent_key(selected_theme,hex)};
    for(unsigned i=0;matches && i<3;++i){char *value=g_key_file_get_string(file,"Desktop",keys[i],NULL);
        matches=value && !strcmp(value,words[i]);g_free(value);}
    GError *experience_error=NULL;
    gboolean experience=g_key_file_get_boolean(file,BROOK_EXPERIENCE_GROUP,BROOK_EXPERIENCE_DETAILS_KEY,&experience_error);
    matches=matches && !experience_error && experience==gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pro_mode_check));
    if(audio_supported){GError *audio_error=NULL;int value=g_key_file_get_integer(file,"Audio","output",&audio_error);
        matches=matches && !audio_error && value==(int)audio_control;g_clear_error(&audio_error);}
    g_clear_error(&experience_error);
    g_key_file_unref(file);return matches;
}
static void apply(GtkButton *button,gpointer unused) {
    (void)button;(void)unused;
    long previous=syscall(0xB02L,(long)BROOK_THEME_OP_MATERIAL_QUERY,0L),previous_scale=syscall(0xB04L,0L,0L);
    long previous_appearance=syscall(BROOK_SYS_WM_APPEARANCE,0L,0L);
    long previous_metrics=syscall(BROOK_SYS_WM_TASKBAR_METRICS,(long)BROOK_TASKBAR_OP_GET_METRICS,0L);
    long previous_pro_mode=pro_mode_supported?
        syscall(BROOK_SYS_WM_TASKBAR_METRICS,(long)BROOK_TASKBAR_OP_GET_PRO_MODE,0L):-1;
    const gboolean pro_mode=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pro_mode_check));
    selected_metrics=0;
    for(unsigned i=0;i<7;++i)if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(metric_choices[i])))selected_metrics|=1u<<i;
    if(previous<0||previous_scale<0||previous_appearance<0||previous_metrics<0||
       (pro_mode_supported && !brook_experience_pro_mode_valid((uint64_t)previous_pro_mode))){gtk_label_set_text(GTK_LABEL(status),"Appearance controls need a matching Brook kernel.");return;}
    int applied=syscall(0xB02L,(long)BROOK_THEME_OP_MATERIAL_SET,(long)selected_theme)==0 && syscall(0xB04L,1L,(long)selected_scale)==0;
    if(applied){
        /* Opacity and blur; the dark and performance bits as the kernel now
         * reports them for the new selection, so they change nothing. */
        const long now=syscall(BROOK_SYS_WM_APPEARANCE,0L,0L);
        const uint32_t views=now<0?0u:(uint32_t)now&(BROOK_APPEARANCE_DARK|BROOK_APPEARANCE_PERFORMANCE);
        applied=now>=0 && syscall(BROOK_SYS_WM_APPEARANCE,1L,(long)((selected_appearance&~3u)|views))==0 &&
                syscall(BROOK_SYS_WM_TASKBAR_METRICS,(long)BROOK_TASKBAR_OP_SET_METRICS,(long)selected_metrics)==0;
        if(applied && pro_mode_supported)
            applied=syscall(BROOK_SYS_WM_TASKBAR_METRICS,(long)BROOK_TASKBAR_OP_SET_PRO_MODE,(long)pro_mode)==0;
    }
    GError *error=NULL;
    if(applied && save_appearance(selected_theme,selected_scale,selected_appearance,pro_mode,&error)) {
        gtk_label_set_text(GTK_LABEL(status),"Settings saved. Keyboard layout and cursor size apply after restart.");
        printf("BROOK_SETTINGS_SAVED theme=%s mode=%s accent=%s scale=%u appearance=%u\n",brook_theme_key(sel_theme()),
               brook_mode_key(sel_mode()),brook_hue_key(sel_hue()),selected_scale,selected_appearance);fflush(stdout);return;
    }
    if(applied && saved_matches_selection()) {
        gtk_label_set_text(GTK_LABEL(status),"Appearance applied; file updated, but save durability is unconfirmed.");g_clear_error(&error);return;
    }
    int restored=syscall(0xB02L,(long)BROOK_THEME_OP_MATERIAL_SET,previous)==0;
    restored=(syscall(0xB04L,1L,previous_scale)==0)&&restored;
    restored=(syscall(BROOK_SYS_WM_APPEARANCE,1L,previous_appearance)==0)&&restored;
    restored=(syscall(BROOK_SYS_WM_TASKBAR_METRICS,(long)BROOK_TASKBAR_OP_SET_METRICS,previous_metrics)==0)&&restored;
    if(pro_mode_supported)
        restored=(syscall(BROOK_SYS_WM_TASKBAR_METRICS,(long)BROOK_TASKBAR_OP_SET_PRO_MODE,previous_pro_mode)==0)&&restored;
    char *message=g_strdup_printf(restored?"Could not save; previous appearance restored: %s":"Could not save or fully restore appearance: %s",error?error->message:"apply failed");
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
    settings_path=g_strdup(brook_user_settings_path());
    g_set_prgname("brook-settings");g_set_application_name("Settings");
    gtk_init(&argc,&argv);
    g_object_set(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",TRUE,NULL);
    GtkCssProvider *brook_css=gtk_css_provider_new();
    /* Shape only: every colour comes from the theme's content tokens
     * (brook_gtk_appearance.h's @define-color names). */
    gtk_css_provider_load_from_data(brook_css,
        ".eyebrow { font-size: 10px; font-weight: bold; }"
        ".heading { font-size: 27px; font-weight: 500; } .muted { font-size: 11px; }"
        "button, combobox { border: 1px solid @brook_line; border-radius: 4px; }"
        "button.swatch { background: transparent; border: 2px solid transparent; padding: 3px; box-shadow: none; }"
        "button.swatch.selected { border-color: @brook_accent; } button.swatch label { font-size: 10px; }"
        "button.hue { padding: 1px; min-width: 16px; min-height: 16px; }"
        "flowboxchild { padding: 0; }"
        "button.primary { border-color: @brook_accent; padding: 8px 14px; }"
        "separator { min-height: 1px; }",-1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(brook_css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(brook_css);
    selected_appearance=brook_appearance_read();appearance_style=brook_gtk_appearance_new(0);
    {
        const long theme=syscall(0xB02L,(long)BROOK_THEME_OP_MATERIAL_QUERY,0L);
        theme_supported=theme>=0 && brook_selection_valid((uint64_t)theme);
        if(theme_supported)selected_theme=(uint64_t)theme;
        const long glass=syscall(0xB02L,(long)BROOK_THEME_OP_STATUS,0L);
        glass_available=glass<0?1:(glass&BROOK_THEME_STATUS_GLASS_AVAILABLE)!=0;
    }
    long output=syscall(BROOK_SYS_AUDIO_CONTROL,0L,0L);
    audio_supported=output>=0 && brook_audio_control_valid((uint32_t)output);
    if(audio_supported)audio_control=(uint32_t)output;
    long metrics=syscall(BROOK_SYS_WM_TASKBAR_METRICS,(long)BROOK_TASKBAR_OP_GET_METRICS,0L);
    const long runtime_pro_mode=syscall(BROOK_SYS_WM_TASKBAR_METRICS,(long)BROOK_TASKBAR_OP_GET_PRO_MODE,0L);
    pro_mode_supported=brook_experience_pro_mode_valid((uint64_t)runtime_pro_mode);
    if(metrics>=0 && brook_taskbar_metrics_valid((uint32_t)metrics))selected_metrics=(uint32_t)metrics;
    long scale=syscall(0xB04L,0L,0L);
    if(scale>=0 && brook_desktop_scale_valid((unsigned)scale))selected_scale=(unsigned)scale;
    const int scale_index=(selected_scale-BROOK_DESKTOP_SCALE_MIN)/BROOK_DESKTOP_SCALE_STEP;
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
    GtkWidget *audio_panel=gtk_frame_new("Sound output");
    GtkWidget *audio_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);gtk_container_set_border_width(GTK_CONTAINER(audio_box),12);
    gtk_container_add(GTK_CONTAINER(audio_panel),audio_box);gtk_box_pack_start(GTK_BOX(box),audio_panel,FALSE,FALSE,0);
    GtkWidget *volume_label=gtk_label_new("Master volume");gtk_widget_set_halign(volume_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(audio_box),volume_label,FALSE,FALSE,0);
    volume_slider=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,100,1);
    gtk_scale_set_digits(GTK_SCALE(volume_slider),0);gtk_scale_set_value_pos(GTK_SCALE(volume_slider),GTK_POS_RIGHT);
    gtk_range_set_value(GTK_RANGE(volume_slider),brook_audio_volume(audio_control));gtk_box_pack_start(GTK_BOX(audio_box),volume_slider,FALSE,FALSE,0);
    mute_check=gtk_check_button_new_with_label("Mute all sound");gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mute_check),(audio_control&BROOK_AUDIO_MUTED)!=0);
    gtk_box_pack_start(GTK_BOX(audio_box),mute_check,FALSE,FALSE,0);
    audio_status=gtk_label_new(!audio_supported?"Audio controls need a matching Brook kernel.":
        syscall(BROOK_SYS_AUDIO_CONTROL,2L,0L)>0?((audio_control&BROOK_AUDIO_MUTED)?"Output is muted.":"Audio device ready."):"No audio device available.");
    gtk_label_set_line_wrap(GTK_LABEL(audio_status),TRUE);gtk_label_set_xalign(GTK_LABEL(audio_status),0);
    gtk_style_context_add_class(gtk_widget_get_style_context(audio_status),"muted");gtk_box_pack_start(GTK_BOX(audio_box),audio_status,FALSE,FALSE,0);
    gtk_widget_set_sensitive(volume_slider,audio_supported);gtk_widget_set_sensitive(mute_check,audio_supported);
    slider_ignore_wheel(volume_slider);g_signal_connect(volume_slider,"value-changed",G_CALLBACK(audio_changed),NULL);g_signal_connect(mute_check,"toggled",G_CALLBACK(audio_changed),NULL);
    if(audio_supported)g_timeout_add(500,audio_follow,NULL);
    pointer_speed=syscall(BROOK_SYS_POINTER_SPEED,0L,0L);
    GtkWidget *pointer_panel=gtk_frame_new("Touchpad");
    GtkWidget *pointer_box=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);gtk_container_set_border_width(GTK_CONTAINER(pointer_box),12);
    gtk_container_add(GTK_CONTAINER(pointer_panel),pointer_box);gtk_box_pack_start(GTK_BOX(box),pointer_panel,FALSE,FALSE,0);
    GtkWidget *pointer_label=gtk_label_new("Pointer speed (%)");gtk_widget_set_halign(pointer_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(pointer_box),pointer_label,FALSE,FALSE,0);
    pointer_slider=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,BROOK_POINTER_SPEED_MIN,BROOK_POINTER_SPEED_MAX,5);
    gtk_scale_set_digits(GTK_SCALE(pointer_slider),0);gtk_scale_set_value_pos(GTK_SCALE(pointer_slider),GTK_POS_RIGHT);
    gtk_scale_add_mark(GTK_SCALE(pointer_slider),BROOK_POINTER_SPEED_DEFAULT,GTK_POS_BOTTOM,"default");
    gtk_range_set_value(GTK_RANGE(pointer_slider),pointer_speed>0?pointer_speed:BROOK_POINTER_SPEED_DEFAULT);
    gtk_box_pack_start(GTK_BOX(pointer_box),pointer_slider,FALSE,FALSE,0);
    pointer_status=gtk_label_new(pointer_speed>0?"Two-finger scroll; click with two fingers resting for right-click.":"Pointer speed needs a matching Brook kernel.");
    gtk_label_set_line_wrap(GTK_LABEL(pointer_status),TRUE);gtk_label_set_xalign(GTK_LABEL(pointer_status),0);
    gtk_style_context_add_class(gtk_widget_get_style_context(pointer_status),"muted");gtk_box_pack_start(GTK_BOX(pointer_box),pointer_status,FALSE,FALSE,0);
    gtk_widget_set_sensitive(pointer_slider,pointer_speed>0);
    slider_ignore_wheel(pointer_slider);g_signal_connect(pointer_slider,"value-changed",G_CALLBACK(pointer_changed),NULL);
    touchpad_tap=syscall(BROOK_SYS_TOUCHPAD_TAP,0L,0L);
    tap_check=gtk_check_button_new_with_label("Tap to click");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tap_check),touchpad_tap>0);
    gtk_widget_set_sensitive(tap_check,touchpad_tap>=0);
    gtk_box_pack_start(GTK_BOX(pointer_box),tap_check,FALSE,FALSE,0);
    gtk_box_reorder_child(GTK_BOX(pointer_box),tap_check,2);   /* above the status line */
    g_signal_connect(tap_check,"toggled",G_CALLBACK(tap_changed),NULL);
    GtkWidget *eyebrow=gtk_label_new("APPEARANCE");gtk_widget_set_halign(eyebrow,GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(eyebrow),"eyebrow");gtk_box_pack_start(GTK_BOX(box),eyebrow,FALSE,FALSE,0);
    GtkWidget *heading=gtk_label_new("Choose your\ntheme.");gtk_label_set_justify(GTK_LABEL(heading),GTK_JUSTIFY_LEFT);gtk_widget_set_halign(heading,GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading),"heading");gtk_box_pack_start(GTK_BOX(box),heading,FALSE,FALSE,0);
    GtkWidget *helper=gtk_label_new(theme_supported?"Preview the frame and app colours.":"Appearance controls need a matching Brook kernel.");
    gtk_widget_set_halign(helper,GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(helper),"muted");gtk_box_pack_start(GTK_BOX(box),helper,FALSE,FALSE,0);
    preview=gtk_drawing_area_new();gtk_widget_set_size_request(preview,-1,77);gtk_box_pack_start(GTK_BOX(box),preview,FALSE,FALSE,0);
    g_signal_connect(preview,"draw",G_CALLBACK(draw_preview),NULL);
    GtkWidget *material_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),material_row,FALSE,FALSE,0);
    GtkWidget *material_label=gtk_label_new("Material");gtk_widget_set_halign(material_label,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(material_row),material_label,TRUE,TRUE,0);
    GtkWidget *material_group=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
    gtk_style_context_add_class(gtk_widget_get_style_context(material_group),"linked");
    gtk_box_pack_end(GTK_BOX(material_row),material_group,FALSE,FALSE,0);
    for(unsigned id=0;id<BROOK_THEME_COUNT;++id){
        material_buttons[id]=id?gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(material_buttons[0]),brook_theme_name(id)):
                                gtk_radio_button_new_with_label(NULL,brook_theme_name(id));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(material_buttons[id]),FALSE);
        gtk_box_pack_start(GTK_BOX(material_group),material_buttons[id],FALSE,FALSE,0);
        g_signal_connect(material_buttons[id],"toggled",G_CALLBACK(select_material),GUINT_TO_POINTER(id));
    }
    accel_note=gtk_label_new("");gtk_label_set_line_wrap(GTK_LABEL(accel_note),TRUE);gtk_label_set_xalign(GTK_LABEL(accel_note),0);
    gtk_style_context_add_class(gtk_widget_get_style_context(accel_note),"muted");gtk_box_pack_start(GTK_BOX(box),accel_note,FALSE,FALSE,0);
    gtk_widget_set_no_show_all(accel_note,TRUE);

    GtkWidget *mode_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),mode_row,FALSE,FALSE,0);
    GtkWidget *mode_label=gtk_label_new("Mode");gtk_widget_set_halign(mode_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(mode_row),mode_label,TRUE,TRUE,0);
    GtkWidget *mode_group=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
    gtk_style_context_add_class(gtk_widget_get_style_context(mode_group),"linked");gtk_box_pack_end(GTK_BOX(mode_row),mode_group,FALSE,FALSE,0);
    for(unsigned id=0;id<BROOK_MODE_COUNT;++id){
        mode_buttons[id]=id?gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(mode_buttons[0]),brook_mode_name(id)):
                            gtk_radio_button_new_with_label(NULL,brook_mode_name(id));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(mode_buttons[id]),FALSE);
        gtk_box_pack_start(GTK_BOX(mode_group),mode_buttons[id],FALSE,FALSE,0);
        g_signal_connect(mode_buttons[id],"toggled",G_CALLBACK(select_mode),GUINT_TO_POINTER(id));
    }
    gtk_widget_set_tooltip_text(mode_group,"Light throughout, dark chrome around light apps, or dark throughout.");
    /* Thirteen hues, each shown by its swatch. */
    GtkWidget *hue_label=gtk_label_new("Colour");gtk_widget_set_halign(hue_label,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(box),hue_label,FALSE,FALSE,0);
    GtkWidget *hue_row=gtk_flow_box_new();gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(hue_row),GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(hue_row),BROOK_HUE_COUNT);gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(hue_row),7);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(hue_row),0);gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(hue_row),0);
    gtk_box_pack_start(GTK_BOX(box),hue_row,FALSE,FALSE,0);
    for(unsigned id=0;id<BROOK_HUE_COUNT;++id){
        GtkWidget *button=gtk_button_new();hue_buttons[id]=button;
        gtk_style_context_add_class(gtk_widget_get_style_context(button),"swatch");
        gtk_style_context_add_class(gtk_widget_get_style_context(button),"hue");
        gtk_widget_set_tooltip_text(button,brook_hue_name(id));
        GtkWidget *sample=gtk_drawing_area_new();gtk_widget_set_size_request(sample,16,16);gtk_container_add(GTK_CONTAINER(button),sample);
        g_signal_connect(sample,"draw",G_CALLBACK(draw_hue),GUINT_TO_POINTER(id));
        gtk_container_add(GTK_CONTAINER(hue_row),button);g_signal_connect(button,"clicked",G_CALLBACK(select_hue),GUINT_TO_POINTER(id));
    }
    GtkWidget *custom_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(box),custom_row,FALSE,FALSE,0);
    GtkWidget *custom_label=gtk_label_new("Custom colour");gtk_widget_set_halign(custom_label,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(custom_row),custom_label,TRUE,TRUE,0);
    custom_picker=gtk_color_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(custom_picker),"swatch");
    gtk_color_button_set_title(GTK_COLOR_BUTTON(custom_picker),"Choose an accent colour");
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(custom_picker),FALSE);
    atk_object_set_name(gtk_widget_get_accessible(custom_picker),"Custom accent colour");
    gtk_box_pack_end(GTK_BOX(custom_row),custom_picker,FALSE,FALSE,0);
    g_signal_connect(custom_picker,"color-set",G_CALLBACK(select_custom),NULL);
    custom_swatch=gtk_drawing_area_new();gtk_widget_set_size_request(custom_swatch,20,20);
    gtk_widget_set_no_show_all(custom_swatch,TRUE);
    g_signal_connect(custom_swatch,"draw",G_CALLBACK(draw_hue),GUINT_TO_POINTER(BROOK_HUE_CUSTOM));
    gtk_box_pack_end(GTK_BOX(custom_row),custom_swatch,FALSE,FALSE,0);
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
    slider_ignore_wheel(opacity_slider);g_signal_connect(opacity_slider,"value-changed",G_CALLBACK(controls_changed),NULL);
    g_signal_connect(blur_choice,"changed",G_CALLBACK(controls_changed),NULL);
    g_timeout_add(1000,follow_status,NULL);
    GtkWidget *scale_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,16);gtk_box_pack_start(GTK_BOX(box),scale_row,FALSE,FALSE,4);
    GtkWidget *scale_label=gtk_label_new("Desktop scale");gtk_widget_set_halign(scale_label,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(scale_row),scale_label,TRUE,TRUE,0);
    scale_choice=gtk_combo_box_text_new();
    for (unsigned choice=BROOK_DESKTOP_SCALE_MIN; choice<=BROOK_DESKTOP_SCALE_MAX;
         choice+=BROOK_DESKTOP_SCALE_STEP) {
        char text[8];
        snprintf(text,sizeof(text),"%u%%",choice);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(scale_choice),text);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(scale_choice),scale_index);
    gtk_box_pack_end(GTK_BOX(scale_row),scale_choice,FALSE,FALSE,0);
    g_signal_connect(scale_choice,"changed",G_CALLBACK(scale_changed),NULL);

    GtkWidget *input = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(input), 16);
    gtk_grid_set_row_spacing(GTK_GRID(input), 8);
    GtkWidget *keyboard_label = gtk_label_new("Keyboard layout");
    GtkWidget *cursor_label = gtk_label_new("Cursor size");
    gtk_widget_set_halign(keyboard_label, GTK_ALIGN_START);
    gtk_widget_set_halign(cursor_label, GTK_ALIGN_START);
    keyboard_choice = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(keyboard_choice), "English (US)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(keyboard_choice), "English (UK)");
    cursor_choice = gtk_combo_box_text_new();
    for (unsigned i = 0; i < G_N_ELEMENTS(cursor_sizes); ++i) {
        char size[16];
        g_snprintf(size, sizeof(size), "%u", cursor_sizes[i]);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cursor_choice), size);
    }
    long layout = syscall(BROOK_SYS_INPUT_PREFERENCES,(long)BROOK_INPUT_PREF_KEYBOARD,0L,0L,0L,0L,0L);
    long cursor_size = syscall(BROOK_SYS_INPUT_PREFERENCES,(long)BROOK_INPUT_PREF_CURSOR,0L,0L,0L,0L,0L);
    GKeyFile *input_settings = g_key_file_new();
    if (g_key_file_load_from_file(input_settings, settings_path, 0, NULL)) {
        char *name = g_key_file_get_string(input_settings, "Desktop", "keyboard_layout", NULL);
        if (name) layout = brook_keyboard_layout_parse(name, layout);
        g_free(name);
        const int saved_size = g_key_file_get_integer(input_settings, "Desktop", "cursor_size", NULL);
        if (brook_cursor_size_valid(saved_size)) cursor_size = saved_size;
    }
    g_key_file_unref(input_settings);
    gtk_combo_box_set_active(GTK_COMBO_BOX(keyboard_choice), layout == BROOK_KEYBOARD_GB ? 1 : 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(cursor_choice), 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(cursor_sizes); ++i)
        if (cursor_size == cursor_sizes[i]) gtk_combo_box_set_active(GTK_COMBO_BOX(cursor_choice), i);
    gtk_widget_set_tooltip_text(keyboard_choice, "Applies when Brook restarts.");
    gtk_widget_set_tooltip_text(cursor_choice, "Applies when Brook restarts.");
    gtk_grid_attach(GTK_GRID(input), keyboard_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(input), keyboard_choice, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(input), cursor_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(input), cursor_choice, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), input, FALSE, FALSE, 4);

    GtkWidget *metrics_panel=gtk_expander_new("Taskbar statistics");
    GtkWidget *metrics_grid=gtk_grid_new();gtk_grid_set_column_spacing(GTK_GRID(metrics_grid),12);gtk_grid_set_row_spacing(GTK_GRID(metrics_grid),6);
    const char *metric_names[]={"CPU usage","Memory usage","Disk throughput","Network throughput","Compositor frames","Focused app frames","Temperature and power"};
    for(unsigned i=0;i<7;++i){
        metric_choices[i]=gtk_check_button_new_with_label(metric_names[i]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(metric_choices[i]),(selected_metrics&(1u<<i))!=0);
        gtk_grid_attach(GTK_GRID(metrics_grid),metric_choices[i],i%2,i/2,1,1);
    }
    gtk_container_add(GTK_CONTAINER(metrics_panel),metrics_grid);
    gtk_box_pack_start(GTK_BOX(box),metrics_panel,FALSE,FALSE,0);

    pro_mode_check=gtk_check_button_new_with_label(pro_mode_label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pro_mode_check),brook_user_technical_details(settings_path));
    gtk_widget_set_tooltip_text(pro_mode_check,pro_mode_tooltip);
    gtk_box_pack_start(GTK_BOX(box),pro_mode_check,FALSE,FALSE,8);

    gtk_box_pack_start(GTK_BOX(outer),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,0);
    GtkWidget *footer=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_container_set_border_width(GTK_CONTAINER(footer),20);gtk_box_pack_start(GTK_BOX(outer),footer,FALSE,FALSE,0);
    status=gtk_label_new("Choose a theme, colour and scale.");gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);gtk_label_set_xalign(GTK_LABEL(status),0);
    gtk_style_context_add_class(gtk_widget_get_style_context(status),"muted");gtk_box_pack_start(GTK_BOX(footer),status,TRUE,TRUE,0);
    GtkWidget *button=gtk_button_new_with_label("Apply settings");gtk_style_context_add_class(gtk_widget_get_style_context(button),"primary");
    gtk_box_pack_end(GTK_BOX(footer),button,FALSE,FALSE,0);g_signal_connect(button,"clicked",G_CALLBACK(apply),NULL);
    controls_changed(NULL,NULL);gtk_widget_show_all(window);refresh_appearance();gtk_main();brook_gtk_appearance_free(appearance_style);g_free(settings_path);return 0;
}
