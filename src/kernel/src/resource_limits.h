#pragma once
// Per-process setrlimit/prlimit64 policy for the limits Brook stores
// (RLIMIT_CORE, RLIMIT_NOFILE). Pure and host-tested.
#include <stdint.h>
#include "core_autodump_policy.h"

namespace brook {

static constexpr uint32_t RlimitCore = 4;
static constexpr uint32_t RlimitNofile = 7;
// Linux desktop defaults: soft 1024 keeps select()/FD_SETSIZE users safe and
// hard 524288 (systemd since v240) lets Proton/Wine raise the soft limit for
// esync. fs.nr_open's default 1048576 caps any hard limit.
static constexpr uint64_t RlimitNofileDefaultSoft = 1024;
static constexpr uint64_t RlimitNofileDefaultHard = 524288;
static constexpr uint64_t RlimitNrOpen = 1048576;

inline CoreResourceLimits ResourceLimitDefaultNofile()
{
    return {RlimitNofileDefaultSoft, RlimitNofileDefaultHard};
}

// Linux do_prlimit order: soft > hard is EINVAL, NOFILE above nr_open is
// EPERM, and raising the hard limit needs CAP_SYS_RESOURCE (EPERM).
inline int ResourceLimitAdmit(uint32_t resource, CoreResourceLimits current,
                              CoreResourceLimits requested, bool privileged)
{
    if (requested.soft > requested.hard) return -22;
    if (resource == RlimitNofile && requested.hard > RlimitNrOpen) return -1;
    if (!privileged && requested.hard > current.hard) return -1;
    return 0;
}

} // namespace brook
