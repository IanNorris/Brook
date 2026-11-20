#include "task_rows.h"

#include <errno.h>
#include <gtk/gtk.h>
#include <string.h>
#include <unistd.h>

#define APP_ID "org.brook.TaskManager"

enum {
    COL_NAME, COL_PID, COL_CPU_TEXT, COL_RESIDENT_TEXT, COL_GPU_TEXT,
    COL_CPU, COL_RESIDENT, COL_GPU, COL_FLAGS, COL_INCARNATION, COLUMN_COUNT
};

typedef struct TaskManager {
    GtkWindow *window;
    GtkTreeView *tree;
    GtkListStore *store;
    GtkScrolledWindow *scroller;
    GtkLabel *status;
    GtkWidget *end_button;
    BrookTaskManagerRow *rows;
    BrookTaskManagerRow *scratch;
    BrookTaskManagerSnapshot sample;
    guint timer;
    bool sample_available;
    bool confirming;
} TaskManager;

static TaskIdentity selected_task(TaskManager *manager)
{
    GtkTreeIter iter;
    GtkTreeModel *model;
    TaskIdentity selected = {0};
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(manager->tree), &model, &iter))
        gtk_tree_model_get(model, &iter, COL_PID, &selected.pid, COL_INCARNATION, &selected.incarnation, -1);
    return selected;
}

static void selection_changed(GtkTreeSelection *selection, gpointer data)
{
    (void)selection;
    TaskManager *manager = data;
    const bool can_end = manager->sample_available && !manager->confirming &&
        task_can_end(manager->rows, manager->sample.rows, selected_task(manager));
    gtk_widget_set_sensitive(manager->end_button, can_end);
}

static char *display_name(const BrookTaskManagerRow *row)
{
    return row->name[0] ? g_utf8_make_valid(row->name, strnlen(row->name, sizeof(row->name))) :
        g_strdup("Unknown");
}

static gboolean refresh_tasks(gpointer data)
{
    TaskManager *manager = data;
    BrookTaskManagerSnapshot next = {0};
    const long result = syscall(BROOK_SYS_TASK_MANAGER, (unsigned long)BROOK_TASK_MANAGER_SNAPSHOT,
                                (unsigned long)BROOK_TASK_MANAGER_VERSION, (unsigned long)(uintptr_t)&next,
                                (unsigned long)(uintptr_t)manager->scratch,
                                (unsigned long)BROOK_TASK_MANAGER_MAX_ROWS, 0UL);
    if (result || next.version != BROOK_TASK_MANAGER_VERSION ||
        next.rows > BROOK_TASK_MANAGER_MAX_ROWS || next.total < next.rows) {
        const int error = errno;
        manager->sample_available = false;
        gtk_label_set_text(manager->status,
            result && (error == ENOSYS || error == EOPNOTSUPP) ? "Process metrics unavailable" :
            "Could not refresh processes");
        selection_changed(NULL, manager);
        return G_SOURCE_CONTINUE;
    }

    const TaskIdentity selected = selected_task(manager);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(manager->tree);
    GtkAdjustment *scroll = gtk_scrolled_window_get_vadjustment(manager->scroller);
    const double offset = gtk_adjustment_get_value(scroll);
    gtk_list_store_clear(manager->store);

    qsort(manager->scratch, next.rows, sizeof(*manager->scratch), task_row_compare);
    for (uint32_t i = 0; i < next.rows; ++i) {
        const BrookTaskManagerRow *row = &manager->scratch[i];
        const TaskIdentity identity = task_identity(row);
        const BrookTaskManagerRow *previous = task_row_find(manager->rows, manager->sample.rows, identity);
        const double cpu = task_cpu_percent(row, previous, next.timestamp_ms, manager->sample.timestamp_ms);
        char *name = display_name(row);
        char *cpu_text = cpu < 0 ? g_strdup("Unknown") : g_strdup_printf("%.1f%%", cpu);
        char *resident_text = (row->flags & BROOK_TASK_MANAGER_RESIDENT_KNOWN) ?
            g_format_size_full(row->resident_bytes, G_FORMAT_SIZE_IEC_UNITS) : g_strdup("Unknown");
        char *gpu_text = (row->flags & BROOK_TASK_MANAGER_GPU_MEMORY_KNOWN) ?
            g_format_size_full(row->gpu_memory_bytes, G_FORMAT_SIZE_IEC_UNITS) : g_strdup("Unknown");

        GtkTreeIter iter;
        gtk_list_store_insert_with_values(manager->store, &iter, -1,
            COL_NAME, name, COL_PID, row->pid, COL_CPU_TEXT, cpu_text,
            COL_RESIDENT_TEXT, resident_text, COL_GPU_TEXT, gpu_text,
            COL_CPU, cpu, COL_RESIDENT, row->resident_bytes, COL_GPU, row->gpu_memory_bytes,
            COL_FLAGS, row->flags, COL_INCARNATION, row->incarnation, -1);
        if (!task_identity_compare(identity, selected))
            gtk_tree_selection_select_iter(selection, &iter);
        g_free(gpu_text);
        g_free(resident_text);
        g_free(cpu_text);
        g_free(name);
    }

    BrookTaskManagerRow *old_rows = manager->rows;
    manager->rows = manager->scratch;
    manager->scratch = old_rows;
    manager->sample = next;
    manager->sample_available = true;
    gtk_adjustment_set_value(scroll, offset);

    char *status = next.rows < next.total ?
        g_strdup_printf("Showing %u of %u processes", next.rows, next.total) :
        g_strdup_printf("%u %s", next.rows, next.rows == 1 ? "process" : "processes");
    gtk_label_set_text(manager->status, status);
    g_free(status);
    selection_changed(NULL, manager);
    return G_SOURCE_CONTINUE;
}

