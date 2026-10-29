#include <assert.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "brook_user_experience.h"
int main(void) {
    GError *error=NULL;char *directory=g_dir_make_tmp("brook-preferences-XXXXXX",&error);
    assert(directory && !error);char *path=g_build_filename(directory,"settings.ini",NULL);
    assert(!brook_user_technical_details(path));
    assert(g_file_set_contents(path,"[Experience]\ntechnical_details=true\n",-1,&error));
    assert(brook_user_technical_details(path));
    assert(g_file_set_contents(path,"[Experience]\ntechnical_details=false\n",-1,&error));
    assert(!brook_user_technical_details(path));
    assert(g_file_set_contents(path,"[Experience]\ntechnical_details=perhaps\n",-1,&error));
    assert(!brook_user_technical_details(path));
    assert(g_file_set_contents(path,"malformed",-1,&error));
    assert(!brook_user_technical_details(path));
    assert(g_file_set_contents(path,"[Desktop]\ntheme=1\n",-1,&error));
    assert(!brook_user_technical_details(path));
    g_setenv("BROOK_USER_SETTINGS",path,TRUE);assert(!g_strcmp0(path,brook_user_settings_path()));
    g_remove(path);g_rmdir(directory);g_free(path);g_free(directory);
    return 0;
}
