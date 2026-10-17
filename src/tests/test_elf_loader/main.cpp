#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "elf.h"
#include "process.h"
#include "memory/address_space.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::abort(); } } while (0)
using namespace brook;
namespace {
std::unordered_map<uint64_t, PhysicalAddress> leaves;
std::unordered_set<uint64_t> frames;
unsigned heapBlocks=0, allocations=0;
unsigned failAllocation=0, failMapping=0, mappingCalls=0;
bool failHeap=false;

std::vector<uint8_t> fixture(bool overlap=false, bool reverse=false) {
    std::vector<uint8_t> bytes(0x4000,0);
    std::memset(bytes.data(),0xA1,overlap?0x1800:0x800);
    std::memset(bytes.data()+0x2800,0xB2,0x400);
    auto* h=reinterpret_cast<Elf64_Ehdr*>(bytes.data());*h={};
    h->e_ident[0]=ELFMAG0;h->e_ident[1]=ELFMAG1;h->e_ident[2]=ELFMAG2;h->e_ident[3]=ELFMAG3;
    h->e_ident[4]=ELFCLASS64;h->e_ident[5]=ELFDATA2LSB;h->e_ident[6]=1;
    h->e_type=ET_DYN;h->e_machine=EM_X86_64;h->e_version=1;
    h->e_entry=0x400;h->e_ehsize=sizeof(*h);h->e_phoff=sizeof(*h);
    h->e_phentsize=sizeof(Elf64_Phdr);h->e_phnum=2;
    auto* ph=reinterpret_cast<Elf64_Phdr*>(bytes.data()+h->e_phoff);
    ph[0]={};ph[0].p_type=PT_LOAD;ph[0].p_flags=PF_R|PF_X;ph[0].p_align=4096;
    ph[0].p_filesz=overlap?0x1800:0x800;ph[0].p_memsz=0x1800;
    ph[1]={};ph[1].p_type=PT_LOAD;ph[1].p_flags=PF_R|PF_W;ph[1].p_align=4096;
    ph[1].p_offset=0x2800;ph[1].p_vaddr=overlap?0x1800:0x400800;
    ph[1].p_filesz=0x400;ph[1].p_memsz=0x1000;
    if(reverse)std::swap(ph[0],ph[1]);
    return bytes;
}
uint8_t byte(uint64_t address) {
    auto physical=VmmVirtToPhys(KernelPageTable,VirtualAddress(address));CHECK(physical);
    return *reinterpret_cast<uint8_t*>(physical.raw());
}
void clean(AddressSpaceState& space) {
    MappingRangeDestroy(space.ranges);
    for(auto leaf:leaves)PmmFreePage(leaf.second);
    leaves.clear();CHECK(frames.empty()&&heapBlocks==0);
    allocations=mappingCalls=failAllocation=failMapping=0;failHeap=false;
}
}
namespace brook {
bool ElfValidateMainImage(const uint8_t*,uint64_t);
bool ElfLoad(const uint8_t*,uint64_t,ElfBinary*,AddressSpaceState*,uint16_t);
uint64_t ElfLoadAt(const uint8_t*,uint64_t,uint64_t,AddressSpaceState*,uint16_t);
extern "C" void SerialPrintf(const char*,...) {}
extern "C" void SerialPuts(const char*) {}
extern "C" void* kmalloc(uint64_t size) {
    if(failHeap)return nullptr;
    auto* result=std::malloc(size);if(result)++heapBlocks;return result;
}
extern "C" void kfree(void* memory) { if(memory){CHECK(heapBlocks);--heapBlocks;std::free(memory);} }
PhysicalAddress PmmAllocPage(MemTag tag,uint16_t) {
    CHECK(tag==MemTag::User);++allocations;
    if(allocations==failAllocation)return PhysicalAddress{};
    void* frame=nullptr;CHECK(posix_memalign(&frame,4096,4096)==0);
    std::memset(frame,0xCC,4096);frames.insert(reinterpret_cast<uint64_t>(frame));
    return PhysicalAddress(reinterpret_cast<uint64_t>(frame));
}
void PmmFreePage(PhysicalAddress page) { CHECK(frames.erase(page.raw())==1);std::free(reinterpret_cast<void*>(page.raw())); }
bool VmmMapPage(PageTable,VirtualAddress address,PhysicalAddress frame,uint64_t flags,MemTag,uint16_t) {
    ++mappingCalls;CHECK(flags==(VMM_WRITABLE|VMM_USER));
    if(mappingCalls==failMapping || leaves.count(address.raw()))return false;
    leaves.emplace(address.raw(),frame);return true;
}
PhysicalAddress VmmVirtToPhys(PageTable,VirtualAddress address) {
    auto found=leaves.find(address.raw()&~uint64_t{4095});
    return found==leaves.end()?PhysicalAddress{}:found->second+(address.raw()&4095);
}
void FileMappingRegister(FileMappingRegistration* registration,AddressSpaceState*,MappingBacking* backing) {
    CHECK(!backing);*registration={};
}
void FileMappingRetire(FileMappingRegistration*) {}
void FileMappingUnregister(FileMappingRegistration*) {}
}
extern "C" [[noreturn]] void KernelPanic(const char*,...) { std::abort(); }

