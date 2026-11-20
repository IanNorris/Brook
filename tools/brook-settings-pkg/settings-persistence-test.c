/* Exercise the real Settings writer without opening a GTK display. */
#define main brook_settings_app_main
#include "settings.c"
#undef main

#include <glib/gstdio.h>

static int read_advanced_details(void)
{
    GKeyFile *file=g_key_file_new();
    GError *error=NULL;
    const char *key=BROOK_TASKBAR_ADVANCED_DETAILS_KEY;
    g_assert_true(g_key_file_load_from_file(file,settings_path,0,&error));
    g_assert_no_error(error);
    char *value=g_key_file_get_value(file,"Desktop",key,NULL);
    int result=BROOK_TASKBAR_ADVANCED_DETAILS_DEFAULT;
    if(value){
        char *line=g_strconcat(key,"=",value,NULL);
        result=brook_taskbar_advanced_details_setting(result,line);
        g_free(line);g_free(value);
    }
    g_assert_cmpint(g_key_file_get_integer(file,"Other","retained",NULL),==,27);
    g_key_file_unref(file);
    return result;
}

static void save(int value)
{
    GError *error=NULL;
    g_assert_true(save_appearance(BROOK_THEME_SELECTION_DEFAULT,100,BROOK_APPEARANCE_DEFAULT,
                                  value,FALSE,&error));
    g_assert_no_error(error);
}

int main(void)
{
    GError *error=NULL;
    char *directory=g_dir_make_tmp("brook-settings-test-XXXXXX",&error);
    g_assert_no_error(error);
    g_assert_nonnull(directory);
    settings_path=g_build_filename(directory,"settings.ini",NULL);
    g_assert_true(g_file_set_contents(settings_path,"[Desktop]\nscale=100\n[Other]\nretained=27\n",-1,&error));
    g_assert_no_error(error);
    g_assert_cmpint(read_advanced_details(),==,0);
    save(1);
    g_assert_cmpint(read_advanced_details(),==,1);
    save(-1); /* An older kernel has no advanced-details operation. */
    g_assert_cmpint(read_advanced_details(),==,1);
    save(0);
    g_assert_cmpint(read_advanced_details(),==,0);
    g_assert_cmpint(g_remove(settings_path),==,0);
    g_assert_cmpint(g_rmdir(directory),==,0);
    g_free(settings_path);g_free(directory);
    return 0;
}
