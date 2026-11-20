#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#undef NULL
#include "update_slots.h"
#include "config.h"
#include "fs.h"
#include "memory.h"
#include "elf.h"
#include "../../shared/update/payload.h"
#include "../../shared/update/recovery_guard.h"
#include "../../shared/update/guard_slot_health.h"
#include "../../shared/boot_protocol/boot_protocol.h"
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
static void expect(bool success,uint64_t generation=0) {
    uint8_t *k=nullptr,*i=nullptr;UINTN kb=0,ib=0;
    brook::BootProtocol boot{};
    boot.version=brook::BootProtocolVersion;
    boot.extensions=brook::BootExtensionTimingMagic;
    assert(!brook::BootHasGuardSlot(&boot));
    boot.guardSlot.generation=100; // Failed selection must clear stale metadata.
    using Result=brook::bootloader::UpdatePairResult;
    assert(brook::bootloader::LoadUpdatePair(nullptr,nullptr,k,kb,i,ib,boot.guardSlot)==
           (success?Result::Loaded:Result::Unavailable));
    boot.timing.flags|=brook::BootTimingGuardSlot;
    assert(brook::BootHasGuardSlot(&boot)==success);
    if(success) {
        const auto& selection=boot.guardSlot;
        const auto& record=files.at(selection.slot?u"UPDATES\\SELECT1.BIN":u"UPDATES\\SELECT0.BIN");
        uint8_t expected[32]{};
        BrookUpdateHash(record.data(),record.size(),expected);
        assert(selection.generation==BrookUpdateGet64(record.data()+8));
        assert(!generation || selection.generation==generation);
        assert(BrookUpdateEqual(selection.selectorDigest,expected,sizeof(expected)));
    }
    if(success){assert(k&&i&&kb==4096&&ib==512);brook::bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)k,kb);brook::bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)i,ib);}
    else assert(!k&&!i);
    assert(allocations.empty());
}
static void expectRecovery(bool success) {
    uint8_t *k=nullptr,*i=nullptr;UINTN kb=0,ib=0;
    assert(brook::bootloader::LoadRecoveryPair(nullptr,nullptr,k,kb,i,ib)==success);
    if(success){assert(k&&i&&kb==4096&&ib==512);brook::bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)k,kb);brook::bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)i,ib);}
    else assert(!k&&!i);
    assert(allocations.empty());
}
static void expectAlternate(bool success) {
    uint8_t *k=nullptr,*i=nullptr;UINTN kb=0,ib=0;
    assert(brook::bootloader::LoadAlternateRecoveryPair(nullptr,nullptr,k,kb,i,ib)==success);
    if(success){assert(k&&i&&kb==4096&&ib==512);brook::bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)k,kb);brook::bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)i,ib);}
    else assert(!k&&!i);
    assert(allocations.empty());
}
struct SlotHealthIo {
    uint8_t sectors[brook::GuardSlotCount][brook::GuardSlotSectorBytes]{};
    bool failWrite=false;
    bool failFlush=false;
    bool read(unsigned slot,uint8_t* data) const {
        memcpy(data,sectors[slot],sizeof(sectors[slot]));
        return true;
    }
    bool write(unsigned slot,const uint8_t* data) {
        memcpy(sectors[slot],data,failWrite?32:sizeof(sectors[slot]));
        return !failWrite;
    }
    bool flush() const { return !failFlush; }
};

static void SlotHealthPolicy() {
    using namespace brook;
    const BootGuardSlot previous{1,0,0,{10}};
    const BootGuardSlot newest{2,1,0,{20}};
    const BootGuardSlot otherPair{2,1,0,{21}};
    const BootGuardSlot replacement{3,1,0,{30}};
    GuardSlotHealth health{};
    assert(GuardChooseSlot(health,newest,previous)==GuardSlotChoice::Newest);

    // One failure does not switch slots. Confirmed handoff forgives it.
    assert(GuardSlotStart(health,newest));
    assert(GuardChooseSlot(health,newest,previous)==GuardSlotChoice::Newest);
    assert(!GuardSlotFinish(health,otherPair,false));
    assert(GuardSlotFinish(health,newest,false));
    assert(!GuardSlotFinish(health,newest,false));
    assert(GuardSlotStart(health,newest));
    assert(GuardSlotStart(health,newest));
    assert(!GuardSlotStart(health,newest));
    assert(GuardChooseSlot(health,newest,previous)==GuardSlotChoice::Previous);

    // A replacement or different exact selector must never inherit a strike.
    assert(GuardChooseSlot(health,replacement,previous)==GuardSlotChoice::Newest);
    assert(GuardChooseSlot(health,otherPair,previous)==GuardSlotChoice::Newest);
    assert(GuardChooseSlot(health,newest,{})==GuardSlotChoice::Recovery);
    assert(GuardChooseSlot(health,newest,{1,1,0,{10}})==GuardSlotChoice::Recovery);

    assert(GuardSlotStart(health,previous));
    assert(GuardSlotStart(health,previous));
    assert(GuardChooseSlot(health,newest,previous)==GuardSlotChoice::Recovery);
    assert(GuardSlotStart(health,replacement));
    assert(GuardSlotFinish(health,replacement,true));
    assert(GuardChooseSlot(health,replacement,newest)==GuardSlotChoice::Recovery);
    assert(!GuardSlotStart(health,replacement));
}

