/* Read, but never change, Brook's global audio control. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include "../../src/shared/brook_audio_control.h"

int main(void) {
    long control = syscall(BROOK_SYS_AUDIO_CONTROL, 0L, 0L);
    long available = syscall(BROOK_SYS_AUDIO_CONTROL, 2L, 0L);
    if (control < 0 || available < 0 || !brook_audio_control_valid((unsigned)control)) {
        perror("audio control query");
        return 1;
    }
    printf("AUDIO_CONTROL_QUERY level=%u muted=%u available=%ld\n",
           brook_audio_volume((unsigned)control),
           ((unsigned)control & BROOK_AUDIO_MUTED) != 0, available);
    return 0;
}
