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
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    fprintf(stderr, "PROBE: WINDOW FOCUS_LOST\n");
                }
                break;
            case SDL_KEYDOWN:
                fprintf(stderr,
                    "PROBE KEYDOWN scancode=%d(%s) sym=%d(%s) mod=0x%x\n",
                    e.key.keysym.scancode,
                    SDL_GetScancodeName(e.key.keysym.scancode),
                    e.key.keysym.sym, SDL_GetKeyName(e.key.keysym.sym),
                    e.key.keysym.mod);
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
