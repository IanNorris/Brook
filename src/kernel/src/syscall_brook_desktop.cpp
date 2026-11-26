#include "intel_bdw_scanout_target.h"
// syscall_brook_desktop.cpp -- Brook desktop syscalls: layer capture, scene graph, debug frame, overlay.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "mouse.h"

#include "syscall.h"
#include "desktop_layers_export.h"
#include "desktop_capture_access.h"
#include "fs_context.h"
#include "mount_namespace.h"
#include "process_fs.h"
#include "string.h"
#include "desktop_present_policy.h"
#include "intel_bdw_scanout.h"
#include "memory/user_mapping.h"
#include "compositor.h"
#include "compositor_scene.h"
#include "wm_chrome_key.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_drm.h"
#include "syscall_memory.h"
#include "syscall_handlers.h"

namespace brook {

WmChromeCaptureStats g_chromeCaptureStats{}; // window.h / wm_chrome_key.h
DesktopCaptureCounters g_desktopCaptureStats{}; // desktop_layers_export.h
uint32_t g_desktopPresentPolicy=BROOK_DESKTOP_POLICY_APP_CADENCE|BROOK_DESKTOP_POLICY_DIRECT_SHADOW,g_desktopPresentDeferUs=0; // desktop_present_policy.h
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
static bool DesktopCaptureOwner(Process* proc, uint64_t owner)
{
    // Attach alone is also used by GPU diagnostics. Only the initrd renderer
    // launched as a system service may read another process's window.
    const bool service = proc->lifecycleRole == ProcessLifecycleRole::Service;
    const bool bootRenderer = !strcmp(proc->exePath, "/boot/VKRENDER.ELF") ||
                              !strcmp(proc->exePath, "/BOOT/VKRENDER.ELF");
    const bool originalNamespace = !proc->userNamespace &&
        __atomic_load_n(&proc->mountNamespaceMember, __ATOMIC_ACQUIRE) == VfsMountNamespaceInitial();
    if (!DesktopLayerContentAllowed(service, originalNamespace, bootRenderer, true)) return false;
    auto* fs = ProcessFsAcquire(proc);
    const bool globalRoot = fs && FsContextHasNamespaceRoot(fs);
    if (fs) FsContextRelease(fs);
    return globalRoot && BdwOffscreenRendererOwnedBy(owner);
}

static int64_t CaptureDesktopChrome(uint64_t owner,uint64_t op,uint64_t address,uint64_t size)
{
    auto* wire=static_cast<BrookDesktopLayers*>(kmalloc(sizeof(BrookDesktopLayers)));
    auto* snapshot=static_cast<brook::WmDesktopLayers*>(kmalloc(sizeof(brook::WmDesktopLayers)));
    if(!wire || !snapshot){kfree(wire);kfree(snapshot);return -ENOMEM;}
    int64_t result=-EFAULT;
    const bool keep=op==BROOK_DESKTOP_CHROME_TRACKED_KEEP;
    // Spans (stage 1.4): keyed, writing only the frame strips.
    const bool spans=op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_SPANS;
    const bool keyed=op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_KEYED || spans;
    if(CopyFromUser(wire,address,sizeof(*wire)) && UserBufferWritable(address,sizeof(*wire))){
        const uint64_t heldKey=wire->reserved;
        if(wire->version!=BROOK_DESKTOP_LAYERS_VERSION || (keep?!heldKey:wire->reserved!=0) || !wire->layers[0].window)result=-EINVAL;
        else if(!brook::WmAcquireDesktopLayers(snapshot))result=-95;
        else {
            auto& layer=wire->layers[0];uint32_t index=0;
            while(index<snapshot->count && snapshot->layers[index].window!=layer.window)++index;
            if(index==snapshot->count)result=-ENOENT;
            else {
                brook::WmDesktopChrome chrome{};
                const bool tracked=op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE || keyed || keep;
                int32_t keyMouseX=-1,keyMouseY=-1;
                if(keep)MouseGetPosition(&keyMouseX,&keyMouseY);
                result=brook::WmDesktopChromeCapture(*snapshot,index,&chrome,nullptr,0,keyMouseX,keyMouseY,tracked);
                const uint64_t bytes=uint64_t(chrome.width)*chrome.height*4;
                const bool capture=op!=BROOK_DESKTOP_CHROME_QUERY && !keep;
                const auto& source=snapshot->layers[index];
                uint64_t renderedKey=0;
                if(!result && keep){
                    // Unchanged chrome: the caller's buffer already holds these
                    // exact pixels. Anything else (content, size, hover, focus,
                    // title, theme) means recapture.
                    if(wire->count!=1 || layer.incarnation!=source.incarnation || layer.width!=chrome.width ||
                       layer.height!=chrome.height || layer.bytes!=bytes || chrome.key!=heldKey){
                        __atomic_add_fetch(&brook::g_chromeCaptureStats.keepMisses,1,__ATOMIC_RELAXED);result=-116;
                    }else if(!brook::WmDesktopLayersCaptureCurrent(*snapshot))result=-EAGAIN;
                    else {
                        renderedKey=heldKey;
                        __atomic_add_fetch(&brook::g_chromeCaptureStats.keeps,1,__ATOMIC_RELAXED);
                        __atomic_add_fetch(&brook::g_chromeCaptureStats.bytesKept,bytes,__ATOMIC_RELAXED);
                    }
                }
                if(!result && capture && (wire->count!=1 || layer.incarnation!=source.incarnation ||
                   (!tracked && layer.generation!=source.generation) || layer.width!=chrome.width || layer.height!=chrome.height ||
                   layer.clientX!=chrome.x || layer.clientY!=chrome.y || layer.bytes!=bytes))result=-EAGAIN;
                if(!result && capture && bytes && (!layer.pixels || layer.capacity<bytes || layer.pixels>~uint64_t{0}-bytes ||
                   (layer.pixels<address+size && address<layer.pixels+bytes)))result=-EINVAL;
                uint32_t* pixels=nullptr;
                if(!result && capture && bytes){
                    pixels=static_cast<uint32_t*>(kmalloc(bytes));
                    if(!pixels)result=-ENOMEM;
                    else {
                        brook::WmDesktopChrome current{};
                        int32_t mouseX=0,mouseY=0;MouseGetPosition(&mouseX,&mouseY);
                        result=brook::WmDesktopChromeCapture(*snapshot,index,&current,pixels,bytes,mouseX,mouseY,tracked,spans);
                        if(!result && (current.x!=chrome.x || current.y!=chrome.y || current.width!=chrome.width || current.height!=chrome.height))result=-EAGAIN;
                        uint64_t copied=bytes;
                        const uint32_t ix=current.interiorX,iy=current.interiorY,iw=current.interiorW,ih=current.interiorH;
                        if(!result && spans && iw && ih){
                            // Only the strips: the interior of pixels is not
                            // initialised and must never reach the caller.
                            const uint64_t row=uint64_t(chrome.width)*4;copied=0;
                            auto copy=[&](uint64_t offset,uint64_t n){
                                if(!n)return true;copied+=n;
                                return CopyToUser(layer.pixels+offset,reinterpret_cast<uint8_t*>(pixels)+offset,n);
                            };
                            bool ok=copy(0,row*iy) && copy(row*(iy+ih),row*(chrome.height-iy-ih));
                            for(uint32_t y=iy;ok && y<iy+ih;++y)
                                ok=copy(row*y,uint64_t(ix)*4) && copy(row*y+uint64_t(ix+iw)*4,uint64_t(chrome.width-ix-iw)*4);
                            if(!ok)result=-EFAULT;
                        }else if(!result && !CopyToUser(layer.pixels,pixels,bytes))result=-EFAULT;
                        if(!result){
                            renderedKey=current.key;
                            __atomic_add_fetch(&brook::g_chromeCaptureStats.captures,1,__ATOMIC_RELAXED);
                            __atomic_add_fetch(&brook::g_chromeCaptureStats.bytesCopied,copied,__ATOMIC_RELAXED);
                            if(copied<bytes)__atomic_add_fetch(&brook::g_chromeCaptureStats.bytesKept,bytes-copied,__ATOMIC_RELAXED);
                        }
                        if(!result && !(tracked?brook::WmDesktopLayersCaptureCurrent(*snapshot):brook::WmDesktopLayersCurrent(*snapshot)))result=-EAGAIN;
                    }
                }
                kfree(pixels);
                if(!result && (op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE || keyed || keep))
                    result=brook::BdwOffscreenRegisterChrome(owner,source);
                if(!result){
                    layer.incarnation=source.incarnation;layer.generation=source.generation;layer.bytes=bytes;
                    layer.width=layer.stride=layer.clientWidth=chrome.width;layer.height=layer.clientHeight=chrome.height;
                    layer.clientX=chrome.x;layer.clientY=chrome.y;layer.upscale=1;layer.flags=0;wire->count=1;
                    // A chrome-less window has no pixels; its key is still
                    // meaningful (0 bytes) so the caller can keep it too.
                    wire->reserved=(keyed||keep)?(renderedKey?renderedKey:chrome.key):0;
                    if(!CopyToUser(address,wire,sizeof(*wire)))result=-EFAULT;
                }
            }
            brook::WmReleaseDesktopLayers(snapshot);
        }
    }
    kfree(snapshot);kfree(wire);return result;
}
static int64_t CaptureDesktopBackdrop(uint64_t owner,uint64_t op,uint64_t address,uint64_t size)
{
    auto* wire=static_cast<BrookDesktopLayers*>(kmalloc(sizeof(BrookDesktopLayers)));
    auto* scratch=static_cast<uint32_t*>(kmalloc(4096));
    if(!wire || !scratch){kfree(wire);kfree(scratch);return -ENOMEM;}
    int64_t result=-EFAULT;
    if(CopyFromUser(wire,address,sizeof(*wire)) && UserBufferWritable(address,sizeof(*wire))){
        brook::CompositorBackdropInfo info{};
        if(wire->version!=BROOK_DESKTOP_LAYERS_VERSION || wire->reserved)result=-EINVAL;
        else if(!brook::CompositorGetBackdropInfo(&info))result=-ENODEV;
        // The scene rule (gpu_scene.h), as the client-layer export: no fixed byte total.
        else if(info.width>brook::GPU_SCENE_MAX_DIMENSION || info.height>brook::GPU_SCENE_MAX_DIMENSION)result=-E2BIG;
        else {
            auto& layer=wire->layers[0];const uint64_t bytes=uint64_t(info.width)*info.height*4;
            const bool capture=op==BROOK_DESKTOP_BACKDROP_CAPTURE ||
                op==BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE;
            result=op==BROOK_DESKTOP_BACKDROP_TRACKED_BIND?
                brook::BdwOffscreenRegisterBackdrop(owner,info):0;
            if(capture && (wire->count!=1 || layer.incarnation!=info.display || layer.generation!=info.generation ||
               layer.width!=info.width || layer.height!=info.height || layer.stride!=info.width || layer.bytes!=bytes))result=-EAGAIN;
            else if(capture && (!layer.pixels || layer.capacity<bytes || layer.pixels>~uint64_t{0}-bytes ||
                    (layer.pixels<address+size && address<layer.pixels+bytes)))result=-EINVAL;
            if(!result && capture){
                for(uint32_t y=0;y<info.height && !result;++y)for(uint32_t x=0;x<info.width;){
                    const uint32_t count=info.width-x<1024?info.width-x:1024;
                    if(!brook::CompositorReadBackdropSpan(info,y,x,count,scratch)){result=-EAGAIN;break;}
                    if(!CopyToUser(layer.pixels+(uint64_t(y)*info.width+x)*4,scratch,uint64_t(count)*4)){result=-EFAULT;break;}
                    x+=count;
                }
                brook::CompositorBackdropInfo after{};
                if(!result && (!brook::CompositorGetBackdropInfo(&after) || after.display!=info.display ||
                   after.generation!=info.generation || after.width!=info.width || after.height!=info.height))result=-EAGAIN;
                if(!result && op==BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE)
                    result=brook::BdwOffscreenRegisterBackdrop(owner,info);
            }
            if(!result){
                layer.incarnation=info.display;layer.generation=info.generation;layer.bytes=bytes;
                layer.width=layer.stride=layer.clientWidth=info.width;layer.height=layer.clientHeight=info.height;
                layer.window=0;layer.clientX=layer.clientY=0;layer.upscale=1;layer.flags=0;wire->count=1;
                if(!CopyToUser(address,wire,sizeof(*wire)))result=-EFAULT;
            }
        }
    }
    kfree(scratch);kfree(wire);return result;
}
#endif
int64_t sys_brook_desktop_layers(uint64_t op,uint64_t address,uint64_t size,uint64_t a,uint64_t b,uint64_t c)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(!brook::BdwRenderExperimentEnabled())return -ENODEV;
    const uint64_t owner=proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation;
    if (DesktopLayerContentOperation(op) && !DesktopCaptureOwner(proc, owner)) return -EPERM;
    if(op==BROOK_DESKTOP_FOCUS_QUERY){
        if((size!=sizeof(BrookDesktopFocus) && size!=BROOK_DESKTOP_FOCUS_V1_SIZE) || a || b || c || !address ||
           address>~uint64_t{0}-size)return -EINVAL;
        BrookDesktopFocus focus{};
        focus.version=size==sizeof(BrookDesktopFocus)?BROOK_DESKTOP_FOCUS_VERSION_POINTER:BROOK_DESKTOP_FOCUS_VERSION;
        (void)brook::WmSnapshotFocus(&focus.window,&focus.incarnation,&focus.state);
        focus.policy=brook::DesktopPresentPolicy();focus.deferUs=brook::DesktopPresentDeferUs();
        if(size==sizeof(BrookDesktopFocus)){
            MouseGetPosition(&focus.pointerX,&focus.pointerY);
            focus.shell=(brook::WmLauncherVisible()?BROOK_DESKTOP_SHELL_LAUNCHER:0u)|(brook::WmVolumePopupVisible()?BROOK_DESKTOP_SHELL_VOLUME:0u);
        }
        return CopyToUser(address,&focus,size)?0:-EFAULT;
    }
    if(op==BROOK_DESKTOP_RENDERER_CAPS){
        // Stage 1.5: the renderer's backdrop-blur capability, used while it
        // holds the present lease (owner as in the capture ops).
        if(address || size || a!=BROOK_DESKTOP_CAPS_VERSION || b>0xffffffffULL || c)return -EINVAL;
        return CompositorSetRendererCapabilities(proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation,
                                                 static_cast<uint32_t>(b));
    }
    if(op==BROOK_DESKTOP_CLIENT_FRAME){
        if(address || size || !a || a>BROOK_DESKTOP_LAYER_LIMIT || !b || !c)return -EINVAL;
        brook::WmNoteClientFrame(b,c);return 0;
    }
    if(op==BROOK_DESKTOP_SURFACES_QUERY){
        if(size!=sizeof(BrookDesktopSurfaces) || a || b || c || !address || address>~uint64_t{0}-size)return -EINVAL;
        auto* wire=static_cast<BrookDesktopSurfaces*>(kmalloc(sizeof(BrookDesktopSurfaces)));
        if(!wire)return -ENOMEM;
        int64_t result=-EFAULT;
        if(CopyFromUser(wire,address,sizeof(*wire))){
            if(wire->version!=BROOK_DESKTOP_SURFACES_VERSION || wire->reserved)result=-EINVAL;
            else {
                const uint32_t version=wire->version;__builtin_memset(wire,0,sizeof(*wire));wire->version=version;
                wire->count=brook::WmDesktopSurfaceRecords(wire->surfaces,BROOK_DESKTOP_LAYER_LIMIT);
                result=CopyToUser(address,wire,sizeof(*wire))?0:-EFAULT;
            }
        }
        kfree(wire);return result;
    }
    if((op==BROOK_DESKTOP_CHROME_QUERY || op==BROOK_DESKTOP_CHROME_CAPTURE || op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE ||
        op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_KEYED || op==BROOK_DESKTOP_CHROME_TRACKED_KEEP ||
        op==BROOK_DESKTOP_CHROME_TRACKED_CAPTURE_SPANS) &&
       size==sizeof(BrookDesktopLayers) && !a && !b && !c && address<=~uint64_t{0}-size)
        return CaptureDesktopChrome(owner,op,address,size);
    if((op==BROOK_DESKTOP_BACKDROP_QUERY || op==BROOK_DESKTOP_BACKDROP_CAPTURE ||
        op==BROOK_DESKTOP_BACKDROP_TRACKED_CAPTURE || op==BROOK_DESKTOP_BACKDROP_TRACKED_BIND) &&
       size==sizeof(BrookDesktopLayers) && !a && !b && !c && address<=~uint64_t{0}-size)
        return CaptureDesktopBackdrop(owner,op,address,size);
    const bool keepOp=op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_KEEP;
    // c: BROOK_DESKTOP_LAYERS_CULL_OCCLUDED with the keep op only.
    const bool cull=keepOp && c==BROOK_DESKTOP_LAYERS_CULL_OCCLUDED;
    const bool tracked=op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED || op==BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE || keepOp;
    if((op!=BROOK_DESKTOP_LAYERS_QUERY && op!=BROOK_DESKTOP_LAYERS_CAPTURE && !tracked) ||
       size!=sizeof(BrookDesktopLayers) || (op!=BROOK_DESKTOP_LAYERS_CAPTURE_TRACKED_SELECTIVE && !keepOp && a) ||
       a>0xffffffffULL || (!keepOp && b) || b>0xffffffffULL || (c && !cull) || address>~uint64_t{0}-size)return -EINVAL;
    const bool capture=op!=BROOK_DESKTOP_LAYERS_QUERY;
    auto* wire=static_cast<BrookDesktopLayers*>(kmalloc(sizeof(BrookDesktopLayers)));
    auto* layers=static_cast<brook::WmDesktopLayers*>(kmalloc(sizeof(brook::WmDesktopLayers)));
    uint64_t scratchBytes=65536;
    auto* scratch=capture?static_cast<uint8_t*>(kmalloc(scratchBytes)):nullptr;
    if(capture && !scratch){scratchBytes=4096;scratch=static_cast<uint8_t*>(kmalloc(scratchBytes));}
    if(!wire || !layers || (capture && !scratch)){
        kfree(wire);kfree(layers);kfree(scratch);return -ENOMEM;
    }
    int64_t result=-EFAULT;
    if(CopyFromUser(wire,address,sizeof(*wire)) && UserBufferWritable(address,sizeof(*wire))){
        if(!brook::WmAcquireDesktopLayers(layers))result=-95;
        else {
            auto sink=[&](const brook::WmDesktopLayer& layer,uint64_t offset,uint64_t destination,uint64_t bytes){
                // Never let pixel output overwrite the request/metadata.
                if(destination<address+size && address<destination+bytes)return false;
                if(!brook::UserMappingBufferRead(layer.backing,offset,scratch,bytes))return false;
                brook::DesktopInvertCaret(layer,offset,scratch,bytes);
                return CopyToUser(destination,scratch,bytes);
            };
            brook::DesktopCaptureCounters tally{};
            brook::DesktopLayerExportOptions options{};options.cullOccluded=cull;
            brook::CompositorBackdropInfo output{};
            if(brook::CompositorGetBackdropInfo(&output))options.copyLimit=brook::DesktopLayerCopyLimit(output.width,output.height);
            result=brook::ExportDesktopLayers(*layers,*wire,capture,sink,
                [tracked](const brook::WmDesktopLayers& current){
                    return tracked
                        ?brook::WmDesktopLayersCaptureCurrent(current)
                        :brook::WmDesktopLayersCurrent(current);
                },scratchBytes,static_cast<uint32_t>(a),static_cast<uint32_t>(b),&tally,options);
            if(!result && tally.calls){auto& g=brook::g_desktopCaptureStats;
                __atomic_add_fetch(&g.calls,tally.calls,__ATOMIC_RELAXED);__atomic_add_fetch(&g.keeps,tally.keeps,__ATOMIC_RELAXED);
                __atomic_add_fetch(&g.keepMisses,tally.keepMisses,__ATOMIC_RELAXED);__atomic_add_fetch(&g.captures,tally.captures,__ATOMIC_RELAXED);
                __atomic_add_fetch(&g.bytesCopied,tally.bytesCopied,__ATOMIC_RELAXED);__atomic_add_fetch(&g.bytesKept,tally.bytesKept,__ATOMIC_RELAXED);
                __atomic_add_fetch(&g.occluded,tally.occluded,__ATOMIC_RELAXED);__atomic_add_fetch(&g.bytesOccluded,tally.bytesOccluded,__ATOMIC_RELAXED);}
            bool adopted=false;
            if(!result && tracked){
                result=brook::BdwOffscreenRegisterScene(owner,layers,&wire->reserved);adopted=!result;
            }
            if(!result && !CopyToUser(address,wire,sizeof(*wire)))result=-EFAULT;
            if(!adopted)brook::WmReleaseDesktopLayers(layers);
            else layers=nullptr;
        }
    }
    kfree(scratch);kfree(layers);kfree(wire);return result;
#else
    (void)op;(void)address;(void)size;(void)a;(void)b;(void)c;return -ENODEV;
#endif
}