static void SlotHealthPersistence() {
    using namespace brook;
    const uint8_t disk[32]={1}, foreign[32]={2};
    const BootGuardSlot selected{8,1,0,{7}};
    SlotHealthIo io;
    GuardSlotJournal journal;
    assert(GuardSlotRead(io,disk,journal) && journal.newest==-1);
    assert(GuardSlotStart(journal.health,selected));
    assert(GuardSlotWrite(io,disk,journal.health));
    assert(!GuardSlotWrite(io,disk,journal.health)); // stale snapshot
    assert(GuardSlotRead(io,disk,journal) && journal.newest==0);
    assert(journal.health.sequence==1 && journal.health.slots[1].starts==1);

    assert(GuardSlotStart(journal.health,selected));
    io.failWrite=true;
    assert(!GuardSlotWrite(io,disk,journal.health));
    assert(GuardSlotRead(io,disk,journal) && journal.newest==0);
    assert(journal.health.sequence==1 && journal.health.slots[1].starts==1);
    assert(GuardSlotStart(journal.health,selected));
    io.failWrite=false;
    assert(GuardSlotWrite(io,disk,journal.health));
    assert(GuardSlotRead(io,disk,journal) && journal.newest==1);
    assert(journal.health.sequence==2 && journal.health.slots[1].starts==2);
    SlotHealthIo uncertain=io;
    uncertain.failFlush=true;
    assert(GuardSlotFinish(journal.health,selected,false));
    assert(!GuardSlotWrite(uncertain,disk,journal.health));
    assert(!GuardSlotRead(io,foreign,journal));

    // Even byte-identical records with the same sequence are ambiguous.
    memcpy(io.sectors[0],io.sectors[1],sizeof(io.sectors[0]));
    assert(!GuardSlotRead(io,disk,journal));
    SlotHealthIo truncated;
    memcpy(truncated.sectors[0],io.sectors[0],32);
    assert(!GuardSlotRead(truncated,disk,journal));
}

