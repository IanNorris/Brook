/* Host regression for the portable Quake 2 8-bit sound mixer. */
#include "client.h"
#include "snd_loc.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

cvar_t *s_volume;
extern portable_samplepair_t paintbuffer[];
void S_InitScaletable(void);
void S_PaintChannelFrom8(channel_t *, sfxcache_t *, int, int);

int main(void)
{
    cvar_t volume = {0};
    volume.value = 1.0f;
    s_volume = &volume;
    S_InitScaletable();
    sfxcache_t *sound = calloc(1, sizeof(*sound) + 2);
    assert(sound);
    sound->data[0] = 32;
    sound->data[1] = (byte)-32;
    channel_t channel = {0};
    channel.leftvol = 255;
    channel.rightvol = 0;
    memset(paintbuffer, 0, 2 * sizeof(*paintbuffer));
    S_PaintChannelFrom8(&channel, sound, 2, 0);
    assert(paintbuffer[0].left > 0 && paintbuffer[1].left < 0);
    assert(paintbuffer[0].right == 0 && paintbuffer[1].right == 0);
    assert(channel.pos == 2);
    free(sound);
    return 0;
}