// 0xB06/0xB07: capability-based retained scenes. See gpu_scene_abi.h.
int64_t sys_brook_scene_create(uint64_t window,uint64_t abi,uint64_t flags,
                               uint64_t a,uint64_t b,uint64_t c)
{
    auto* proc=ProcessCurrent();if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (!window || window>WM_MAX_WINDOWS || abi!=SCENE_ABI_VERSION || flags || a || b || c) return -EINVAL;
    int error=0;auto* handle=SceneCreate(proc,static_cast<uint32_t>(window),&error);
    if (!handle) return error;
    FdEntry owned{};owned.type=FdType::CompositorScene;owned.handle=handle;owned.fdFlags=1;
    SceneHandleRef(handle); // FD publication owns a reference independently of this call.
    const int fd=FdPublishNew(proc,owned,0);
    if (fd<0) { SceneHandleUnref(handle);SceneHandleUnref(handle);return fd; }
    const bool activated=SceneActivate(handle);
    if (!activated) {
        // The published slot may already have been closed/reused by a sibling.
        // Retain the constructor handle while comparing, then pin the exact
        // slot so rollback cannot close an unrelated replacement descriptor.
        FdRef published(proc,fd);
        if (published && published->type==FdType::CompositorScene && published->handle==handle)
            CloseProcessFd(proc,fd);
        SceneHandleUnref(handle);return -116;
    }
    SceneHandleUnref(handle);
    return fd;
}
int64_t sys_brook_scene_ctl(uint64_t fd,uint64_t op,uint64_t arg,uint64_t size,
                            uint64_t flags,uint64_t reserved)
{
    auto* proc=ProcessCurrent();if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (fd>=MAX_FDS) return -EBADF;
    if (flags || reserved) return -EINVAL;
    FdRef ref(proc,static_cast<int>(fd));
    if (!ref || ref->type!=FdType::CompositorScene || !ref->handle) return -EBADF;
    auto* handle=static_cast<SceneHandle*>(ref->handle);
    if (op==SCENE_CTL_STATS) {
        if (size!=sizeof(SceneStatsDesc)) return -EINVAL;
        SceneStatsDesc stats{};SceneGetStats(handle,stats);
        return CopyToUser(arg,&stats,sizeof(stats))?0:-EFAULT;
    }
    if (op==SCENE_CTL_RELEASE) {
        if (size) return -EINVAL;
        return SceneRelease(handle,arg);
    }
    if (op==SCENE_CTL_UPLOAD) {
        if (size!=sizeof(SceneUploadDesc)) return -EINVAL;
        SceneUploadDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        return SceneUpload(proc,handle,desc);
    }
    if (op==SCENE_CTL_CAPTURE) {
        if (size!=sizeof(SceneCaptureDesc)) return -EINVAL;
        SceneCaptureDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved) return -EINVAL;
        FdRef drm(proc,desc.drmFd);
        if (!drm || drm->type!=FdType::DevDri || !drm->handle) return -EBADF;
        MappingBackingGuard lease;
        int32_t producer;uint32_t resource,format;
        {
            auto* context=static_cast<DrmCtx*>(drm->handle);DrmMappingGuard guard(context);
            auto* gem=DrmGemGet(context,desc.gem);
            if (!gem || gem->resource->owner->config.capsetId==4 || gem->resource->target!=2 || !GpuCaptureFormatSupported(gem->resource->format) ||
                desc.width>gem->resource->width || desc.height>gem->resource->height) return -EINVAL;
            auto* image=gem->resource;MappingBackingRef(image->lifetime);lease.value=image->lifetime;
            producer=image->owner->ctxId;resource=image->gres;format=image->format;
        }
        return SceneCapture(proc,handle,desc,lease.value,producer,resource,format);
    }
    if (op==SCENE_CTL_CAPTURE_DMABUF) {
        if(size!=sizeof(SceneDmabufCaptureDesc))return -EINVAL;
        SceneDmabufCaptureDesc desc{};if(!CopyFromUser(&desc,arg,sizeof(desc)))return -EFAULT;
        const BrookDmabufLayout layout{desc.width,desc.height,desc.fourcc,desc.stride,desc.offset,desc.flags,desc.modifier};
        FdRef drm(proc,desc.drmFd);
        if(!drm || drm->type!=FdType::DevDri || !drm->handle)return -EBADF;
        MappingBackingGuard lease;DrmResource* image=nullptr;
        {
            auto* context=static_cast<DrmCtx*>(drm->handle);DrmMappingGuard guard(context);
            auto* gem=DrmGemGet(context,desc.gem);if(!gem)return -EINVAL;
            image=gem->resource;
            if(!__atomic_load_n(&image->everExported,__ATOMIC_ACQUIRE))return -EINVAL;
            const bool venus=image->owner->config.capsetId==4;
            if(BrookDmabufValidate(&layout,image->size,!venus)!=BROOK_DMABUF_LAYOUT_OK)return -EINVAL;
            if(!venus && (image->target!=2 || desc.width!=image->width || desc.height!=image->height ||
               desc.stride!=image->stride || desc.offset ||
               !GpuCaptureViewSupported(image->format,desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u))) {
                SerialPrintf("drm: rejected image view: target=%u backing=%ux%u stride=%u format=%u view=%ux%u stride=%u fourcc=%x offset=%u\n",
                             image->target,image->width,image->height,image->stride,image->format,
                             desc.width,desc.height,desc.stride,desc.fourcc,desc.offset);
                return -EINVAL;
            }
            if(venus && (!image->hostBlob || !(image->blobFlags&2u)))return -EINVAL;
            MappingBackingRef(image->lifetime);lease.value=image->lifetime;
        }
        const bool venus=image->owner->config.capsetId==4;
        if(venus) {
            // Only the current frame's imported WRITE fence is evidence of
            // Venus queue completion. A prior capture or ring0 ack is not.
            KMutexLock(&image->reservationLock);
            const bool admitted=image->reservation.BeginCapture();
            KMutexUnlock(&image->reservationLock);
            if(!admitted)return -11; // EAGAIN, producer synchronization absent
        }
        const SceneCaptureDesc capture{desc.drmFd,desc.gem,desc.width,desc.height,
                                      desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u,0};
        // VirGL BGRA/BGRX values, distinct from the scene's alpha policy.
        const uint32_t format=venus?(desc.fourcc==BROOK_DMABUF_ARGB8888?1u:2u):image->format;
        return SceneCapture(proc,handle,capture,lease.value,image->owner->ctxId,image->gres,format,
                            venus?&layout:nullptr,venus?SceneCaptureSync{image,DrmCaptureComplete}:SceneCaptureSync{});
    }
    if (op==SCENE_CTL_CLONE) {
        if (size!=sizeof(SceneCloneDesc)) return -EINVAL;
        SceneCloneDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved) return -EINVAL;
        if (desc.sourceFd<0 || static_cast<uint32_t>(desc.sourceFd)>=MAX_FDS) return -EBADF;
        FdRef source(proc,desc.sourceFd);
        if (!source || source->type!=FdType::CompositorScene || !source->handle) return -EBADF;
        return SceneClone(proc,handle,static_cast<SceneHandle*>(source->handle),desc.version);
    }
    if (op==SCENE_CTL_PRESENT) {
        if (size!=sizeof(ScenePresentDesc)) return -EINVAL;
        ScenePresentDesc desc{};if (!CopyFromUser(&desc,arg,sizeof(desc))) return -EFAULT;
        if (desc.reserved || desc.count>GPU_SCENE_MAX_LAYERS) return -EINVAL;
        auto* layers=static_cast<SceneLayerDesc*>(kmalloc(sizeof(SceneLayerDesc)*GPU_SCENE_MAX_LAYERS));
        if (!layers) return -ENOMEM;
        if (desc.count && !CopyFromUser(layers,desc.layers,sizeof(SceneLayerDesc)*desc.count)) {
            kfree(layers);return -EFAULT;
        }
        const int64_t result=ScenePresent(proc,handle,desc,layers);kfree(layers);return result;
    }
    return -EINVAL;
}

