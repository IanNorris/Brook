#include "core_dump_elf.h"

#include "apic.h"
#include "memory/heap.h"
#include "memory/user_mapping.h"
#include "memory/virtual_memory.h"
#include "process.h"
#include "scheduler.h"
#include "signal_context.h"
#include "vfs.h"
#include "vfs_path.h"

extern "C" void* memcpy(void*,const void*,unsigned long);

namespace brook {

void CoreDumpPublishRegisters(Process* process,const CoreRegisters& registers)
{
    if(!process->addressSpace || process->addressSpace->kernel)return;
    auto& slot=process->coreRegisters;
    // The sole writer is the current task with interrupts disabled.
    __atomic_add_fetch(&slot.sequence,1,__ATOMIC_ACQ_REL);
    __atomic_store_n(&slot.addressSpace,process->addressSpace->identity,__ATOMIC_RELAXED);
    uint64_t words[sizeof(registers)/sizeof(uint64_t)];
    memcpy(words,&registers,sizeof(words));
    auto* destination=reinterpret_cast<uint64_t*>(&slot.registers);
    for(uint32_t i=0;i<sizeof(words)/sizeof(*words);++i)
        __atomic_store_n(destination+i,words[i],__ATOMIC_RELAXED);
    __atomic_add_fetch(&slot.sequence,1,__ATOMIC_RELEASE);
}

void CoreDumpCaptureIrq(Process* process,const IrqSignalFrame& frame)
{
    CoreDumpPublishRegisters(process,{
        frame.r15,frame.r14,frame.r13,frame.r12,frame.rbp,frame.rbx,frame.r11,frame.r10,
        frame.r9,frame.r8,frame.rax,frame.rcx,frame.rdx,frame.rsi,frame.rdi,~uint64_t{0},
        frame.rip,frame.cs,frame.rflags,frame.rsp,frame.ss,process->fsBase,
        process->savedCtx.gsBase,0,0,0,0
    });
}

bool CoreDumpReadRegisters(const Process* process,uint64_t addressSpace,CoreRegisters& out)
{
    const auto& slot=process->coreRegisters;
    for(unsigned attempt=0;attempt<4;++attempt){
        const uint64_t start=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE);
        if(!start || (start&1))continue;
        if(__atomic_load_n(&slot.addressSpace,__ATOMIC_RELAXED)!=addressSpace)return false;
        uint64_t words[sizeof(out)/sizeof(uint64_t)];
        const auto* source=reinterpret_cast<const uint64_t*>(&slot.registers);
        for(uint32_t i=0;i<sizeof(words)/sizeof(*words);++i)
            words[i]=__atomic_load_n(source+i,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQ_REL);
        if(__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE)==start){
            memcpy(&out,words,sizeof(out));
            return true;
        }
    }
    return false;
}

namespace {
constexpr uint32_t MaxPrograms=4096,MaxFileNotes=2048;

template<class T> struct Allocation {
    T* ptr;
    explicit Allocation(uint64_t count):ptr(static_cast<T*>(kmalloc(count*sizeof(T)))){}
    ~Allocation(){kfree(ptr);}
};

struct CoreFileName {
    uint64_t begin,end,pageOffset;
    char name[1024];
};
struct Writer {
    Vnode* node;
    const CoreDumpDestination& destination;
    uint64_t offset=0;
    int error=0;

    bool write(const void* data,uint64_t size){
        if(!size)return true;
        const auto* bytes=static_cast<const uint8_t*>(data);
        while(size){
            if(destination.expired && destination.expired(destination.context)){error=-110;return false;}
            const int written=VfsWrite(node,bytes,size,&offset);
            if(written<=0){error=written?written:-5;return false;}
            bytes+=written;size-=written;
        }
        return true;
    }
};

uint32_t ElfProtection(uint32_t protection){
    return ((protection&1)?4u:0u)|(protection&2)|((protection&4)?1u:0u);
}

uint32_t NameLength(const char* name){
    uint32_t length=0;
    while(name[length])++length;
    return length;
}

void CopyName(char* destination,uint32_t capacity,const char* name){
    uint32_t i=0;
    while(i+1<capacity && name[i]){destination[i]=name[i];++i;}
    destination[i]=0;
}
}

