#include <cassert>
#include <cstring>
#include <vector>
#include <elf.h>
#include <sys/procfs.h>
#include <sys/user.h>
#include "core_dump_elf.h"

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

int main(){
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
