#include <cassert>
#include <cstdlib>
#include <cstring>
#include "guard_readonly_view.h"
#include "device.h"
#include "vfs.h"

using namespace brook;
namespace {
Device* reserved=nullptr;bool registered=true,ready=true;
unsigned reads=0,writes=0,flushes=0;int resultOverride=-999;
uint64_t blocksValue=8,lastRequest=0;uint32_t blockSizeValue=512;
int failAllocation=-1,liveAllocations=0;
uint8_t backing[4096]{};
int Read(Device*,uint64_t offset,void* out,uint64_t size) {
    ++reads;lastRequest=size;if(resultOverride!=-999)return resultOverride;
    assert(offset<=sizeof(backing) && size<=sizeof(backing)-offset);
    std::memcpy(out,backing+offset,size);return int(size);
}
int Write(Device*,uint64_t,const void*,uint64_t) {++writes;return -5;}
int Ioctl(Device*,uint32_t command,void*) {
    if(command==DEVICE_IOCTL_READ_ONLY)return ready?1:-19;
    if(command==DEVICE_IOCTL_FLUSH)++flushes;
    return -95;
}
uint64_t Blocks(Device*) {return blocksValue;}
uint32_t Size(Device*) {return blockSizeValue;}
}
namespace brook {
bool AhciSystemGuardIsReservedDevice(Device* d) {return d==reserved;}
extern "C" bool DeviceIsRegistered(Device* d) {return registered && d==reserved;}
extern "C" void* kmalloc(uint64_t n) {
    if(failAllocation==0)return nullptr;
    if(failAllocation>0)--failAllocation;
    void* p=std::malloc(n);if(p)++liveAllocations;return p;
}
extern "C" void kfree(void* p) {if(p)--liveAllocations;std::free(p);}
}
int main() {
    BlockDeviceOps block{Read,Write,Ioctl,nullptr,Blocks,Size};
    Device device{reinterpret_cast<const DeviceOps*>(&block),"guard",DeviceType::Block,nullptr};
    Device other{reinterpret_cast<const DeviceOps*>(&block),"other",DeviceType::Block,nullptr};
    reserved=&device;int error=0;
    assert(!GuardCreateReadOnlyView(nullptr,VFS_O_READ,nullptr));
    assert(!GuardCreateReadOnlyView(&other,VFS_O_READ,&error));
    ready=false;assert(!GuardCreateReadOnlyView(&device,VFS_O_READ,&error));ready=true;
    registered=false;assert(!GuardCreateReadOnlyView(&device,VFS_O_READ,&error));registered=true;
    assert(!GuardCreateReadOnlyView(&device,VFS_O_WRITE,&error) && error==-30);
    blocksValue=0;assert(!GuardCreateReadOnlyView(&device,VFS_O_READ,&error));
    blocksValue=~uint64_t{0};assert(!GuardCreateReadOnlyView(&device,VFS_O_READ,&error));
    blocksValue=8;blockSizeValue=1024;assert(!GuardCreateReadOnlyView(&device,VFS_O_READ,&error));blockSizeValue=512;
    for(int i=0;i<2;++i) {failAllocation=i;assert(!GuardCreateReadOnlyView(&device,VFS_O_READ,&error) && error==-12 && !liveAllocations);}
    failAllocation=-1;
    Vnode* noError=GuardCreateReadOnlyView(&device,VFS_O_READ,nullptr);assert(noError);
    noError->ops->close(noError);kfree(noError);assert(!liveAllocations);
    blocksValue=1ull<<25;Vnode* large=GuardCreateReadOnlyView(&device,VFS_O_READ,&error);assert(large);
    uint8_t byte=0;uint64_t largeOffset=0;resultOverride=1;
    assert(large->ops->read(large,&byte,~uint64_t{0},&largeOffset)==1 && lastRequest==0x7ffff000ULL);
    large->ops->close(large);kfree(large);blocksValue=8;resultOverride=-999;
    Vnode* node=GuardCreateReadOnlyView(&device,VFS_O_READ,&error);assert(node && !error);
    assert(node->type==VnodeType::File && node->refCount==1 && !node->cacheId &&
           node->ops->deny_mapping && !node->ops->file_object);
    VnodeStat stat{};assert(!node->ops->stat(node,&stat) && stat.size==sizeof(backing) && stat.mode==0100400);
    for(unsigned i=0;i<sizeof(backing);++i)backing[i]=uint8_t(i);
    uint8_t out[16]{};uint64_t at=3;
    assert(node->ops->read(node,out,7,&at)==7 && at==10 && out[0]==3 && out[6]==9);
    at=4093;assert(node->ops->read(node,out,10,&at)==3 && at==4096);
    at=4095;assert(node->ops->read(node,out,~uint64_t{0},&at)==1 && at==4096);
    const auto before=reads;assert(!node->ops->read(node,out,10,&at) && reads==before);
    at=~uint64_t{0};assert(!node->ops->read(node,out,10,&at) && reads==before);
    at=0;assert(!node->ops->read(node,nullptr,0,&at) && reads==before);
    assert(node->ops->read(node,nullptr,1,&at)==-22);
    resultOverride=2;assert(node->ops->read(node,out,7,&at)==2 && at==2);
    resultOverride=0;assert(node->ops->read(node,out,7,&at)==-5 && at==2);
    resultOverride=-5;assert(node->ops->read(node,out,7,&at)==-5 && at==2);
    resultOverride=8;assert(node->ops->read(node,out,7,&at)==-5 && at==2);
    assert(node->ops->write(node,out,1,&at)==-30 && node->ops->truncate(node,0)==-30);
    assert(node->ops->append(node,out,1,&at)==-30 && node->ops->chmod(node,0777)==-30);
    assert(node->ops->chown(node,1,1)==-30 && node->ops->set_times(node,nullptr)==-30);
    assert(node->ops->open(node,VFS_O_WRITE)==-30 && node->ops->access(node,2,false)==-30);
    assert(node->ops->access(node,1,false)==-13 && !node->ops->access(node,4,false));
    assert(!writes && !flushes);node->ops->close(node);kfree(node);assert(!liveAllocations);
}