int main() {
    for(bool reverse:{false,true})for(bool overlap:{false,true}) {
        auto bytes=fixture(overlap,reverse);AddressSpaceState space{};ElfBinary binary{};
        space.ranges.owner=&space;
        CHECK(ElfValidateMainImage(bytes.data(),bytes.size()));
        CHECK(ElfLoad(bytes.data(),bytes.size(),&binary,&space,7));
        CHECK(leaves.size()==(overlap?3u:4u));
        CHECK(allocations==leaves.size()); // Shared boundary allocated once.
        constexpr uint64_t base=0x200000;
        CHECK(binary.baseAddress==base&&binary.entryPoint==base+0x400);
        CHECK(binary.programBreakLow==base+(overlap?0x3000:0x402000));
        CHECK(binary.allocatedSize==(overlap?0x3000u:0x402000u));
        CHECK(binary.phdrVaddr==base+sizeof(Elf64_Ehdr));
        CHECK(byte(base+0x500)==0xA1);
        const uint64_t second=base+(overlap?0x1800:0x400800);
        CHECK(byte(second)==0xB2&&byte(second+0x3ff)==0xB2);
        CHECK(byte(second+0x400)==0&&byte(second+0xfff)==0);
        if(!overlap) {
            CHECK(byte(base+0x800)==0&&byte(base+0x17ff)==0);
            CHECK(!VmmVirtToPhys(space.pageTable,VirtualAddress(base+0x2000)));
            CHECK(!MappingRangeFind(space.ranges,base+0x2000));
        }
        for(auto leaf:leaves) {
            auto* range=MappingRangeFind(space.ranges,leaf.first);
            CHECK(range&&range->kind==MappingKind::Image&&range->sharing==MappingSharing::Private);
            CHECK(range->leafOwnership==MappingLeafOwnership::Managed);
        }
        clean(space);
    }
    // Static ET_EXEC keeps its linked addresses instead of receiving PIE bias.
    {
        auto bytes=fixture();auto* h=reinterpret_cast<Elf64_Ehdr*>(bytes.data());
        auto* ph=reinterpret_cast<Elf64_Phdr*>(bytes.data()+h->e_phoff);
        h->e_type=ET_EXEC;h->e_entry+=0x400000;
        for(unsigned i=0;i<h->e_phnum;++i)ph[i].p_vaddr+=0x400000;
        AddressSpaceState space{};ElfBinary binary{};
        CHECK(ElfLoad(bytes.data(),bytes.size(),&binary,&space,7));
        CHECK(binary.entryPoint==0x400400&&binary.baseAddress==0x400000&&leaves.size()==4);
        CHECK(byte(0x400500)==0xA1&&!MappingRangeFind(space.ranges,0x402000));clean(space);
    }
    // A filesz-zero segment still owns real zeroed BSS pages; surrounding
    // virtual holes do not. Prefill host frames so missing zeroing is visible.
    {
        auto bytes=fixture();auto* h=reinterpret_cast<Elf64_Ehdr*>(bytes.data());
        auto* ph=reinterpret_cast<Elf64_Phdr*>(bytes.data()+h->e_phoff);h->e_phnum=3;
        ph[2]={};ph[2].p_type=PT_LOAD;ph[2].p_flags=PF_R|PF_W;
        ph[2].p_vaddr=0x100000;ph[2].p_memsz=0x2000;ph[2].p_align=4096;
        AddressSpaceState space{};ElfBinary binary{};
        CHECK(ElfLoad(bytes.data(),bytes.size(),&binary,&space,7));CHECK(leaves.size()==6);
        for(uint64_t i=0;i<0x2000;++i)CHECK(byte(0x300000+i)==0);
        CHECK(!MappingRangeFind(space.ranges,0x302000));clean(space);
    }
    // Malformed input must fail before allocating frames/range metadata.
    auto bytes=fixture();auto* h=reinterpret_cast<Elf64_Ehdr*>(bytes.data());
    auto* ph=reinterpret_cast<Elf64_Phdr*>(bytes.data()+h->e_phoff);
    ph[1].p_filesz=bytes.size();AddressSpaceState space{};ElfBinary binary{};
    CHECK(!ElfValidateMainImage(bytes.data(),bytes.size()));
    CHECK(!ElfLoad(bytes.data(),bytes.size(),&binary,&space,7));
    CHECK(!allocations&&!space.ranges.head);clean(space);
    // Allocation/map failure leaves an unpublished address space that its
    // owner can destroy, with rejected frames returned immediately.
    for(unsigned failure=1;failure<=4;++failure)for(bool mapping:{false,true}) {
        bytes=fixture();if(mapping)failMapping=failure;else failAllocation=failure;
        CHECK(!ElfLoad(bytes.data(),bytes.size(),&binary,&space,7));
        CHECK(leaves.size()==failure-1&&frames.size()==leaves.size());clean(space);
    }
    bytes=fixture();failHeap=true;
    CHECK(!ElfLoad(bytes.data(),bytes.size(),&binary,&space,7));CHECK(!allocations);clean(space);
    // An unrelated pre-existing mapping must survive rejection unchanged.
    bytes=fixture();MappingRange foreign{0x200000,0x201000,0,0,nullptr,7,7,
        MappingKind::Anonymous,MappingSharing::Private,MappingLeafOwnership::Managed};
    { MappingRangePlan plan;CHECK(plan.Prepare(1)&&plan.Insert(space.ranges,foreign)); }
    auto existing=PmmAllocPage(MemTag::User,7);
    CHECK(VmmMapPage(space.pageTable,VirtualAddress(0x200000),existing,VMM_WRITABLE|VMM_USER,MemTag::User,7));
    CHECK(!ElfLoad(bytes.data(),bytes.size(),&binary,&space,7));CHECK(allocations==1&&leaves.size()==1);
    CHECK(VmmVirtToPhys(space.pageTable,VirtualAddress(0x200000))==existing);
    CHECK(MappingRangeFind(space.ranges,0x200000)->kind==MappingKind::Anonymous);clean(space);
    // The unchanged interpreter loader must agree about sparse/overlapping
    // extents and shared boundary pages, including reversed header order.
    for(bool reverse:{false,true})for(bool overlap:{false,true}) {
        bytes=fixture(overlap,reverse);
        CHECK(ElfLoadAt(bytes.data(),bytes.size(),0x800000,&space,7)==0x800400);
        CHECK(leaves.size()==(overlap?3u:4u)&&allocations==leaves.size());
        CHECK(byte(0x800500)==0xA1);
        CHECK(byte(0x800000+(overlap?0x1800:0x400800))==0xB2);
        if(!overlap)CHECK(!MappingRangeFind(space.ranges,0x802000));
        clean(space);
    }
    std::puts("ELF_LOADER PASS sparse/overlap/reversed-phdrs/BSS/metadata/malformed/OOM/map-failure");
}
