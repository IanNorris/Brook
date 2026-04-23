/*
 * vid_brook.c — Brook OS video interface for Quake 2
 *
 * Replaces vid_so.c. Instead of dlopen'ing a renderer shared library,
 * we link ref_soft statically and call GetRefAPI directly.
 */

#include "client.h"

#include <stdio.h>
#include <stdarg.h>

// Global video state (client-side)
viddef_t viddef;

// Joystick cvar stub (referenced by menu.c)
cvar_t *in_joystick;

// Statically linked renderer
extern refexport_t GetRefAPI(refimport_t rimp);

// Module-scope renderer and imports
refexport_t re;
static qboolean reflib_active = false;

// Video modes (Q2 standard)
typedef struct {
    int width, height;
} vidmode_t;

static vidmode_t vid_modes[] = {
    { 320,  240 },
    { 400,  300 },
    { 512,  384 },
    { 640,  480 },
    { 800,  600 },
    { 960,  720 },
    { 1024, 768 },
    { 1152, 864 },
    { 1280, 960 },
    { 1600, 1200 },
};
#define VID_NUM_MODES (sizeof(vid_modes) / sizeof(vid_modes[0]))

void VID_Printf(int print_level, char *fmt, ...)
{
    va_list argptr;
    char msg[4096];

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    if (print_level == PRINT_ALL)
        Com_Printf("%s", msg);
    else
        Com_DPrintf("%s", msg);
}

void VID_Error(int err_level, char *fmt, ...)
{
    va_list argptr;
    char msg[4096];

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    Com_Error(err_level, "%s", msg);
}

void VID_NewWindow(int width, int height)
{
    viddef.width = width;
    viddef.height = height;
}

qboolean VID_GetModeInfo(int *width, int *height, int mode)
{
    if (mode < 0 || mode >= (int)VID_NUM_MODES)
        return false;

    *width = vid_modes[mode].width;
    *height = vid_modes[mode].height;
    return true;
}

void VID_Init(void)
{
    refimport_t ri;

    ri.Cmd_AddCommand = Cmd_AddCommand;
    ri.Cmd_RemoveCommand = Cmd_RemoveCommand;
    ri.Cmd_Argc = Cmd_Argc;
    ri.Cmd_Argv = Cmd_Argv;
    ri.Cmd_ExecuteText = Cbuf_ExecuteText;
    ri.Con_Printf = VID_Printf;
    ri.Sys_Error = VID_Error;
    ri.FS_LoadFile = FS_LoadFile;
    ri.FS_FreeFile = FS_FreeFile;
    ri.FS_Gamedir = FS_Gamedir;
    ri.Cvar_Get = Cvar_Get;
    ri.Cvar_Set = Cvar_Set;
    ri.Cvar_SetValue = Cvar_SetValue;
    ri.Vid_GetModeInfo = VID_GetModeInfo;
    ri.Vid_MenuInit = VID_MenuInit;
    ri.Vid_NewWindow = VID_NewWindow;

    /* Initialize cvars owned by the video/input subsystem */
    in_joystick = Cvar_Get("in_joystick", "0", CVAR_ARCHIVE);

    re = GetRefAPI(ri);

    if (re.api_version != API_VERSION)
    {
        Com_Error(ERR_FATAL, "VID_Init: ref_soft API version mismatch (%d vs %d)",
                  re.api_version, API_VERSION);
    }

    if (re.Init(NULL, NULL) == -1)
    {
        Com_Error(ERR_FATAL, "VID_Init: renderer init failed");
    }

    reflib_active = true;
}

void VID_Shutdown(void)
{
    if (reflib_active)
    {
        re.Shutdown();
        reflib_active = false;
    }
}

void VID_CheckChanges(void)
{
    // No dynamic renderer switching in Brook
}

void VID_MenuInit(void)
{
    // Simplified — no video mode menu
}

void VID_MenuDraw(void)
{
}

const char *VID_MenuKey(int key)
{
    return NULL;
}
