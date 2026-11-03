#include "guard_checker_runner.h"
#include "guard_checker_session.h"
#include "guard_fat_profile.h"
#include "exec_image.h"
#include "process.h"
#include "scheduler.h"
#include "kprintf.h"
#include "string.h"
#include "vfs.h"
#include "../../shared/update/payload.h"

namespace brook {
namespace {
// Pinned static dosfstools 4.2 image from nix/nixpkgs.nix. A new checker
// requires an explicitly reviewed digest and recovery-image rebuild.
constexpr uint8_t checkerHash[32]={
    0x99,0xcc,0x81,0x60,0xe7,0xa0,0x76,0xec,0xd9,0xcb,0x04,0xe1,0x3b,0xdb,0xe7,0x1c,
    0x4a,0xad,0x4c,0x64,0xbf,0xdc,0x43,0xea,0x83,0xa0,0x16,0xc8,0xfe,0xe9,0xd5,0xe3};
constexpr const char* checkerPath="/boot/FATFSCK.ELF";
constexpr uint64_t checkerMax=4*1024*1024;
void Worker(void* arg) {
    KPuts("brook-ssd-recovery-readonly-v1\n");
    auto* device=static_cast<Device*>(arg);
    Vnode* vnode=nullptr;char resolved[256]{};
    int err=VfsOpenExecutable(checkerPath,&vnode,resolved,sizeof(resolved));
    VnodeStat stat{};ExecutableImage image{};
    if(!err)err=VfsStat(vnode,&stat);
    if(!err && (stat.isDir || stat.isSymlink || !stat.size || stat.size>checkerMax))err=-8;
    if(!err)err=ReadExecutableImage(vnode,&image);
    if(vnode)VfsRelease(vnode);
    uint8_t hash[32]{};char interpreter[256]{};
    if(!err) {
        BrookUpdateHash(image.data,image.size,hash);
        if(!BrookUpdateEqual(hash,checkerHash,sizeof(hash)) ||
           !ElfMainInterpreter(image.data,image.size,interpreter,sizeof(interpreter)) || interpreter[0])err=-8;
    }
    if(err) {
        FreeExecutableImage(&image);GuardCheckerAbort();
        KPrintf("GUARD_CHECK_INCOMPLETE load=%d\n",err);return;
    }
    const char* argv[]={checkerPath,"-n","--variant=standard","/dev/brook-guard-esp",nullptr};
    const char* env[]={"PATH=/boot","LANG=C",nullptr};
    FdEntry fds[3]{};fds[0].type=FdType::DevNull;
    fds[1].type=fds[2].type=FdType::DebugConsole;
    fds[1].statusFlags=fds[2].statusFlags=1;
    Process* child=ProcessCreateExecutable(image,4,argv,2,env,fds,0,0);
    FreeExecutableImage(&image);
    if(!child) {GuardCheckerAbort();KPuts("GUARD_CHECK_INCOMPLETE create\n");return;}
    child->parentPid=ProcessCurrent()->tgid;
    child->fds[3]={};
    const uint16_t pid=child->pid;const uint64_t incarnation=child->incarnation;
    if(!GuardCheckerBegin(device,child)) {
        GuardCheckerAbort();ProcessDiscardUnpublished(child);
        KPuts("GUARD_CHECK_INCOMPLETE session\n");return;
    }
    SchedulerAddProcess(child);child=nullptr;
    ChildWaitSnapshot result{};int waited=0;
    for(unsigned i=0;i<12000 && !waited;++i) {
        waited=SchedulerWaitChild(ProcessCurrent(),1,pid,4,&result);
        if(!waited)SchedulerSleepMs(10);
    }
    GuardCheckerAbort();
    if(waited!=1) {
        auto* target=SchedulerFindProcessReference(pid);
        if(target) {
            if(target->incarnation==incarnation)SchedulerRequestFatalSignal(target,9);
            ProcessUnref(target);
        }
        for(unsigned i=0;i<3000 && waited==0;++i) {
            SchedulerSleepMs(10);
            waited=SchedulerWaitChild(ProcessCurrent(),1,pid,4,&result);
        }
        KPrintf("GUARD_CHECK_INCOMPLETE wait=%d\n",waited);return;
    }
    const auto status=GuardCheckerGetStatus();
    if(status.active || status.views || status.reads) {
        KPrintf("GUARD_CHECK_INCOMPLETE drain views=%u reads=%u\n",status.views,status.reads);return;
    }
    if(result.code!=1) {KPrintf("GUARD_CHECK_INCOMPLETE signal status=%d\n",result.status);return;}
    if(result.status==0) {
        GuardFatProfile profile{};
        if(!GuardFatCheckDevice(device,profile)) {KPuts("GUARD_CHECK_INCOMPLETE fat-profile\n");return;}
    }
    KPrintf("GUARD_CHECK_RESULT exit=%u\n",static_cast<unsigned>(result.status>>8));
}
}
bool GuardCheckerStart(Device* device) {
    if(!device)return false;
    auto* worker=KernelThreadCreate("guard_check",Worker,device);
    if(!worker)return false;
    SchedulerAddProcess(worker);return true;
}
}
