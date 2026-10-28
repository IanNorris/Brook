#include "page_store_probe.h"
#include "page_store.h"
#include "device.h"
#include "fw_cfg.h"
#include "heap.h"
#include "serial.h"
#include "string.h"

namespace brook {
static int Read(void* context,uint64_t offset,void* data,uint64_t size) {
    auto* device=static_cast<Device*>(context);
    return device->ops->read(device,offset,data,size);
}
static int Write(void* context,uint64_t offset,const void* data,uint64_t size) {
    return DeviceWrite(static_cast<Device*>(context),offset,data,size);
}
void PageStoreBootProbe() {
    char name[32]{};
    const uint32_t size=FwCfgReadFile("opt/page-store-probe",name,sizeof(name));
    if(!size)return;
    if(size>=sizeof(name)){SerialPuts("page-store probe: FAIL device name\n");return;}
    name[size]=0;
    auto* device=DeviceFind(name);
    if(!device || device->type!=DeviceType::Block || !device->ops->read || !device->ops->write) {
        SerialPuts("page-store probe: FAIL missing block device\n");return;
    }
    const uint64_t count=DeviceBlockCount(device),block=DeviceBlockSize(device);
    if(!block || count>~uint64_t{0}/block){SerialPuts("page-store probe: FAIL capacity\n");return;}
    // Resident static scratch avoids large boot-stack frames and needs no
    // fault-in allocation. This is not a background pager or a retained attach.
    alignas(4096) static uint8_t source[4096],output[4096];
    if(Read(device,0,output,4096)!=4096){SerialPuts("page-store probe: FAIL header read\n");return;}
    PageStoreHeader header{};memcpy(&header,output,sizeof(header));
    bool reserved=false;for(unsigned i=sizeof(header);i<4096;++i)reserved|=output[i]!=0;
    if(reserved || !PageStoreHeaderValid(header,count*block) || header.slots<4 || header.slots>4096) {
        SerialPuts("page-store probe: FAIL signature/header (no writes)\n");return;
    }
    auto* slots=static_cast<PageStoreSlot*>(kmalloc(header.slots*sizeof(PageStoreSlot)));
    if(!slots){SerialPuts("page-store probe: FAIL metadata allocation\n");return;}
    PageStore store;uint64_t tokens[4]{};bool pass=store.Initialize(header,count*block,slots,header.slots,{device,Read,Write});
    int error=0;
    for(unsigned i=0;pass && i<4;++i) {
        tokens[i]=store.Reserve();pass=tokens[i]!=0;
        for(unsigned j=0;j<4096;++j)source[j]=uint8_t(j*73+i*31+(j>>8));
        if(pass){error=store.Store(tokens[i],source);pass=error==0;}
    }
    if(pass){error=DeviceFlush(device);pass=error==0;}
    for(unsigned i=0;pass && i<4;++i) {
        error=store.Load(tokens[i],output);pass=error==0;
        for(unsigned j=0;pass && j<4096;++j)pass=output[j]==uint8_t(j*73+i*31+(j>>8));
    }
    for(auto token:tokens)if(token)pass=store.Release(token)&&pass;
    const auto stats=store.Stats();pass=pass && !stats.used && !stats.stored;
    SerialPrintf("page-store probe: %s device=%s writes=%lu reads=%lu live=%lu error=%d (backing only; no eviction)\n",
        pass?"PASS":"FAIL",name,stats.writes,stats.reads,stats.used,error);
    kfree(slots);
}
}
