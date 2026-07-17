/* SDL2 keyboard/text-input probe for Brook BRO-216 (yquake2 console text dead).
 *
 * The headless wl_keymap_probe PASSed on Brook, proving the kernel + waylandd +
 * xkb keymap path is clean. So the dead-text gate is inside SDL3/sdl2-compat
 * (the libSDL2 yquake2 links is sdl2-compat shimming onto SDL3). This GUI probe
 * exercises that exact stack with a MINIMAL app and reports the ground truth:
 * whether SDL_TEXTINPUT actually arrives when keys are pressed, alongside the
 * SDL_KEYDOWN scancode+sym (the bindings path yquake2 uses).
 *
 * It maps its window the SAME way yquake2 does: SDL_WINDOW_OPENGL +
 * SDL_GL_CreateContext + SDL_GL_SwapWindow. sdl2-compat does NOT support the
 * SDL2 software paths (SDL_CreateRenderer / SDL_GetWindowSurface) on Wayland --
 * they fail and the surface never commits a buffer, so the window never maps and
 * never gets keyboard focus (observed: 5 dead xdg_toplevels, keyboardFocus=nil).
 * The GL path is what yquake2 uses and it maps + focuses on Brook.
 *
 * Interpreting a run (the WM focuses on map; type "asdf 1"):
 *   KEYDOWN sym valid + TEXTINPUT present  => the SDL stack delivers text fine
 *        => BRO-216 is yquake2-specific (its window handling / console state)
 *   KEYDOWN sym valid + NO TEXTINPUT       => sdl2-compat/SDL3 text gate on Brook
 *        (SDL_TextInputActive(focus) / seat focus in keyboard_handle_key)
 *   KEYDOWN sym==0/UNKNOWN                  => xkb keysym resolution broke in SDL
 *        (would contradict wl_keymap_probe -- unexpected)
 *   no FOCUS_GAINED / no KEYDOWN            => the window never mapped/focused
 *
 * Run under Brook's waylandd + GPU compositor:
 *   BROOK_GPU=gl BROOK_COMPOSITE=gpu ... --script wayland_kbdprobe
 * Read the PROBE lines on serial.
 */
#include <SDL.h>
#include <stdio.h>
#include <dlfcn.h>

/* Query SDL3's REAL per-window text-input state directly (bypassing
 * sdl2-compat's global-flag wrappers) to expose the global-vs-per-window
 * desync that fusion identified as the BRO-216 root cause. */
static void *(*g_sdl3_GetKeyboardFocus)(void);
static int   (*g_sdl3_TextInputActive)(void *);
static int   (*g_sdl3_EventEnabled)(unsigned);
static int   (*g_sdl3_StartTextInput)(void *);   /* bool SDL_StartTextInput(SDL_Window*) */
static void *(*g_sdl3_GetWindows)(int *);         /* SDL_Window** SDL_GetWindows(int*) */

static void load_sdl3_syms(void)
{
    void *h = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
    if (!h) h = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "PROBE: dlopen(libSDL3.so.0) failed: %s\n", dlerror()); return; }
    g_sdl3_GetKeyboardFocus = (void *(*)(void))dlsym(h, "SDL_GetKeyboardFocus");
    g_sdl3_TextInputActive  = (int (*)(void *))dlsym(h, "SDL_TextInputActive");
    g_sdl3_EventEnabled     = (int (*)(unsigned))dlsym(h, "SDL_EventEnabled");
    g_sdl3_StartTextInput   = (int (*)(void *))dlsym(h, "SDL_StartTextInput");
    g_sdl3_GetWindows       = (void *(*)(int *))dlsym(h, "SDL_GetWindows");
    fprintf(stderr, "PROBE: SDL3 syms: GetKeyboardFocus=%p TextInputActive=%p EventEnabled=%p\n",
            (void*)g_sdl3_GetKeyboardFocus, (void*)g_sdl3_TextInputActive, (void*)g_sdl3_EventEnabled);
    fflush(stderr);
}

/* SDL_EVENT_TEXT_INPUT = 0x303 in SDL3. */
static void report_sdl3_state(const char *when)
{
    void *f3 = g_sdl3_GetKeyboardFocus ? g_sdl3_GetKeyboardFocus() : (void *)-1;
    int   a3 = (g_sdl3_TextInputActive && f3 && f3 != (void *)-1) ? g_sdl3_TextInputActive(f3) : -1;
    int   ge = g_sdl3_EventEnabled ? g_sdl3_EventEnabled(0x303) : -1;
    fprintf(stderr, "PROBE: [%s] SDL3 focus=%p perWindowTextActive=%d globalTextEvent=%d\n",
            when, f3, a3, ge);
    fflush(stderr);
}

