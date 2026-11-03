#include "guard_checker_runner.h"
#include "guard_checker_session.h"
#include "guard_fat_profile.h"
#include "ahci_system_volume.h"
#include "exec_image.h"
#include "fatfs_vfs.h"
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
#include "fw_cfg.h"
#endif
#if defined(BROOK_SSD_GUARD_EXT2_TEST)
#include "guard_repair_session.h"
#endif
#include "process.h"
#include "scheduler.h"
#include "kprintf.h"
#include "string.h"
#include "vfs.h"
#include "../../shared/update/payload.h"
#include "../../shared/update/recovery_guard.h"

namespace brook {
namespace {
// Pinned static dosfstools 4.2 image from nix/nixpkgs.nix. A new checker
// requires an explicitly reviewed digest and recovery-image rebuild.
constexpr uint8_t checkerHash[32]={
    0x99,0xcc,0x81,0x60,0xe7,0xa0,0x76,0xec,0xd9,0xcb,0x04,0xe1,0x3b,0xdb,0xe7,0x1c,
    0x4a,0xad,0x4c,0x64,0xbf,0xdc,0x43,0xea,0x83,0xa0,0x16,0xc8,0xfe,0xe9,0xd5,0xe3};
constexpr const char* checkerPath="/boot/FATFSCK.ELF";
constexpr uint64_t checkerMax=4*1024*1024;
#if defined(BROOK_SSD_GUARD_EXT2_TEST)
constexpr uint8_t ext2Hash[32]={
    0xac,0xc6,0xde,0xe6,0xfb,0x21,0xaf,0xd6,0x70,0xd0,0x52,0x61,0x7b,0x66,0x55,0x88,
    0x07,0x0d,0x53,0x6e,0x2e,0x73,0xb5,0xc4,0xf9,0x5d,0xb1,0x1f,0x98,0x92,0x22,0x68};
int RunExt2Checker(Device* device,bool writable) {
    constexpr const char* path="/boot/E2FSCK.ELF";
    Vnode* node=nullptr;char resolved[256]{};ExecutableImage image{};VnodeStat stat{};
    int error=VfsOpenExecutable(path,&node,resolved,sizeof(resolved));
    if(!error)error=VfsStat(node,&stat);
    if(!error && (stat.isDir || stat.isSymlink || !stat.size || stat.size>checkerMax))error=-8;
    if(!error)error=ReadExecutableImage(node,&image);
    if(node)VfsRelease(node);
    uint8_t hash[32]{};char interpreter[256]{};
    if(!error) {
        BrookUpdateHash(image.data,image.size,hash);
        if(!BrookUpdateEqual(hash,ext2Hash,sizeof(hash)) ||
           !ElfMainInterpreter(image.data,image.size,interpreter,sizeof(interpreter)) || interpreter[0])error=-8;
    }
    if(error) {FreeExecutableImage(&image);return error;}
    const char* preen[]={path,"-p","/dev/brook-guard-data",nullptr};
    const char* verify[]={path,"-f","-n","/dev/brook-guard-data",nullptr};
    const char* env[]={"PATH=/boot","LANG=C",nullptr};
    FdEntry fds[3]{};fds[0].type=FdType::DevNull;fds[1].type=fds[2].type=FdType::DebugConsole;
    fds[1].statusFlags=fds[2].statusFlags=1;
    Process* child=ProcessCreateExecutable(image,writable?3:4,writable?preen:verify,2,env,fds,0,0);
    FreeExecutableImage(&image);
    if(!child)return -12;
    child->parentPid=ProcessCurrent()->tgid;child->fds[3]={};
    const uint16_t pid=child->pid;const uint64_t incarnation=child->incarnation;
    if(!GuardRepairBeginTracked(device,child,writable)) {ProcessDiscardUnpublished(child);return -13;}
    SchedulerAddProcess(child);child=nullptr;
    ChildWaitSnapshot result{};int waited=0;
    for(unsigned i=0;i<12000 && !waited;++i) {
        waited=GuardRepairPollTracked(device,pid,incarnation,&result);
        if(!waited)SchedulerSleepMs(10);
    }
    bool drained=false;
    if(waited==1)drained=GuardRepairConsumeCompletion(device,writable);else GuardRepairAbort();
    if(waited!=1) {
        auto* process=SchedulerFindProcessReference(pid);
        if(process) {if(process->incarnation==incarnation)SchedulerRequestFatalSignal(process,9);ProcessUnref(process);}
        for(unsigned i=0;i<3000 && waited==0;++i) {
            SchedulerSleepMs(10);waited=SchedulerWaitChild(ProcessCurrent(),1,pid,4,&result);
        }
        return GuardRepairQuiescent()?-110:-115;
    }
    if(!drained)return GuardRepairRejectedOutcome(GuardRepairQuiescent());
    if(result.code!=1)return -5;
    return result.status>>8;
}
#endif
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
    if(!GuardCheckerBeginTracked(device,child)) {
        GuardCheckerAbort();ProcessDiscardUnpublished(child);
        KPuts("GUARD_CHECK_INCOMPLETE session\n");return;
    }
    SchedulerAddProcess(child);child=nullptr;
    ChildWaitSnapshot result{};int waited=0;
    for(unsigned i=0;i<12000 && !waited;++i) {
        waited=GuardCheckerPollTracked(device,pid,incarnation,&result);
        if(!waited)SchedulerSleepMs(10);
    }
    if(waited!=1) {
        GuardCheckerAbort();
        const int failure=waited;
        auto* target=SchedulerFindProcessReference(pid);
        if(target) {
            if(target->incarnation==incarnation)SchedulerRequestFatalSignal(target,9);
            ProcessUnref(target);
        }
        int cleanupWait=0;
        for(unsigned i=0;i<3000 && cleanupWait==0;++i) {
            SchedulerSleepMs(10);
            cleanupWait=SchedulerWaitChild(ProcessCurrent(),1,pid,4,&result);
        }
        KPrintf("GUARD_CHECK_INCOMPLETE wait=%d cleanup=%d\n",failure,cleanupWait);return;
    }
    const bool completed=GuardCheckerCompletionAvailable(device);
    const auto status=GuardCheckerGetStatus();
    if(status.active || status.views || status.reads) {
        KPrintf("GUARD_CHECK_INCOMPLETE drain views=%u reads=%u\n",status.views,status.reads);return;
    }
    if(result.code!=1) {KPrintf("GUARD_CHECK_INCOMPLETE signal status=%d\n",result.status);return;}
    if(result.status!=0) {
        KPrintf("GUARD_CHECK_RESULT exit=%u\n",static_cast<unsigned>(result.status>>8));return;
    }
    if(!completed) {GuardCheckerAbort();KPuts("GUARD_CHECK_INCOMPLETE completion\n");return;}
    GuardFatProfile profile{};
    if(!GuardFatCheckDevice(device,profile)) {KPuts("GUARD_CHECK_INCOMPLETE fat-profile\n");return;}
    KPuts("GUARD_CHECK_RESULT exit=0\n");
    uint8_t records[2][128]{};char identity[97]{};
    if(!AhciSystemGuardIdentity(identity)) {KPuts("GUARD_RECORDS_INCOMPLETE identity\n");return;}
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
    auto* adapter=AhciSystemGuardTestPrepare(device);
    FatPrivateMount* mount=nullptr;
    if(!adapter || !FatFsMountPrivateRecovery(1,adapter,&mount) ||
       !FatFsPrivateRecoveryGeometryMatches(mount,profile)) {
        AhciSystemGuardTestFinish(adapter);KPuts("GUARD_PENDING_TEST_INCOMPLETE mount\n");return;
    }
    const int inspection=FatFsPrivateRecoveryInspect(mount,records[0],records[1]);
    if(inspection || !BrookRecoveryGuardAllowsNormal(records[0],128,records[1],128,identity)) {
        AhciSystemGuardTestFinish(adapter);KPrintf("GUARD_PENDING_TEST_INCOMPLETE records=%d\n",inspection);return;
    }
    if(!AhciSystemGuardTestBeginWrites(adapter)) {
        AhciSystemGuardTestFinish(adapter);KPuts("GUARD_PENDING_TEST_INCOMPLETE authorization\n");return;
    }
    char injection[3]{};const uint32_t rawLength=FwCfgReadFile("opt/guard-pending-fail",injection,sizeof(injection));
    const uint32_t length=rawLength && rawLength<=sizeof(injection) && !injection[rawLength-1]?rawLength-1:rawLength;
    uint32_t failAt=0;
    for(uint32_t i=0;i<length && i<sizeof(injection);++i) {
        if(injection[i]<'0' || injection[i]>'9') {failAt=99;break;}
        failAt=failAt*10+uint32_t(injection[i]-'0');
    }
    if(rawLength>sizeof(injection) || length>2 || failAt>11 || (rawLength && !failAt)) {
        AhciSystemGuardTestFinish(adapter);KPuts("GUARD_PENDING_TEST_INCOMPLETE injection\n");return;
    }
    const int pending=FatFsPrivateRecoveryTestPending(mount,identity,failAt);
    AhciSystemGuardTestFinish(adapter);
    if(FatFsPrivateRecoveryTestPending(mount,identity,failAt)!=0) {
        KPuts("GUARD_PENDING_TEST_INCOMPLETE repeated\n");return;
    }
    KPrintf("GUARD_PENDING_TEST_RESULT outcome=%d\n",pending);
#if defined(BROOK_SSD_GUARD_EXT2_TEST)
    if(pending!=2)return;
    auto* data=AhciSystemGuardTestDataArm(adapter,pending);
    if(!data) {KPuts("GUARD_EXT2_INCOMPLETE data-admission\n");return;}
    const int preen=RunExt2Checker(data,true);
    KPrintf("GUARD_EXT2_PREEN exit=%d\n",preen);
    if((preen!=0 && preen!=1) || !AhciSystemGuardTestDataVerify(data)) {
        if(preen!=-115)AhciSystemGuardTestDataFinish(data);
        KPuts("GUARD_EXT2_INCOMPLETE preen\n");return;
    }
    const int verification=RunExt2Checker(data,false);
    const bool identityOk=verification==0 && AhciSystemGuardTestDataIdentityValid(data);
    if(verification!=-115)AhciSystemGuardTestDataFinish(data);
    if(verification!=0 || !identityOk) {KPrintf("GUARD_EXT2_INCOMPLETE verify=%d identity=%u\n",verification,identityOk?1u:0u);return;}
    KPuts("GUARD_EXT2_VERIFIED_PENDING\n");
#if defined(BROOK_SSD_GUARD_COMPLETE_TEST)
    // Derive the exact pending record from the pair admitted before repair.
    // The repair generation identifies the attempt across both transitions.
    BrookUpdatePut64(records[0]+8,BrookUpdateGet64(records[0]+8)+1);
    BrookUpdatePut32(records[0]+16,BROOK_RECOVERY_GUARD_PENDING);
    BrookUpdatePut32(records[0]+124,BrookUpdateCrc(records[0],124));
    char completeInjection[3]{};
    const uint32_t completeRaw=FwCfgReadFile("opt/guard-complete-fail",completeInjection,sizeof(completeInjection));
    const uint32_t completeLength=completeRaw && completeRaw<=sizeof(completeInjection) && !completeInjection[completeRaw-1]?completeRaw-1:completeRaw;
    uint32_t completeFail=0;
    for(uint32_t i=0;i<completeLength && i<sizeof(completeInjection);++i) {
        if(completeInjection[i]<'0' || completeInjection[i]>'9') {completeFail=99;break;}
        completeFail=completeFail*10+uint32_t(completeInjection[i]-'0');
    }
    if(completeRaw>sizeof(completeInjection) || completeLength>2 || completeFail>11 || (completeRaw && !completeFail) ||
       !AhciSystemGuardTestBeginCompletion(adapter,identityOk)) {
        KPuts("GUARD_COMPLETE_TEST_INCOMPLETE authorization\n");return;
    }
    const int complete=FatFsPrivateRecoveryTestComplete(mount,identity,records[0],completeFail);
    AhciSystemGuardTestFinish(adapter);
    if(FatFsPrivateRecoveryTestComplete(mount,identity,records[0],completeFail)!=0) {
        KPuts("GUARD_COMPLETE_TEST_INCOMPLETE repeated\n");return;
    }
    KPrintf("GUARD_COMPLETE_TEST_RESULT outcome=%d\n",complete);
#endif
#endif
    return;
#else
    const int inspection=FatFsGuardInspectReadOnly(1,device,profile,records);
    if(inspection) {KPrintf("GUARD_RECORDS_INCOMPLETE read=%d\n",inspection);return;}
    if(!BrookRecoveryGuardAllowsNormal(records[0],128,records[1],128,identity)) {
        KPuts("GUARD_RECORDS_INCOMPLETE mismatch\n");return;
    }
    // This is diagnostic evidence only. It grants neither repair nor normal
    // boot admission; the read-only binding remains held until reboot.
    KPuts("GUARD_RECORDS_MATCH\n");
#endif
}
}
bool GuardCheckerStart(Device* device) {
    if(!device)return false;
    auto* worker=KernelThreadCreate("guard_check",Worker,device);
    if(!worker)return false;
    SchedulerAddProcess(worker);return true;
}
}
