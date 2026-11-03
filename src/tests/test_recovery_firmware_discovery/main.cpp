#include <cassert>
#include <cstdlib>
#include <cstring>
#undef NULL
#include "recovery_firmware_discovery.h"
#include "Protocol/LoadedImage.h"
#include "Protocol/DevicePath.h"
#include "../../kernel/src/strict_gpt_inventory.h"
using namespace brook::bootloader;
static EFI_GUID loadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,blockGuid=EFI_BLOCK_IO_PROTOCOL_GUID,pathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_LOADED_IMAGE_PROTOCOL loaded{};
static EFI_BLOCK_IO_MEDIA espMedia{},diskMedia{};
static EFI_BLOCK_IO_PROTOCOL esp{},disk{};
static uint8_t espPath[4096]{},diskPath[4096]{},otherPath[4096]{};
static EFI_HANDLE image=reinterpret_cast<EFI_HANDLE>(1),child=reinterpret_cast<EFI_HANDLE>(2),
    parent=reinterpret_cast<EFI_HANDLE>(3),other=reinterpret_cast<EFI_HANDLE>(4);
static bool duplicate=false,missingParent=false,nullHandles=false,changeEsp=false,changePath=false,
    nullParentMedia=false,changeAlloc=false,changeRead=false,nested=false;
