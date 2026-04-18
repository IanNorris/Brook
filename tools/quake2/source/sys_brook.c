/*
 * sys_brook.c — Brook OS system interface for Quake 2
 *
 * Replaces sys_linux.c. Provides filesystem, timing, and game module loading.
 * The game module is linked statically (no dlopen).
 */

#include "qcommon.h"
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

// Forward declarations for Brook platform init
void Brook_Video_Init(void);
void Brook_Video_Shutdown(void);
void Brook_Input_Frame(void);
void Brook_SendKeyEvents(void);

// ---------- Timing ----------

unsigned sys_frame_time;

void Sys_Init(void)
{
}

void Sys_mkdir(char *path)
{
    mkdir(path, 0755);
}

// Also provide the capitalized version used by qcommon
void Sys_Mkdir(char *path)
{
    mkdir(path, 0755);
}

// ---------- System ----------

void Sys_Error(char *error, ...)
{
    va_list argptr;
    char string[1024];

    va_start(argptr, error);
    vsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);

    fprintf(stderr, "Error: %s\n", string);
    exit(1);
}

void Sys_Quit(void)
{
    CL_Shutdown();
    Qcommon_Shutdown();
    exit(0);
}

// ---------- Console ----------

char *Sys_ConsoleInput(void)
{
    return NULL;
}

void Sys_ConsoleOutput(char *string)
{
    fputs(string, stdout);
}

// ---------- Dynamic library (static linking shim) ----------

extern game_export_t *GetGameAPI(game_import_t *import);

void Sys_UnloadGame(void)
{
    // Static link — nothing to unload
}

void *Sys_GetGameAPI(void *parms)
{
    return GetGameAPI((game_import_t *)parms);
}

// ---------- Millisecond timer ----------

int curtime;

int Sys_Milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    curtime = (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return curtime;
}

void Sys_SendKeyEvents(void)
{
    Brook_SendKeyEvents();
    sys_frame_time = Sys_Milliseconds();
}

// ---------- Clipboard ----------

char *Sys_GetClipboardData(void)
{
    return NULL;
}

// ---------- Copy protection (none) ----------

void Sys_CopyProtect(void)
{
}

void Sys_AppActivate(void)
{
}

// ---------- Find files ----------

static char findbase[MAX_OSPATH];
static char findpath[MAX_OSPATH];
static char findpattern[MAX_OSPATH];
static DIR *fdir;

static qboolean CompareAttributes(char *path, char *name, unsigned musthave, unsigned canthave)
{
    struct stat st;
    char fn[MAX_OSPATH];

    snprintf(fn, sizeof(fn), "%s/%s", path, name);
    if (stat(fn, &st) == -1)
        return false;

    if ((canthave & SFF_SUBDIR) && S_ISDIR(st.st_mode))
        return false;
    if ((musthave & SFF_SUBDIR) && !S_ISDIR(st.st_mode))
        return false;

    return true;
}

// Simple pattern matching
static qboolean PatternMatch(const char *pattern, const char *string)
{
    while (*pattern)
    {
        if (*pattern == '*')
        {
            pattern++;
            if (!*pattern) return true;
            while (*string)
            {
                if (PatternMatch(pattern, string)) return true;
                string++;
            }
            return false;
        }
        else if (*pattern == '?')
        {
            if (!*string) return false;
            pattern++;
            string++;
        }
        else
        {
            if (*pattern != *string) return false;
            pattern++;
            string++;
        }
    }
    return *string == '\0';
}

char *Sys_FindFirst(char *path, unsigned musthave, unsigned canthave)
{
    char *p;

    if (fdir)
        Sys_Error("Sys_BeginFind without close");

    strncpy(findbase, path, sizeof(findbase) - 1);
    p = strrchr(findbase, '/');
    if (p)
    {
        *p = '\0';
        strncpy(findpattern, p + 1, sizeof(findpattern) - 1);
    }
    else
    {
        strcpy(findbase, ".");
        strncpy(findpattern, path, sizeof(findpattern) - 1);
    }

    fdir = opendir(findbase);
    if (!fdir) return NULL;

    struct dirent *d;
    while ((d = readdir(fdir)) != NULL)
    {
        if (!PatternMatch(findpattern, d->d_name)) continue;
        if (!CompareAttributes(findbase, d->d_name, musthave, canthave)) continue;

        snprintf(findpath, sizeof(findpath), "%s/%s", findbase, d->d_name);
        return findpath;
    }

    return NULL;
}

char *Sys_FindNext(unsigned musthave, unsigned canthave)
{
    if (!fdir) return NULL;

    struct dirent *d;
    while ((d = readdir(fdir)) != NULL)
    {
        if (!PatternMatch(findpattern, d->d_name)) continue;
        if (!CompareAttributes(findbase, d->d_name, musthave, canthave)) continue;

        snprintf(findpath, sizeof(findpath), "%s/%s", findbase, d->d_name);
        return findpath;
    }

    return NULL;
}

void Sys_FindClose(void)
{
    if (fdir)
    {
        closedir(fdir);
        fdir = NULL;
    }
}

// ---------- Main ----------

int main(int argc, char **argv)
{
    int time, oldtime, newtime;

    Qcommon_Init(argc, argv);

    oldtime = Sys_Milliseconds();

    while (1)
    {
        newtime = Sys_Milliseconds();
        time = newtime - oldtime;

        if (time > 0)
            Qcommon_Frame(time);

        oldtime = newtime;
    }

    return 0;
}
