#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>
#include <vector>
#include "../../kernel/src/gpu_app.h"
#include "../../drivers/virtio_gpu/virgl_submit.h"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"SHARED_CONTEXT line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
namespace {
constexpr uint32_t APP_FIRST_CTX=256,MAX_APP_CTX=16,VIRTIO_GPU_CAPSET_VENUS=4;
constexpr uint32_t CMD_REQ_OFF=0,CMD_RESP_CAP=4096,VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE=0x203;
constexpr uint32_t CMD_PAGES=2,VIRTIO_GPU_CMD_SUBMIT_3D=0x207;
constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D=0x205,VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D=0x206;
struct Config{uint32_t capsetId=2;};
struct AppCtx{bool used=false;uint64_t incarnation=0;Config config{};};
AppCtx contexts[MAX_APP_CTX];
struct Header{uint32_t type=0,ctx_id=0;};
struct VirtioGpuCtxResource{Header hdr;uint32_t resource_id=0;};
struct VirtioGpuCmdSubmit{Header hdr;uint32_t size;};
struct Box{uint32_t x,y,z,w,h,d;};
struct VirtioGpuTransferHost3D{Header hdr;uint32_t resource_id;Box box;uint64_t offset;uint32_t level,stride,layer_stride;};
struct Blob{bool used=false;int32_t context=0;};Blob g_appBlobs[1];
alignas(16) uint8_t buffer[8192];uint8_t* g_cmdBuf=buffer;
bool g_controlQueuePoisoned=false,failAllocation=false;
uint32_t calls=0,failAt=0,allocations=0;
std::set<std::pair<uint32_t,uint32_t>> attached,issued;
std::vector<std::pair<char,uint32_t>> operations;
struct GpuCmdGuard{GpuCmdGuard(){} ~GpuCmdGuard(){}};
static AppCtx* AppCtxFind(int32_t id){return id>=256 && id<272 && contexts[id-256].used?&contexts[id-256]:nullptr;}
static bool AppResValid(int32_t id,uint32_t resource){return issued.count({id,resource});}
static void* kmalloc(size_t n){if(failAllocation)return nullptr;++allocations;return std::malloc(n);}
static void kfree(void* p){CHECK(p && allocations);--allocations;std::free(p);}
static bool step(char c,uint32_t id){operations.push_back({c,id});return ++calls!=failAt;}
static bool AppFenceLocked(uint32_t id){return step('F',id);}
static bool CtxAttachResource(uint32_t id,uint32_t resource){if(!step('A',id))return false;attached.insert({id,resource});return true;}
static bool SubmitCommand(uint32_t,uint32_t){auto* r=reinterpret_cast<VirtioGpuCtxResource*>(g_cmdBuf);
 if(r->hdr.type==VIRTIO_GPU_CMD_SUBMIT_3D)return step('S',r->hdr.ctx_id);
 if(r->hdr.type==VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D || r->hdr.type==VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D)return step('T',r->hdr.ctx_id);
 CHECK(r->hdr.type==VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE);if(!step('D',r->hdr.ctx_id))return false;attached.erase({r->hdr.ctx_id,r->resource_id});return true;}
static bool CmdRespOk(bool ok){return ok;}
static void SerialPrintf(const char*,...){}
#include "../../drivers/virtio_gpu/shared_contexts.inc"
#include "../../drivers/virtio_gpu/app_submit3d.inc"
#include "../../drivers/virtio_gpu/app_transfer3d.inc"
static void reset(){
 // Fake device is stopped before reclaiming quarantined association metadata.
 while(g_appSharedResources){auto* n=g_appSharedResources->next;kfree(g_appSharedResources);g_appSharedResources=n;}
 CHECK(!allocations);for(auto& c:contexts)c={};
 contexts[0]={true,1,{2}};contexts[1]={true,2,{2}};contexts[2]={true,3,{2}};
 attached.clear();issued={{256,65536},{256,65552}};calls=failAt=0;operations.clear();
 failAllocation=g_controlQueuePoisoned=false;
}
}
int main(){
 reset();CHECK(AppImportResource(257,256,65536)==0);CHECK(allocations==1 && attached.count({257,65536}));
 CHECK(operations.size()==2 && operations[0].first=='F' && operations[1].first=='A');
 const auto before=calls;CHECK(AppImportResource(257,256,65536)==0 && calls==before);
 CHECK(AppImportResource(256,256,65536)==0 && calls==before);
 bool shared=false;CHECK(AppPrepareSharedUseLocked(256,shared) && shared);
 CHECK(AppPrepareSharedUseLocked(257,shared) && shared);
 CHECK(AppPrepareSharedUseLocked(258,shared) && !shared);
 CHECK(!AppReleaseSharedContextLocked(256)); // Creator cannot leave a live canonical resource.
 CHECK(AppImportResource(258,256,65536)==0);
 CHECK(AppReleaseSharedContextLocked(257));CHECK(!attached.count({257,65536}) && attached.count({258,65536}));
 CHECK(AppDetachSharedResourceLocked(256,65536));CHECK(!allocations && attached.empty());
 // Closing/reopening a destination association cannot leave an old detach that
 // later invalidates the new incarnation; confirmed detach precedes reuse.
 CHECK(AppImportResource(257,256,65536)==0);CHECK(AppReleaseSharedContextLocked(257));
 contexts[1].incarnation=10;CHECK(AppImportResource(257,256,65536)==0);
 CHECK(g_appSharedResources->importers[1]==10);CHECK(AppDetachSharedResourceLocked(256,65536));
 reset();contexts[1].config.capsetId=4;CHECK(AppImportResource(257,256,65536)<0 && !allocations);
 reset();failAllocation=true;CHECK(AppImportResource(257,256,65536)<0 && attached.empty() && !allocations);
 for(uint32_t fail=1;fail<=2;++fail){
  reset();failAt=fail;CHECK(AppImportResource(257,256,65536)<0);
  if(fail==1)CHECK(!allocations && attached.empty());
  else {CHECK(allocations==1 && g_appSharedResources->uncertain);CHECK(!AppDetachSharedResourceLocked(256,65536));}
 }
 // A successful first detach stays recorded if a later importer fence/detach
 // fails. Uncertain state is retained and never silently treated as reclaimed.
 for(uint32_t fail=1;fail<=4;++fail){
  reset();CHECK(AppImportResource(257,256,65536)==0 && AppImportResource(258,256,65536)==0);
  failAt=calls+fail;CHECK(!AppDetachSharedResourceLocked(256,65536));CHECK(allocations==1 && g_appSharedResources->uncertain);
  CHECK((g_appSharedResources->importers[1]==0)==(fail>2));
 }
 reset();CHECK(AppImportResource(257,256,65536)==0);failAt=calls+1;
 CHECK(!AppPrepareSharedUseLocked(257,shared));CHECK(g_appSharedResources->uncertain);
 CHECK(!AppReleaseSharedContextLocked(257));
 reset();CHECK(AppImportResource(257,256,65536)==0);contexts[1].incarnation=99;
 const auto untouched=calls;CHECK(!AppReleaseSharedContextLocked(257) && calls==untouched);
 CHECK(!AppDetachSharedResourceLocked(256,65536));
 reset();CHECK(AppImportResource(257,256,65536)==0);AppSharedFailureLocked(257);
 CHECK(!AppPrepareSharedUseLocked(256,shared));CHECK(!AppDetachSharedResourceLocked(256,65536));
 // Execute the real submission body, including an acknowledged first chunk
 // followed by a failed second chunk. Completion is attempted before poison.
 constexpr uint32_t cap=(4096-sizeof(VirtioGpuCmdSubmit))/4;
 std::vector<uint32_t> chunks(cap+1,0);uint32_t single=0;
 reset();CHECK(AppImportResource(257,256,65536)==0);
 auto start=calls;failAt=start+4;CHECK(AppSubmit3D(257,chunks.data(),chunks.size())<0);
 CHECK(calls==start+5 && operations.back().first=='F' && g_appSharedResources->uncertain);
 reset();CHECK(AppImportResource(257,256,65536)==0);
 start=calls;failAt=start+4;CHECK(AppSubmit3D(257,&single,1)<0);
 CHECK(g_appSharedResources->uncertain && operations.back().first=='F');
 reset();CHECK(AppImportResource(257,256,65536)==0);
 CHECK(AppSubmit3D(257,&single,1)==0 && !g_appSharedResources->uncertain);
 CHECK(operations.back().first=='F' && operations.back().second==257);
 const brook::GpuTransfer3D transfer{0,0,0,1,1,1,0,0,4,4};
 for(uint32_t fail=3;fail<=4;++fail){
  reset();CHECK(AppImportResource(257,256,65536)==0);start=calls;failAt=start+fail;
  CHECK(AppTransfer3D(256,65536,1,transfer)<0);
  CHECK(operations.back().first=='F' && g_appSharedResources->uncertain);
 }
 reset();CHECK(AppImportResource(257,256,65536)==0);CHECK(AppTransfer3D(256,65536,1,transfer)==0);
 reset();std::puts("GPU_SHARED_CONTEXTS_PASS canonical validation/dedup/initial and later use fences/destination-first/resource-first/incarnation/failure quarantine");
}
