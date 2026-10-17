#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define INDEX_PATH "/nix/index/packages.idx"
#define MANIFEST_PATH "/nix/profile/manifest.tsv"
#define RESULT_LIMIT 200
static const char *index_path=INDEX_PATH,*manifest_path=MANIFEST_PATH;

typedef struct { char *name,*version,*description,*search; gboolean available; } Package;
static GPtrArray *catalog;
static GHashTable *installed;
static GtkWidget *window,*search,*only_installed,*list,*status,*install_button,*remove_button,*cancel_button,*spinner,*log_view;
static GtkListStore *model;
static GSubprocess *child;
static gboolean busy,waited,drained,operation_finished,cancel_sent,close_after_operation;
static int child_status;
static char *output_error;
static guint filter_idle;
static guint scan_at,matches;
static char *query;
static Package *chosen;

static gboolean valid_name(const char *s) {
    if (!*s || strlen(s)>255 || *s=='-') return FALSE;
    for (;*s;++s) if (!(g_ascii_isalnum(*s)||strchr("+._-",*s))) return FALSE;
    return TRUE;
}
static Package *package_new(const char *name,const char *version,const char *description,gboolean available) {
    Package *p=g_new0(Package,1);
    p->name=g_strdup(name);p->version=g_utf8_make_valid(version,-1);
    p->description=g_utf8_make_valid(description,-1);p->available=available;
    char *joined=g_strconcat(p->name," ",p->description,NULL);p->search=g_ascii_strdown(joined,-1);g_free(joined);
    return p;
}
static void package_free(gpointer data) {
    Package *p=data;g_free(p->name);g_free(p->version);g_free(p->description);g_free(p->search);g_free(p);
}
/* Read-only discovery does not invoke the installer or launch a shell. */
static GHashTable *read_installed(GError **error) {
    GHashTable *table=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
    FILE *f=fopen(manifest_path,"r");
    if (!f) {
        if(errno!=ENOENT)g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),"Cannot read installed packages: %s",strerror(errno));
        return table;
    }
    char *line=NULL;size_t size=0;ssize_t n;
    while((n=getline(&line,&size,f))>=0) {
        if(n>16383 || !n || line[n-1]!='\n'){g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Installed package manifest has an incomplete row.");break;}
        if(n==1 || line[0]=='#')continue;
        char *version=strchr(line,'\t'),*store=version?strchr(version+1,'\t'):NULL;
        if(!version||!store){g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Installed package manifest is malformed.");break;}
        *version++=0;*store=0;
        if(!valid_name(line)){g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Installed package name is invalid.");break;}
        g_hash_table_replace(table,g_strdup(line),g_utf8_make_valid(version,-1));
    }
    if(ferror(f) && !*error)g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),"Cannot read installed packages: %s",strerror(errno));
    free(line);fclose(f);return table;
}
static void load_catalog(GTask *task,gpointer source,gpointer data,GCancellable *cancel) {
    (void)source;(void)data;(void)cancel;
    FILE *f=fopen(index_path,"r");
    if(!f){g_task_return_new_error(task,G_FILE_ERROR,g_file_error_from_errno(errno),"Cannot open package index: %s",strerror(errno));return;}
    GPtrArray *items=g_ptr_array_new_with_free_func(package_free);
    GHashTable *seen=g_hash_table_new(g_str_hash,g_str_equal);
    char *line=NULL;size_t size=0;ssize_t n;GError *error=NULL;
    while((n=getline(&line,&size,f))>=0) {
        if(n>16383 || !n || line[n-1]!='\n'){g_set_error_literal(&error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Package index has an incomplete or oversized row.");break;}
        line[n-1]=0;
        if(n==1 || line[0]=='#')continue;
        char *version=strchr(line,'\t'),*store=version?strchr(version+1,'\t'):NULL,*description=store?strchr(store+1,'\t'):NULL;
        if(!description){g_set_error_literal(&error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Package index is malformed.");break;}
        *version++=0;*store++=0;*description++=0;
        if(!valid_name(line)||strchr(store,'/')){g_set_error_literal(&error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Package index contains an invalid name.");break;}
        if(g_hash_table_contains(seen,line))continue;
        Package *p=package_new(line,version,description,TRUE);g_ptr_array_add(items,p);g_hash_table_add(seen,p->name);
    }
    if(ferror(f)&&!error)g_set_error(&error,G_FILE_ERROR,g_file_error_from_errno(errno),"Cannot read package index: %s",strerror(errno));
    free(line);fclose(f);
    if(!error) {
        GHashTable *current=read_installed(&error);GHashTableIter it;gpointer name,version;
        g_hash_table_iter_init(&it,current);
        while(!error&&g_hash_table_iter_next(&it,&name,&version))if(!g_hash_table_contains(seen,name))g_ptr_array_add(items,package_new(name,version,"Installed locally; not in this package index",FALSE));
        g_hash_table_unref(current);
    }
    g_hash_table_unref(seen);
    if(error){g_ptr_array_unref(items);g_task_return_error(task,error);}
    else g_task_return_pointer(task,items,(GDestroyNotify)g_ptr_array_unref);
}
static void controls(void) {
    gboolean present=chosen&&g_hash_table_contains(installed,chosen->name),allowed=geteuid()==1000&&!busy;
    gtk_widget_set_sensitive(install_button,allowed&&chosen&&chosen->available);
    gtk_button_set_label(GTK_BUTTON(install_button),present?"Repair":"Install");
    gtk_widget_set_sensitive(remove_button,allowed&&present);
    gtk_widget_set_sensitive(cancel_button,busy&&!waited&&!cancel_sent);
}
static void selection_changed(GtkTreeSelection *selection,gpointer unused) {
    (void)unused;GtkTreeModel *m=NULL;GtkTreeIter it;chosen=NULL;
    if(gtk_tree_selection_get_selected(selection,&m,&it))gtk_tree_model_get(m,&it,4,&chosen,-1);
    controls();
}
static gboolean filter_batch(gpointer unused) {
    (void)unused;
    for(unsigned done=0;done<400&&scan_at<catalog->len;++done,++scan_at) {
        Package *p=g_ptr_array_index(catalog,scan_at);
        const char *version=g_hash_table_lookup(installed,p->name);
        if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(only_installed))&&!version)continue;
        if(!strstr(p->search,query))continue;
        if(matches++>=RESULT_LIMIT)continue;
        GtkTreeIter it;gtk_list_store_append(model,&it);
        gtk_list_store_set(model,&it,0,p->name,1,version?version:p->version,2,version?"Installed":"Available",3,p->description,4,p,-1);
    }
    if(scan_at<catalog->len)return G_SOURCE_CONTINUE;
    filter_idle=0;
    if(!busy&&!operation_finished) {
        char *text=g_strdup_printf("%u matching packages%s · %u installed%s",matches,matches>RESULT_LIMIT?" (showing first 200; refine search)":"",g_hash_table_size(installed),geteuid()==1000?"":" · browse only: launch as brook to install");
        gtk_label_set_text(GTK_LABEL(status),text);g_free(text);
    }
    return G_SOURCE_REMOVE;
}
static void filter_changed(GtkWidget *widget,gpointer unused) {
    (void)unused;if(!catalog)return;
    if(widget)operation_finished=FALSE;
    if(filter_idle)g_source_remove(filter_idle);
    gtk_list_store_clear(model);chosen=NULL;controls();
    g_free(query);query=g_ascii_strdown(gtk_entry_get_text(GTK_ENTRY(search)),-1);
    scan_at=matches=0;filter_idle=g_idle_add(filter_batch,NULL);
}
static void catalog_ready(GObject *source,GAsyncResult *result,gpointer unused) {
    (void)source;(void)unused;GError *error=NULL;
    catalog=g_task_propagate_pointer(G_TASK(result),&error);
    if(catalog) {
        GHashTable *current=read_installed(&error);
        if(!error){g_hash_table_unref(installed);installed=current;filter_changed(NULL,NULL);}
        else g_hash_table_unref(current);
    }
    gtk_spinner_stop(GTK_SPINNER(spinner));
    if(error){gtk_label_set_text(GTK_LABEL(status),error->message);g_error_free(error);}
}
static void finish_operation(void) {
    if(!waited||!drained)return;
    GError *error=NULL;GHashTable *current=read_installed(&error);
    if(!error){g_hash_table_unref(installed);installed=current;}else g_hash_table_unref(current);
    busy=FALSE;operation_finished=TRUE;g_clear_object(&child);gtk_spinner_stop(GTK_SPINNER(spinner));
    filter_changed(NULL,NULL);
    /* Keep the result visible even after the result list finishes filtering. */
    char *message=error?g_strdup(error->message):output_error?g_strdup(output_error):child_status==130?g_strdup("Installation cancelled. Completed downloads remain cached."):g_strdup_printf(child_status==0?"Package operation completed. Installed state refreshed.":"Package operation failed (status %d). See details below; you can retry.",child_status);
    gtk_label_set_text(GTK_LABEL(status),message);g_free(message);g_clear_error(&error);controls();
    if(close_after_operation)gtk_widget_destroy(window);
}
static void output_ready(GObject *source,GAsyncResult *result,gpointer unused) {
    (void)unused;GError *error=NULL;GBytes *bytes=g_input_stream_read_bytes_finish(G_INPUT_STREAM(source),result,&error);
    gsize size=0;const char *raw=bytes?g_bytes_get_data(bytes,&size):NULL;
    if(!size) {
        if(error){g_free(output_error);output_error=g_strdup(error->message);g_clear_error(&error);}
        if(bytes)g_bytes_unref(bytes);
        drained=TRUE;finish_operation();return;
    }
    char *text=g_utf8_make_valid(raw,(gssize)size);GtkTextBuffer *buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer,&end);gtk_text_buffer_insert(buffer,&end,text,-1);g_free(text);g_bytes_unref(bytes);
    int count=gtk_text_buffer_get_char_count(buffer);
    if(count>65536){GtkTextIter start,cut;gtk_text_buffer_get_start_iter(buffer,&start);gtk_text_buffer_get_iter_at_offset(buffer,&cut,count-65536);gtk_text_buffer_delete(buffer,&start,&cut);}
    gtk_text_buffer_get_end_iter(buffer,&end);gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(log_view),&end,0,FALSE,0,0);
    g_input_stream_read_bytes_async(G_INPUT_STREAM(source),4096,G_PRIORITY_DEFAULT,NULL,output_ready,NULL);
}
static void operation_waited(GObject *source,GAsyncResult *result,gpointer unused) {
    (void)unused;GError *error=NULL;
    gboolean ok=g_subprocess_wait_finish(G_SUBPROCESS(source),result,&error);
    child_status=ok&&g_subprocess_get_if_exited(G_SUBPROCESS(source))?g_subprocess_get_exit_status(G_SUBPROCESS(source)):128;
    g_clear_error(&error);waited=TRUE;controls();finish_operation();
}
/* Block the request across exec until nix installs its handler, so an early
 * button press cannot terminate a writer before it can clean up its lock. */
static void prepare_installer_child(gpointer unused) {
    (void)unused;sigset_t block;sigemptyset(&block);sigaddset(&block,SIGUSR1);
    sigprocmask(SIG_BLOCK,&block,NULL);
}
static void cancel_operation(GtkButton *button,gpointer unused) {
    (void)button;(void)unused;
    if(!busy||waited||cancel_sent)return;
    cancel_sent=TRUE;g_subprocess_send_signal(child,SIGUSR1);controls();
    gtk_label_set_text(GTK_LABEL(status),"Stopping safely… Finishing the current download or profile update.");
}
static void operate(GtkButton *button,gpointer unused) {
    (void)unused;if(!chosen||busy||geteuid()!=1000)return;
    gboolean removing=GTK_WIDGET(button)==remove_button;
    g_autofree char *name=g_strdup(chosen->name);
    GtkWidget *dialog=gtk_message_dialog_new(GTK_WINDOW(window),GTK_DIALOG_MODAL,GTK_MESSAGE_QUESTION,GTK_BUTTONS_OK_CANCEL,"%s %s?",removing?"Remove":"Install or repair",name);
    gtk_window_set_title(GTK_WINDOW(dialog),"App Gallery");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s",removing?"Remove this package from the shared desktop profile? Its downloaded files stay cached on disk.":"Install this package and its dependencies into the shared desktop profile?");
    int response=gtk_dialog_run(GTK_DIALOG(dialog));gtk_widget_destroy(dialog);if(response!=GTK_RESPONSE_OK)return;
    GError *error=NULL;
    GSubprocessLauncher *launcher=g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_set_child_setup(launcher,prepare_installer_child,NULL,NULL);
    child=g_subprocess_launcher_spawn(launcher,&error,"/nix/bin/nix",removing?"remove":"install",name,NULL);
    g_object_unref(launcher);
    if(!child){gtk_label_set_text(GTK_LABEL(status),error->message);g_error_free(error);return;}
    g_clear_pointer(&output_error,g_free);
    busy=TRUE;waited=drained=operation_finished=cancel_sent=close_after_operation=FALSE;controls();gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view)),"",0);
    gtk_label_set_text(GTK_LABEL(status),"Working… Keep App Gallery open until the installer finishes.");
    g_input_stream_read_bytes_async(g_subprocess_get_stdout_pipe(child),4096,G_PRIORITY_DEFAULT,NULL,output_ready,NULL);
    g_subprocess_wait_async(child,NULL,operation_waited,NULL);
}
static gboolean close_requested(GtkWidget *widget,GdkEvent *event,gpointer unused) {
    (void)widget;(void)event;(void)unused;
    if(busy){close_after_operation=TRUE;cancel_operation(NULL,NULL);return TRUE;}
    return FALSE;
}
int main(int argc,char **argv) {
    if(argc==4&&!strcmp(argv[1],"--check-catalog")) {
        index_path=argv[2];manifest_path=argv[3];
        GTask *task=g_task_new(NULL,NULL,NULL,NULL);load_catalog(task,NULL,NULL,NULL);
        GError *error=NULL;GPtrArray *items=g_task_propagate_pointer(task,&error);g_object_unref(task);
        if(!items){fprintf(stderr,"CATALOGCHECK FAIL %s\n",error->message);g_error_free(error);return 1;}
        unsigned available=0;for(guint i=0;i<items->len;++i)available+=((Package*)g_ptr_array_index(items,i))->available;
        printf("CATALOGCHECK PASS total=%u available=%u local=%u\n",items->len,available,items->len-available);g_ptr_array_unref(items);return 0;
    }

    g_set_prgname("brook-gallery");g_set_application_name("App Gallery");gtk_init(&argc,&argv);
    GtkCssProvider *style=gtk_css_provider_new();
    gtk_css_provider_load_from_data(style,"* { font-family: Sans; font-size: 16px; } window { background: #f4f7fb; } label, entry, treeview, textview, button { color: #25384e; } button:disabled, button:disabled label { color: #8793a3; } treeview:selected { color: #ffffff; }",-1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(style),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(style);
    installed=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
    window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"App Gallery");gtk_window_set_default_size(GTK_WINDOW(window),920,700);
    gtk_window_set_icon_name(GTK_WINDOW(window),"brook-gallery");
    g_signal_connect(window,"delete-event",G_CALLBACK(close_requested),NULL);g_signal_connect(window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,12);gtk_container_set_border_width(GTK_CONTAINER(box),20);gtk_container_add(GTK_CONTAINER(window),box);
    GtkWidget *heading=gtk_label_new("Apps for Brook");gtk_widget_set_halign(heading,GTK_ALIGN_START);gtk_box_pack_start(GTK_BOX(box),heading,FALSE,FALSE,0);
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),row,FALSE,FALSE,0);
    search=gtk_search_entry_new();gtk_entry_set_placeholder_text(GTK_ENTRY(search),"Search packages, editors, games…");gtk_box_pack_start(GTK_BOX(row),search,TRUE,TRUE,0);
    only_installed=gtk_check_button_new_with_label("Installed only");gtk_box_pack_start(GTK_BOX(row),only_installed,FALSE,FALSE,0);
    g_signal_connect(search,"search-changed",G_CALLBACK(filter_changed),NULL);g_signal_connect(only_installed,"toggled",G_CALLBACK(filter_changed),NULL);
    model=gtk_list_store_new(5,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_POINTER);list=gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(list),3);
    const char *titles[]={"Package","Version","Status","Description"};
    for(int i=0;i<4;++i){GtkCellRenderer *renderer=gtk_cell_renderer_text_new();if(i==3)g_object_set(renderer,"ellipsize",PANGO_ELLIPSIZE_END,NULL);GtkTreeViewColumn *column=gtk_tree_view_column_new_with_attributes(titles[i],renderer,"text",i,NULL);gtk_tree_view_column_set_resizable(column,TRUE);if(i==3)gtk_tree_view_column_set_expand(column,TRUE);gtk_tree_view_append_column(GTK_TREE_VIEW(list),column);}
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);gtk_container_add(GTK_CONTAINER(scroll),list);gtk_box_pack_start(GTK_BOX(box),scroll,TRUE,TRUE,0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)),"changed",G_CALLBACK(selection_changed),NULL);
    row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);gtk_box_pack_start(GTK_BOX(box),row,FALSE,FALSE,0);
    spinner=gtk_spinner_new();gtk_box_pack_start(GTK_BOX(row),spinner,FALSE,FALSE,0);
    status=gtk_label_new("Reading package catalog…");gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);gtk_label_set_xalign(GTK_LABEL(status),0);gtk_box_pack_start(GTK_BOX(row),status,TRUE,TRUE,0);
    install_button=gtk_button_new_with_label("Install");remove_button=gtk_button_new_with_label("Remove");gtk_box_pack_start(GTK_BOX(row),install_button,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(row),remove_button,FALSE,FALSE,0);
    cancel_button=gtk_button_new_with_label("Stop");gtk_box_pack_start(GTK_BOX(row),cancel_button,FALSE,FALSE,0);
    g_signal_connect(cancel_button,"clicked",G_CALLBACK(cancel_operation),NULL);
    g_signal_connect(install_button,"clicked",G_CALLBACK(operate),NULL);g_signal_connect(remove_button,"clicked",G_CALLBACK(operate),NULL);
    GtkWidget *expander=gtk_expander_new("Installer details");gtk_box_pack_start(GTK_BOX(box),expander,FALSE,FALSE,0);
    scroll=gtk_scrolled_window_new(NULL,NULL);gtk_widget_set_size_request(scroll,-1,140);gtk_container_add(GTK_CONTAINER(expander),scroll);
    log_view=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view),FALSE);gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view),TRUE);gtk_container_add(GTK_CONTAINER(scroll),log_view);
    controls();gtk_widget_show_all(window);gtk_spinner_start(GTK_SPINNER(spinner));
    GTask *task=g_task_new(NULL,NULL,catalog_ready,NULL);g_task_run_in_thread(task,load_catalog);g_object_unref(task);
    gtk_main();if(filter_idle)g_source_remove(filter_idle);if(catalog)g_ptr_array_unref(catalog);g_hash_table_unref(installed);g_free(query);g_free(output_error);g_object_unref(model);return 0;
}
