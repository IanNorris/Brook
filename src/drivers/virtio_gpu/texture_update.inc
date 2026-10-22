// Actual persistent-backing texture update, shared with response/retry tests.
static bool CompUpdateTexture(GpuTexId t,uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{
    GpuCmdGuard guard;
    GpuTexture* gt=CompTex(t);
    if(!gt || gt->owned || g_controlQueuePoisoned || x>=gt->w || y>=gt->h)return false;
    if(w>gt->w-x)w=gt->w-x;
    if(h>gt->h-y)h=gt->h-y;
    if(!w || !h)return false;
    return TransferToHost3D(COMP_CTX_ID,gt->resId,x,y,w,h,gt->w,gt->h);
}