static void GuardedSlotSelection() {
    using namespace brook;
    using Result=bootloader::UpdatePairResult;
    const char identity[]="01000000000000000000000000000000"
                          "02000000000000000000000000000000"
                          "03000000000000000000000000000000";
    uint8_t disk[32]{};
    BrookUpdateHash(reinterpret_cast<const uint8_t*>(identity),sizeof(identity)-1,disk);
    provision(0,1);
    provision(1,2);
    const auto& newest=files.at(u"UPDATES\\SELECT1.BIN");
    const auto& previous=files.at(u"UPDATES\\SELECT0.BIN");
    BootGuardSlot recent{2,1,0,{}}, prior{1,0,0,{}};
    BrookUpdateHash(newest.data(),newest.size(),recent.selectorDigest);
    BrookUpdateHash(previous.data(),previous.size(),prior.selectorDigest);
    SlotHealthIo io;
    GuardSlotJournal journal;
    const auto save=[&] {
        assert(GuardSlotWrite(io,disk,journal.health));
        assert(GuardSlotRead(io,disk,journal));
        auto& bytes=files[u"RECOVERY\\BOOTSTAT.BIN"];
        bytes.resize(sizeof(io.sectors));
        memcpy(bytes.data(),io.sectors,bytes.size());
    };
    const auto load=[&](Result expected,uint64_t generation) {
        uint8_t *kernel=nullptr,*initrd=nullptr;UINTN kb=0,ib=0;
        BootGuardSlot selected{};
        assert(bootloader::LoadUpdatePair(nullptr,nullptr,kernel,kb,initrd,ib,selected,identity)==expected);
        assert(selected.generation==generation);
        if(generation) {
            assert(kernel&&initrd);
            bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)kernel,kb);
            bootloader::FreePages(nullptr,(EFI_PHYSICAL_ADDRESS)initrd,ib);
        } else assert(!kernel&&!initrd);
        assert(allocations.empty());
    };

    load(Result::Loaded,2); // No journal on an existing installation.
    assert(GuardSlotStart(journal.health,recent));
    save();
    load(Result::Loaded,2);
    auto& recentInitrd=files.at(u"UPDATES\\B\\INITRD.IMG");
    auto& priorInitrd=files.at(u"UPDATES\\A\\INITRD.IMG");
    recentInitrd[0]^=1;
    priorInitrd[0]^=1;
    load(Result::RecoveryRequired,0);
    recentInitrd[0]^=1;
    priorInitrd[0]^=1;
    assert(GuardSlotStart(journal.health,recent));
    save();
    load(Result::Previous,1);

    priorInitrd[0]^=1;
    load(Result::RecoveryRequired,0);
    priorInitrd[0]^=1;
    assert(GuardSlotStart(journal.health,prior));
    assert(GuardSlotStart(journal.health,prior));
    save();
    load(Result::RecoveryRequired,0); // Both verified slots exhausted.

    journal.health.slots[1].starts=1;
    assert(GuardSlotFinish(journal.health,recent,true));
    journal.health.slots[0].starts=0;
    save();
    load(Result::RecoveryRequired,0); // A shared profile is not repaired by fallback.

    // An explicit choice pins only this generation and clears only that slot.
    assert(GuardSlotPrefer(journal.health,prior,recent.generation));
    save();
    load(Result::Previous,1);
    assert(journal.health.slots[1].profileRejected);
    assert(GuardSlotStart(journal.health,prior));
    assert(GuardSlotFinish(journal.health,prior,false));
    save();
    load(Result::Previous,1);
    const BootGuardSlot later{3,0,0,{42}};
    assert(GuardChooseSlot(journal.health,later,recent)==GuardSlotChoice::Newest);
    assert(GuardSlotStart(journal.health,later));
    assert(!journal.health.preferredAtGeneration);
    save();
    files.clear();
}

