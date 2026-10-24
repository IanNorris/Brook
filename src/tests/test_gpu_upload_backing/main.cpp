// Fault-inject actual driver upload, retirement and context-destroy bodies.
// Only host commands, VMM, context lookup and command lock are mocked.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include "../../drivers/virtio_gpu/upload_backing.h"
#include "../../drivers/virtio_gpu/retirement_protocol.h"
#include "../../drivers/virtio_gpu/resource_ids.h"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line%d %s\n",__LINE__,#x);std::abort();}}while(0)
using AppUpload=brook::GpuUploadBacking;
namespace brook { struct GpuContextConfig {unsigned capsetId=1,numRings=0,pollRingsMask=0;}; }
struct AppCtx { bool used;uint32_t ctxId,pid;brook::GpuContextConfig config;uint64_t incarnation; };
static constexpr unsigned MAX_APP_CTX=1;
static AppCtx g_appCtx[MAX_APP_CTX]{{true,256,100,{},1}};
static AppCtx& context=g_appCtx[0];
static uint64_t g_appContextIncarnation=1;
static constexpr int APP_FIRST_CTX=256,MAX_APP_UPLOADS=64;
static AppUpload g_appUploads[MAX_APP_UPLOADS];
static bool g_controlQueuePoisoned,g_compReady=true;
static constexpr int COMP_CTX_ID=2;
static unsigned depth,operation,failAt=~0u,freed;
static bool attached,contextAttached,hostCreated,producerDone,compositorDone;
static void* allocation;
static std::vector<void*> liveAllocations;
struct HostResource {uint64_t vaddr=0;bool created=false,attached=false,context=false;};
static std::map<uint32_t,HostResource> resources;
static std::vector<unsigned> commands;
struct GpuCmdGuard {GpuCmdGuard(){CHECK(!depth);++depth;}~GpuCmdGuard(){--depth;}};
struct VirtualAddress {uint64_t value;explicit VirtualAddress(uint64_t n):value(n){} explicit operator bool()const{return value;}uint64_t raw()const{return value;}};
static constexpr int VMM_WRITABLE=1,KernelPid=0,VIRGL_BIND_VERTEX_BUFFER=1;
namespace MemTag {static constexpr int Device=1;}
static VirtualAddress VmmAllocPages(uint64_t pages,int,int,int) {allocation=std::calloc(pages,4096);liveAllocations.push_back(allocation);return VirtualAddress(reinterpret_cast<uint64_t>(allocation));}
static void VmmFreePages(VirtualAddress va,uint64_t pages) {CHECK(pages==1);for(const auto& r:resources)if(r.second.vaddr==va.raw())CHECK(!r.second.attached);void* pointer=reinterpret_cast<void*>(va.raw());
 bool found=false;for(auto& p:liveAllocations)if(p==pointer){p=nullptr;found=true;break;}CHECK(found);
 std::free(pointer);allocation=nullptr;for(auto p:liveAllocations)if(p)allocation=p;++freed;}
