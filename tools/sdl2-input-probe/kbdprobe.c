/* SDL2 keyboard/text-input probe for Brook BRO-216 (yquake2 console text dead).
 *
 * The headless wl_keymap_probe proved the kernel + waylandd + xkb keymap path is
 * clean (real keymap compiles, text extracts). So the dead-text gate is inside
 * SDL3/sdl2-compat (the libSDL2 yquake2 links is sdl2-compat shimming onto SDL3).
 * This GUI probe exercises that exact stack with a MINIMAL app and reports the
 * ground truth: whether SDL_TEXTINPUT actually arrives when keys are pressed,
 * alongside the SDL_KEYDOWN scancode+sym (the bindings path yquake2 uses).
 *
 * Interpreting a run (focus the window, then type "asdf 1"):
 *   KEYDOWN sym valid + TEXTINPUT present  => the SDL stack delivers text fine
 *        => BRO-216 is yquake2-specific (its window handling / console state)
 *   KEYDOWN sym valid + NO TEXTINPUT       => sdl2-compat/SDL3 text gate on Brook
 *        (SDL_TextInputActive(focus) / seat focus in keyboard_handle_key)
 *   KEYDOWN sym==0/UNKNOWN                  => xkb keysym resolution broke in SDL
 *        (would contradict wl_keymap_probe -- unexpected)
 *   no KEYDOWN at all                       => the window never got keyboard focus
 *
 * A plain SDL window with no buffer may never be mapped/focused by a Wayland
 * compositor, so this creates a SOFTWARE renderer and presents a frame: the
 * surface then maps and can receive focus + key events. No GL, so it sidesteps
 * the yquake2 GL crashes.
 *
 * Run under Brook's waylandd:  SDL_VIDEODRIVER=wayland <out>/bin/kbdprobe
 * Focus the window, type; Esc quits. Read the PROBE lines on stdout/serial.
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

    SDL_Window *w = SDL_CreateWindow("kbdprobe", SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED, 320, 240,
                                     SDL_WINDOW_SHOWN);
    if (!w) {
        fprintf(stderr, "PROBE: CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Create any renderer and present a frame so the wl_surface commits a buffer
     * and maps. Brook's WM focuses on map, so this gets keyboard focus without
     * a click. sdl2-compat's software framebuffer paths (GetWindowSurface,
     * RENDERER_SOFTWARE) are unsupported on Wayland, so let SDL pick (flags=0);
     * on Brook that is the virgl GL renderer -- a trivial clear is low risk.
     * If renderer creation fails we continue: the window may still map. */
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, 0);
    if (!r) {
        fprintf(stderr, "PROBE: CreateRenderer failed: %s (continuing)\n", SDL_GetError());
    } else {
        fprintf(stderr, "PROBE: renderer created\n");
    }

    SDL_StartTextInput();
    report_text_input_state("after StartTextInput");
    fprintf(stderr, "PROBE: focus the window and type \"asdf 1\"; Esc quits.\n");
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
        if (r) {
            SDL_SetRenderDrawColor(r, 0x20, 0x30, 0x50, 0xff);
            SDL_RenderClear(r);
            SDL_RenderPresent(r);
        }
        SDL_Delay(16);
    }
    SDL_StopTextInput();
    if (r) SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
