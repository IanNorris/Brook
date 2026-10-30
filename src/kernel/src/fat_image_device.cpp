#include "fat_image_device.h"
#include "fatfs_vfs.h"
#include "device.h"
#include "memory/heap.h"
namespace brook {
namespace {
struct Image {
    Device device;
    FatImageLease* lease;
    uint64_t bytes;
    char name[32];
};
int Transfer(Device* device,uint64_t offset,void* data,uint64_t bytes,bool write) {
    auto* image=static_cast<Image*>(device->priv);
    if((!data && bytes) || bytes>0x7fffffff ||
       offset>image->bytes || bytes>image->bytes-offset)return -22;
    // Geometry is512B, but ext2 also needs byte-sized metadata operations.
    // FatFs serializes each chunk including its partial-sector RMW. Whole
    // multi-chunk requests are not atomic; ext2 supplies upper serialization.
    uint64_t done=0;
    while(done<bytes) {
        const uint32_t chunk=static_cast<uint32_t>(bytes-done>65536?65536:bytes-done);
        auto* at=static_cast<uint8_t*>(data)+done;
        const int result=write?FatFsImageWrite(image->lease,offset+done,at,chunk):
                               FatFsImageRead(image->lease,offset+done,at,chunk);
        if(result!=static_cast<int>(chunk))return result<0?result:-5;
        done+=chunk;
    }
    return static_cast<int>(bytes);
}
int Read(Device* device,uint64_t offset,void* data,uint64_t bytes)
{return Transfer(device,offset,data,bytes,false);}
int Write(Device* device,uint64_t offset,const void* data,uint64_t bytes)
{return Transfer(device,offset,const_cast<void*>(data),bytes,true);}
int Control(Device* device,uint32_t command,void*) {
    if(command==DEVICE_IOCTL_READ_ONLY)return 0;
    if(command==DEVICE_IOCTL_FLUSH)return FatFsImageFlush(static_cast<Image*>(device->priv)->lease);
    return -95;
}
// There is no runtime unmount/rebinding lifecycle for these boot devices yet.
// A void close callback cannot silently discard a failed flush/backing lease.
void Close(Device*) {}
uint64_t Blocks(Device* device){return static_cast<Image*>(device->priv)->bytes/512;}
uint32_t BlockSize(Device*){return 512;}
const BlockDeviceOps operations{Read,Write,Control,Close,Blocks,BlockSize};
}
Device* FatImageDeviceCreate(Vnode* source,uint64_t bytes,const char* name,int* error) {
    int local=0;if(!error)error=&local;*error=0;
    if(!name || !name[0]){*error=-22;return nullptr;}
    uint32_t length=0;
    while(length<32 && name[length])++length;
    if(length==32){*error=-36;return nullptr;}
    auto* image=static_cast<Image*>(kmalloc(sizeof(Image)));
    if(!image){*error=-12;return nullptr;}
    *image={};
    *error=FatFsAcquireImageLease(source,bytes,&image->lease);
    if(*error){kfree(image);return nullptr;}
    image->bytes=bytes;
    for(uint32_t i=0;i<=length;++i)image->name[i]=name[i];
    image->device={reinterpret_cast<const DeviceOps*>(&operations),image->name,DeviceType::Block,image};
    return &image->device;
}
int FatImageDeviceDestroyUnregistered(Device* device) {
    if(!device || device->ops!=reinterpret_cast<const DeviceOps*>(&operations))return -22;
    if(DeviceIsRegistered(device))return -16;
    auto* image=static_cast<Image*>(device->priv);
    if(int error=FatFsReleaseImageLease(image->lease))return error;
    kfree(image);return 0;
}
}
