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

void VID_MenuInit(void);
void VID_MenuDraw(void);
const char *VID_MenuKey(int key);

// ---- Direct port of id Software's linux/vid_menu.c (GPL-2, matches Brook
// license). Stripped to the software-only branch: Brook has no OpenGL, no X11,
// no fullscreen toggle. All remaining items are cvars that ref_soft honours
// immediately or on next renderer restart.
// ------------------------------------------------------------------------

extern cvar_t *vid_gamma;
extern cvar_t *scr_viewsize;

static cvar_t *sw_mode;
static cvar_t *sw_stipplealpha;

extern void M_ForceMenuOff(void);

static menuframework_s  s_software_menu;
static menulist_s       s_mode_list;
static menuslider_s     s_screensize_slider;
static menuslider_s     s_brightness_slider;
static menulist_s       s_stipple_box;
static menuaction_s     s_apply_action;
static menuaction_s     s_defaults_action;

static void ScreenSizeCallback(void *s)
{
    menuslider_s *slider = (menuslider_s *)s;
    Cvar_SetValue("viewsize", slider->curvalue * 10);
}

static void BrightnessCallback(void *s)
{
    menuslider_s *slider = (menuslider_s *)s;
    float gamma = (0.8 - (slider->curvalue / 10.0 - 0.5)) + 0.5;
    Cvar_SetValue("vid_gamma", gamma);
}

static void ResetDefaults(void *unused)
{
    (void)unused;
    VID_MenuInit();
}

static void ApplyChanges(void *unused)
{
    (void)unused;
    float gamma = (0.8 - (s_brightness_slider.curvalue / 10.0 - 0.5)) + 0.5;
    Cvar_SetValue("vid_gamma", gamma);
    Cvar_SetValue("sw_stipplealpha", s_stipple_box.curvalue);
    Cvar_SetValue("sw_mode", s_mode_list.curvalue);
    Cvar_Set("vid_ref", "soft");
    M_ForceMenuOff();
}

void VID_MenuInit(void)
{
    static const char *resolutions[] = {
        "[320 240  ]", "[400 300  ]", "[512 384  ]", "[640 480  ]",
        "[800 600  ]", "[960 720  ]", "[1024 768 ]", "[1152 864 ]",
        "[1280 960 ]", "[1600 1200]", 0
    };
    static const char *yesno_names[] = { "no", "yes", 0 };

    if (!sw_mode)
        sw_mode = Cvar_Get("sw_mode", "0", 0);
    if (!sw_stipplealpha)
        sw_stipplealpha = Cvar_Get("sw_stipplealpha", "0", CVAR_ARCHIVE);
    if (!scr_viewsize)
        scr_viewsize = Cvar_Get("viewsize", "100", CVAR_ARCHIVE);

    s_mode_list.curvalue          = sw_mode->value;
    s_screensize_slider.curvalue  = scr_viewsize->value / 10;
    s_brightness_slider.curvalue  = (1.3 - vid_gamma->value + 0.5) * 10;

    fprintf(stderr,
            "VID_MenuInit: cur=%dx%d sw_mode=%d vid_gamma=%.2f viewsize=%.0f\n",
            (int)viddef.width, (int)viddef.height,
            (int)sw_mode->value, vid_gamma->value, scr_viewsize->value);

    s_software_menu.x      = viddef.width * 0.50;
    s_software_menu.nitems = 0;

    s_mode_list.generic.type            = MTYPE_SPINCONTROL;
    s_mode_list.generic.name            = "video mode";
    s_mode_list.generic.x               = 0;
    s_mode_list.generic.y               = 10;
    s_mode_list.itemnames               = resolutions;

    s_screensize_slider.generic.type    = MTYPE_SLIDER;
    s_screensize_slider.generic.x       = 0;
    s_screensize_slider.generic.y       = 20;
    s_screensize_slider.generic.name    = "screen size";
    s_screensize_slider.minvalue        = 3;
    s_screensize_slider.maxvalue        = 12;
    s_screensize_slider.generic.callback = ScreenSizeCallback;

    s_brightness_slider.generic.type    = MTYPE_SLIDER;
    s_brightness_slider.generic.x       = 0;
    s_brightness_slider.generic.y       = 30;
    s_brightness_slider.generic.name    = "brightness";
    s_brightness_slider.generic.callback = BrightnessCallback;
    s_brightness_slider.minvalue        = 5;
    s_brightness_slider.maxvalue        = 13;

    s_stipple_box.generic.type          = MTYPE_SPINCONTROL;
    s_stipple_box.generic.x             = 0;
    s_stipple_box.generic.y             = 60;
    s_stipple_box.generic.name          = "stipple alpha";
    s_stipple_box.itemnames             = yesno_names;
    s_stipple_box.curvalue              = sw_stipplealpha->value;

    s_defaults_action.generic.type      = MTYPE_ACTION;
    s_defaults_action.generic.name      = "reset to default";
    s_defaults_action.generic.x         = 0;
    s_defaults_action.generic.y         = 90;
    s_defaults_action.generic.callback  = ResetDefaults;

    s_apply_action.generic.type         = MTYPE_ACTION;
    s_apply_action.generic.name         = "apply";
    s_apply_action.generic.x            = 0;
    s_apply_action.generic.y            = 100;
    s_apply_action.generic.callback     = ApplyChanges;

    Menu_AddItem(&s_software_menu, &s_mode_list);
    Menu_AddItem(&s_software_menu, &s_screensize_slider);
    Menu_AddItem(&s_software_menu, &s_brightness_slider);
    Menu_AddItem(&s_software_menu, &s_stipple_box);
    Menu_AddItem(&s_software_menu, &s_defaults_action);
    Menu_AddItem(&s_software_menu, &s_apply_action);

    Menu_Center(&s_software_menu);
    s_software_menu.x -= 8;
}

void VID_MenuDraw(void)
{
    int w = 0, h = 0;
    re.DrawGetPicSize(&w, &h, "m_banner_video");
    if (w > 0 && h > 0)
        re.DrawPic(viddef.width / 2 - w / 2, viddef.height / 2 - 110, "m_banner_video");

    Menu_AdjustCursor(&s_software_menu, 1);
    Menu_Draw(&s_software_menu);
}

const char *VID_MenuKey(int key)
{
    extern void M_PopMenu(void);
    menuframework_s *m = &s_software_menu;
    static const char *sound = "misc/menu1.wav";

    switch (key)
    {
    case K_ESCAPE:
        M_PopMenu();
        return NULL;
    case K_UPARROW:
        m->cursor--;
        Menu_AdjustCursor(m, -1);
        break;
    case K_DOWNARROW:
        m->cursor++;
        Menu_AdjustCursor(m, 1);
        break;
    case K_LEFTARROW:
        Menu_SlideItem(m, -1);
        break;
    case K_RIGHTARROW:
        Menu_SlideItem(m, 1);
        break;
    case K_ENTER:
        Menu_SelectItem(m);
        break;
    }

    return sound;
}