static void end_task(GtkButton *button, gpointer data)
{
    (void)button;
    TaskManager *manager = data;
    if (manager->confirming || !manager->sample_available)
        return;

    const TaskIdentity chosen = selected_task(manager);
    const BrookTaskManagerRow *row = task_row_find(manager->rows, manager->sample.rows, chosen);
    if (!row || !(row->flags & BROOK_TASK_MANAGER_CAN_END))
        return;

    char *name = display_name(row);
    GtkWidget *dialog = gtk_message_dialog_new(manager->window,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
        "End %s?", name);
    g_free(name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
        "Process %u will be terminated. Unsaved changes may be lost.", chosen.pid);
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL,
                          "End Task", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    gtk_style_context_add_class(gtk_widget_get_style_context(
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)),
        GTK_STYLE_CLASS_DESTRUCTIVE_ACTION);

    GtkWindow *window = g_object_ref(manager->window);
    manager->confirming = true;
    selection_changed(NULL, manager);
    const int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    manager->confirming = false;
    if (!manager->timer) {
        g_object_unref(window);
        return;
    }
    if (response == GTK_RESPONSE_ACCEPT) {
        if (!manager->sample_available || !task_can_end(manager->rows, manager->sample.rows, chosen)) {
            gtk_label_set_text(manager->status, "This process is no longer available to end");
        } else if (syscall(BROOK_SYS_TASK_MANAGER, (unsigned long)BROOK_TASK_MANAGER_END_TASK,
                           (unsigned long)BROOK_TASK_MANAGER_VERSION, (unsigned long)chosen.pid,
                           (unsigned long)chosen.incarnation, 0UL, 0UL)) {
            const int error = errno;
            gtk_label_set_text(manager->status,
                error == ESRCH ? "This process has already exited" :
                (error == EPERM || error == EACCES) ? "You do not have permission to end this process" :
                "Could not end this process");
        } else {
            refresh_tasks(manager);
        }
    }
    selection_changed(NULL, manager);
    g_object_unref(window);
}

static gint compare_memory(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
    const int column = GPOINTER_TO_INT(data);
    const guint known = column == COL_GPU ?
        BROOK_TASK_MANAGER_GPU_MEMORY_KNOWN : BROOK_TASK_MANAGER_RESIDENT_KNOWN;
    guint fa, fb;
    guint64 x, y;
    gtk_tree_model_get(model, a, COL_FLAGS, &fa, column, &x, -1);
    gtk_tree_model_get(model, b, COL_FLAGS, &fb, column, &y, -1);
    if ((fa & known) != (fb & known))
        return (fa & known) ? 1 : -1;
    return (x > y) - (x < y);
}

