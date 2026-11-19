#include "core_autodump.h"

#include "apic.h"
#include "klog.h"
#include "process.h"
#include "rtc.h"
#include "scheduler.h"
#include "vfs_path.h"

namespace brook {
namespace {

constexpr char DirectoryName[]="/data/home/.local/share/brook/cores";
constexpr char PendingName[]=".brook-core-pending";
enum : uint32_t { Idle, Capturing, Pending, Writing };
uint32_t g_state=Idle;
uint32_t g_mode=static_cast<uint32_t>(CoreAutoMode::Uid1000);
bool g_worker;
uint64_t g_started, g_queued, g_completed, g_failed, g_timedOut, g_busy, g_rateLimited;
CoreAutoRate g_rate;
CoreThreadNote g_notes[MAX_PROCESSES];
CoreAutoJob g_job;

void Increment(uint64_t& counter){__atomic_add_fetch(&counter,1,__ATOMIC_RELAXED);}
uint64_t Read(const uint64_t& counter){return __atomic_load_n(&counter,__ATOMIC_RELAXED);}

uint64_t ExecutableKey(const Process* leader)
{
    const char* name=leader->exePath[0]?leader->exePath:leader->name;
    const uint32_t bytes=leader->exePath[0]?sizeof(leader->exePath):sizeof(leader->name);
    uint64_t key=14695981039346656037ull;
    for(uint32_t i=0;i<bytes && name[i];++i)
        key=(key^static_cast<uint8_t>(name[i]))*1099511628211ull;
    return key?key:1;
}

struct Path {
    VfsPath path{};
    ~Path(){VfsPathRelease(path);}
};

bool Deadline(void*){
    return CoreAutoExpired(g_job.started,ApicTickCount());
}

int EnterDirectory(const VfsPath& root,Path& current,const char* child,bool privateDirectory)
{
    if(Deadline(nullptr))return -110;
    Path next;
    int result=VfsPathResolve(root,current.path,child,&next.path,false);
    if(result==-2){
        result=VfsMkdirAt(root,current.path,child,privateDirectory?0700:0755,0);
        if(!result && !privateDirectory){
            // Leave the ordinary user home directories usable by their owner.
            result=VfsChownAt(root,current.path,child,1000,1000,false);
        }
        if(result && result!=-17)return result;
        result=VfsPathResolve(root,current.path,child,&next.path,false);
    }
    if(result)return result;
    VnodeStat st{};
    if((result=VfsPathStat(next.path,&st)))return result;
    if(!st.isDir || st.isSymlink)return -20;
    if(privateDirectory && (st.uid || (st.mode&0777)!=0700))return -13;
    VfsPathRelease(current.path);
    current.path=next.path;
    next.path={};
    return 0;
}

bool IsNameCharacter(char ch)
{
    return (ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9') ||
           ch=='_' || ch=='-';
}

bool Number(const char*& next,uint64_t& value)
{
    if(*next<'0' || *next>'9')return false;
    value=0;
    while(*next>='0' && *next<='9'){
        const uint32_t digit=static_cast<uint32_t>(*next++-'0');
        if(value>(~uint64_t{0}-digit)/10)return false;
        value=value*10+digit;
    }
    return true;
}

bool Timestamp(const char* name,uint64_t& stamp)
{
    const char* start=name;
    while(IsNameCharacter(*name))++name;
    if(name==start || *name++!='.')return false;
    uint64_t pid=0;
    if(!Number(name,pid) || !pid || pid>65535 || *name++!='.' || !Number(name,stamp))return false;
    const char suffix[]=".core";
    for(const char ch:suffix)if(*name++!=ch)return false;
    return true;
}

struct BudgetDirectory {
    const VfsPath& root;
    const VfsPath& dir;
};

int Reserve(void* raw,uint64_t incoming)
{
    auto& path=*static_cast<BudgetDirectory*>(raw);
    if(incoming>CoreAutoMaxBytes)return -27;
    for(;;){
        if(Deadline(nullptr))return -110;
        int error=0;
        auto* listing=VfsPathOpen(path.dir,VFS_O_READ,&error);
        if(!listing)return error?error:-5;
        uint32_t cookie=0,visited=0;
        CoreAutoRetention retention;
        char oldest[256]{};
        while(!Deadline(nullptr)){
            DirEntry entry{};
            const int next=VfsReaddir(listing,&entry,&cookie);
            if(next<=0){error=next;break;}
            if(++visited>1024){error=-7;break;}
            uint64_t stamp=0;
            if(entry.isDir || !Timestamp(entry.name,stamp))continue;
            VnodeStat st{};
            error=VfsStatAt(path.root,path.dir,entry.name,&st,false);
            if(error)break;
            if(st.isSymlink || st.uid || (st.mode&0170000)!=0100000 || st.nlink!=1)continue;
            bool isOldest=false;
            if(!retention.add(st.size,stamp,isOldest)){error=-75;break;}
            if(isOldest){
                for(uint32_t i=0;i<sizeof(oldest);++i)oldest[i]=entry.name[i];
            }
        }
        VfsClose(listing);
        if(Deadline(nullptr))return -110;
        if(error)return error;
        if(!retention.needsRotation(incoming))return 0;
        if(!retention.count)return -27;
        if(int removed=VfsRemoveAt(path.root,path.dir,oldest,false))return removed;
    }
}

char* WriteDecimal(char* end,uint64_t value)
{
    char reversed[20];
    unsigned length=0;
    do {
        reversed[length++]=char('0'+value%10);
        value/=10;
    } while(value);
    while(length)*end++=reversed[--length];
    return end;
}

void CoreCommandName(const CoreProcessNote& info,char (&comm)[16])
{
    for(uint32_t i=0;i+1<sizeof(comm) && info.name[i];++i){
        const char ch=info.name[i];
        comm[i]=IsNameCharacter(ch)?ch:'_';
    }
    if(!comm[0])comm[0]='_';
}

int WriteJob(const char (&comm)[16],char (&name)[80])
{
    auto& snapshot=g_job.snapshot;
    char* end=name;
    for(char ch:comm){if(!ch)break;*end++=ch;}
    *end++='.';
    end=WriteDecimal(end,unsigned(snapshot.info.pid));
    *end++='.';
    end=WriteDecimal(end,RtcNow());
    for(char ch:".core")*end++=ch;

    Path root,dir;
    if(Deadline(nullptr))return -110;
    int error=VfsPathRoot(&root.path);
    if(error)return error;
    error=VfsPathResolve(root.path,root.path,"/data/home",&dir.path,false);
    if(error)return error;
    VnodeStat home{};
    if((error=VfsPathStat(dir.path,&home)))return error;
    if(!home.isDir || home.isSymlink)return -20;
    const char* parts[]={".local","share","brook","cores"};
    for(uint32_t i=0;i<4;++i)if((error=EnterDirectory(root.path,dir,parts[i],i==3)))return error;

    VnodeStat staged{};
    error=VfsStatAt(root.path,dir.path,PendingName,&staged,false);
    if(!error){
        if(staged.uid || staged.isSymlink || (staged.mode&0170000)!=0100000 || staged.nlink!=1)return -13;
        if((error=VfsRemoveAt(root.path,dir.path,PendingName,false)))return error;
    }else if(error!=-2)return error;

    BudgetDirectory budget{root.path,dir.path};
    const CoreDumpDestination destination{&root.path,&dir.path,g_job.byteLimit,Deadline,Reserve,&budget};
    error=CoreDumpWriteSnapshot(snapshot,PendingName,false,destination);
    // No further target memory access once the file contents are written.
    AddressSpaceUnref(snapshot.space);
    snapshot.space=nullptr;
    if(!error && Deadline(nullptr))error=-110;
    if(!error)error=VfsRenameNoReplaceAt(root.path,dir.path,PendingName,dir.path,name);
    if(error)(void)VfsRemoveAt(root.path,dir.path,PendingName,false);
    return error;
}

void Worker(void*)
{
    for(;;){
        if(__atomic_load_n(&g_state,__ATOMIC_ACQUIRE)!=Pending){SchedulerSleepMs(100);continue;}
        __atomic_store_n(&g_state,Writing,__ATOMIC_RELEASE);
        auto& snapshot=g_job.snapshot;
        char comm[16]{},name[80]{};
        CoreCommandName(snapshot.info,comm);
        const int status=WriteJob(comm,name);
        if(snapshot.space)AddressSpaceUnref(snapshot.space);
        const unsigned pid=static_cast<unsigned>(snapshot.info.pid);
        if(status){
            if(status==-110 || Deadline(nullptr))Increment(g_timedOut);
            else Increment(g_failed);
            KLOG_WARN(General,"core-dump: %s pid=%u sig=%d abandoned at %s (error=%d)",
                      comm,pid,g_job.signal,snapshot.result.stage,status);
        }else{
            Increment(g_completed);
            KLOG_INFO(General,"core-dump: %s pid=%u sig=%d -> %s/%s (%lu)",
                      comm,pid,g_job.signal,DirectoryName,name,snapshot.result.bytes);
        }
        __atomic_store_n(&g_started,uint64_t{0},__ATOMIC_RELAXED);
        __atomic_store_n(&g_state,Idle,__ATOMIC_RELEASE);
    }
}

} // namespace

CoreAutoJob* CoreAutoClaim(const Process* target,const Process* leader,int signal)
{
    const auto mode=static_cast<CoreAutoMode>(__atomic_load_n(&g_mode,__ATOMIC_RELAXED));
    if(!__atomic_load_n(&g_worker,__ATOMIC_ACQUIRE) ||
       !CoreAutoEligible(mode,target->isKernelThread,target->uid,target->euid,target->suid,
                         signal,leader->coreLimits))return nullptr;
    uint32_t idle=Idle;
    if(!__atomic_compare_exchange_n(&g_state,&idle,Capturing,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)){
        Increment(g_busy);return nullptr;
    }
    const uint64_t now=ApicTickCount();
    if(!g_rate.Admit(ExecutableKey(leader),now)){
        Increment(g_rateLimited);
        __atomic_store_n(&g_state,Idle,__ATOMIC_RELEASE);
        return nullptr;
    }
    g_job={};
    g_job.snapshot.threads=g_notes;
    g_job.snapshot.capacity=MAX_PROCESSES;
    g_job.signal=signal;
    g_job.started=now;
    g_job.byteLimit=CoreAutoBudget(leader->coreLimits);
    __atomic_store_n(&g_started,now,__ATOMIC_RELAXED);
    return &g_job;
}

void CoreAutoCommit(CoreAutoJob*)
{
    Increment(g_queued);
    __atomic_store_n(&g_state,Pending,__ATOMIC_RELEASE);
}

void CoreAutoStart()
{
    if(auto* worker=KernelThreadCreate("core_dump",Worker,nullptr,2)){
        SchedulerAddProcess(worker);
        __atomic_store_n(&g_worker,true,__ATOMIC_RELEASE);
    }
}

void CoreAutoSetMode(CoreAutoMode mode)
{
    __atomic_store_n(&g_mode,static_cast<uint32_t>(mode),__ATOMIC_RELEASE);
}

CoreAutoStats CoreAutoReadStats()
{
    const auto mode=static_cast<CoreAutoMode>(__atomic_load_n(&g_mode,__ATOMIC_ACQUIRE));
    const uint64_t started=Read(g_started);
    return {mode,Read(g_queued),Read(g_completed),Read(g_failed),Read(g_timedOut),
            Read(g_busy),Read(g_rateLimited),started?ApicTickCount()-started:0,
            __atomic_load_n(&g_worker,__ATOMIC_ACQUIRE)};
}

} // namespace brook
