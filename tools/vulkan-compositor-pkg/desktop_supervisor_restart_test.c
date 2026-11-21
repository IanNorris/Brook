#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include "desktop_supervisor_restart.h"

#define NS 1000000000ull

int main(void)
{
    struct DesktopRestartPolicy policy = {.retry_s=1};
    const int kernel = BROOK_GPU_WINDOW_RESTART_SIGNAL;
    assert(desktop_restart_delay(&policy, SIGKILL, true, 0, 0) == 1);
    assert(desktop_restart_delay(&policy, 7 << 8, true, NS, 0) == 2);
    assert(desktop_restart_delay(&policy, kernel, true, 2 * NS, 0) == 1);
    assert(desktop_restart_delay(&policy, kernel, true, 6 * NS, 0) == 1);
    assert(desktop_restart_delay(&policy, kernel, true, 10 * NS, 0) == 1);
    assert(desktop_restart_delay(&policy, kernel, true, 14 * NS, 0) == 4);
    assert(desktop_restart_delay(&policy, SIGSEGV, true, 18 * NS, 0) == 8);
    assert(desktop_restart_delay(&policy, kernel, true, 62 * NS - 1, 0) == 16);

    /* The window ages per grant, including while ordinary crash backoff runs. */
    assert(desktop_restart_delay(&policy, kernel, true, 62 * NS, 0) == 1);
    assert(desktop_restart_delay(&policy, kernel, true, 63 * NS, 0) == 30);
    assert(desktop_restart_delay(&policy, kernel, true, 66 * NS, 0) == 1);
    assert(desktop_restart_delay(&policy, kernel, true, 70 * NS, 0) == 1);
    assert(desktop_restart_delay(&policy, kernel, true, 71 * NS, 0) == 30);
    assert(desktop_restart_delay(&policy, SIGKILL, true, 105 * NS, 30 * NS) == 1);
    assert(desktop_restart_delay(&policy, kernel, false, 0, 0) == 2);

    /* Deliberate exit and fork failure keep their old backoff semantics. */
    assert(desktop_restart_delay(&policy, 76 << 8, true, 106 * NS, 0) == 1);
    assert(desktop_restart_delay(&policy, -1, true, 107 * NS, 0) == 1);
    assert(desktop_restart_delay(&policy, kernel, true, 108 * NS, 0) == 2);
    puts("DESKTOP_SUPERVISOR_RESTART_WINDOW_PASS");
    return 0;
}
