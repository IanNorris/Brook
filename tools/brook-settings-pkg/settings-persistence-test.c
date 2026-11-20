/* Exercise the real Settings writer without opening a GTK display. */
#define main brook_settings_app_main
#include "settings.c"
#undef main

#include <glib/gstdio.h>

static void check_pro_mode(gboolean expected)
{
    GKeyFile *file=g_key_file_new();
    GError *error=NULL;
    g_assert_true(g_key_file_load_from_file(file,settings_path,0,&error));
    g_assert_no_error(error);
    char *value=g_key_file_get_value(file,BROOK_EXPERIENCE_GROUP,BROOK_EXPERIENCE_DETAILS_KEY,NULL);
    int from_ini=BROOK_EXPERIENCE_PRO_MODE_DEFAULT;
    if(value){
        char *line=g_strconcat(BROOK_EXPERIENCE_DETAILS_KEY,"=",value,NULL);
        from_ini=brook_experience_pro_mode_setting(from_ini,line);
        g_free(line);g_free(value);
    }
    g_assert_cmpint(from_ini,==,expected);
    g_assert_cmpint(brook_user_technical_details(settings_path),==,expected);
    g_assert_false(g_key_file_has_key(file,"Desktop","show_advanced_details",NULL));
    g_assert_cmpint(g_key_file_get_integer(file,"Other","retained",NULL),==,27);
    g_key_file_unref(file);
}

static void save(gboolean value)
{
    GError *error=NULL;
    g_assert_true(save_appearance(BROOK_THEME_SELECTION_DEFAULT,100,BROOK_APPEARANCE_DEFAULT,
                                  value,&error));
    g_assert_no_error(error);
}

int main(void)
{
    GError *error=NULL;
    g_assert_cmpstr(pro_mode_label,==,"Pro mode");
    g_assert_cmpstr(pro_mode_tooltip,==,"Show technical details where available, including system message diagnostics and detailed CPU and memory views.");
    char *directory=g_dir_make_tmp("brook-settings-test-XXXXXX",&error);
    g_assert_no_error(error);
    g_assert_nonnull(directory);
    settings_path=g_build_filename(directory,"settings.ini",NULL);
    g_assert_true(g_file_set_contents(settings_path,"[Desktop]\nscale=100\n[Other]\nretained=27\n",-1,&error));
    g_assert_no_error(error);
    check_pro_mode(FALSE);
    /* Existing settings already used by brook-message. */
    g_assert_true(g_file_set_contents(settings_path,
        "[Desktop]\nscale=100\n[Experience]\ntechnical_details=true\n[Other]\nretained=27\n",-1,&error));
    g_assert_no_error(error);
    check_pro_mode(TRUE);
    save(FALSE);
    check_pro_mode(FALSE);
    save(TRUE);
    check_pro_mode(TRUE);
    g_assert_cmpint(g_remove(settings_path),==,0);
    g_assert_cmpint(g_rmdir(directory),==,0);
    g_free(settings_path);g_free(directory);
    return 0;
}
