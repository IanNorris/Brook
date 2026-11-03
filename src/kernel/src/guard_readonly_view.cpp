#include "guard_readonly_view.h"
#include "ahci_system_volume.h"
#include "device.h"
#include "memory/heap.h"
#include "vfs.h"

namespace brook {
namespace {
struct View {Device* device;uint64_t bytes;};
int Open(Vnode*,int flags) {return flags==VFS_O_READ?0:-30;}
int Read(Vnode* node,void* out,uint64_t length,uint64_t* offset) {
    if(!node || !node->priv || !offset || (!out && length))return -22;
    auto* view=static_cast<View*>(node->priv);
    if(*offset>=view->bytes || !length)return 0;
    if(length>view->bytes-*offset)length=view->bytes-*offset;
    if(length>0x7ffff000ULL)length=0x7ffff000ULL;
    const int result=view->device->ops->read(view->device,*offset,out,length);
    if(result<0)return result;
    if(!result || uint64_t(result)>length)return -5;
    *offset+=uint64_t(result);return result;
}
int Write(Vnode*,const void*,uint64_t,uint64_t*) {return -30;}
int Truncate(Vnode*,uint64_t) {return -30;}
int Times(Vnode*,const VnodeTime*) {return -30;}
int Chmod(Vnode*,uint16_t) {return -30;}
int Chown(Vnode*,uint32_t,uint32_t) {return -30;}
int Append(Vnode*,const void*,uint64_t,uint64_t*) {return -30;}
int Access(Vnode*,uint8_t required,bool) {return required&2?-30:required&1?-13:0;}
void Close(Vnode* node) {if(node)kfree(node->priv);}
int Stat(Vnode* node,VnodeStat* out) {
    if(!node || !node->priv || !out)return -22;
    *out={};out->size=static_cast<View*>(node->priv)->bytes;out->mode=0100400;
    out->ino=1;return 0;
}
const VnodeOps ops={.open=Open,.read=Read,.write=Write,.readdir=nullptr,.close=Close,
    .stat=Stat,.fsync=nullptr,.set_times=Times,.chmod=Chmod,.truncate=Truncate,
    .file_object=nullptr,.chown=Chown,.append=Append,.space=nullptr,.access=Access,
    .directory_path=nullptr,.deny_mapping=true};
}
Vnode* GuardCreateReadOnlyView(Device* device,int flags,int* error) {
    if(error)*error=-22;
    if(flags!=VFS_O_READ) {if(error)*error=-30;return nullptr;}
    if(!device || !AhciSystemGuardIsReservedDevice(device) || !DeviceIsRegistered(device) ||
       device->type!=DeviceType::Block || !device->ops || !device->ops->read ||
       !device->ops->ioctl || device->ops->ioctl(device,DEVICE_IOCTL_READ_ONLY,nullptr)!=1)
        return nullptr;
    const uint64_t blocks=DeviceBlockCount(device),blockSize=DeviceBlockSize(device);
    if(blockSize!=512 || !blocks || blocks>~uint64_t{0}/blockSize)return nullptr;
    auto* view=static_cast<View*>(kmalloc(sizeof(View)));if(!view) {if(error)*error=-12;return nullptr;}
    auto* node=static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
    if(!node) {kfree(view);if(error)*error=-12;return nullptr;}
    *view={device,blocks*blockSize};*node={};node->ops=&ops;node->type=VnodeType::File;
    node->priv=view;node->refCount=1;node->cacheId=0;
    if(error)*error=0;
    return node;
}
}
