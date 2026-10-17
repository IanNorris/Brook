#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../../kernel/src/module.cpp"
namespace {
int failures=0,live=0;
uint64_t lastBytes=0;
bool failAllocation=false;
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
    std::printf("MODULE_SECTIONS %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
