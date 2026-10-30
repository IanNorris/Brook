#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#undef NULL
#include "update_slots.h"
#include "fs.h"
#include "memory.h"
#include "elf.h"
#include "../../shared/update/payload.h"
using Bytes=std::vector<uint8_t>;
static std::map<std::u16string,Bytes> files;
static std::map<void*,UINTN> allocations;
namespace brook { namespace bootloader {
uint8_t* ReadFile(EFI_HANDLE,EFI_BOOT_SERVICES*,const char16_t* name,UINTN* size,UINTN maximum,UINTN prefix) {
    auto i=files.find(name);if(i==files.end()||i->second.empty()||i->second.size()>maximum||prefix>i->second.size())return nullptr;
    *size=prefix?prefix:i->second.size();auto* result=(uint8_t*)malloc(*size);assert(result);
    memcpy(result,i->second.data(),*size);allocations[result]=*size;return result;
}
void FreePages(EFI_BOOT_SERVICES*,EFI_PHYSICAL_ADDRESS address,UINT64 size) {
    void* p=(void*)address;assert(allocations.at(p)==size);allocations.erase(p);free(p);
}
void ConsolePrint(const char16_t*){}
void ConsolePrintLine(const char16_t*){}
void ConsolePrintDec(UINT64){}
}}
static Bytes kernel() {
    Bytes data(4096);auto* h=(Elf64Header*)data.data();
    memcpy(h->ident,"\177ELF\2\1\1",7);h->machine=62;h->type=2;h->version=1;h->ehSize=sizeof(*h);
    h->phOffset=sizeof(*h);h->phEntSize=sizeof(Elf64ProgramHeader);h->phCount=1;h->entry=0xffffffff80000000ull;
    auto* p=(Elf64ProgramHeader*)(data.data()+h->phOffset);p->type=PtLoad;p->flags=PfExec;p->vaddr=h->entry;p->fileSize=data.size();p->memSize=data.size();
    return data;
}
static void provision(unsigned slot,uint64_t generation,Bytes elf=kernel()) {
    Bytes initrd(512);initrd[510]=0x55;initrd[511]=0xaa;
    Bytes r(128);memcpy(r.data(),"BROOKUP1",8);BrookUpdatePut64(r.data()+8,generation);BrookUpdatePut32(r.data()+16,slot);
    BrookUpdatePut64(r.data()+24,elf.size());BrookUpdatePut64(r.data()+32,initrd.size());
    BrookUpdateHash(elf.data(),elf.size(),r.data()+40);BrookUpdateHash(initrd.data(),initrd.size(),r.data()+72);
    BrookUpdatePut32(r.data()+124,BrookUpdateCrc(r.data(),124));
    auto directory=slot?u"UPDATES\\B\\":u"UPDATES\\A\\";
    files[std::u16string(directory)+u"KERNEL.ELF"]=elf;files[std::u16string(directory)+u"INITRD.IMG"]=initrd;
    files[slot?u"UPDATES\\SELECT1.BIN":u"UPDATES\\SELECT0.BIN"]=r;
}
static void expect(bool success) {
    uint8_t *k=nullptr,*i=nullptr;UINTN kb=0,ib=0;
    assert(brook::bootloader::LoadUpdatePair(nullptr,nullptr,k,kb,i,ib)==success);
    if(success){assert(k&&i&&kb==4096&&ib==512);brook::bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)k,kb);brook::bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)i,ib);}
    else assert(!k&&!i);
    assert(allocations.empty());
}
int main() {
    expect(false);provision(0,1);expect(true);provision(1,2);expect(true);
    files.erase(u"UPDATES\\B\\INITRD.IMG");expect(true); // no mixed root/A initrd
    files.erase(u"UPDATES\\A\\INITRD.IMG");expect(false);
    provision(0,1);provision(1,2);files[u"UPDATES\\B\\KERNEL.ELF"][200]^=1;expect(true);
    // Correct hash does not excuse an out-of-range ELF program-header table.
    Bytes bad=kernel();((Elf64Header*)bad.data())->phOffset=~uint64_t{0};provision(1,2,bad);expect(true);
    files.erase(u"UPDATES\\A\\KERNEL.ELF");expect(false);
    provision(0,2);provision(1,2);expect(false); // ambiguous selection fails closed
    provision(1,3);expect(true);
    files[u"UPDATES\\B\\KERNEL.ELF"].resize(BROOK_UPDATE_KERNEL_MAX+1);expect(true); // older A only
    files.erase(u"UPDATES\\A\\KERNEL.ELF");expect(false);
    puts("UPDATE_SLOTS_PASS");
}
