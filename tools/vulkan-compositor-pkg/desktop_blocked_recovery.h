#pragma once
/* The caller exits without destructing Vulkan objects only after the kernel
 * reports that its owner-bound freeze has drained. A failed/expired attempt
 * retains all renderer resources; abort removes the freeze where possible. */
enum DesktopBlockedRecovery { DESKTOP_BLOCKED_RETAIN, DESKTOP_BLOCKED_EXIT };
struct DesktopBlockedRecoveryOps {
    void *context;
    long (*begin)(void *);
    long (*status)(void *);
    long (*abort)(void *);
    void (*wait)(void *);
};
static inline enum DesktopBlockedRecovery desktop_blocked_recovery(
    const struct DesktopBlockedRecoveryOps *ops,unsigned polls){
    if(!ops || !ops->begin || !ops->status || !ops->abort || !ops->wait || !polls)return DESKTOP_BLOCKED_RETAIN;
    /* begin returns 0, or a negative errno supplied by the caller adapter. */
    const long begin=ops->begin(ops->context);
    if(begin){if(begin==-5)(void)ops->abort(ops->context);return DESKTOP_BLOCKED_RETAIN;}
    for(unsigned i=0;i<polls;++i){
        const long status=ops->status(ops->context);
        if(!status)return DESKTOP_BLOCKED_EXIT;
        if(status<0)break;
        if(i+1<polls)ops->wait(ops->context);
    }
    (void)ops->abort(ops->context);return DESKTOP_BLOCKED_RETAIN;
}
