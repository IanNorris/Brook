#include "recovery_firmware_discovery.h"
#include "Protocol/LoadedImage.h"
#include "Protocol/DevicePath.h"
#include "recovery_firmware_reader.h"

namespace brook { namespace bootloader {
// EFI applications often have small stacks and no __chkstk runtime. Keep
// bounded path copies out of the stack; reject re-entry instead of sharing
// the buffers across concurrent/nested firmware callbacks.
static uint8_t espCopy[4096],wholeCopy[4096],pathScratchBusy;
struct PathScratchLease {
    ~PathScratchLease() { __atomic_store_n(&pathScratchBusy,0,__ATOMIC_RELEASE); }
};
// UEFI firmware owns these protocol pointers and must supply readable,
// correctly allocated device paths; UEFI exposes no allocation length here.
// The 4096-byte traversal cap cannot make an invalid firmware pointer safe.
// Within that trusted-firmware contract reject malformed/multi-instance paths.
static uint32_t PathBytes(const EFI_DEVICE_PATH_PROTOCOL* raw) {
    if(!raw)return 0;
    const auto* p=reinterpret_cast<const uint8_t*>(raw);
    uint32_t at=0;
    for(unsigned nodes=0;nodes<128 && at<=4092;++nodes) {
        const uint32_t len=uint32_t(p[at+2])|(uint32_t(p[at+3])<<8);
        if(len<4 || len>4096-at)return 0;
        if(p[at]==0x7f)return p[at+1]==0xff && len==4 ? at+4 : 0;
        at+=len;
    }
    return 0;
}
bool RecoveryFindFirmwareDisk(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,RecoveryFirmwareDisk& out) {
    out={};if(!image || !bs || !bs->HandleProtocol || !bs->LocateHandleBuffer || !bs->FreePool)return false;
    if(__atomic_exchange_n(&pathScratchBusy,1,__ATOMIC_ACQUIRE))return false;
    PathScratchLease lease{};
    EFI_GUID loadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,blockGuid=EFI_BLOCK_IO_PROTOCOL_GUID,
             pathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* loaded=nullptr;
    if(EFI_ERROR(bs->HandleProtocol(image,&loadedGuid,reinterpret_cast<void**>(&loaded))) ||
       !loaded || !loaded->DeviceHandle)return false;
    EFI_BLOCK_IO_PROTOCOL* esp=nullptr;EFI_DEVICE_PATH_PROTOCOL* espRaw=nullptr;
    if(EFI_ERROR(bs->HandleProtocol(loaded->DeviceHandle,&blockGuid,reinterpret_cast<void**>(&esp))) ||
       EFI_ERROR(bs->HandleProtocol(loaded->DeviceHandle,&pathGuid,reinterpret_cast<void**>(&espRaw))) ||
       !esp || !esp->Media || !esp->Media->MediaPresent || !esp->Media->LogicalPartition ||
       esp->Media->BlockSize!=512 || esp->Media->LastBlock==~uint64_t{0})return false;
    const uint32_t espBytes=PathBytes(espRaw);RecoveryEspPath parsed{};
    if(!espBytes || espBytes>sizeof(espCopy))return false;
    for(uint32_t i=0;i<espBytes;++i)espCopy[i]=reinterpret_cast<const uint8_t*>(espRaw)[i];
    if(!RecoveryParseEspPath(espCopy,espBytes,parsed) ||
       esp->Media->LastBlock+1!=parsed.sectors)return false;
    EFI_BLOCK_IO_MEDIA* espMedia=esp->Media;
    const UINT32 espId=espMedia->MediaId,espAlign=espMedia->IoAlign;const EFI_LBA espLast=espMedia->LastBlock;
    EFI_HANDLE* handles=nullptr;UINTN count=0;
    if(EFI_ERROR(bs->LocateHandleBuffer(ByProtocol,&blockGuid,nullptr,&count,&handles)))return false;
    EFI_HANDLE selected=nullptr;EFI_BLOCK_IO_PROTOCOL* whole=nullptr;EFI_BLOCK_IO_MEDIA* wholeMedia=nullptr;
    EFI_DEVICE_PATH_PROTOCOL* wholePath=nullptr;uint32_t wholeBytes=0;
    UINT32 wholeId=0,wholeAlign=0;EFI_LBA wholeLast=0;bool valid=count && count<=4096 && handles;
    for(UINTN i=0;valid && i<count;++i) {
        EFI_BLOCK_IO_PROTOCOL* candidate=nullptr;EFI_DEVICE_PATH_PROTOCOL* path=nullptr;
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&blockGuid,reinterpret_cast<void**>(&candidate))) ||
           !candidate || !candidate->Media || candidate->Media->LogicalPartition ||
           !candidate->Media->MediaPresent || candidate->Media->BlockSize!=512 ||
           EFI_ERROR(bs->HandleProtocol(handles[i],&pathGuid,reinterpret_cast<void**>(&path))))continue;
        const uint32_t pathBytes=PathBytes(path);
        if(!RecoveryParentPathMatches(espCopy,espBytes,parsed,
                reinterpret_cast<const uint8_t*>(path),pathBytes))continue;
        if(selected) {valid=false;break;}
        if(!candidate->Media || candidate->Media->LogicalPartition || !candidate->Media->MediaPresent ||
           candidate->Media->BlockSize!=512) {valid=false;break;}
        selected=handles[i];whole=candidate;wholeMedia=candidate->Media;wholePath=path;wholeBytes=pathBytes;
        for(uint32_t j=0;j<pathBytes;++j)wholeCopy[j]=reinterpret_cast<const uint8_t*>(path)[j];
        wholeId=wholeMedia->MediaId;wholeAlign=wholeMedia->IoAlign;wholeLast=wholeMedia->LastBlock;
    }
    if(handles && EFI_ERROR(bs->FreePool(handles)))valid=false;
    if(!valid || !selected || !whole || !wholeMedia || whole->Media!=wholeMedia ||
       !wholeMedia->MediaPresent || wholeMedia->LogicalPartition || wholeMedia->BlockSize!=512 ||
       wholeMedia->MediaId!=wholeId || wholeMedia->IoAlign!=wholeAlign || wholeMedia->LastBlock!=wholeLast ||
       esp->Media!=espMedia || !espMedia->MediaPresent || !espMedia->LogicalPartition ||
       espMedia->BlockSize!=512 || espMedia->MediaId!=espId || espMedia->IoAlign!=espAlign ||
       espMedia->LastBlock!=espLast || PathBytes(espRaw)!=espBytes || PathBytes(wholePath)!=wholeBytes ||
       !RecoveryParentPathMatches(espCopy,espBytes,parsed,wholeCopy,wholeBytes))return false;
    for(uint32_t i=0;i<espBytes;++i)if(espCopy[i]!=reinterpret_cast<const uint8_t*>(espRaw)[i])return false;
    for(uint32_t i=0;i<wholeBytes;++i)if(wholeCopy[i]!=reinterpret_cast<const uint8_t*>(wholePath)[i])return false;
    out.handle=selected;out.whole=whole;out.esp=esp;out.espPath=parsed;
    out.wholeMedia=wholeMedia;out.espMedia=espMedia;out.wholeMediaId=wholeId;out.espMediaId=espId;
    out.wholeAlign=wholeAlign;out.espAlign=espAlign;out.wholeLast=wholeLast;out.espLast=espLast;return true;
}
RecoveryDiskState RecoveryInspectFirmwareDisk(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,
                                               const RecoveryDiskIdentity& expected) {
    if(!bs || !bs->AllocatePages || !bs->FreePages)return RecoveryDiskState::Invalid;
    RecoveryFirmwareDisk found{};
    if(!RecoveryFindFirmwareDisk(image,bs,found) || !found.esp || !found.esp->Media)
        return RecoveryDiskState::Invalid;
    auto sameMedia=[&]() {
        const auto* e=found.esp->Media;const auto* w=found.whole->Media;
        return e==found.espMedia && w==found.wholeMedia && e && w && e->MediaPresent &&
            e->LogicalPartition && e->BlockSize==512 && e->MediaId==found.espMediaId &&
            e->LastBlock==found.espLast && e->IoAlign==found.espAlign && w->MediaPresent &&
            !w->LogicalPartition && w->BlockSize==512 && w->MediaId==found.wholeMediaId &&
            w->LastBlock==found.wholeLast && w->IoAlign==found.wholeAlign;
    };
    if(!sameMedia())return RecoveryDiskState::Invalid;
    const auto* esp=found.esp->Media;
    const UINT32 espMediaId=esp->MediaId,espBlockSize=esp->BlockSize,espAlign=esp->IoAlign;
    const EFI_LBA espLast=esp->LastBlock;
    RecoveryDiskIdentity id=expected;
    id.espPartitionNumber=found.espPath.partitionNumber;id.espFirst=found.espPath.first;
    id.espLast=found.espPath.first+found.espPath.sectors-1;
    for(unsigned i=0;i<16;++i)id.espGuid[i]=found.espPath.guid[i];
    // GPT supports at most 1 MiB of entries. One page-aligned allocation
    // supplies two independent arrays and one firmware-aligned bounce buffer.
    constexpr UINTN area=1024*1024,pages=(3*area)/4096;
    EFI_PHYSICAL_ADDRESS memory=0;
    if(EFI_ERROR(bs->AllocatePages(AllocateAnyPages,EfiLoaderData,pages,&memory)))return RecoveryDiskState::Invalid;
    if(!memory) {bs->FreePages(memory,pages);return RecoveryDiskState::Invalid;}
    if(!sameMedia()) {bs->FreePages(memory,pages);return RecoveryDiskState::Invalid;}
    auto* base=reinterpret_cast<uint8_t*>(memory);
    RecoveryFirmwareReader<EFI_BLOCK_IO_PROTOCOL> reader(found.whole,base+2*area,area);
    RecoveryDiskState result=RecoveryReadDiskState(reader,reader.Sectors(),id,base,base+area,area);
    const auto* after=found.esp->Media;
    if(after!=found.espMedia || !after || !after->MediaPresent || !after->LogicalPartition || after->MediaId!=espMediaId ||
       after->BlockSize!=espBlockSize || after->IoAlign!=espAlign || after->LastBlock!=espLast)
        result=RecoveryDiskState::Invalid;
    if(!sameMedia())result=RecoveryDiskState::Invalid;
    if(EFI_ERROR(bs->FreePages(memory,pages)))result=RecoveryDiskState::Invalid;
    if(!sameMedia())result=RecoveryDiskState::Invalid;
    return result;
}
}}
