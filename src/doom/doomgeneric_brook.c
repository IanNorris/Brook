// doomgeneric_brook.c — Brook OS platform layer for DOOM
//
// Adapted from doomgeneric_enkel.c (IanNorris/doomgeneric_enkel, MIT).
// Provides framebuffer output, PS/2 keyboard input, and timing.

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/fb.h>

#include <termios.h>
#include <time.h>
#include <sched.h>

static int FrameBufferFd = -1;
static int* FrameBuffer = 0;

static int KeyboardFd = -1;

#define KEYQUEUE_SIZE 16

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static unsigned int s_PositionX = 0;
static unsigned int s_PositionY = 0;

static unsigned int s_ScreenWidth = 0;
static unsigned int s_ScreenHeight = 0;

static unsigned char convertToDoomKey(unsigned char scancode)
{
    unsigned char key = 0;

    switch (scancode)
    {
    case 0x9C:
    case 0x1C:
        key = KEY_ENTER;
        break;
    case 0x01:
        key = KEY_ESCAPE;
        break;
    case 0xCB:
    case 0x4B:
        key = KEY_LEFTARROW;
        break;
    case 0xCD:
    case 0x4D:
        key = KEY_RIGHTARROW;
        break;
    case 0xC8:
    case 0x48:
        key = KEY_UPARROW;
        break;
    case 0xD0:
    case 0x50:
        key = KEY_DOWNARROW;
        break;
    case 0x1D:
        key = KEY_FIRE;
        break;
    case 0x39:
        key = KEY_USE;
        break;
    case 0x2A:
    case 0x36:
        key = KEY_RSHIFT;
        break;
    case 0x15:
        key = 'y';
        break;
    default:
        break;
    }

    return key;
}

static void addKeyToQueue(int pressed, unsigned char keyCode)
{
    unsigned char key = convertToDoomKey(keyCode);
    if (key == 0) return;

    unsigned short keyData = (pressed << 8) | key;

    s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
    s_KeyQueueWriteIndex++;
    s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

static struct termios orig_termios;

static void disableRawMode(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void enableRawMode(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO);
    raw.c_cc[VMIN] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void DG_Init()
{
    FrameBufferFd = open("/dev/fb0", 0);

    static struct fb_var_screeninfo vinfo;
    static struct fb_fix_screeninfo finfo;

    if (FrameBufferFd >= 0)
    {
        if (ioctl(FrameBufferFd, FBIOGET_FSCREENINFO, &finfo)) {
            printf("Error reading fixed information.\n");
            exit(2);
        }

        if (ioctl(FrameBufferFd, FBIOGET_VSCREENINFO, &vinfo)) {
            printf("Error reading variable information.\n");
            exit(3);
        }

        s_ScreenWidth = vinfo.xres;
        s_ScreenHeight = vinfo.yres;

        // Center DOOM's 640x400 output on the screen
        if (s_ScreenWidth > DOOMGENERIC_RESX)
            s_PositionX = (s_ScreenWidth - DOOMGENERIC_RESX) / 2;
        if (s_ScreenHeight > DOOMGENERIC_RESY)
            s_PositionY = (s_ScreenHeight - DOOMGENERIC_RESY) / 2;

        uint64_t screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;

        FrameBuffer = (int*)mmap(0, screensize, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, FrameBufferFd, 0);
        if ((int64_t)FrameBuffer == -1) {
            printf("Error: failed to map framebuffer device to memory.\n");
            exit(4);
        }

        fprintf(stderr, "Brook DOOM: framebuffer %ux%u, render at (%u,%u)\n",
               s_ScreenWidth, s_ScreenHeight, s_PositionX, s_PositionY);
    }
    else
    {
        fprintf(stderr, "Error: framebuffer device not available.\n");
    }

    enableRawMode();

    KeyboardFd = open("keyboard", 0);

    if (KeyboardFd >= 0)
    {
        // Enter non-blocking mode
        ioctl(KeyboardFd, 1, (void*)1);
    }
}

static void handleKeyInput(void)
{
    if (KeyboardFd < 0)
        return;

    unsigned char scancode = 0;

    // Drain all available scancodes
    while (read(KeyboardFd, &scancode, 1) > 0)
    {
        unsigned char keyRelease = (0x80 & scancode);
        scancode = (0x7F & scancode);

        if (0 == keyRelease)
            addKeyToQueue(1, scancode);
        else
            addKeyToQueue(0, scancode);
    }
}

static int uptime = 0;
static int s_FrameCount = 0;
static int s_FrameLimit = 0;  // 0 = unlimited
static long s_StartTimeMs = 0;

static long getWallTimeMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void DG_DrawFrame()
{
    if (FrameBuffer)
    {
        for (int i = 0; i < DOOMGENERIC_RESY; ++i)
        {
            memcpy(FrameBuffer + s_PositionX + (i + s_PositionY) * s_ScreenWidth,
                   DG_ScreenBuffer + i * DOOMGENERIC_RESX,
                   DOOMGENERIC_RESX * 4);
        }

        // Signal compositor that this framebuffer has new content.
        if (FrameBufferFd >= 0)
            write(FrameBufferFd, "", 1);
    }

    handleKeyInput();

    s_FrameCount++;
    if (s_FrameCount == 1)
        s_StartTimeMs = getWallTimeMs();

    if (s_FrameLimit > 0 && s_FrameCount >= s_FrameLimit)
    {
        long elapsed = getWallTimeMs() - s_StartTimeMs;
        long fps = (elapsed > 0) ? (s_FrameCount * 1000L) / elapsed : 0;
        fprintf(stderr, "BENCH: %d frames in %ld ms (wall), %ld fps avg\n",
               s_FrameCount, elapsed, fps);
        // Spin forever instead of exit() — process exit has a known bug
        // with page table cleanup. This lets all instances report results.
        for (;;) {
            struct timespec ts = { 1, 0 };
            nanosleep(&ts, NULL);
        }
    }
}

void DG_SleepMs(uint32_t ms)
{
    // Yield the timeslice instead of actually sleeping. DOOM still runs as
    // fast as possible but voluntarily gives up the CPU when it would have
    // slept, reducing contention and letting the scheduler work efficiently.
    (void)ms;
    uptime += ms;
    sched_yield();
}

uint32_t DG_GetTicksMs()
{
    return (uint32_t)getWallTimeMs();
}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex)
        return 0;

    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex++;
    s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

    *pressed = keyData >> 8;
    *doomKey = keyData & 0xFF;

    return 1;
}

void DG_SetWindowTitle(const char* title)
{
    (void)title;
}

int main(int argc, char** argv)
{
    // Parse and strip -frames N before passing argv to DOOM engine.
    for (int i = 1; i < argc - 1; i++)
    {
        if (strcmp(argv[i], "-frames") == 0)
        {
            s_FrameLimit = atoi(argv[i + 1]);
            // Remove -frames N from argv by shifting remaining args down.
            for (int j = i; j < argc - 2; j++)
                argv[j] = argv[j + 2];
            argc -= 2;
            break;
        }
    }

    doomgeneric_Create(argc, argv);

    for (;;)
    {
        doomgeneric_Tick();
    }

    return 0;
}
