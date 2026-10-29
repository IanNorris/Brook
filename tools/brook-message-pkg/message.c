#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include "message_format.h"
#include "brook_user_experience.h"
#include "brook_gtk_appearance.h"

static gboolean print_only, technical_override;
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
int main(int argc, char **argv) {
    GOptionEntry options[] = {
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
    if(event_file) {
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
    char simple[512],technical[3072];
    if(!brook_system_message_format(&record,simple,sizeof(simple),technical,sizeof(technical))) {
        fputs("brook-message: unsupported or invalid event\n",stderr);return 65;
    }
    if(print_only){printf("%s\n\n%s\n",simple,technical);return 0;}
    g_set_prgname("brook-message");g_set_application_name("Brook");
    if(!gtk_init_check(&argc,&argv)){fputs("brook-message: display unavailable\n",stderr);return 69;}
    BrookGtkAppearance *appearance=brook_gtk_appearance_new(0);
    GtkWidget *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window),"Brook — Memory");
    gtk_window_set_default_size(GTK_WINDOW(window),520,-1);
    gtk_container_set_border_width(GTK_CONTAINER(window),20);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,16);gtk_container_add(GTK_CONTAINER(window),box);
    char *safe_simple=g_utf8_make_valid(simple,-1),*safe_technical=g_utf8_make_valid(technical,-1);
    GtkWidget *label=gtk_label_new(safe_simple);gtk_label_set_line_wrap(GTK_LABEL(label),TRUE);
    gtk_label_set_xalign(GTK_LABEL(label),0);gtk_label_set_selectable(GTK_LABEL(label),TRUE);
    gtk_box_pack_start(GTK_BOX(box),label,FALSE,FALSE,0);
    GtkWidget *details=gtk_expander_new("Technical details");
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),180);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),360);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll),TRUE);
    GtkWidget *numbers=gtk_label_new(safe_technical);gtk_label_set_line_wrap(GTK_LABEL(numbers),TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(numbers),PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(numbers),TRUE);gtk_label_set_xalign(GTK_LABEL(numbers),0);
    gtk_container_add(GTK_CONTAINER(scroll),numbers);gtk_container_add(GTK_CONTAINER(details),scroll);
    gtk_expander_set_expanded(GTK_EXPANDER(details),technical_override || brook_user_technical_details(brook_user_settings_path()));
    gtk_box_pack_start(GTK_BOX(box),details,TRUE,TRUE,0);
    GtkWidget *close=gtk_button_new_with_label("OK");gtk_widget_set_halign(close,GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(box),close,FALSE,FALSE,0);
    g_signal_connect_swapped(close,"clicked",G_CALLBACK(gtk_widget_destroy),window);
    g_signal_connect(window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    gtk_widget_show_all(window);gtk_main();
    g_free(safe_simple);g_free(safe_technical);brook_gtk_appearance_free(appearance);
    g_free(code);g_free(application);g_free(path);g_free(event_file);return 0;
}
