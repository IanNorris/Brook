#pragma once
#include <glib.h>

/* Presentation preference only. It must never change recovery policy. */
static inline const char *brook_user_settings_path(void) {
    const char *path = g_getenv("BROOK_USER_SETTINGS");
    return path && *path ? path : "/data/home/.config/brook/settings.ini";
}
static inline gboolean brook_user_technical_details(const char *path) {
    GKeyFile *file = g_key_file_new();
    gboolean technical = FALSE;
    if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
        GError *error = NULL;
        technical = g_key_file_get_boolean(file, "Experience", "technical_details", &error);
        if (error) { technical = FALSE; g_error_free(error); }
    }
    g_key_file_unref(file);
    return technical;
}
