/* SDL2 keyboard/text-input probe for Brook BRO-216.
 *
 * Splits, in ONE observation, why yquake2's console text is dead while its
 * scancode bindings work. yquake2 calls SDL_StartTextInput() at init and emits
 * console text from SDL_TEXTINPUT (xkb utf8); its bindings/menu come from
 * SDL_KEYDOWN scancodes (a FIXED evdev->SDL_Scancode table, no xkb).
 *
 * SDL2's Wayland backend (SDL_waylandevents.c) sends text via SDL_SendKeyboardText
 * only when keyboard_input_get_text() succeeds, which needs input->xkb.state to be
 * a valid xkb state (built in keyboard_handle_keymap from the wl_keyboard.keymap
 * memfd). Scancodes never need xkb. So:
 *
 *   scancode OK, sym==0/UNKNOWN, no TEXTINPUT  => xkb.state is NULL/invalid
 *        (keymap event not processed, or keymap mmap/compile failed on Brook)
 *   scancode OK, sym valid, TEXTINPUT present  => xkb works; bug is elsewhere
 *
 * Run under Brook's waylandd (SDL_VIDEODRIVER=wayland). Press qwerty etc. and
 * read the PROBE lines on stdout/serial.
 */
#include <SDL.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "PROBE: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    fprintf(stderr, "PROBE: video driver = %s\n", SDL_GetCurrentVideoDriver());

    SDL_Window *w = SDL_CreateWindow("kbdprobe", SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED, 320, 240, 0);
    if (!w) {
        fprintf(stderr, "PROBE: CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_StartTextInput();
    fprintf(stderr, "PROBE: TextInputActive=%d (expect 1)\n",
            SDL_IsTextInputActive());
    fflush(stderr);

    SDL_Event e;
    int running = 1;
    while (running) {
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
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
                fprintf(stderr, "PROBE TEXTINPUT '%s'\n", e.text.text);
                break;
            case SDL_QUIT:
                running = 0;
                break;
            default: break;
            }
            fflush(stderr);
        }
        SDL_Delay(10);
    }
    SDL_StopTextInput();
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
