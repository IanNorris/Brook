/* Brook native WM input; raw captured deltas are independent of desktop DPI. */

#include "client.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

extern unsigned int brook_q2_window;
static int mouse_active = 1;
static uint32_t capture_token;
static unsigned int buttons;
static float mouse_dx, mouse_dy;
struct brook_q2_event {
    uint8_t type, scan, ascii, mods;
    int16_t x, y;
    uint32_t token;
};

static void release_buttons(void)
{
    unsigned int i;
    for (i = 0; i < 3; ++i)
        if (buttons & (1u << i)) Key_Event(K_MOUSE1 + i, false, Sys_Milliseconds());
    buttons = 0;
    mouse_dx = mouse_dy = 0;
}
static void release_capture(void)
{
    if (capture_token && brook_q2_window)
        syscall(0xB0A, (long)brook_q2_window, 2L, (long)capture_token, 0L, 0L, 0L);
    capture_token = 0;
    release_buttons();
}

// PS/2 set 1 scancode → Quake 2 key mapping
static int scancode_to_q2key(unsigned char sc)
{
    switch (sc)
    {
    case 0x01: return K_ESCAPE;
    case 0x1C: return K_ENTER;
    case 0x39: return K_SPACE;
    case 0x0E: return K_BACKSPACE;
    case 0x0F: return K_TAB;

    case 0x48: return K_UPARROW;
    case 0x50: return K_DOWNARROW;
    case 0x4B: return K_LEFTARROW;
    case 0x4D: return K_RIGHTARROW;

    case 0x1D: return K_CTRL;
    case 0x38: return K_ALT;
    case 0x2A: case 0x36: return K_SHIFT;

    case 0x3B: return K_F1;  case 0x3C: return K_F2;
    case 0x3D: return K_F3;  case 0x3E: return K_F4;
    case 0x3F: return K_F5;  case 0x40: return K_F6;
    case 0x41: return K_F7;  case 0x42: return K_F8;
    case 0x43: return K_F9;  case 0x44: return K_F10;
    case 0x57: return K_F11; case 0x58: return K_F12;

    case 0x02: return '1';  case 0x03: return '2';
    case 0x04: return '3';  case 0x05: return '4';
    case 0x06: return '5';  case 0x07: return '6';
    case 0x08: return '7';  case 0x09: return '8';
    case 0x0A: return '9';  case 0x0B: return '0';
    case 0x0C: return '-';  case 0x0D: return '=';

    case 0x10: return 'q';  case 0x11: return 'w';
    case 0x12: return 'e';  case 0x13: return 'r';
    case 0x14: return 't';  case 0x15: return 'y';
    case 0x16: return 'u';  case 0x17: return 'i';
    case 0x18: return 'o';  case 0x19: return 'p';
    case 0x1A: return '[';  case 0x1B: return ']';
    case 0x1E: return 'a';  case 0x1F: return 's';
    case 0x20: return 'd';  case 0x21: return 'f';
    case 0x22: return 'g';  case 0x23: return 'h';
    case 0x24: return 'j';  case 0x25: return 'k';
    case 0x26: return 'l';  case 0x27: return ';';
    case 0x28: return '\''; case 0x29: return '`';
    case 0x2B: return '\\';
    case 0x2C: return 'z';  case 0x2D: return 'x';
    case 0x2E: return 'c';  case 0x2F: return 'v';
    case 0x30: return 'b';  case 0x31: return 'n';
    case 0x32: return 'm';  case 0x33: return ',';
    case 0x34: return '.';  case 0x35: return '/';

    case 0x47: return K_HOME;   case 0x49: return K_PGUP;
    case 0x4F: return K_END;    case 0x51: return K_PGDN;
    case 0x52: return K_INS;    case 0x53: return K_DEL;

    default: return 0;
    }
}

void IN_Init(void)
{
    mouse_active = 1;
    Com_Printf("Native WM input: click game to capture; Ctrl+Alt+Esc releases mouse\n");
}
void IN_Shutdown(void) { release_capture(); }
void IN_Commands(void) { }
void IN_Activate(qboolean active)
{
    mouse_active = active;
    if (!active) release_capture();
}
void IN_ActivateMouse(void) { mouse_active = 1; }
void IN_DeactivateMouse(void) { IN_Activate(false); }

static int native_key(unsigned char scan)
{
    static const int extended[] = {
        K_UPARROW, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW,
        K_HOME, K_END, K_INS, K_DEL, K_PGUP, K_PGDN
    };
    if (scan >= 0x80 && scan < 0x8a) return extended[scan - 0x80];
    return scancode_to_q2key(scan);
}

void Brook_SendKeyEvents(void)
{
    struct brook_q2_event events[32];
    long n, i;
    if (!brook_q2_window) return;
    if (capture_token && syscall(0xB0A, (long)brook_q2_window,
            0L, (long)capture_token, 0L, 0L, 0L) != capture_token)
    {
        capture_token = 0;
        release_buttons();
    }
    while ((n = syscall(510, (long)brook_q2_window, (long)events, 32L, 0L, 0L, 0L)) > 0)
    {
        for (i = 0; i < n; ++i)
        {
            struct brook_q2_event *e = &events[i];
            if (e->type <= 1)
            {
                int key = native_key(e->scan);
                if (key) Key_Event(key, e->type == 0, Sys_Milliseconds());
            }
            else if (e->type == 0x80) Cbuf_AddText("quit\n");
            else if (e->type == 0x82)
            {
                release_capture();
                Key_ClearStates();
            }
            else if (e->type == 6 && capture_token && e->token == capture_token)
            {
                mouse_dx += e->x;
                mouse_dy += e->y;
            }
            else if ((e->type == 3 || e->type == 4) && e->scan < 3)
            {
                /* A fresh click is required after focus loss or manual release. */
                if (e->type == 3 && e->scan == 0 && !capture_token &&
                    mouse_active && cls.key_dest == key_game && !cl.attractloop)
                {
                    long token = syscall(0xB0A, (long)brook_q2_window, 1L, 0L, 0L, 0L, 0L);
                    if (token > 0)
                    {
                        capture_token = (uint32_t)token;
                        /* Capture's sequence fence may discard an already queued
                         * release. Consume this click instead of starting fire. */
                        continue;
                    }
                }
                if (capture_token && e->token != capture_token) continue;
                if (e->type == 3) buttons |= 1u << e->scan;
                else buttons &= ~(1u << e->scan);
                Key_Event(K_MOUSE1 + e->scan, e->type == 3, Sys_Milliseconds());
            }
        }
    }
}
void IN_Frame(void)
{
    if (!mouse_active || cls.key_dest != key_game || cl.attractloop)
        release_capture();
}

void IN_Move(usercmd_t *cmd)
{
    if (!mouse_active)
        return;

    float sensitivity = 3.0f;
    cvar_t *m_sens = Cvar_Get("sensitivity", "3", 0);
    if (m_sens)
        sensitivity = m_sens->value;

    cl.viewangles[YAW] -= sensitivity * mouse_dx * 0.022f;
    cl.viewangles[PITCH] += sensitivity * mouse_dy * 0.022f;

    // Clamp pitch
    if (cl.viewangles[PITCH] > 80) cl.viewangles[PITCH] = 80;
    if (cl.viewangles[PITCH] < -80) cl.viewangles[PITCH] = -80;

    mouse_dx = 0;
    mouse_dy = 0;
}
