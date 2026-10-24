// Caller holds GpuCmdGuard. These are association records, not extra canonical
// resource references. Canonical GEM/PRIME/VMA leases retain the creator until
// final retirement; destination destruction detaches before its ID can recycle.
struct AppSharedResource {
    int32_t creator;
    uint32_t resource;
    uint64_t incarnation;
    uint64_t importers[MAX_APP_CTX];
    bool uncertain;
    AppSharedResource* next;
};
static AppSharedResource* g_appSharedResources;
static void AppSharedFailureLocked(int32_t context);

static bool AppDetachContextResourceLocked(uint32_t context,uint32_t resource)
{
    auto* req=reinterpret_cast<VirtioGpuCtxResource*>(g_cmdBuf+CMD_REQ_OFF);
    memset(req,0,sizeof(*req));req->hdr.type=VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    req->hdr.ctx_id=context;req->resource_id=resource;
    return CmdRespOk(SubmitCommand(sizeof(*req),CMD_RESP_CAP));
}

static int32_t AppImportResourceLocked(int32_t destination,int32_t creator,uint32_t resource)
{
    auto* source=AppCtxFind(creator);auto* target=AppCtxFind(destination);
    if(!source || !target || !AppResValid(creator,resource) ||
       source->config.capsetId==VIRTIO_GPU_CAPSET_VENUS ||
       target->config.capsetId==VIRTIO_GPU_CAPSET_VENUS || g_controlQueuePoisoned) return -1;
    if(destination==creator)return 0;
    auto* record=g_appSharedResources;
    while(record && record->resource!=resource)record=record->next;
    if(record && (record->creator!=creator || record->incarnation!=source->incarnation || record->uncertain))return -1;
    const uint32_t slot=static_cast<uint32_t>(destination)-APP_FIRST_CTX;
    if(record && record->importers[slot])return record->importers[slot]==target->incarnation ? 0 : -1;
    // Complete work that predates sharing before publishing an attachment.
    if(!AppFenceLocked(static_cast<uint32_t>(creator))) {AppSharedFailureLocked(creator);return -1;}
    if(!record) {
        record=static_cast<AppSharedResource*>(kmalloc(sizeof(AppSharedResource)));
        if(!record)return -1;
        memset(record,0,sizeof(*record));record->creator=creator;
        record->incarnation=source->incarnation;record->resource=resource;
        record->next=g_appSharedResources;g_appSharedResources=record;
    }
    // Record ownership before host publication, including ambiguous failure.
    record->importers[slot]=target->incarnation;
    if(!CtxAttachResource(static_cast<uint32_t>(destination),resource)) {
        record->uncertain=true;return -1;
    }
    return 0;
}

static int32_t AppImportResource(int32_t destination,int32_t creator,uint32_t resource)
{
    GpuCmdGuard guard;
    return AppImportResourceLocked(destination,creator,resource);
}

static void AppSharedFailureLocked(int32_t context)
{
    const uint32_t slot=static_cast<uint32_t>(context)-APP_FIRST_CTX;
    if(slot>=MAX_APP_CTX)return;
    for(auto* record=g_appSharedResources;record;record=record->next)
        if(record->creator==context || record->importers[slot])record->uncertain=true;
}

// Conservative shared execution: the GPU mutex excludes other submissions;
// complete directly sharing contexts before use, and this context after use.
// Once sharing starts, this covers later producer writes and legacy submissions
// too, rather than relying on a one-time import fence or a caller's BO list.
static bool AppPrepareSharedUseLocked(int32_t context,bool& shared)
{
    shared=false;bool dependencies[MAX_APP_CTX]{};
    auto* current=AppCtxFind(context);if(!current)return false;
    const uint32_t slot=static_cast<uint32_t>(context)-APP_FIRST_CTX;
    for(auto* record=g_appSharedResources;record;record=record->next) {
        if(record->creator!=context && record->importers[slot]!=current->incarnation)continue;
        auto* source=AppCtxFind(record->creator);
        if(record->uncertain || !source || source->incarnation!=record->incarnation)return false;
        shared=true;dependencies[static_cast<uint32_t>(record->creator)-APP_FIRST_CTX]=true;
        for(uint32_t i=0;i<MAX_APP_CTX;++i) {
            if(!record->importers[i])continue;
            auto* target=AppCtxFind(static_cast<int32_t>(APP_FIRST_CTX+i));
            if(!target || target->incarnation!=record->importers[i])return false;
            dependencies[i]=true;
        }
    }
    for(uint32_t i=0;i<MAX_APP_CTX;++i)
        if(dependencies[i] && !AppFenceLocked(APP_FIRST_CTX+i)) {
            AppSharedFailureLocked(static_cast<int32_t>(APP_FIRST_CTX+i));return false;
        }
    return true;
}

static bool AppDetachSharedResourceLocked(int32_t creator,uint32_t resource)
{
    auto** link=&g_appSharedResources;
    while(*link && (*link)->resource!=resource)link=&(*link)->next;
    if(!*link)return true;
    auto* record=*link;auto* source=AppCtxFind(creator);
    if(record->uncertain || record->creator!=creator || !source || source->incarnation!=record->incarnation)return false;
    for(uint32_t i=0;i<MAX_APP_CTX;++i) {
        if(!record->importers[i])continue;
        auto* target=AppCtxFind(static_cast<int32_t>(APP_FIRST_CTX+i));
        if(!target || target->incarnation!=record->importers[i] ||
           !AppFenceLocked(APP_FIRST_CTX+i) ||
           !AppDetachContextResourceLocked(APP_FIRST_CTX+i,resource)) {
            record->uncertain=true;return false;
        }
        record->importers[i]=0;
    }
    *link=record->next;kfree(record);return true;
}

static bool AppReleaseSharedContextLocked(int32_t context)
{
    auto* current=AppCtxFind(context);if(!current)return false;
    const uint32_t slot=static_cast<uint32_t>(context)-APP_FIRST_CTX;
    bool fenced=false;
    for(auto** link=&g_appSharedResources;*link;) {
        auto* record=*link;
        // Canonical resource references must drain before creator destruction.
        if(record->creator==context)return false;
        if(record->importers[slot]) {
            if(record->uncertain || record->importers[slot]!=current->incarnation ||
               (!fenced && !AppFenceLocked(static_cast<uint32_t>(context))) ||
               !AppDetachContextResourceLocked(static_cast<uint32_t>(context),record->resource)) {
                record->uncertain=true;return false;
            }
            fenced=true;record->importers[slot]=0;
        }
        bool live=false;for(auto incarnation:record->importers)live|=incarnation!=0;
        if(!live) { *link=record->next;kfree(record); }
        else link=&record->next;
    }
    return true;
}
