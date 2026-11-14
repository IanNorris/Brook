// Embedded volume-change feedback clip (src/kernel/assets/README.md).
// 44.1 kHz stereo s16le, 3540 frames; played through the mixer.
#include <stdint.h>
__asm__(".section .rodata.volume_pop,\"a\",@progbits\n"
        ".balign 16\n"
        ".global g_volumePopPcm\n"
        "g_volumePopPcm:\n"
        ".incbin \"" BROOK_VOLUME_POP_PATH "\"\n"
        ".global g_volumePopPcmEnd\n"
        "g_volumePopPcmEnd:\n"
        ".previous\n");