static EFI_BOOT_SERVICES* activeBs=nullptr;
static unsigned freed=0,pagesFreed=0;
static uint8_t mbr[512]{},primary[512]{},backup[512]{},entries[16384]{},sb[1024]{};
static EFI_STATUS EFIAPI handle(EFI_HANDLE h,EFI_GUID* guid,void** out) {
    *out=nullptr;
    if(h==image && !std::memcmp(guid,&loadedGuid,sizeof(*guid))){*out=&loaded;return EFI_SUCCESS;}
    if(!std::memcmp(guid,&blockGuid,sizeof(*guid))) {
        if(h==child)*out=&esp;else if(h==parent || h==other)*out=&disk;
    } else if(!std::memcmp(guid,&pathGuid,sizeof(*guid))) {
        if(h==parent && nullParentMedia)disk.Media=nullptr;
        if(h==child)*out=espPath;else if(h==parent)*out=diskPath;else if(h==other)*out=otherPath;
    }
    return *out?EFI_SUCCESS:EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE,EFI_GUID*,void*,UINTN* count,EFI_HANDLE** out) {
    if(nested){RecoveryFirmwareDisk inner{};assert(!RecoveryFindFirmwareDisk(image,activeBs,inner));}
    if(changeEsp)++espMedia.MediaId;if(changePath)espPath[4]^=1;
    *count=missingParent?1:3;
    if(nullHandles){*out=nullptr;return EFI_SUCCESS;}
    *out=static_cast<EFI_HANDLE*>(std::malloc(*count*sizeof(EFI_HANDLE)));assert(*out);
    (*out)[0]=child;if(!missingParent){(*out)[1]=parent;(*out)[2]=other;}return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI freePool(void* p){++freed;std::free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI allocatePages(EFI_ALLOCATE_TYPE,EFI_MEMORY_TYPE,UINTN pages,EFI_PHYSICAL_ADDRESS* out){
    void* p=nullptr;if(posix_memalign(&p,4096,pages*4096))return EFI_OUT_OF_RESOURCES;
    *out=reinterpret_cast<EFI_PHYSICAL_ADDRESS>(p);if(changeAlloc)++diskMedia.MediaId;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI freePages(EFI_PHYSICAL_ADDRESS p,UINTN){++pagesFreed;std::free(reinterpret_cast<void*>(p));return EFI_SUCCESS;}
static EFI_STATUS EFIAPI readBlocks(EFI_BLOCK_IO_PROTOCOL*,UINT32 media,EFI_LBA lba,UINTN bytes,void* out){
    if(media!=diskMedia.MediaId)return EFI_MEDIA_CHANGED;
    const uint8_t* source=lba==0?mbr:lba==1?primary:lba==99999?backup:lba==2||lba==99967?entries:lba==4098?sb:nullptr;
    UINTN expected=lba==2||lba==99967?16384:lba==4098?1024:512;
    if(!source || expected!=bytes)return EFI_DEVICE_ERROR;
    std::memcpy(out,source,bytes);if(changeRead)++diskMedia.MediaId;return EFI_SUCCESS;
}
static void put32(uint8_t* p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
int main() {
    EFI_BOOT_SERVICES bs{};bs.HandleProtocol=handle;bs.LocateHandleBuffer=locate;bs.FreePool=freePool;
    activeBs=&bs;
    bs.AllocatePages=allocatePages;bs.FreePages=freePages;disk.ReadBlocks=readBlocks;
    loaded.DeviceHandle=child;esp.Media=&espMedia;disk.Media=&diskMedia;
    espMedia.MediaPresent=true;espMedia.LogicalPartition=true;espMedia.BlockSize=512;espMedia.LastBlock=2047;
    diskMedia.MediaPresent=true;diskMedia.BlockSize=512;diskMedia.LastBlock=99999;
    espPath[0]=1;espPath[1]=1;espPath[2]=6;espPath[6]=4;espPath[7]=1;espPath[8]=42;
    put32(espPath+10,1);put64(espPath+14,2048);put64(espPath+22,2048);espPath[30]=42;
    espPath[46]=2;espPath[47]=2;espPath[48]=0x7f;espPath[49]=0xff;espPath[50]=4;
    std::memcpy(diskPath,espPath,6);diskPath[6]=0x7f;diskPath[7]=0xff;diskPath[8]=4;
    std::memcpy(otherPath,diskPath,10);otherPath[4]=1;
    RecoveryFirmwareDisk result{};
    assert(RecoveryFindFirmwareDisk(image,&bs,result) && result.handle==parent && result.whole==&disk && result.esp==&esp);
    assert(result.espPath.first==2048 && result.espPath.sectors==2048 && freed==1);
    nested=true;assert(RecoveryFindFirmwareDisk(image,&bs,result));nested=false;
    duplicate=true;if(duplicate)std::memcpy(otherPath,diskPath,10);
    assert(!RecoveryFindFirmwareDisk(image,&bs,result) && !result.handle && freed==3);
    otherPath[4]=1;missingParent=true;assert(!RecoveryFindFirmwareDisk(image,&bs,result));missingParent=false;
    nullHandles=true;assert(!RecoveryFindFirmwareDisk(image,&bs,result));nullHandles=false;
    espMedia.LastBlock=2046;assert(!RecoveryFindFirmwareDisk(image,&bs,result));espMedia.LastBlock=2047;
    diskMedia.LogicalPartition=true;assert(!RecoveryFindFirmwareDisk(image,&bs,result));diskMedia.LogicalPartition=false;
    changeEsp=true;assert(!RecoveryFindFirmwareDisk(image,&bs,result));changeEsp=false;espMedia.MediaId--;
    changePath=true;assert(!RecoveryFindFirmwareDisk(image,&bs,result));changePath=false;espPath[4]^=1;
    nullParentMedia=true;assert(!RecoveryFindFirmwareDisk(image,&bs,result));nullParentMedia=false;disk.Media=&diskMedia;
    // Real firmware-adapter path with an independently constructed GPT and ext2 header.
    mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0xee;put32(mbr+454,1);put32(mbr+458,99999);
    const uint8_t types[2][16]={{0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b},
        {0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4}};
    std::memcpy(entries,types[0],16);entries[16]=42;put64(entries+32,2048);put64(entries+40,4095);
    std::memcpy(entries+128,types[1],16);entries[144]=2;put64(entries+160,4096);put64(entries+168,8191);
    auto makeHeader=[&](uint8_t* h,bool last){std::memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
        put64(h+24,last?99999:1);put64(h+32,last?1:99999);put64(h+40,34);put64(h+48,99966);h[56]=1;
        put64(h+72,last?99967:2);put32(h+80,128);put32(h+84,128);put32(h+88,brook::StrictCrc32(entries,sizeof(entries)));
        put32(h+16,brook::StrictCrc32(h,92,true));};makeHeader(primary,false);makeHeader(backup,true);
    put32(sb,64);put32(sb+4,512);put32(sb+12,512);put32(sb+16,64);put32(sb+24,2);put32(sb+28,2);
    put32(sb+32,32768);put32(sb+36,32768);put32(sb+40,64);sb[56]=0x53;sb[57]=0xef;sb[58]=1;
    put32(sb+76,1);put32(sb+84,11);sb[88]=128;put32(sb+96,2);put32(sb+100,3);sb[104]=3;
    RecoveryDiskIdentity expected{};expected.diskGuid[0]=1;expected.dataGuid[0]=2;expected.fsUuid[0]=3;
    expected.dataFirst=4096;expected.dataSectors=4096;
    assert(RecoveryInspectFirmwareDisk(image,&bs,expected)==RecoveryDiskState::Clean && pagesFreed==1);
    sb[58]=0;assert(RecoveryInspectFirmwareDisk(image,&bs,expected)==RecoveryDiskState::NeedsCheck);sb[58]=1;
    changeAlloc=true;assert(RecoveryInspectFirmwareDisk(image,&bs,expected)==RecoveryDiskState::Invalid);
    changeAlloc=false;diskMedia.MediaId--;
    changeRead=true;assert(RecoveryInspectFirmwareDisk(image,&bs,expected)==RecoveryDiskState::Invalid);
    changeRead=false;diskMedia.MediaId--;
    assert(pagesFreed==4);
    espPath[49]=1;assert(!RecoveryFindFirmwareDisk(image,&bs,result));
}
