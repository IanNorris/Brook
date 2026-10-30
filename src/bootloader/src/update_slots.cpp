#include "update_slots.h"
#include "fs.h"
#include "memory.h"
#include "console.h"
#include "../../shared/update/payload.h"
#include "../../shared/update/executable.h"
namespace brook { namespace bootloader {
static bool Pair(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,const uint8_t* r,
                 uint8_t*& kernel,UINTN& kb,uint8_t*& initrd,UINTN& ib) {
    bool b=BrookUpdateGet32(r+16)!=0;
    kb=BrookUpdateGet64(r+24);ib=BrookUpdateGet64(r+32);
    kernel=ReadFile(image,bs,b?u"UPDATES\\B\\KERNEL.ELF":u"UPDATES\\A\\KERNEL.ELF",&kb,BROOK_UPDATE_KERNEL_MAX,kb);
    if(!kernel)return false;
    initrd=ReadFile(image,bs,b?u"UPDATES\\B\\INITRD.IMG":u"UPDATES\\A\\INITRD.IMG",&ib,BROOK_UPDATE_INITRD_MAX,ib);
    uint8_t kh[32],ih[32];BrookUpdateHash(kernel,kb,kh);
    if(initrd)BrookUpdateHash(initrd,ib,ih);
    if(initrd&&BrookUpdateEqual(kh,r+40,32)&&BrookUpdateEqual(ih,r+72,32)&&BrookUpdateExecutable(kernel,kb))return true;
    if(initrd)FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(initrd),ib);
    FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(kernel),kb);
    kernel=nullptr;initrd=nullptr;return false;
}
bool LoadUpdatePair(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,
                    uint8_t*& kernel,UINTN& kb,uint8_t*& initrd,UINTN& ib) {
    uint8_t records[2][128]{};
    for(unsigned i=0;i<2;i++) {
        UINTN n=0;auto* p=ReadFile(image,bs,i?u"UPDATES\\SELECT1.BIN":u"UPDATES\\SELECT0.BIN",&n,128);
        if(p){if(n==128)for(unsigned j=0;j<128;j++)records[i][j]=p[j];FreePages(bs,reinterpret_cast<EFI_PHYSICAL_ADDRESS>(p),n);}
    }
    kernel=nullptr;initrd=nullptr;kb=0;ib=0;
    int first=BrookUpdateNewest(records[0],records[1]);
    if(first>=0) {
        for(unsigned attempt=0;attempt<2;attempt++) {
            unsigned index=attempt?1u-static_cast<unsigned>(first):static_cast<unsigned>(first);
            if(!BrookUpdateRecordValid(records[index]))continue;
            if(Pair(image,bs,records[index],kernel,kb,initrd,ib)) {
                ConsolePrint(u"Brook update generation ");ConsolePrintDec(BrookUpdateGet64(records[index]+8));ConsolePrintLine(u" verified");return true;
            }
        }
    }
    ConsolePrintLine(u"Brook recovery payload selected");return false;
}
}}