static void append_column(GtkTreeView *tree, const char *name, int text_column, int sort_column)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    const bool is_name = text_column == COL_NAME;
    g_object_set(renderer, "xalign", is_name ? 0.0f : 1.0f, NULL);
    if (is_name)
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
        name, renderer, "text", text_column, NULL);
    gtk_tree_view_column_set_sort_column_id(column, sort_column);
    gtk_tree_view_column_set_resizable(column, TRUE);
    if (is_name)
        gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(tree, column);
}

static void window_closed(GtkWidget *window, gpointer data)
{
    (void)window;
    TaskManager *manager = data;
    g_clear_handle_id(&manager->timer, g_source_remove);
}

static void free_manager(gpointer data)
{
    TaskManager *manager = data;
    g_object_unref(manager->store);
    g_free(manager->scratch);
    g_free(manager->rows);
    g_free(manager);
}

static void activate(GtkApplication *app, gpointer data)
{
    (void)data;
    GtkWindow *existing = gtk_application_get_active_window(app);
    if (existing) {
        gtk_window_present(existing);
        return;
    }

    TaskManager *manager = g_new0(TaskManager, 1);
    manager->rows = g_new(BrookTaskManagerRow, BROOK_TASK_MANAGER_MAX_ROWS);
    manager->scratch = g_new(BrookTaskManagerRow, BROOK_TASK_MANAGER_MAX_ROWS);
    GtkWidget *window = gtk_application_window_new(app);
    manager->window = GTK_WINDOW(window);
    g_object_set_data_full(G_OBJECT(window), "task-manager", manager, free_manager);
    g_signal_connect(window, "destroy", G_CALLBACK(window_closed), manager);
    gtk_window_set_title(manager->window, "Task Manager");
    gtk_window_set_default_size(manager->window, 690, 430);
    gtk_window_set_icon_name(manager->window, "utilities-system-monitor");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    manager->scroller = GTK_SCROLLED_WINDOW(scroller);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);

    manager->store = gtk_list_store_new(COLUMN_COUNT,
        G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_DOUBLE, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT, G_TYPE_UINT64);
    GtkTreeSortable *sort = GTK_TREE_SORTABLE(manager->store);
    gtk_tree_sortable_set_sort_func(sort, COL_RESIDENT, compare_memory, GINT_TO_POINTER(COL_RESIDENT), NULL);
    gtk_tree_sortable_set_sort_func(sort, COL_GPU, compare_memory, GINT_TO_POINTER(COL_GPU), NULL);
    gtk_tree_sortable_set_sort_column_id(sort, COL_NAME, GTK_SORT_ASCENDING);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(manager->store));
    manager->tree = GTK_TREE_VIEW(tree);
    gtk_tree_view_set_enable_search(manager->tree, TRUE);
    gtk_tree_view_set_search_column(manager->tree, COL_NAME);
    gtk_widget_set_tooltip_text(tree, "CPU shows the share of one core. A process can use more than 100%.");
    append_column(manager->tree, "Process", COL_NAME, COL_NAME);
    append_column(manager->tree, "PID", COL_PID, COL_PID);
    append_column(manager->tree, "CPU", COL_CPU_TEXT, COL_CPU);
    append_column(manager->tree, "Memory", COL_RESIDENT_TEXT, COL_RESIDENT);
    append_column(manager->tree, "GPU memory", COL_GPU_TEXT, COL_GPU);
    gtk_container_add(GTK_CONTAINER(scroller), tree);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(footer), 10);
    gtk_box_pack_start(GTK_BOX(box), footer, FALSE, FALSE, 0);
    GtkWidget *status = gtk_label_new(NULL);
    manager->status = GTK_LABEL(status);
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(footer), status, TRUE, TRUE, 0);
    manager->end_button = gtk_button_new_with_label("End Task");
    gtk_widget_set_sensitive(manager->end_button, FALSE);
    gtk_box_pack_end(GTK_BOX(footer), manager->end_button, FALSE, FALSE, 0);
    g_signal_connect(gtk_tree_view_get_selection(manager->tree), "changed",
                     G_CALLBACK(selection_changed), manager);
    g_signal_connect(manager->end_button, "clicked", G_CALLBACK(end_task), manager);

    refresh_tasks(manager);
    manager->timer = g_timeout_add(1000, refresh_tasks, manager);
    gtk_widget_show_all(window);
    gtk_window_present(manager->window);
}

int main(int argc, char **argv)
{
    // GTK3 takes the Wayland toplevel ID from GLib's program name.
    g_set_prgname(APP_ID);
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