int CoreDumpWriteSnapshot(CoreDumpSnapshot& snapshot,const char* path,bool full,
                          const CoreDumpDestination& destination)
{
    auto& result=snapshot.result;
    auto& info=snapshot.info;
    auto* space=snapshot.space;
    auto* threads=snapshot.threads;
    const uint64_t start=ApicTickCount();
    struct Duration { CoreDumpResult& result;uint64_t start;~Duration(){result.durationMs=ApicTickCount()-start;} } duration{result,start};
    const uint64_t byteLimit=destination.byteLimit?destination.byteLimit:CoreDumpLimit;
    result.stage="byte-budget";
    if(byteLimit<CorePageSize)return -27;
    const auto expired=[&]{return destination.expired && destination.expired(destination.context);};
    if(expired())return -110;
    result.stage="allocate";
    const uint64_t maxResidentPages=byteLimit/CorePageSize;
    Allocation<CoreProgramHeader> programs(MaxPrograms);
    Allocation<uint64_t> identities(MaxPrograms),addresses(maxResidentPages);
    Allocation<CoreFileName> files(MaxFileNotes);
    Allocation<uint8_t> page(CorePageSize);
    if(!programs.ptr || !identities.ptr || !addresses.ptr || !files.ptr || !page.ptr)return -12;

    const uint32_t argBytes=UserMappingReadArguments(space,0,info.args,sizeof(info.args)-1);
    for(uint32_t i=0;i<argBytes;++i)if(!info.args[i])info.args[i]=' ';
    if(!argBytes)CopyName(info.args,sizeof(info.args),info.name);

    uint32_t programCount=1,fileCount=0,auxWords=0;
    uint64_t aux[AddressSpaceState::SavedAuxvWords]{}, fileNameBytes=0, loadBytes=0;
    int layoutError=0;
    {
        KMutexLock(&space->mutation);
        struct Unlock { KMutex* lock;~Unlock(){KMutexUnlock(lock);} } unlock{&space->mutation};
        auxWords=space->savedAuxvWords;
        if(auxWords>AddressSpaceState::SavedAuxvWords){result.stage="auxv";return -5;}
        result.stage="layout";
        memcpy(aux,space->savedAuxv,uint64_t(auxWords)*sizeof(*aux));
        for(const auto* node=space->ranges.head;node;node=node->next){
            if(expired())return -110;
            const auto& range=node->range;
            if(range.kind==MappingKind::Device || !(range.protection&1))continue;
            if(range.sharing==MappingSharing::Shared && range.kind!=MappingKind::File && !full)continue;
            const auto* file=range.kind==MappingKind::File && range.backing?range.backing->fileVnode:nullptr;
            const char* name=file?file->directoryPath:nullptr;
            const bool named=name && name[0]=='/' && NameLength(name)<sizeof(CoreFileName::name);
            if(named){
                if(fileCount==MaxFileNotes){result.stage="file-notes";return -7;}
                auto& entry=files.ptr[fileCount++];
                entry.begin=range.begin;entry.end=range.end;entry.pageOffset=range.offset/CorePageSize;
                CopyName(entry.name,sizeof(entry.name),name);
                fileNameBytes+=NameLength(entry.name)+1;
            }
            ++result.mappings;
            uint64_t pages=0;
            if(!VmmCollectRangeAddresses(space->pageTable,range.begin,range.end,addresses.ptr,
                                         maxResidentPages,&pages)){result.stage="resident-pages";return -7;}
            const uint32_t flags=ElfProtection(range.protection);
            if(!named || full)result.holes+=(range.end-range.begin)/CorePageSize-pages;
            uint32_t active=0;
            for(uint64_t i=0;i<pages;++i){
                if((i&255)==0 && expired())return -110;
                const uint64_t va=addresses.ptr[i];
                const uint64_t leaf=VmmReadLeaf(space->pageTable,VirtualAddress(va));
                if((leaf&(VMM_PRESENT|VMM_USER))!=(VMM_PRESENT|VMM_USER)){
                    if(!named || full)++result.holes;
                    continue;
                }
                // Writable private file pages can contain modifications even
                // if a concurrent protection change cleared their dirty bit.
                if(!full && named && !(leaf&VMM_DIRTY) &&
                   !(range.sharing==MappingSharing::Private && (range.protection&2)))continue;
                if(loadBytes>byteLimit-CorePageSize){result.stage="byte-budget";layoutError=-27;break;}
                if(active && programs.ptr[active].address+programs.ptr[active].memorySize==va){
                    programs.ptr[active].memorySize+=CorePageSize;
                    programs.ptr[active].fileSize+=CorePageSize;
                }else{
                    if(programCount==MaxPrograms){result.stage="load-segments";layoutError=-7;break;}
                    active=programCount++;
                    programs.ptr[active]={1,flags,0,va,0,CorePageSize,CorePageSize,CorePageSize};
                    identities.ptr[active]=range.identity;
                }
                loadBytes+=CorePageSize;
            }
            if(layoutError)break;
        }
    }
    if(layoutError)return layoutError;

    const uint64_t fileNoteBytes=16+uint64_t(fileCount)*24+fileNameBytes;
    uint64_t notes=CoreNoteSize(sizeof(info))+uint64_t(result.threads)*CoreNoteSize(sizeof(CoreThreadNote));
    if(auxWords)notes+=CoreNoteSize(uint64_t(auxWords)*8);
    if(fileCount)notes+=CoreNoteSize(fileNoteBytes);
    const uint64_t noteOffset=sizeof(CoreElfHeader)+uint64_t(programCount)*sizeof(CoreProgramHeader);
    programs.ptr[0]={4,0,noteOffset,0,0,notes,0,4};
    uint64_t finalBytes=CorePageAlign(noteOffset+notes);
    if(finalBytes>byteLimit || loadBytes>byteLimit-finalBytes){result.stage="byte-budget";return -27;}
    for(uint32_t i=1;i<programCount;++i){
        programs.ptr[i].offset=finalBytes;
        finalBytes+=programs.ptr[i].fileSize;
    }

    struct RootPin {VfsPath path{};~RootPin(){VfsPathRelease(path);} } root;
    if(expired())return -110;
    if(!destination.root){
        result.stage="root";
        if(int error=VfsPathRoot(&root.path))return error;
    }
    const auto& tree=destination.root?*destination.root:root.path;
    const auto& directory=destination.directory?*destination.directory:tree;
    if(destination.reserve){
        result.stage="rotation";
        if(int status=destination.reserve(destination.context,finalBytes))return status;
    }
    constexpr char transferRoot[]="/data/.brook-transfer";
    uint32_t match=0;
    while(transferRoot[match] && path[match]==transferRoot[match])++match;
    if(!transferRoot[match] && path[match]=='/'){
        result.stage="transfer-directory";
        const int mkdirResult=VfsMkdirAt(tree,tree,transferRoot,0700,0);
        if(mkdirResult && mkdirResult!=-17)return mkdirResult;
    }
    if(expired())return -110;
    int openError=0;
    result.stage="open";
    auto* node=VfsOpenAt(tree,directory,path,VFS_O_WRITE|VFS_O_CREATE|VFS_O_EXCL,0600,0,&openError);
    if(!node)return openError?openError:-5;
    Writer out{node,destination};
    const auto emit=[&]{
        if(!CoreWriteHeader(out,programs.ptr,programCount) || !CoreWriteNote(out,CoreNtInfo,&info,sizeof(info)))return false;
        for(uint32_t i=0;i<result.threads;++i)
            if(!CoreWriteNote(out,CoreNtStatus,&threads[i],sizeof(CoreThreadNote)))return false;
        if(auxWords && !CoreWriteNote(out,CoreNtAuxv,aux,auxWords*8))return false;
        if(fileCount){
            const CoreNoteHeader header{5,uint32_t(fileNoteBytes),CoreNtFile};
            const char owner[8]="CORE";
            const uint64_t counts[]={fileCount,CorePageSize};
            if(!out.write(&header,sizeof(header)) || !out.write(owner,sizeof(owner)) ||
               !out.write(counts,sizeof(counts)))return false;
            for(uint32_t i=0;i<fileCount;++i){
                const auto& entry=files.ptr[i];
                const uint64_t mapping[]={entry.begin,entry.end,entry.pageOffset};
                if(!out.write(mapping,sizeof(mapping)))return false;
            }
            for(uint32_t i=0;i<fileCount;++i)
                if(!out.write(files.ptr[i].name,NameLength(files.ptr[i].name)+1))return false;
            const uint32_t zero=0;
            if(!out.write(&zero,(-fileNoteBytes)&3))return false;
        }
        for(uint32_t i=1;i<programCount;++i){
            const auto& load=programs.ptr[i];
            for(uint64_t offset=0;offset<load.fileSize;offset+=CorePageSize){
                if(expired()){out.error=-110;return false;}
                if(!UserMappingReadResidentPage(space,load.address+offset,identities.ptr[i],page.ptr)){
                    ++result.holes;
                    continue;
                }
                out.offset=load.offset+offset;
                if(!out.write(page.ptr,CorePageSize))return false;
            }
        }
        return true;
    };
    result.stage="write";
    int status=out.error;
    if(emit()){
        result.stage="size";
        status=expired()?-110:VfsTruncate(node,finalBytes);
    }else status=out.error;
    if(!status && expired())status=-110;
    if(!status && destination.setOwner){
        result.stage="file-owner";
        status=VfsChown(node,destination.uid,destination.gid);
    }
    if(!status){result.stage="sync";status=VfsSync(node);}
    if(!status && expired())status=-110;
    VfsClose(node);
    if(status)return status; // an exclusive output may be incomplete on I/O failure
    result.bytes=finalBytes;
    return 0;
}
int CoreDumpProcess(uint16_t pid,const char* path,bool full,CoreDumpResult& result)
{
    if(!ProcessCurrent() || !pid || !path || *path!='/')return -22;
    Allocation<CoreThreadNote> threads(MAX_PROCESSES);
    if(!threads.ptr)return -12;
    CoreDumpSnapshot snapshot{};
    snapshot.threads=threads.ptr;
    snapshot.capacity=MAX_PROCESSES;
    struct AddressPin {AddressSpaceState* p;~AddressPin(){if(p)AddressSpaceUnref(p);} } pin{
        SchedulerCoreDumpThreads(pid,snapshot.threads,snapshot.capacity,snapshot.info,snapshot.result)};
    if(!pin.p)return -3;
    snapshot.space=pin.p;
    const int status=CoreDumpWriteSnapshot(snapshot,path,full,{});
    result=snapshot.result;
    return status;
}

}
