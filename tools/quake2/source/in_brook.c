/*
 * in_brook.c — Brook OS input (keyboard + mouse) for Quake 2
 *
 * Reads PS/2 scancodes from /dev/keyboard and mouse packets from /dev/mouse.
 * Both devices are opened non-blocking.
 */

#include "client.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

static int kb_fd = -1;
static int mouse_fd = -1;
static int mouse_active = 0;
static float mouse_dx = 0;
static float mouse_dy = 0;

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
    kb_fd = open("/dev/keyboard", O_RDONLY);
    if (kb_fd >= 0)
        ioctl(kb_fd, 1, (void *)1); /* non-blocking raw scancode mode */
    else
        Com_Printf("IN_Init: can't open /dev/keyboard\n");

    mouse_fd = open("/dev/mouse", O_RDONLY);
    if (mouse_fd >= 0)
        ioctl(mouse_fd, 1, (void *)1); /* non-blocking mode */
    else
        Com_Printf("IN_Init: can't open /dev/mouse\n");

    mouse_active = 1;
    Com_Printf("IN_Init: keyboard=%d mouse=%d\n", kb_fd, mouse_fd);
}

void IN_Shutdown(void)
{
    if (kb_fd >= 0) { close(kb_fd); kb_fd = -1; }
    if (mouse_fd >= 0) { close(mouse_fd); mouse_fd = -1; }
    mouse_active = 0;
}

void IN_Commands(void)
{
    // Mouse button events could be generated here
}

void IN_Activate(qboolean active)
{
    mouse_active = active;
}

void IN_ActivateMouse(void)
{
    mouse_active = 1;
}

void IN_DeactivateMouse(void)
{
    mouse_active = 0;
}

// Called from Sys_SendKeyEvents
void Brook_SendKeyEvents(void)
{
    if (kb_fd < 0) return;

    unsigned char buf[64];
    ssize_t n;
    while ((n = read(kb_fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
        {
            unsigned char sc = buf[i];
            // E0 prefix — skip it, next byte has the key
            if (sc == 0xE0) continue;

            int released = (sc & 0x80) != 0;
            int key = scancode_to_q2key(sc & 0x7F);
            if (key)
                Key_Event(key, !released, Sys_Milliseconds());
        }
    }

    // Read mouse
    if (mouse_fd < 0 || !mouse_active) return;

    unsigned char mbuf[64];
    while ((n = read(mouse_fd, mbuf, sizeof(mbuf))) > 0)
    {
        for (ssize_t i = 0; i + 2 < n; i += 3)
        {
            unsigned char flags = mbuf[i];
            int dx = (int)(signed char)mbuf[i + 1];
            int dy = (int)(signed char)mbuf[i + 2];

            if (flags & 0x10) dx |= ~0xFF;  // sign extend
            if (flags & 0x20) dy |= ~0xFF;

            mouse_dx += dx;
            mouse_dy -= dy;  // invert Y

            // Mouse buttons
            if (flags & 0x01)
                Key_Event(K_MOUSE1, true, Sys_Milliseconds());
            if (flags & 0x02)
                Key_Event(K_MOUSE2, true, Sys_Milliseconds());
        }
    }
}

void IN_Frame(void)
{
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