static void report_text_input_state(const char *when)
{
    fprintf(stderr, "PROBE: [%s] IsTextInputActive=%d keyboardFocus=%p\n",
            when, SDL_IsTextInputActive(), (void *)SDL_GetKeyboardFocus());
    fflush(stderr);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "PROBE: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    load_sdl3_syms();
    fprintf(stderr, "PROBE: video driver = %s\n", SDL_GetCurrentVideoDriver());
    fprintf(stderr, "PROBE: SDL revision = %s\n", SDL_GetRevision());
    {
        SDL_version v;
        SDL_GetVersion(&v);
        fprintf(stderr, "PROBE: linked SDL2 ABI = %d.%d.%d (sdl2-compat shims SDL3)\n",
                v.major, v.minor, v.patch);
    }
    fflush(stderr);

    /* Mirror yquake2's GL window setup so the surface maps and gets focus. */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    SDL_Window *w = SDL_CreateWindow("kbdprobe", SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED, 320, 240,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!w) {
        fprintf(stderr, "PROBE: CreateWindow(OPENGL) failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(w);
    if (!gl) {
        fprintf(stderr, "PROBE: GL_CreateContext failed: %s (continuing)\n", SDL_GetError());
    } else {
        fprintf(stderr, "PROBE: GL context created\n");
        SDL_GL_MakeCurrent(w, gl);
        /* Present a few frames so the wl_surface commits a buffer and maps. */
        for (int i = 0; i < 3; i++) SDL_GL_SwapWindow(w);
    }
    fflush(stderr);

    SDL_StartTextInput();
    report_text_input_state("after StartTextInput");
    fprintf(stderr, "PROBE: type \"asdf 1\" (WM focuses on map); Esc quits.\n");
    fflush(stderr);

    SDL_Event e;
    int running = 1;
    while (running) {
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    fprintf(stderr, "PROBE: WINDOW FOCUS_GAINED\n");
                    report_text_input_state("focus gained");
                    report_sdl3_state("focus gained (SDL3 view)");
                    /* Stop->Start TOGGLE: the Stop clears sdl2-compat's global
                     * flag so the following Start actually re-runs per-window
                     * activation (a plain re-Start early-outs on the still-set
                     * global flag). Fusion's decisive test for the desync. */
                    SDL_StopTextInput();
                    SDL_StartTextInput();
                    report_text_input_state("after Stop->Start toggle");
                    report_sdl3_state("after Stop->Start toggle (SDL3 view)");
                    /* Decisive: activate text input DIRECTLY on SDL3's focus
                     * window (bypass sdl2-compat). If this sets the per-window
                     * flag, sdl2-compat was targeting the wrong window. */
                    if (g_sdl3_GetWindows) {
                        int nw = 0; void **wl = (void **)g_sdl3_GetWindows(&nw);
                        void *f3 = g_sdl3_GetKeyboardFocus ? g_sdl3_GetKeyboardFocus() : NULL;
                        fprintf(stderr, "PROBE: SDL3 windows=%d focus=%p:", nw, f3);
                        if (wl) for (int i = 0; wl[i]; i++) fprintf(stderr, " %p", wl[i]);
                        fprintf(stderr, "\n"); fflush(stderr);
                    }
                    if (g_sdl3_StartTextInput && g_sdl3_GetKeyboardFocus) {
                        void *f3 = g_sdl3_GetKeyboardFocus();
                        int rc = f3 ? g_sdl3_StartTextInput(f3) : -1;
                        fprintf(stderr, "PROBE: direct SDL3 SDL_StartTextInput(focus=%p) -> %d\n", f3, rc);
                        fflush(stderr);
                        report_sdl3_state("after DIRECT SDL3 StartTextInput");
                    }
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    fprintf(stderr, "PROBE: WINDOW FOCUS_LOST\n");
                }
                break;
            case SDL_KEYDOWN:
                fprintf(stderr,
                    "PROBE KEYDOWN scancode=%d(%s) sym=%d(%s) mod=0x%x "
                    "focus=%p textActive=%d\n",
                    e.key.keysym.scancode,
                    SDL_GetScancodeName(e.key.keysym.scancode),
                    e.key.keysym.sym, SDL_GetKeyName(e.key.keysym.sym),
                    e.key.keysym.mod,
                    (void *)SDL_GetKeyboardFocus(), SDL_IsTextInputActive());
                report_sdl3_state("KEYDOWN (SDL3 view)");
                if (e.key.keysym.sym == SDLK_ESCAPE) running = 0;
                break;
            case SDL_TEXTINPUT:
                fprintf(stderr, "PROBE TEXTINPUT '%s'  <-- text delivered\n", e.text.text);
                break;
            case SDL_QUIT:
                running = 0;
                break;
            default: break;
            }
            fflush(stderr);
        }
        if (gl) SDL_GL_SwapWindow(w);
        SDL_Delay(16);
    }
    SDL_StopTextInput();
    if (gl) SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
