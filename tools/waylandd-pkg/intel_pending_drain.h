#ifndef BROOK_INTEL_PENDING_DRAIN_H
#define BROOK_INTEL_PENDING_DRAIN_H
struct IntelPendingDrainOps {
    void *context;
    unsigned (*count)(void *);
    void (*poll)(void *);
    void (*wait)(void *);
};
static inline int intel_pending_drain(const struct IntelPendingDrainOps *ops,unsigned limit){
    if(!ops || !ops->count || !ops->poll || !ops->wait)return 0;
    for(unsigned i=0;i<limit && ops->count(ops->context);++i){
        ops->poll(ops->context);
        if(ops->count(ops->context))ops->wait(ops->context);
    }
    return !ops->count(ops->context);
}
#endif
