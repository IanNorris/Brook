// SDL_mixer.h — Minimal shim for Brook OS (no SDL dependency)
// Provides just enough types/defines for i_sound.c to compile
// with FEATURE_SOUND. Actual audio is handled by i_osssound.c.

#ifndef _SDL_mixer_h
#define _SDL_mixer_h

// Empty — all sound module function pointers are resolved at link time
// via DG_sound_module and DG_music_module in i_osssound.c.

#endif
