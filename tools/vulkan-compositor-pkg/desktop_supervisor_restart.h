#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/wait.h>
#include "gpu_window_source_abi.h"

enum { DESKTOP_PROMPT_RESTARTS = 3 };
#define DESKTOP_PROMPT_WINDOW_NS 60000000000ull
#define DESKTOP_STABLE_RUN_NS 30000000000ull

struct DesktopRestartPolicy {
    uint64_t prompt_ns[DESKTOP_PROMPT_RESTARTS];
    unsigned prompt_count, prompt_next;
    unsigned retry_s;
};

static inline bool desktop_claim_prompt_restart(struct DesktopRestartPolicy *policy, uint64_t now_ns)
{
    if (policy->prompt_count == DESKTOP_PROMPT_RESTARTS) {
        const uint64_t oldest_ns = policy->prompt_ns[policy->prompt_next];
        if (now_ns < oldest_ns || now_ns - oldest_ns < DESKTOP_PROMPT_WINDOW_NS)
            return false;
    } else {
        ++policy->prompt_count;
    }
    policy->prompt_ns[policy->prompt_next] = now_ns;
    policy->prompt_next = (policy->prompt_next + 1) % DESKTOP_PROMPT_RESTARTS;
    return true;
}

static inline unsigned desktop_restart_delay(struct DesktopRestartPolicy *policy, int status,
                                             bool clock_ok, uint64_t now_ns, uint64_t runtime_ns)
{
    if (runtime_ns >= DESKTOP_STABLE_RUN_NS)
        policy->retry_s = 1;

    /* Exit 76 keeps the renderer's existing deliberate restart behavior. */
    if (status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 76) {
        policy->retry_s = 1;
        return 1;
    }
    /* Prompt kernel-directed recoveries leave crash backoff intact. Only
     * granted prompt restarts occupy slots in the rolling window. */
    if (status >= 0 && WIFSIGNALED(status) && WTERMSIG(status) == BROOK_GPU_WINDOW_RESTART_SIGNAL &&
        clock_ok && desktop_claim_prompt_restart(policy, now_ns))
        return 1;

    const unsigned delay = policy->retry_s;
    if (delay < 30)
        policy->retry_s = delay > 15 ? 30 : delay * 2;
    return delay;
}
