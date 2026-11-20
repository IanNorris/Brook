#include "update_slots.h"
#include "fs.h"
#include "memory.h"
#include "console.h"
#include "../../shared/update/payload.h"
#include "../../shared/update/executable.h"
#include "../../shared/update/recovery_guard.h"
#include "../../shared/update/guard_slot_health.h"
namespace brook { namespace bootloader {
bool RecoveryGuardAllowsNormal(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,const char* identity) {
    UINTN an=0,bn=0;
    auto* a=ReadFile(image,bs,u"RECOVERY\\GUARD0.BIN",&an,BROOK_RECOVERY_GUARD_SIZE);
    auto* b=ReadFile(image,bs,u"RECOVERY\\GUARD1.BIN",&bn,BROOK_RECOVERY_GUARD_SIZE);
    const bool allowed=BrookRecoveryGuardAllowsNormal(a,an,b,bn,identity);
    if(a)FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(a),an);
    if(b)FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(b),bn);
    return allowed;
}
static bool Pair(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,const uint8_t* r,
                 uint8_t*& kernel,UINTN& kb,uint8_t*& initrd,UINTN& ib,unsigned recovery=0) {
    bool b=BrookUpdateGet32(r+16)!=0;
    kb=BrookUpdateGet64(r+24);ib=BrookUpdateGet64(r+32);
    kernel=ReadFile(image,bs,recovery==2?u"RECOV2\\KERNEL.ELF":recovery?u"RECOVERY\\KERNEL.ELF":b?u"UPDATES\\B\\KERNEL.ELF":u"UPDATES\\A\\KERNEL.ELF",&kb,BROOK_UPDATE_KERNEL_MAX,kb);
    if(!kernel)return false;
    initrd=ReadFile(image,bs,recovery==2?u"RECOV2\\INITRD.IMG":recovery?u"RECOVERY\\INITRD.IMG":b?u"UPDATES\\B\\INITRD.IMG":u"UPDATES\\A\\INITRD.IMG",&ib,BROOK_UPDATE_INITRD_MAX,ib);
    uint8_t kh[32],ih[32];BrookUpdateHash(kernel,kb,kh);
    if(initrd)BrookUpdateHash(initrd,ib,ih);
    if(initrd&&BrookUpdateEqual(kh,r+40,32)&&BrookUpdateEqual(ih,r+72,32)&&BrookUpdateExecutable(kernel,kb))return true;
    if(initrd)FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(initrd),ib);
    FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(kernel),kb);
    kernel=nullptr;initrd=nullptr;return false;
}
static bool LoadRecovery(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,
                      uint8_t*& kernel,UINTN& kb,uint8_t*& initrd,UINTN& ib,unsigned which) {
    kernel=nullptr;initrd=nullptr;kb=0;ib=0;
    UINTN n=0;
    auto* record=ReadFile(image,bs,which==2?u"RECOV2\\SELECT.BIN":u"RECOVERY\\SELECT.BIN",&n,128);
    if(!record)return false;
    const bool valid=n==128 && BrookUpdateRecordValid(record);
    const bool result=valid && Pair(image,bs,record,kernel,kb,initrd,ib,which);
    FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(record),n);
    return result;
}
bool LoadRecoveryPair(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,
                      uint8_t*& kernel,UINTN& kb,uint8_t*& initrd,UINTN& ib) {
    return LoadRecovery(image,bs,kernel,kb,initrd,ib,1);
}
bool LoadAlternateRecoveryPair(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,
                      uint8_t*& kernel,UINTN& kb,uint8_t*& initrd,UINTN& ib) {
    return LoadRecovery(image,bs,kernel,kb,initrd,ib,2);
}
static BootGuardSlot SlotKey(const uint8_t (&record)[128]) {
    if(!BrookUpdateRecordValid(record))return {};
    BootGuardSlot selection{};
    selection.generation=BrookUpdateGet64(record+8);
    selection.slot=BrookUpdateGet32(record+16);
    BrookUpdateHash(record,sizeof(record),selection.selectorDigest);
    return selection;
}

static bool ReadSlotHealth(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,const char* identity,
                           GuardSlotHealth& health) {
    if(!identity)return true;
    UINTN bytes=0;
    constexpr unsigned length=GuardSlotCount*GuardSlotSectorBytes;
    auto* data=ReadFile(image,bs,u"RECOVERY\\BOOTSTAT.BIN",&bytes,length);
    if(!data)return true; // Older installations do not have the optional journal.

    struct SlotFile {
        const uint8_t* data;
        bool read(unsigned index,uint8_t* output) const {
            for(unsigned i=0;i<GuardSlotSectorBytes;++i)output[i]=data[index*GuardSlotSectorBytes+i];
            return true;
        }
    } file{data};
    uint8_t disk[32];
    BrookUpdateHash(reinterpret_cast<const uint8_t*>(identity),96,disk); // The normal guard has checked this exact identity.
    GuardSlotJournal journal;
    const bool valid=bytes==length && GuardSlotRead(file,disk,journal);
    FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(data),bytes);
    if(valid)health=journal.health;
    return valid;
}

UpdatePairResult LoadUpdatePair(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,
                    uint8_t*& kernel,UINTN& kb,uint8_t*& initrd,UINTN& ib,
                    BootGuardSlot& selection,const char* guardIdentity) {
    selection={};kernel=nullptr;initrd=nullptr;kb=0;ib=0;
    uint8_t records[2][128]{};
    for(unsigned i=0;i<2;i++) {
        UINTN n=0;auto* p=ReadFile(image,bs,i?u"UPDATES\\SELECT1.BIN":u"UPDATES\\SELECT0.BIN",&n,128);
        if(p){if(n==128)for(unsigned j=0;j<128;j++)records[i][j]=p[j];FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(p),n);}
    }
    GuardSlotHealth health{};
    if(!ReadSlotHealth(image,bs,guardIdentity,health))return UpdatePairResult::RecoveryRequired;
    const auto unavailable=health.sequence?UpdatePairResult::RecoveryRequired:UpdatePairResult::Unavailable;
    const int first=BrookUpdateNewest(records[0],records[1]);
    if(first<0)return unavailable;

    const unsigned newest=unsigned(first),previous=1-newest;
    const BootGuardSlot keys[]={SlotKey(records[0]),SlotKey(records[1])};
    const auto choice=GuardChooseSlot(health,keys[newest],keys[previous]);
    if(choice==GuardSlotChoice::Recovery)return UpdatePairResult::RecoveryRequired;

    // A fallback selected by retry policy must never fall through to the
    // exhausted slot when the older pair's contents no longer verify.
    const unsigned start=choice==GuardSlotChoice::Previous?1:0;
    const bool manual=health.preferredAtGeneration &&
        health.preferredAtGeneration==keys[newest].generation;
    const unsigned limit=manual && start==0?1:2;
    for(unsigned attempt=start;attempt<limit;++attempt) {
        const unsigned index=attempt?previous:newest;
        if(!GuardSlotKeyValid(keys[index]))continue;
        if(attempt && GuardChooseSlot(health,keys[index],{})==GuardSlotChoice::Recovery)
            return UpdatePairResult::RecoveryRequired;
        if(Pair(image,bs,records[index],kernel,kb,initrd,ib)) {
            selection=keys[index];
            ConsolePrint(u"Brook update generation ");
            ConsolePrintDec(selection.generation);
            ConsolePrintLine(u" verified");
            return choice==GuardSlotChoice::Previous?UpdatePairResult::Previous:UpdatePairResult::Loaded;
        }
    }
    return unavailable;
}

}}
