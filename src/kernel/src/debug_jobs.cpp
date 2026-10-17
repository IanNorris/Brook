#include "debug_jobs.h"
#include "debug_job_wire.h"
#include "process.h"
#include "scheduler.h"
#include "shell.h"
#include "pipe.h"
#include "net.h"
#include "sync/kmutex.h"
#include "memory/heap.h"
#include "string.h"

namespace brook {
namespace {
// At most 32 MiB retained output + 8 MiB request bodies per boot. IDs remain
// reserved until reboot so an old commit can never execute a second process.
constexpr uint32_t MaxJobs=256,MaxActive=8,Retain=65536;
enum class State { Uploading,Queued,Running,Complete,Failed };
struct Output { char* bytes;uint64_t total,fullWaits;bool eof; };
struct Job {
    char id[33];State state;uint8_t* request;uint32_t size,received;
    uint16_t pid;uint64_t incarnation;uint64_t tag;
    bool leaderExited,waited;int waitStatus;
    uint32_t tasks,groupTasks;
    Output output[2];
    const char* error;
};
Job jobs[MaxJobs]{};
KMutex lock{};
struct Guard { Guard(){KMutexLock(&lock);} ~Guard(){KMutexUnlock(&lock);} };
char responseBytes[4096];uint32_t responseLength;
struct Response {
    Guard guard;
    Response(){responseLength=0;}
    ~Response(){responseBytes[responseLength]=0;DebugChannelSend(responseBytes);}
};
const char* StateName(State state) {
    switch(state) {
    case State::Uploading:return "uploading";case State::Queued:return "queued";
    case State::Running:return "running";case State::Complete:return "completed";
    case State::Failed:return "failed";
    }return "unknown";
}
void Send(const char* text) {
    while(*text && responseLength<sizeof(responseBytes)-1) responseBytes[responseLength++]=*text++;
}
void Number(uint64_t value) {
    char buf[24];uint32_t n=0;
    do {buf[n++]=char('0'+value%10);value/=10;} while(value);
    char forward[24];for(uint32_t i=0;i<n;++i) forward[i]=buf[n-i-1];forward[n]=0;Send(forward);
}
void Field(const char* name,uint64_t value) {Send(",\"");Send(name);Send("\":");Number(value);}
void Error(const char* error) {Send("{\"job_error\":\"");Send(error);Send("\"}\n");}
void Status(const Job& job) {
    Send("{\"job_id\":\"");Send(job.id);Send("\",\"state\":\"");Send(StateName(job.state));Send("\"");
    Field("received",job.received);Field("request_size",job.size);Field("pid",job.pid);
    Field("incarnation",job.incarnation);Field("live_tasks",job.tasks);Field("original_group_tasks",job.groupTasks);
    Send(",\"leader_exited\":");Send(job.leaderExited?"true":"false");
    Send(",\"exit_collected\":");Send(job.waited?"true":"false");
    Field("wait_status",static_cast<uint32_t>(job.waitStatus));
    Send(",\"stdout_eof\":");Send(job.output[0].eof?"true":"false");
    Send(",\"stderr_eof\":");Send(job.output[1].eof?"true":"false");
    Field("stdout_bytes",job.output[0].total);Field("stderr_bytes",job.output[1].total);
    Field("stdout_full_waits",job.output[0].fullWaits);Field("stderr_full_waits",job.output[1].fullWaits);
    if (job.error) {Send(",\"error\":\"");Send(job.error);Send("\"");}
    Send("}\n");
}
int Hex(char c) {return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:-1;}
bool Decimal(const char* text,uint64_t* out) {
    if (!*text) return false;
    uint64_t v=0;for(;*text;++text) {
        if (*text<'0' || *text>'9' || v>(~uint64_t(0)-uint64_t(*text-'0'))/10) return false;
        v=v*10+uint64_t(*text-'0');
    }*out=v;return true;
}
bool Id(const char* text) {
    for(uint32_t i=0;i<32;++i) if (!text[i] || Hex(text[i])<0) return false;
    return !text[32];
}
void Drain(Output& output,PipeBuffer* pipe) {
    char bytes[4096];uint32_t n;
    // Bound each turn so continuous stdout cannot starve stderr or control.
    for (uint32_t turn=0;turn<16 && (n=pipe->read(bytes,sizeof(bytes)));++turn) {
        { Guard guard;
          for(uint32_t i=0;i<n;++i) output.bytes[(output.total+i)%Retain]=bytes[i];
          output.total+=n; }
        // Ordinary pipe writers already retry on a 10 ms timeout. Their
        // legacy raw waiter pointer is not lifetime-pinned, so this diagnostic
        // reader must never dereference it after a signalled producer exits.
    }
    SpinLockAcquire(&pipe->lock);
    bool eof=pipe->writers==0 && pipe->empty();
    SpinLockRelease(&pipe->lock);
    {Guard guard;output.eof=eof;output.fullWaits=__atomic_load_n(&pipe->fullWriteWaits,__ATOMIC_RELAXED);}
}
void Worker(void* argument) {
    auto& job=*static_cast<Job*>(argument);
    DebugJobRequest request{};
    if (!DebugJobDecode(job.request,job.size,&request)) {
        Guard guard;job.error="Invalid structured request";job.state=State::Failed;return;
    }
    auto* out=PipeBufferCreate(PIPE_BUF_DEFAULT_SIZE);
    auto* err=PipeBufferCreate(PIPE_BUF_DEFAULT_SIZE);
    if (!out || !err) {
        PipeBufferDestroy(out);PipeBufferDestroy(err);
        Guard guard;job.error="Pipe allocation failed";job.state=State::Failed;return;
    }
    out->readers=out->writers=1;err->readers=err->writers=1;
    FdEntry fds[3]{};fds[0].type=FdType::DevNull;
    fds[1].type=fds[2].type=FdType::Pipe;
    fds[1].flags=fds[2].flags=1;fds[1].statusFlags=fds[2].statusFlags=1;
    fds[1].handle=out;fds[2].handle=err;
    auto* child=ShellCreateDebugJob(request.argc,request.argv,request.cwd,
                                  request.envc,request.env,request.uid,fds);
    if (!child) {
        PipeBufferDestroy(out);PipeBufferDestroy(err);
        Guard guard;job.error="Executable, cwd or process creation failed";job.state=State::Failed;return;
    }
    child->parentPid=ProcessCurrent()->tgid;child->debugJobId=job.tag;
    {Guard guard;job.pid=child->pid;job.incarnation=child->incarnation;job.tasks=1;job.groupTasks=1;job.state=State::Running;}
    SchedulerAddProcess(child);
    for (;;) {
        Drain(job.output[0],out);Drain(job.output[1],err);
        auto snapshot=SchedulerDebugJobSnapshot(job.tag,job.pid,job.incarnation);
        bool waited;{Guard guard;waited=job.waited;}
        ChildWaitSnapshot result{};
        int wait=waited?0:SchedulerWaitChild(ProcessCurrent(),1,job.pid,4|1,&result);
        bool complete;
        { Guard guard;
          job.leaderExited=job.leaderExited || snapshot.leaderExited || wait==1;
          job.tasks=snapshot.liveTasks;job.groupTasks=snapshot.originalGroupTasks;
          if (wait==1) {job.waited=true;job.waitStatus=result.status;}
          complete=job.waited && !job.tasks && job.output[0].eof && job.output[1].eof;
          if (complete) job.state=State::Complete;
        }
        if (complete) break;
        SchedulerSleepMs(10);
    }
    // EOF proves there are no remaining writer references. Discard the legacy
    // raw waiter before releasing the final diagnostic reader endpoint.
    PipeBuffer* pipes[2]={out,err};
    for (auto* pipe : pipes) {
        SpinLockAcquire(&pipe->lock);pipe->writerWaiter=nullptr;SpinLockRelease(&pipe->lock);
        PipeReleaseEnd(pipe,false);
    }
}
} // namespace

bool DebugJobHandleCommand(const char* command) {
    const char* prefix="bridge job ";uint32_t p=0;
    while(prefix[p] && command[p]==prefix[p]) ++p;
    if(prefix[p]) return false;
    Response response;
    char line[224];uint32_t n=0;
    while(command[p] && n<sizeof(line)-1) line[n++]=command[p++];
    if(command[p]) {Error("Command too long");return true;}line[n]=0;
    const char* args[6];uint32_t count=0;char* at=line;
    while(*at && count<6) {
        args[count++]=at;while(*at && *at!=' ') ++at;
        if(*at) {*at++=0;if(!*at) {Error("Invalid fields");return true;}}
    }
    if(*at || !count) {Error("Invalid fields");return true;}
    if(count==1 && !strcmp(args[0],"caps")) {
        Send("{\"job_api\":1,\"max_request_bytes\":32768,\"max_jobs\":256,\"max_active\":8,\"retained_stream_bytes\":65536,\"dedup_retention\":\"until_reboot\",\"stdin\":\"null\",\"auto_window\":false}\n");return true;
    }
    if(count<2 || !Id(args[1])) {Error("Invalid job ID");return true;}
    Job* job=nullptr;Job* free=nullptr;uint32_t active=0;
    for(uint32_t i=0;i<MaxJobs;++i) {
        if(!jobs[i].id[0]) {if(!free) free=&jobs[i];continue;}
        if(!strcmp(jobs[i].id,args[1])) job=&jobs[i];
        if(jobs[i].state==State::Uploading || jobs[i].state==State::Queued || jobs[i].state==State::Running) ++active;
    }
    if(!strcmp(args[0],"begin") && count==3) {
        uint64_t size;if(!Decimal(args[2],&size) || size<16 || size>DebugJobMaxRequest) {Error("Invalid request size");return true;}
        if(job) {
            if(job->size!=size) Error("Request ID size mismatch");else Status(*job);
            return true;
        }
        if(!free || active>=MaxActive) {Error("Job capacity exceeded");return true;}
        auto* request=static_cast<uint8_t*>(kmalloc(size));
        if(!request) {Error("Request allocation failed");return true;}
        job=free;memcpy(job->id,args[1],33);job->request=request;job->size=size;
        job->tag=uint64_t(job-jobs)+1;job->state=State::Uploading;Status(*job);return true;
    }
    if(!job) {Error("Unknown job");return true;}
    if(!strcmp(args[0],"put") && count==4) {
        uint64_t offset;if(!Decimal(args[2],&offset)) {Error("Invalid offset");return true;}
        uint32_t length=0;while(args[3][length]) ++length;
        if(!length || length>128 || length%2 || offset>job->received || offset+length/2>job->size ||
           (offset<job->received && offset+length/2>job->received)) {Error("Invalid chunk extent");return true;}
        uint8_t bytes[64];for(uint32_t i=0;i<length/2;++i) {
            int a=Hex(args[3][i*2]),b=Hex(args[3][i*2+1]);
            if(a<0 || b<0) {Error("Invalid chunk encoding");return true;}bytes[i]=uint8_t(a*16+b);
        }
        if(offset<job->received) {
            if(memcmp(job->request+offset,bytes,length/2)) {Error("Request ID data mismatch");return true;}
        } else {
            if(job->state!=State::Uploading) {Error("Request is immutable");return true;}
            memcpy(job->request+offset,bytes,length/2);job->received+=length/2;
        }
        Status(*job);return true;
    }
    if(!strcmp(args[0],"commit") && count==2) {
        if(job->state!=State::Uploading) {Status(*job);return true;}
        if(job->received!=job->size) {Error("Incomplete request");return true;}
        DebugJobRequest request{};
        if(!DebugJobDecode(job->request,job->size,&request)) {Error("Invalid structured request");return true;}
        auto* out=static_cast<char*>(kmalloc(Retain));auto* err=static_cast<char*>(kmalloc(Retain));
        if(!out || !err) {kfree(out);kfree(err);Error("Output allocation failed");return true;}
        auto* worker=KernelThreadCreate("debug_job",Worker,job);
        if(!worker) {kfree(out);kfree(err);Error("Worker allocation failed");return true;}
        job->output[0].bytes=out;job->output[1].bytes=err;job->state=State::Queued;
        SchedulerAddProcess(worker);Status(*job);return true;
    }
    if(!strcmp(args[0],"status") && count==2) {Status(*job);return true;}
    if(!strcmp(args[0],"read") && count==5) {
        uint64_t offset,limit;
        if((strcmp(args[2],"out") && strcmp(args[2],"err")) || !Decimal(args[3],&offset) ||
           !Decimal(args[4],&limit) || !limit || limit>1024) {Error("Invalid stream cursor");return true;}
        auto& output=job->output[!strcmp(args[2],"err")?1:0];
        if(offset>output.total) {Error("Cursor is ahead of output");return true;}
        uint64_t first=output.total>Retain?output.total-Retain:0;
        uint64_t start=offset<first?first:offset;
        uint64_t length=output.total-start;if(length>limit) length=limit;
        Send("{\"start\":");Number(start);Field("next",start+length);Field("dropped",start-offset);
        Send(",\"eof\":");Send(output.eof && start+length==output.total?"true":"false");
        Send(",\"hex\":\"");char encoded[2049];const char* hex="0123456789abcdef";
        for(uint32_t i=0;i<length;++i) {uint8_t c=output.bytes[(start+i)%Retain];encoded[i*2]=hex[c>>4];encoded[i*2+1]=hex[c&15];}
        encoded[length*2]=0;Send(encoded);Send("\"}\n");return true;
    }
    Error("Unknown job command");return true;
}
}
