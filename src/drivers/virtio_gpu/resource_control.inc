// Actual resource control builder; default operations retain ordinary acknowledgement semantics.
static bool ResourceControlLocked(uint32_t type,uint32_t context,uint32_t resource,bool fenced=false)
{
    if (g_controlQueuePoisoned) return false;
    auto* req=reinterpret_cast<VirtioGpuCtxResource*>(g_cmdBuf+CMD_REQ_OFF);
    memset(req,0,sizeof(*req));req->hdr.type=type;req->hdr.ctx_id=context;
    req->resource_id=resource;
    return CmdRespOk(SubmitCommand(sizeof(*req),CMD_RESP_CAP,fenced));
}
