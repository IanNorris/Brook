/*
 * vid_brook.c — Brook OS video interface for Quake 2
 *
 * Replaces vid_so.c. Instead of dlopen'ing a renderer shared library,
 * we link ref_soft statically and call GetRefAPI directly.
 */

#include "client.h"
#include "qmenu.h"

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
    static const char *resolutions[VID_NUM_MODES + 1];
    static char        resbufs[VID_NUM_MODES][16];
    static menuframework_s s_video_menu;
    static menulist_s      s_mode_list;
    static menuaction_s    s_apply_action;

    // Build the resolution-names array once (pointers stay valid for menu lifetime).
    for (int i = 0; i < (int)VID_NUM_MODES; i++)
    {
        snprintf(resbufs[i], sizeof(resbufs[i]), "%dx%d",
                 vid_modes[i].width, vid_modes[i].height);
        resolutions[i] = resbufs[i];
    }
    resolutions[VID_NUM_MODES] = NULL;

    // Find current mode (closest match to viddef.width/height, else 0).
    int curMode = 0;
    for (int i = 0; i < (int)VID_NUM_MODES; i++)
    {
        if (vid_modes[i].width == (int)viddef.width &&
            vid_modes[i].height == (int)viddef.height)
        {
            curMode = i;
            break;
        }
    }

    fprintf(stderr,
            "VID_MenuInit: cur=%dx%d mapped to mode %d; %u modes available\n",
            (int)viddef.width, (int)viddef.height, curMode, (unsigned)VID_NUM_MODES);

    s_video_menu.x      = viddef.width / 2;
    s_video_menu.y      = viddef.height / 2 - 58;
    s_video_menu.nitems = 0;

    s_mode_list.generic.type         = MTYPE_LIST;
    s_mode_list.generic.name         = "video mode";
    s_mode_list.generic.x            = 0;
    s_mode_list.generic.y            = 0;
    s_mode_list.itemnames            = resolutions;
    s_mode_list.curvalue             = curMode;

    s_apply_action.generic.type      = MTYPE_ACTION;
    s_apply_action.generic.name      = "apply (not supported yet)";
    s_apply_action.generic.x         = 0;
    s_apply_action.generic.y         = 30;
    s_apply_action.generic.flags     = QMF_GRAYED;
    s_apply_action.generic.callback  = NULL;

    Menu_AddItem(&s_video_menu, &s_mode_list);
    Menu_AddItem(&s_video_menu, &s_apply_action);

    Menu_Center(&s_video_menu);
    s_video_menu.x -= 8;

    // Stash for Draw/Key callbacks.
    extern menuframework_s *g_vid_menu;
    g_vid_menu = &s_video_menu;
}

menuframework_s *g_vid_menu = NULL;

void VID_MenuDraw(void)
{
    if (!g_vid_menu)
    {
        fprintf(stderr, "VID_MenuDraw: g_vid_menu NULL — VID_MenuInit never ran?\n");
        return;
    }

    // Center banner (fall back gracefully if the pic is missing).
    int w = 0, h = 0;
    re.DrawGetPicSize(&w, &h, "m_banner_video");
    if (w > 0 && h > 0)
        re.DrawPic(viddef.width / 2 - w / 2, viddef.height / 2 - 110, "m_banner_video");

    Menu_AdjustCursor(g_vid_menu, 1);
    Menu_Draw(g_vid_menu);
}

const char *VID_MenuKey(int key)
{
    extern const char *Default_MenuKey(menuframework_s *m, int key);
    if (!g_vid_menu) return NULL;
    return Default_MenuKey(g_vid_menu, key);
}
