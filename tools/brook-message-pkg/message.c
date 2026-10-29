#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include "message_format.h"
#include "message_pending.h"
#include "brook_user_experience.h"
#include "brook_gtk_appearance.h"

static gboolean print_only, technical_override, watch_mode;
static GtkWidget *active_window,*simple_label,*technical_label;
static BrookGtkAppearance *appearance;
static uint64_t cursor;
static BrookPendingMessages pending_messages;
static BrookSystemMessage current_message;
static char *code, *application, *path, *event_file;
static uint32_t message_code(const char *name) {
    if (!name) return 0;
    if (!strcmp(name,"app-closed-memory")) return BROOK_MESSAGE_APP_CLOSED_MEMORY;
    if (!strcmp(name,"launch-memory")) return BROOK_MESSAGE_LAUNCH_MEMORY;
    if (!strcmp(name,"recovery-stalled")) return BROOK_MESSAGE_RECOVERY_STALLED;
    if (!strcmp(name,"no-memory-victim")) return BROOK_MESSAGE_NO_MEMORY_VICTIM;
    if (!strcmp(name,"termination-requested")) return BROOK_MESSAGE_TERMINATION_REQUESTED;
    return 0;
}
static void closed(GtkWidget *widget,gpointer unused) {
    (void)widget;(void)unused;active_window=NULL;simple_label=NULL;technical_label=NULL;
    if(!watch_mode)gtk_main_quit();
}
static void details_resized(GObject *details,GParamSpec *property,gpointer user_data) {
    (void)property;
    GtkWindow *window=GTK_WINDOW(user_data);
    GdkRectangle area={0,0,800,720};
    GdkDisplay *display=gtk_widget_get_display(GTK_WIDGET(window));
    GdkMonitor *monitor=gdk_display_get_primary_monitor(display);
    if(!monitor && gdk_display_get_n_monitors(display)>0)monitor=gdk_display_get_monitor(display,0);
    if(monitor)gdk_monitor_get_workarea(monitor,&area);
    int width=520;
    if(width>area.width-40)width=area.width-40;
    if(width<320)width=320;
    int height=gtk_expander_get_expanded(GTK_EXPANDER(details))?480:180;
    if(height>area.height-80)height=area.height-80;
    if(height<180)height=180;
    /* Brook's native WM chooses its fallback size unless GTK supplies bounds.
     * Bound this short message rather than leaving an 800x600 empty dialog. */
    GdkGeometry geometry={0};geometry.min_width=geometry.max_width=width;
    geometry.min_height=geometry.max_height=height;
    gtk_window_set_geometry_hints(window,NULL,&geometry,GDK_HINT_MIN_SIZE|GDK_HINT_MAX_SIZE);
    gtk_window_set_default_size(window,width,height);
    gtk_window_resize(window,width,height);
}
static int present(const BrookSystemMessage *record) {
    char simple[512],technical[3072];
    if(!brook_system_message_format(record,simple,sizeof(simple),technical,sizeof(technical))) {
        fputs("brook-message: unsupported or invalid event\n",stderr);return 0;
    }
    current_message=*record;
    char *safe_simple=g_utf8_make_valid(simple,-1),*safe_technical=g_utf8_make_valid(technical,-1);
    if(active_window) {
        gtk_label_set_text(GTK_LABEL(simple_label),safe_simple);
        gtk_label_set_text(GTK_LABEL(technical_label),safe_technical);
        g_free(safe_simple);g_free(safe_technical);return 1;
    }
    GtkWidget *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);active_window=window;
    gtk_window_set_title(GTK_WINDOW(window),"Brook — Memory");
    gtk_window_set_default_size(GTK_WINDOW(window),520,180);
    gtk_container_set_border_width(GTK_CONTAINER(window),20);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,16);gtk_container_add(GTK_CONTAINER(window),box);
    simple_label=gtk_label_new(safe_simple);gtk_label_set_line_wrap(GTK_LABEL(simple_label),TRUE);
    gtk_label_set_xalign(GTK_LABEL(simple_label),0);gtk_label_set_selectable(GTK_LABEL(simple_label),TRUE);
    gtk_box_pack_start(GTK_BOX(box),simple_label,FALSE,FALSE,0);
    GtkWidget *details=gtk_expander_new("Technical details");
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),180);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),360);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll),TRUE);
    technical_label=gtk_label_new(safe_technical);gtk_label_set_line_wrap(GTK_LABEL(technical_label),TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(technical_label),PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(technical_label),TRUE);gtk_label_set_xalign(GTK_LABEL(technical_label),0);
    gtk_container_add(GTK_CONTAINER(scroll),technical_label);gtk_container_add(GTK_CONTAINER(details),scroll);
    gtk_expander_set_expanded(GTK_EXPANDER(details),technical_override || brook_user_technical_details(brook_user_settings_path()));
    gtk_box_pack_start(GTK_BOX(box),details,TRUE,TRUE,0);
    GtkWidget *button=gtk_button_new_with_label("OK");gtk_widget_set_halign(button,GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(box),button,FALSE,FALSE,0);
    g_signal_connect_swapped(button,"clicked",G_CALLBACK(gtk_widget_destroy),window);
    g_signal_connect(window,"destroy",G_CALLBACK(closed),NULL);
    g_signal_connect(details,"notify::expanded",G_CALLBACK(details_resized),window);
    details_resized(G_OBJECT(details),NULL,window);
    gtk_widget_show_all(window);gtk_widget_grab_focus(button);
    g_free(safe_simple);g_free(safe_technical);return 1;
}
static gboolean poll_messages(gpointer unused) {
    (void)unused;
    for(unsigned i=0;i<16;++i) {
        BrookSystemMessage event={0};
        long result=syscall(BROOK_SYS_SYSTEM_MESSAGE,cursor,&event,sizeof(event),0L,0L,0L);
        if(result<0) {
            fputs("brook-message: kernel event interface unavailable\n",stderr);
            gtk_main_quit();return G_SOURCE_REMOVE;
        }
        if(!result)break;
        if(event.sequence<=cursor) {gtk_main_quit();return G_SOURCE_REMOVE;}
        cursor=event.sequence;
        if(active_window && brook_message_same_subject(&current_message,&event))present(&event);
        else brook_pending_message_add(&pending_messages,&event);
    }
    // Coalesce one victim's progress but retain independent launch failures
    // and other applications' events in a bounded queue.
    if(!active_window) {
        BrookSystemMessage next;
        if(brook_pending_message_take(&pending_messages,&next))present(&next);
    }
    return G_SOURCE_CONTINUE;
}
int main(int argc, char **argv) {
    GOptionEntry options[] = {
        {"watch",0,0,G_OPTION_ARG_NONE,&watch_mode,"Present retained kernel events as a desktop service",NULL},
        {"code",0,0,G_OPTION_ARG_STRING,&code,"Error code (for example app-closed-memory)","CODE"},
        {"app",0,0,G_OPTION_ARG_STRING,&application,"Application display name","NAME"},
        {"path",0,0,G_OPTION_ARG_STRING,&path,"Application executable path","PATH"},
        {"event-file",0,0,G_OPTION_ARG_FILENAME,&event_file,"Read one fixed versioned event record","FILE"},
        {"technical",0,0,G_OPTION_ARG_NONE,&technical_override,"Expand technical details for this message",NULL},
        {"print",0,0,G_OPTION_ARG_NONE,&print_only,"Print both descriptions without opening a window",NULL},
        {NULL,0,0,0,NULL,NULL,NULL}
    };
    GOptionContext *context=g_option_context_new("- Brook system message");
    g_option_context_add_main_entries(context,options,NULL);
    GError *error=NULL;
    if(!g_option_context_parse(context,&argc,&argv,&error)) {
        fprintf(stderr,"brook-message: %s\n",error->message);g_error_free(error);g_option_context_free(context);return 64;
    }
    g_option_context_free(context);
    BrookSystemMessage record={0};record.version=BROOK_SYSTEM_MESSAGE_VERSION;record.size=sizeof(record);
    if(watch_mode) {
        if(event_file || code || application || path || print_only) {
            fputs("brook-message: --watch cannot be combined with event/print arguments\n",stderr);return 64;
        }
    } else if(event_file) {
        if(code || application || path) {fputs("brook-message: choose an event file or code arguments\n",stderr);return 64;}
        FILE *file=fopen(event_file,"rb");
        if(!file) {perror("brook-message");return 66;}
        int valid=fread(&record,1,sizeof(record),file)==sizeof(record) && fgetc(file)==EOF && !ferror(file);
        fclose(file);if(!valid){fputs("brook-message: invalid event size\n",stderr);return 65;}
    } else {
        record.code=message_code(code);
        if(!record.code || (application && strlen(application)>=sizeof(record.app_name)) ||
            (path && strlen(path)>=sizeof(record.executable))) {
            fputs("brook-message: valid --code and bounded --app/--path required\n",stderr);return 64;
        }
        if(application)strcpy(record.app_name,application);
        if(path)strcpy(record.executable,path);
    }
    if(watch_mode) {
        const char *runtime=g_getenv("XDG_RUNTIME_DIR"),*display=g_getenv("WAYLAND_DISPLAY");
        if(!runtime || !*runtime)runtime="/tmp";
        if(!display || !*display)display="wayland-0";
        char *socket_path=display[0]=='/'?g_strdup(display):g_build_filename(runtime,display,NULL);
        while(access(socket_path,F_OK)!=0)usleep(250000);
        g_free(socket_path);
    }
    if(!watch_mode && print_only) {
        char simple[512],technical[3072];
        if(!brook_system_message_format(&record,simple,sizeof(simple),technical,sizeof(technical)))return 65;
        printf("%s\n\n%s\n",simple,technical);return 0;
    }
    g_set_prgname("brook-message");g_set_application_name("Brook");
    if(!gtk_init_check(&argc,&argv)){fputs("brook-message: display unavailable\n",stderr);return 69;}
    appearance=brook_gtk_appearance_new(0);
    if(watch_mode)g_timeout_add(250,poll_messages,NULL);
    else if(!present(&record))return 65;
    gtk_main();brook_gtk_appearance_free(appearance);
    g_free(code);g_free(application);g_free(path);g_free(event_file);return 0;
}
