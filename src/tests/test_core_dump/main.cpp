#include <cassert>
#include <cstring>
#include <vector>
#include <elf.h>
#include <sys/procfs.h>
#include <sys/user.h>
#include "core_dump_layout.h"

using namespace brook;

struct Output {
    std::vector<unsigned char> bytes;
    bool write(const void* data,uint64_t size) {
        const auto* first=static_cast<const unsigned char*>(data);
        bytes.insert(bytes.end(),first,first+size);
        return true;
    }
    template<class T> T read(uint64_t offset) const {
        assert(offset+sizeof(T)<=bytes.size());
        T value{};
        std::memcpy(&value,bytes.data()+offset,sizeof(value));
        return value;
    }
};

static void ExtendedProgramCount()
{
    constexpr uint32_t mappingCount=70000;
    std::vector<CorePageRef> pages;
    pages.reserve(mappingCount);
    for(uint32_t i=0;i<mappingCount;++i)
        pages.push_back({uint64_t(i+1)*2*CorePageSize,i+1,PF_R|PF_W});
    const CorePageRange runs[]={{0,0},{0,mappingCount},{mappingCount,mappingCount}};
    const CorePageCursor cursor(pages.data(),runs);
    assert(CoreBuildLoads(cursor,nullptr)==mappingCount+1);
    std::vector<CoreProgramHeader> program(mappingCount+1);
    assert(CoreBuildLoads(cursor,program.data())==program.size());
    for(const uint32_t count:{uint32_t(PN_XNUM)-1,uint32_t(PN_XNUM),mappingCount+1}){
        program[0]={PT_NOTE,0,CoreHeaderBytes(count),0,0,CoreNoteSize(0),0,4};
        Output bytes;
        assert(CoreWriteHeader(bytes,program.data(),count));
        assert(bytes.bytes.size()==CoreHeaderBytes(count));
        const auto elf=bytes.read<Elf64_Ehdr>(0);
        if(count<PN_XNUM)assert(elf.e_phnum==count && !elf.e_shoff);
        else{
            assert(elf.e_phnum==PN_XNUM && elf.e_shentsize==sizeof(Elf64_Shdr) && elf.e_shnum==1);
            const auto section=bytes.read<Elf64_Shdr>(elf.e_shoff);
            assert(section.sh_type==SHT_NULL && section.sh_info==count);
        }
        const auto last=bytes.read<Elf64_Phdr>(elf.e_phoff+uint64_t(count-1)*elf.e_phentsize);
        assert(last.p_type==PT_LOAD && last.p_vaddr==pages[count-2].address && last.p_memsz==CorePageSize);
        assert(bytes.read<Elf64_Phdr>(elf.e_phoff).p_offset==bytes.bytes.size());
    }
}

static void FragmentedBrowserLayout()
{
    constexpr uint32_t groups=5500, stackPages=16, heapPages=32;
    constexpr uint64_t stack=0x7fff00000000,heap=0x7ffd00000000;
    std::vector<CorePageRef> pages;
    for(uint32_t i=0;i<heapPages;++i)pages.push_back({heap+i*CorePageSize,10,PF_R|PF_W});
    for(uint32_t i=0;i<stackPages;++i)pages.push_back({stack+i*CorePageSize,11,PF_R|PF_W});
    const uint32_t essential=pages.size();
    for(uint32_t i=0;i<groups;++i){
        const uint64_t base=0x400000+i*4*CorePageSize;
        pages.push_back({base,100+i*2,PF_R|PF_W});
        pages.push_back({base+CorePageSize,101+i*2,PF_R|PF_W}); // adjacent, different VMA
    }
    const uint32_t data=pages.size();
    // Executable mappings fill the holes between groups and are admitted last.
    for(uint32_t i=0;i<groups;++i)
        pages.push_back({0x400000+(i*4+2)*CorePageSize,30000+i,PF_R|PF_X});
    const CorePageRange full[]={{0,essential},{essential,data},{data,uint32_t(pages.size())}};
    assert(CoreBuildLoads(CorePageCursor(pages.data(),full),nullptr)==groups*2+3);
    // When the file budget is tight, only the first 100 optional pages fit.
    constexpr uint64_t notes=65536;
    const uint32_t allowed=CoreLoadPageBudget(uint64_t(data+101)*(CorePageSize+sizeof(CoreProgramHeader))+
                                           CorePageAlign(CoreHeaderBytes(1)+sizeof(CoreSectionHeader)+notes),notes);
    assert(allowed>=data+100 && allowed<=data+101);
    const CorePageRange trimmed[]={{0,essential},{essential,data},{data,allowed}};
    const CorePageCursor cursor(pages.data(),trimmed);
    std::vector<CoreProgramHeader> program(CoreBuildLoads(cursor,nullptr));
    assert(CoreBuildLoads(cursor,program.data())==groups+allowed-data+3);
    assert(program[1].memorySize==2*CorePageSize && program[1].flags==(PF_R|PF_W));
    assert(program[program.size()-2].address==heap && program[program.size()-2].memorySize==heapPages*CorePageSize);
    assert(program.back().address==stack && program.back().memorySize==stackPages*CorePageSize);
    CorePageCursor pageOrder=cursor;
    CorePageRef first{},second{};
    assert(pageOrder.next(first) && pageOrder.next(second) && second.identity==first.identity+1);
}

