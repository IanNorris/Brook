#pragma once
#include <stdint.h>
#include "credential_ids.h"
namespace brook {
struct UnixCredentials {int32_t pid;uint32_t uid,gid;};
static_assert(sizeof(UnixCredentials)==12,"Linux ucred ABI");
// Brook does not model CAP_SYS_ADMIN/CAP_SETUID/CAP_SETGID overrides here.
// Accept identity claims available without those capabilities; never forge one.
inline int UnixCredentialsCheck(const UnixCredentials& requested,int32_t sender,
                                const CredentialIds& users,const CredentialIds& groups) {
    if(requested.uid==~uint32_t{0} || requested.gid==~uint32_t{0})return -22;
    if(requested.pid!=sender)return -1;
    if(requested.uid!=users.real && requested.uid!=users.effective && requested.uid!=users.saved)return -1;
    if(requested.gid!=groups.real && requested.gid!=groups.effective && requested.gid!=groups.saved)return -1;
    return 0;
}
}