// Root-only completed-frame copy. The compositor lock never spans user memory
// faults, network I/O or filesystem I/O. Process incarnation prevents PID reuse
// from inheriting another process's capture; abandoned leases expire boundedly.
int64_t sys_brook_debug_frame(uint64_t op,uint64_t token,uint64_t destination,uint64_t length,uint64_t offset,uint64_t reserved)
{
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    if(proc->euid!=0)return -EPERM;
    KernelOperationGuard operation(proc);
    if(reserved || token>0xffffffffULL)return -EINVAL;
    const uint64_t owner=proc->incarnation;
    if(op==0){
        if(token||destination||length||offset)return -EINVAL;
        const auto frame=CompositorRequestDebugFrame(owner);
        if(frame.state==DebugFrameState::Busy)return -16; // EBUSY
        return frame.token?int64_t(frame.token):-EIO;
    }
    if(op==1){
        if(offset || length!=sizeof(BrookDebugFrameInfo))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116; // ESTALE
        BrookDebugFrameInfo info{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==4){
        if(offset || length!=sizeof(BrookDebugFrameInfoV2))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116;
        BrookDebugFrameInfoV2 info{{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch},frame.source,0};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==5){
        if(offset || length!=sizeof(BrookDebugFrameInfoV3))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116;
        BrookDebugFrameInfoV3 info{{{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch},frame.source,0},frame.presentationMode,0};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==BROOK_DEBUG_FRAME_REQUEST){
        if(token||offset||length!=sizeof(BrookDebugFrameRequest))return -EINVAL;
        BrookDebugFrameRequest request{};
        if(!CopyFromUser(&request,destination,sizeof(request)))return -EFAULT;
        constexpr uint32_t known = BROOK_DEBUG_FRAME_FLAG_CURSOR | BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER |
                                   BROOK_DEBUG_FRAME_FLAG_WINDOW_CROP;
        const bool window = (request.flags & (BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER | BROOK_DEBUG_FRAME_FLAG_WINDOW_CROP)) != 0;
        if ((request.flags & ~known) ||
            ((request.flags & BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER) && request.flags != BROOK_DEBUG_FRAME_FLAG_WINDOW_BUFFER) ||
            (window ? (!request.window_id || !request.window_incarnation) : (request.window_id || request.window_incarnation)))
            return -EINVAL;
        int error=0;
        const auto frame=CompositorRequestDebugFrameOptions(owner,request.flags,request.window_id,request.window_incarnation,&error);
        if(frame.state==DebugFrameState::Busy)return -16; // EBUSY
        if(error)return error;
        return frame.token?int64_t(frame.token):-EIO;
    }
    if(op==BROOK_DEBUG_FRAME_INFO_V4){
        if(offset || length!=sizeof(BrookDebugFrameInfoV4))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116;
        BrookDebugFrameInfoV4 info{{{{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch},frame.source,0},frame.presentationMode,0},
            frame.plane,frame.planeSurface,frame.planeControl,frame.planeStride,frame.infoFlags,frame.windowId,
            frame.windowScale?frame.windowScale:1,frame.copyUs};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==2){
        if(!length || length>BROOK_DEBUG_FRAME_CHUNK)return -EINVAL;
        void* staged=kmalloc(uint32_t(length));if(!staged)return -ENOMEM;
        int64_t result=CompositorReadDebugFrame(uint32_t(token),owner,offset,staged,uint32_t(length));
        if(result>0 && !CopyToUser(destination,staged,uint32_t(result)))result=-EFAULT;
        kfree(staged);return result;
    }
    if(op==3){
        if(destination||length||offset)return -EINVAL;
        return CompositorReleaseDebugFrame(uint32_t(token),owner)?0:-116; // ESTALE
    }
    return -EINVAL;
}

// Experimental renderer overlay uses the immutable compositor-thread frame
// lease; it shares the single capture slot with root debug screenshots.
int64_t sys_brook_desktop_overlay(uint64_t op,uint64_t token,uint64_t destination,uint64_t length,uint64_t offset,uint64_t reserved)
{
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    auto* proc=ProcessCurrent();if(!proc)return -ESRCH;
    KernelOperationGuard operation(proc);
    if(!brook::BdwRenderExperimentEnabled())return -ENODEV;
    if(token>0xffffffffULL || (reserved &&
       (op!=BROOK_DESKTOP_OVERLAY_CURSOR ||
        (reserved!=BROOK_DESKTOP_CURSOR_PLANE_V1 && reserved!=BROOK_DESKTOP_CURSOR_PLANE_V2 &&
         reserved!=BROOK_DESKTOP_CURSOR_PLANE_V3))))return -EINVAL;
    const uint64_t owner=proc->leaderIncarnation?proc->leaderIncarnation:proc->incarnation;
    if(op==0){
        if(token || destination || length || (offset & ~uint64_t(BROOK_DESKTOP_OVERLAY_NO_CURSOR)))return -EINVAL;
        const auto frame=CompositorRequestDebugFrame(owner,true,!(offset&BROOK_DESKTOP_OVERLAY_NO_CURSOR));
        if(frame.state==DebugFrameState::Busy)return -16;
        return frame.token?int64_t(frame.token):-EIO;
    }
    if(op==1){
        if(offset || length!=sizeof(BrookDebugFrameInfo))return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state==DebugFrameState::Expired)return -116;
        BrookDebugFrameInfo info{frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==2){
        if(!length || length>BROOK_DEBUG_FRAME_CHUNK)return -EINVAL;
        void* staged=kmalloc(uint32_t(length));if(!staged)return -ENOMEM;
        int64_t result=CompositorReadDebugFrame(uint32_t(token),owner,offset,staged,uint32_t(length));
        if(result>0 && !CopyToUser(destination,staged,uint32_t(result)))result=-EFAULT;
        kfree(staged);return result;
    }
    if(op==3){
        if(destination || length || offset)return -EINVAL;
        return CompositorReleaseDebugFrame(uint32_t(token),owner)?0:-116;
    }
    if(op==4){
        if(offset || length!=sizeof(BrookDesktopOverlayBands))return -EINVAL;
        DesktopOverlayBands bands{};const int result=CompositorOverlayBands(uint32_t(token),owner,bands);
        if(result)return result;
        BrookDesktopOverlayBands info{};info.count=bands.count;
        for(uint32_t i=0;i<bands.count;++i)info.bands[i]={bands.bands[i].x,bands.bands[i].y,bands.bands[i].width,bands.bands[i].height};
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==BROOK_DESKTOP_OVERLAY_BANDS_V2){
        if(offset || length!=sizeof(BrookDesktopOverlayBandsV2))return -EINVAL;
        DesktopOverlayBands bands{};const int result=CompositorOverlayBands(uint32_t(token),owner,bands);
        if(result)return result;
        BrookDesktopOverlayBandsV2 info{};info.bands.count=bands.count;info.opaque=bands.opaque;
        for(uint32_t i=0;i<bands.count;++i){
            info.bands.bands[i]={bands.bands[i].x,bands.bands[i].y,bands.bands[i].width,bands.bands[i].height};
            info.keys[i]=bands.keys[i];
        }
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==BROOK_DESKTOP_OVERLAY_SURFACES){
        if(offset || (length!=sizeof(BrookDesktopOverlaySurfaces) &&
                      length!=sizeof(BrookDesktopOverlaySurfacesLegacy)))return -EINVAL;
        BrookDesktopOverlaySurfaces info{};const int result=CompositorOverlaySurfaces(uint32_t(token),owner,info);
        if(result)return result;
        if(length==sizeof(BrookDesktopOverlaySurfacesLegacy) && info.count>BROOK_DESKTOP_OVERLAY_LEGACY_SURFACES)
            return -75; // EOVERFLOW: legacy renderer uses the complete raster bands
        return CopyToUser(destination,&info,length)?0:-EFAULT;
    }
    if(op==BROOK_DESKTOP_OVERLAY_GENERATION){
        if(token || destination || length || offset)return -EINVAL;
        return int64_t(CompositorOverlayGeneration()&0x7fffffffffffffffULL);
    }
    if(op==BROOK_DESKTOP_OVERLAY_WAIT){
        if(length!=sizeof(BrookDesktopOverlayInfo) || offset>BROOK_DESKTOP_OVERLAY_WAIT_MAX_MS)return -EINVAL;
        DebugFrameSnapshot frame{};
        if(CompositorWaitDebugFrame(uint32_t(token),owner,uint32_t(offset),frame))return -116;
        BrookDesktopOverlayInfo info{};
        info.frame={frame.token,uint32_t(frame.state),frame.width,frame.height,frame.stride,
            BROOK_DEBUG_FRAME_BGRA8,uint64_t(frame.width)*frame.height*4,frame.tick,frame.epoch};
        info.generation=frame.state==DebugFrameState::Ready?frame.overlayGeneration:0;
        return CopyToUser(destination,&info,sizeof(info))?0:-EFAULT;
    }
    if(op==BROOK_DESKTOP_OVERLAY_CURSOR_PRESENTED){
        if(token || destination || length || !offset)return -EINVAL;
        brook::BdwCursorRendererPresented(owner,offset);
        return 0;
    }
    if(op==BROOK_DESKTOP_OVERLAY_CURSOR){
        if(token || (length!=sizeof(BrookDesktopOverlayCursor) && length!=sizeof(BrookDesktopOverlayCursorLarge)))
            return -EINVAL;
        const auto deliver=[&](auto* cursor) -> int64_t {
            CompositorOverlayCursor(*cursor,offset);
            bool suppress=false;
            const uint64_t epoch=brook::BdwCursorRendererQuery(owner,reserved==BROOK_DESKTOP_CURSOR_PLANE_V3,suppress);
            if(suppress)cursor->visible=0;
            const uint64_t bytes=cursor->serial==offset?offsetof(BrookDesktopOverlayCursor,pixels):sizeof(*cursor);
            return CopyToUser(destination,cursor,bytes)?int64_t(epoch):-EFAULT;
        };
        static_assert(offsetof(BrookDesktopOverlayCursor,pixels)==offsetof(BrookDesktopOverlayCursorLarge,pixels));
        void* cursor=kmalloc(length);
        if(!cursor)return -ENOMEM;
        const int64_t result=length==sizeof(BrookDesktopOverlayCursorLarge)
            ?deliver(static_cast<BrookDesktopOverlayCursorLarge*>(cursor))
            :deliver(static_cast<BrookDesktopOverlayCursor*>(cursor));
        kfree(cursor);
        return result;
    }
    if(op==BROOK_DESKTOP_OVERLAY_READ_BAND){
        DesktopOverlayBands bands{};int result=CompositorOverlayBands(uint32_t(token),owner,bands);
        if(result)return result;
        if(offset>=bands.count)return -EINVAL;
        const auto frame=CompositorDebugFrameStatus(uint32_t(token),owner);
        if(frame.state!=DebugFrameState::Ready)return -116;
        const auto& band=bands.bands[offset];const uint64_t bytes=uint64_t(band.width)*band.height*4;
        if(!destination || !bytes || bytes!=length || bytes>64ull*1024*1024 || destination>~uint64_t{0}-bytes)return -EINVAL;
        if(!UserBufferWritable(destination,bytes))return -EFAULT;
        auto* scratch=static_cast<uint8_t*>(kmalloc(4096));if(!scratch)return -ENOMEM;
        int64_t failure=-EFAULT;
        const bool copied=DesktopOverlayPackedRead(band,frame.width,frame.height,4096,
            [&](uint64_t from,uint64_t to,uint32_t n){
                const int64_t read=CompositorReadDebugFrame(uint32_t(token),owner,from,scratch,n);
                if(read!=n){failure=read<0?read:-EIO;return false;}
                if(!CopyToUser(destination+to,scratch,n)){failure=-EFAULT;return false;}
                return true;
            });
        kfree(scratch);return copied?int64_t(bytes):failure;
    }
    return -EINVAL;
#else
    (void)op;(void)token;(void)destination;(void)length;(void)offset;(void)reserved;return -ENODEV;
#endif
}

} // namespace brook
