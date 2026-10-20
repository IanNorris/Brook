#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include "../../kernel/src/module.cpp"
namespace {
int failures=0,live=0;
uint64_t lastBytes=0;
bool failAllocation=false;
std::atomic<int> frees{0}, exits{0};
std::atomic<bool> pauseFree{false}, freeEntered{false}, finishFree{false};
void moduleExit() { ++exits; }
const brook::ModuleInfo info{brook::MODULE_MAGIC, brook::MODULE_ABI_VERSION,
    "lifetime-fixture", "1", "test", nullptr, moduleExit};
void resetRegistry() { for (auto& m : brook::g_modules) m={}; frees=0; exits=0; }
void publish(unsigned slot, uint64_t base, bool loading=false) {
    auto& m=brook::g_modules[slot]; m={};m.info=&info;m.baseVirt=base;m.pageCount=2;
    m.active=!loading;m.loading=loading;
}
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"MODULE FAIL line %d: %s\n",__LINE__,#x);++failures;}}while(0)
std::vector<uint8_t> object(uint16_t count) {
    std::vector<uint8_t> bytes(sizeof(Elf64_Ehdr)+uint64_t(count)*sizeof(Elf64_Shdr));
    auto* h=reinterpret_cast<Elf64_Ehdr*>(bytes.data());
    h->e_ident[0]=0x7f;h->e_ident[1]='E';h->e_ident[2]='L';h->e_ident[3]='F';h->e_ident[4]=2;h->e_ident[5]=1;
    h->e_type=ET_REL;h->e_machine=EM_X86_64;
    h->e_shoff=sizeof(*h);h->e_shentsize=sizeof(Elf64_Shdr);h->e_shnum=count;
    return bytes;
}
bool earlyReturn(uint32_t count) {brook::SectionMappings sections(count);return sections.data!=nullptr;}
}
namespace brook {
extern "C" void SerialPuts(const char*) {}
extern "C" void SerialPrintf(const char*,...) {}
void VmmFreePages(VirtualAddress, uint64_t) {
    ++frees;
    if (pauseFree.load()) {
        freeEntered=true;
        while (!finishFree.load()) std::this_thread::yield();
    }
}
extern "C" void* kmalloc(uint64_t size) {lastBytes=size;if(failAllocation)return nullptr;void* p=std::malloc(size);if(p)++live;return p;}
extern "C" void kfree(void* memory) {if(memory)--live;std::free(memory);}
}
int main() {
    auto large=object(70);CHECK(brook::ValidateElf(large.data(),large.size()));
    CHECK(!brook::ValidateElf(large.data(),large.size()-1));
    auto* h=reinterpret_cast<Elf64_Ehdr*>(large.data());
    h->e_shoff=~uint64_t(0)-16;CHECK(!brook::ValidateElf(large.data(),large.size()));
    h->e_shoff=sizeof(*h);h->e_shentsize=63;CHECK(!brook::ValidateElf(large.data(),large.size()));
    h->e_shentsize=sizeof(Elf64_Shdr);h->e_shnum=0;CHECK(!brook::ValidateElf(large.data(),large.size()));
    auto maximum=object(65535);CHECK(brook::ValidateElf(maximum.data(),maximum.size()));
    {brook::SectionMappings sections(70);CHECK(sections.data && live==1 && lastBytes==70*sizeof(brook::LoadedSection));}
    CHECK(live==0);CHECK(earlyReturn(70) && live==0);
    failAllocation=true;CHECK(!earlyReturn(70) && live==0);
    // Exercise actual public retention/unload and production init completion.
    resetRegistry();
    const uintptr_t first=0x1008, second=0x4008, unknown=0x9000;
    publish(0,0x1000,true); // provider is pinnable during init
    CHECK(brook::ModuleRetainAddresses(&first,1));
    CHECK(brook::g_modules[0].resident);
    CHECK(!brook::ModuleCompleteInit(&brook::g_modules[0],-1));
    CHECK(brook::g_modules[0].active && brook::g_modules[0].initFailed && frees==0);
    CHECK(!brook::ModuleUnload(&brook::g_modules[0]) && exits==0 && frees==0);
    resetRegistry();publish(0,0x1000,true);
    CHECK(!brook::ModuleCompleteInit(&brook::g_modules[0],-1));
    CHECK(!brook::g_modules[0].active && !brook::g_modules[0].loading && frees==1);
    resetRegistry();publish(0,0x1000,true);
    CHECK(brook::ModuleCompleteInit(&brook::g_modules[0],0));
    CHECK(brook::ModuleUnload(&brook::g_modules[0]) && exits==1 && frees==1);
    CHECK(!brook::ModuleUnload(&brook::g_modules[0]) && frees==1);
    resetRegistry();publish(0,0x1000);publish(1,0x4000);
    const uintptr_t invalidSet[]={first,unknown};
    CHECK(!brook::ModuleRetainAddresses(invalidSet,2) && !brook::g_modules[0].resident);
    const uintptr_t validSet[]={first,second};
    CHECK(brook::ModuleRetainAddresses(validSet,2));
    CHECK(brook::g_modules[0].resident && brook::g_modules[1].resident);
    const auto snapshot=brook::ModuleSnapshotAt(0);
    CHECK(snapshot.active && snapshot.resident && snapshot.sizeBytes==8192);
    CHECK(std::strcmp(snapshot.name,"lifetime-fixture")==0);
    CHECK(!brook::ModuleSnapshotAt(64).active);
    resetRegistry();
    CHECK(std::strcmp(snapshot.name,"lifetime-fixture")==0); // owned copy survives teardown
    CHECK(!brook::ModuleSnapshotAt(0).active);
    CHECK(!brook::ModuleUnload(nullptr));
    brook::ModuleHandle foreign{};CHECK(!brook::ModuleUnload(&foreign));
    CHECK(!brook::ModuleRetainAddresses(nullptr,1));
    CHECK(!brook::ModuleRetainAddresses(&first,0));

    // A claimed unload excludes retention throughout exit/page teardown.
    resetRegistry();publish(0,0x1000);pauseFree=true;finishFree=false;freeEntered=false;
    bool unloaded=false;
    std::thread retiring([&]{unloaded=brook::ModuleUnload(&brook::g_modules[0]);});
    while(!freeEntered.load()) std::this_thread::yield();
    CHECK(!brook::ModuleRetainAddresses(&first,1));
    {brook::ModuleReservation next;CHECK(next.slot!=&brook::g_modules[0]);}
    finishFree=true;retiring.join();pauseFree=false;
    CHECK(unloaded && frees==1 && exits==1);

    // Concurrent pin/unload has exactly one winner; no freed pinned provider.
    for(unsigned round=0;round<300;++round) {
        resetRegistry();publish(0,0x1000);std::atomic<bool> go{false};
        bool pinned=false, removed=false;
        std::thread pinner([&]{while(!go.load()){} pinned=brook::ModuleRetainAddresses(&first,1);});
        std::thread remover([&]{while(!go.load()){} removed=brook::ModuleUnload(&brook::g_modules[0]);});
        go=true;pinner.join();remover.join();
        CHECK(pinned!=removed);
        CHECK(frees==(removed?1:0) && exits==(removed?1:0));
        CHECK(!pinned || brook::g_modules[0].resident);
    }
    resetRegistry();
    std::atomic<unsigned> ready{0};std::atomic<bool> release{false};
    brook::ModuleHandle* slots[32]{};std::vector<std::thread> reservers;
    for(unsigned i=0;i<32;++i) reservers.emplace_back([&,i]{
        brook::ModuleReservation reservation;slots[i]=reservation.slot;++ready;
        while(!release.load()) std::this_thread::yield();
    });
    while(ready.load()!=32) std::this_thread::yield();release=true;
    for(auto& thread:reservers) thread.join();
    for(unsigned i=0;i<32;++i) {CHECK(slots[i]);for(unsigned j=0;j<i;++j) CHECK(slots[i]!=slots[j]);}
    std::printf("MODULE_SECTIONS %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