static AppCtx* AppCtxFind(int32_t ctx) {return ctx==APP_FIRST_CTX&&context.used?&context:nullptr;}
static brook::GpuAppResourceIds<> g_appResourceIds;
static bool AppResValid(int32_t ctx,uint32_t resource){return ctx==256 && g_appResourceIds.ContainsIssued(0,resource);}
static void SerialPrintf(const char*,...) {}
static void SerialPuts(const char*) {}
static bool advance(unsigned command){CHECK(depth==1);commands.push_back(command);return operation++!=failAt;}
static bool ResourceCreateBuffer(uint32_t resource,int,uint32_t){CHECK(!resources.count(resource));resources[resource].created=true;hostCreated=true;return advance(1);}
static bool ResourceAttachBackingVirt(uint32_t resource,uint64_t va,uint32_t){
 bool found=false;
 for(auto& u:g_appUploads)if(u.used&&u.resource==resource)found=u.published&&u.vaddr==va;
 CHECK(found);
 resources.at(resource).vaddr=va;resources.at(resource).attached=true;attached=true;return advance(2); // even a failed response can follow publication
}
static bool CtxAttachResource(uint32_t,uint32_t resource){resources.at(resource).context=true;contextAttached=true;return advance(3);}
static bool TransferToHostBuffer(uint32_t,uint32_t resource,uint32_t){CHECK(resources.at(resource).attached&&resources.at(resource).context);return advance(4);}
static bool AppFenceLocked(uint32_t ctx){bool ok=advance(ctx==COMP_CTX_ID?6:5);if(ok)(ctx==COMP_CTX_ID?compositorDone:producerDone)=true;return ok;}
struct AppBlob {};
static AppBlob* AppBlobFind(int32_t,uint32_t){return nullptr;}
static int AppRetireBlobLocked(AppBlob*){std::abort();}
struct VirtioGpuCtrlHdr {uint32_t type,ctx_id;};
struct VirtioGpuCtxResource {VirtioGpuCtrlHdr hdr;uint32_t resource_id;};
static constexpr unsigned VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE=7,VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING=8,VIRTIO_GPU_CMD_RESOURCE_UNREF=9,VIRTIO_GPU_CMD_CTX_DESTROY=10;
alignas(8) static unsigned char g_cmdBuf[128];
static constexpr unsigned CMD_REQ_OFF=0,CMD_RESP_CAP=64;
static bool SubmitCommand(unsigned,unsigned){
 auto* req=reinterpret_cast<VirtioGpuCtrlHdr*>(g_cmdBuf);
 bool ok=advance(req->type);
 if(req->type==7){CHECK(producerDone&&(!g_compReady||compositorDone));if(ok){contextAttached=false;resources.at(reinterpret_cast<VirtioGpuCtxResource*>(req)->resource_id).context=false;}}
 if(req->type==8){auto& r=resources.at(reinterpret_cast<VirtioGpuCtxResource*>(req)->resource_id);CHECK(!r.context);if(ok){attached=false;r.attached=false;}}
 if(req->type==9){auto& r=resources.at(reinterpret_cast<VirtioGpuCtxResource*>(req)->resource_id);CHECK(!r.attached);if(ok){hostCreated=false;r.created=false;}}
 if(req->type==10){for(const auto& r:resources)CHECK(!r.second.created);CHECK(!allocation);}
 return ok;
}
static bool CmdRespOk(bool ok){return ok;}
static bool g_gpu3dWorks=true;
struct Caps {uint64_t capsetMask;};
static Caps AppCapabilities(){return {2};}
static bool CtxCreate(uint32_t,unsigned){return advance(11);}
#include "../../drivers/virtio_gpu/app_ctx_create.inc"
#include "../../drivers/virtio_gpu/app_buffer_upload.inc"
// This fixture has no imported resources; their real registry is exercised by
// test_gpu_shared_contexts, independently of upload ownership fault injection.
static bool AppDetachSharedResourceLocked(int32_t,uint32_t){return true;}
static bool AppReleaseSharedContextLocked(int32_t){return true;}
#include "../../drivers/virtio_gpu/app_retire_resource.inc"
#include "../../drivers/virtio_gpu/app_ctx_destroy.inc"
static void reset(){
 // Mock device has been stopped before discarding quarantined test fixtures.
 for(auto p:liveAllocations)if(p)std::free(p);
 liveAllocations.clear();resources.clear();
 allocation=nullptr;for(auto& u:g_appUploads)u={};context={true,256,100,{},1};g_appContextIncarnation=1;g_appResourceIds={};
 depth=operation=freed=0;failAt=~0u;attached=contextAttached=hostCreated=producerDone=compositorDone=false;
 g_controlQueuePoisoned=false;g_compReady=true;commands.clear();
}
int main(){
 char bytes[64]{};
 for(unsigned fail=0;fail<4;++fail){reset();failAt=fail;
  CHECK(AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes))<0);
  if(!fail)CHECK(freed==1&&!g_appUploads[0].used&&!attached);
  else{
   CHECK(!freed&&allocation&&g_appUploads[0].quarantined&&attached);
   const auto count=commands.size();AppCtxDestroy(APP_FIRST_CTX);
   CHECK(context.used&&commands.size()==count&&!freed);
  }
 }
 // Every retirement failure retains backing and prevents context-slot reuse.
 for(unsigned fail=0;fail<5;++fail){reset();int resource=AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes));CHECK(resource>0);
  failAt=operation+fail;AppCtxDestroy(APP_FIRST_CTX);
  CHECK(context.used&&allocation&&!freed&&g_appUploads[0].quarantined);
  auto count=commands.size();AppCtxDestroy(APP_FIRST_CTX);CHECK(commands.size()==count);
 }
 reset();int resource=AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes));CHECK(resource>0);
 CHECK(AppRetireResource(APP_FIRST_CTX,resource)==0&&freed==1&&!g_appUploads[0].used);
 AppCtxDestroy(APP_FIRST_CTX);CHECK(!context.used&&freed==1);
 reset();CHECK(AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes))>0);
 AppCtxDestroy(APP_FIRST_CTX);CHECK(!context.used&&freed==1);
 CHECK(commands==std::vector<unsigned>({1,2,3,4,5,6,7,8,9,10}));
 // Failed CTX_DESTROY cannot authorize slot reuse even after safe retirement.
 reset();CHECK(AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes))>0);failAt=operation+5;
 AppCtxDestroy(APP_FIRST_CTX);CHECK(context.used&&freed==1);
 // Wrong context incarnation cannot release another incarnation's pages.
 reset();resource=AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes));++context.incarnation;
 CHECK(AppRetireResource(APP_FIRST_CTX,resource)<0&&!freed);auto count=commands.size();
 AppCtxDestroy(APP_FIRST_CTX);CHECK(context.used&&commands.size()==count&&!freed);
 reset();CHECK(AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes))>0);g_controlQueuePoisoned=true;
 AppCtxDestroy(APP_FIRST_CTX);CHECK(context.used&&allocation&&!freed);
 // Execute actual context activation and namespace rebasing through slot reuse.
 reset();context.used=false;int ctx=AppCtxCreate(100);CHECK(ctx==APP_FIRST_CTX);
 auto incarnation=context.incarnation;int old=AppBufferUpload(ctx,bytes,sizeof(bytes));
 AppCtxDestroy(ctx);CHECK(!context.used);ctx=AppCtxCreate(100);
 CHECK(ctx==APP_FIRST_CTX&&context.incarnation>incarnation);
 CHECK(AppRetireResource(ctx,old)<0);
 int next=AppBufferUpload(ctx,bytes,sizeof(bytes));CHECK(next>old);
 AppCtxDestroy(ctx);CHECK(!context.used&&freed==2);
 // First upload retires completely, failure in the second must retain only it.
 reset();CHECK(AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes))>0);
 CHECK(AppBufferUpload(APP_FIRST_CTX,bytes,sizeof(bytes))>0);
 failAt=operation+5;AppCtxDestroy(APP_FIRST_CTX);
 CHECK(context.used&&freed==1&&!g_appUploads[0].used&&g_appUploads[1].quarantined);
 count=commands.size();AppCtxDestroy(APP_FIRST_CTX);CHECK(commands.size()==count&&freed==1);
 reset();std::puts("GPU_UPLOAD_BACKING_ACTUAL_BODY_PASS");
}