int main() {
    SlotHealthPolicy();
    SlotHealthPersistence();
    GuardedSlotSelection();
    const char* identity="01000000000000000000000000000000"
                         "02000000000000000000000000000000"
                         "03000000000000000000000000000000";
    auto guard=[&](bool expected) {
        assert(brook::bootloader::RecoveryGuardAllowsNormal(nullptr,nullptr,identity)==expected);
        assert(allocations.empty());
    };
    guard(false);
    Bytes g(128);memcpy(g.data(),"BRKRCV01",8);BrookUpdatePut64(g.data()+8,1);
    BrookUpdatePut32(g.data()+16,BROOK_RECOVERY_GUARD_CLEAN);
    g[24]=1;g[40]=2;g[56]=3;BrookUpdatePut32(g.data()+124,BrookUpdateCrc(g.data(),124));
    files[u"RECOVERY\\GUARD0.BIN"]=g;guard(false);
    files[u"RECOVERY\\GUARD1.BIN"]=g;guard(true);
    files[u"RECOVERY\\GUARD1.BIN"].resize(127);guard(false);
    files[u"RECOVERY\\GUARD1.BIN"]=g;files[u"RECOVERY\\GUARD1.BIN"][125]^=1;guard(false);
    files[u"RECOVERY\\GUARD1.BIN"]=g;
    BrookUpdatePut32(g.data()+16,BROOK_RECOVERY_GUARD_PENDING);BrookUpdatePut32(g.data()+124,BrookUpdateCrc(g.data(),124));
    files[u"RECOVERY\\GUARD0.BIN"]=g;files[u"RECOVERY\\GUARD1.BIN"]=g;guard(false);
    expect(false);provision(0,1);expect(true,1);provision(1,2);expect(true,2);
    files.erase(u"UPDATES\\B\\INITRD.IMG");expect(true,1); // no mixed root/A initrd
    files.erase(u"UPDATES\\A\\INITRD.IMG");expect(false);
    provision(0,1);provision(1,2);files[u"UPDATES\\B\\KERNEL.ELF"][200]^=1;expect(true);
    // Correct hash does not excuse an out-of-range ELF program-header table.
    Bytes bad=kernel();((Elf64Header*)bad.data())->phOffset=~uint64_t{0};provision(1,2,bad);expect(true);
    files.erase(u"UPDATES\\A\\KERNEL.ELF");expect(false);
    expectRecovery(false);
    provision(0,7);
    files[u"RECOVERY\\SELECT.BIN"]=files[u"UPDATES\\SELECT0.BIN"];
    files[u"RECOVERY\\KERNEL.ELF"]=files[u"UPDATES\\A\\KERNEL.ELF"];
    files[u"RECOVERY\\INITRD.IMG"]=files[u"UPDATES\\A\\INITRD.IMG"];
    expectAlternate(false);
    files[u"RECOV2\\SELECT.BIN"]=files[u"RECOVERY\\SELECT.BIN"];
    files[u"RECOV2\\KERNEL.ELF"]=files[u"RECOVERY\\KERNEL.ELF"];
    files[u"RECOV2\\INITRD.IMG"]=files[u"RECOVERY\\INITRD.IMG"];
    expectAlternate(true);expectRecovery(true);
    files[u"RECOV2\\INITRD.IMG"][0]^=1;expectAlternate(false);expectRecovery(true);
    files[u"RECOV2\\INITRD.IMG"]=files[u"RECOVERY\\INITRD.IMG"];
    files[u"RECOV2\\SELECT.BIN"][124]^=1;expectAlternate(false);expectRecovery(true);
    files[u"RECOV2\\SELECT.BIN"]=files[u"RECOVERY\\SELECT.BIN"];
    files.erase(u"RECOV2\\KERNEL.ELF");expectAlternate(false);expectRecovery(true);
    expectRecovery(true);
    files[u"RECOVERY\\INITRD.IMG"][0]^=1;expectRecovery(false);
    files[u"RECOVERY\\INITRD.IMG"]=files[u"UPDATES\\A\\INITRD.IMG"];
    files[u"RECOVERY\\SELECT.BIN"][124]^=1;expectRecovery(false);
    files[u"RECOVERY\\SELECT.BIN"]=files[u"UPDATES\\SELECT0.BIN"];
    files.erase(u"RECOVERY\\KERNEL.ELF");expectRecovery(false);
    files[u"RECOVERY\\KERNEL.ELF"]=files[u"UPDATES\\A\\KERNEL.ELF"];
    files.erase(u"RECOVERY\\INITRD.IMG");expectRecovery(false);
    files[u"RECOVERY\\INITRD.IMG"]=files[u"UPDATES\\A\\INITRD.IMG"];
    files[u"RECOVERY\\INITRD.IMG"].resize(511);expectRecovery(false);
    files[u"RECOVERY\\INITRD.IMG"]=files[u"UPDATES\\A\\INITRD.IMG"];
    bad=kernel();((Elf64Header*)bad.data())->phOffset=~uint64_t{0};provision(0,8,bad);
    files[u"RECOVERY\\SELECT.BIN"]=files[u"UPDATES\\SELECT0.BIN"];
    files[u"RECOVERY\\KERNEL.ELF"]=bad;expectRecovery(false);
    // Every interrupted prefix of the append must leave the loader on either
    // the original disabled setting or the complete, prepared slot setting.
    const std::string oldConfig="TARGET=KERNEL\\BROOK.ELF\nUPDATE_SLOTS=0\n";
    const std::string activation="\nUPDATE_SLOTS=1\n";
    for(size_t n=0;n<=activation.size();++n) {
        const auto config=oldConfig+activation.substr(0,n);
        files[u"BROOK.CFG"]=Bytes(config.begin(),config.end());
        brook::bootloader::LoadConfig(nullptr,nullptr);
        assert(brook::bootloader::g_bootConfig.updateSlots == (config.find("UPDATE_SLOTS=1")!=std::string::npos));
        assert(allocations.empty());
    }
    provision(0,2);provision(1,2);expect(false); // ambiguous selection fails closed
    provision(1,3);expect(true);
    files[u"UPDATES\\B\\KERNEL.ELF"].resize(BROOK_UPDATE_KERNEL_MAX+1);expect(true); // older A only
    files.erase(u"UPDATES\\A\\KERNEL.ELF");expect(false);
    puts("UPDATE_SLOTS_PASS");
}