int main(){
    ExtendedProgramCount();
    FragmentedBrowserLayout();
    static_assert(sizeof(elf_prstatus)==sizeof(CoreThreadNote));
    static_assert(sizeof(elf_prpsinfo)==sizeof(CoreProcessNote));
    static_assert(sizeof(user_regs_struct)==sizeof(CoreRegisters));
    static_assert(offsetof(user_regs_struct,rip)==offsetof(CoreRegisters,rip));
    static_assert(offsetof(user_regs_struct,fs_base)==offsetof(CoreRegisters,fsBase));

    CoreProcessNote process{};
    process.pid=40;
    CoreThreadNote threads[2]{};
    threads[0].pid=40;
    threads[0].registers.rip=0x401234;
    threads[0].registers.rsp=0x7fff0000;
    threads[1].pid=41;
    threads[1].registers.rip=0x402abc;
    threads[1].registers.rsp=0x7ffe0000;
    threads[1].registers.origRax=0; // read
    const uint64_t aux[]={6,4096,0,0};
    uint64_t notes=CoreNoteSize(sizeof(process))+2*CoreNoteSize(sizeof(threads[0]))+CoreNoteSize(sizeof(aux));
    const uint64_t start=sizeof(Elf64_Ehdr)+2*sizeof(Elf64_Phdr);
    const CoreProgramHeader headers[]={
        {PT_NOTE,0,start,0,0,notes,0,4},
        {PT_LOAD,PF_R|PF_W,4096,0x7ffe0000,0,4096,4096,4096},
    };
    Output output;
    assert(CoreWriteHeader(output,headers,2));
    assert(CoreWriteNote(output,CoreNtInfo,&process,sizeof(process)));
    for(const auto& t:threads)assert(CoreWriteNote(output,CoreNtStatus,&t,sizeof(t)));
    assert(CoreWriteNote(output,CoreNtAuxv,aux,sizeof(aux)));
    assert(output.bytes.size()==start+notes);
    output.bytes.resize(8192);
    output.bytes[4096]=42;

    const auto elf=output.read<Elf64_Ehdr>(0);
    assert(!std::memcmp(elf.e_ident,ELFMAG,SELFMAG) && elf.e_type==ET_CORE && elf.e_machine==EM_X86_64 && elf.e_phnum==2);
    const auto note=output.read<Elf64_Phdr>(elf.e_phoff);
    assert(note.p_type==PT_NOTE && note.p_filesz==notes);
    uint64_t cursor=note.p_offset;
    unsigned statuses=0,infos=0,auxvs=0;
    while(cursor<note.p_offset+note.p_filesz){
        const auto h=output.read<Elf64_Nhdr>(cursor);
        assert(h.n_namesz==5 && !std::memcmp(output.bytes.data()+cursor+12,"CORE",5));
        const uint64_t desc=cursor+20;
        if(h.n_type==NT_PRSTATUS){
            const auto status=output.read<elf_prstatus>(desc);
            assert(statuses<2 && h.n_descsz==sizeof(status));
            assert(status.pr_pid==threads[statuses].pid);
            assert(uint64_t(status.pr_reg[16])==threads[statuses].registers.rip);
            assert(uint64_t(status.pr_reg[19])==threads[statuses].registers.rsp);
            ++statuses;
        }else if(h.n_type==NT_PRPSINFO){
            assert(output.read<elf_prpsinfo>(desc).pr_pid==40);
            ++infos;
        }else if(h.n_type==NT_AUXV){
            assert(output.read<uint64_t>(desc+8)==4096);
            ++auxvs;
        }else assert(false);
        cursor=desc+((h.n_descsz+3)&~uint64_t{3});
    }
    assert(cursor==start+notes && statuses==2 && infos==1 && auxvs==1);
    const auto load=output.read<Elf64_Phdr>(elf.e_phoff+elf.e_phentsize);
    assert(load.p_type==PT_LOAD && load.p_vaddr==threads[1].registers.rsp);
    assert(load.p_filesz==4096 && output.bytes[load.p_offset]==42);
}
