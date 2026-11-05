#include "guard_checker_runner.h"
#include "guard_checker_session.h"
#include "guard_fat_profile.h"
#include "ahci_system_volume.h"
#include "exec_image.h"
#include "fatfs_vfs.h"
#include "panic_journal.h"
#if defined(BROOK_SSD_AUTO_RECOVERY)
#include "panic_test_reset.h"
#include "net.h"
#endif
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
#include "fw_cfg.h"
#include "panic.h"
#include "shutdown.h"
#endif
#if defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_GUARDED_DESKTOP)
#include "shell.h"
#endif
#if defined(BROOK_SSD_GUARD_COMPLETION)
#include "platform_power.h"
#endif
#if defined(BROOK_SSD_GUARD_REPAIR)
#include "guard_repair_session.h"
#endif
#include "process.h"
#include "scheduler.h"
#include "kprintf.h"
#include "string.h"
#include "vfs.h"
#include "vfs_path.h"
#include "../../shared/update/payload.h"
#include "../../shared/update/recovery_guard.h"

namespace brook {
#if defined(BROOK_SSD_AUTO_RECOVERY)
namespace {
uint8_t g_recoveryPanicPacket[PanicJournalPayloadMax]{};
GuardPanicSnapshot g_recoveryPanic{};
bool g_recoveryPanicReady=false;
}
bool GuardRecoveryPanicSnapshot(GuardPanicSnapshot& out) {
    out={};if(!__atomic_load_n(&g_recoveryPanicReady,__ATOMIC_ACQUIRE))return false;
    out=g_recoveryPanic;return true;
}
#endif
namespace {
// Pinned static dosfstools 4.2 image from nix/nixpkgs.nix. A new checker
// requires an explicitly reviewed digest and recovery-image rebuild.
constexpr uint8_t checkerHash[32]={
    0x99,0xcc,0x81,0x60,0xe7,0xa0,0x76,0xec,0xd9,0xcb,0x04,0xe1,0x3b,0xdb,0xe7,0x1c,
    0x4a,0xad,0x4c,0x64,0xbf,0xdc,0x43,0xea,0x83,0xa0,0x16,0xc8,0xfe,0xe9,0xd5,0xe3};
constexpr const char* checkerPath="/boot/FATFSCK.ELF";
constexpr uint64_t checkerMax=4*1024*1024;
#if defined(BROOK_SSD_GUARD_REPAIR)
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
#if defined(BROOK_SSD_AUTO_RECOVERY)
    KPuts("brook-ssd-recovery-automatic-v1\n");
#else
    KPuts("brook-ssd-recovery-readonly-v1\n");
#endif
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
#if defined(BROOK_SSD_GUARD_TRANSACTION)
    // Classify the same reserved data partition itself; an F5 boot or a
    // loader hint cannot authorize repair.
    bool autoCondition=false;
#if defined(BROOK_SSD_AUTO_RECOVERY)
    autoCondition=true;
#else
    char autoMode[8]{};
    const auto autoSize=FwCfgReadFile("opt/guard-auto-condition",autoMode,sizeof(autoMode));
    if(autoSize) {
        if(autoSize!=6 || memcmp(autoMode,"test-1",6)) {KPuts("GUARD_AUTO_INVALID\n");return;}
        autoCondition=true;
    }
#endif
    bool autoClean=false,autoInvalid=false;
    if(autoCondition) {
        const auto state=AhciSystemGuardDataState(device);
        autoInvalid=state==AhciGuardDataState::Invalid;
        autoClean=state==AhciGuardDataState::Clean;
        if(!autoClean && !autoInvalid)KPuts("GUARD_AUTO_NEEDS_CHECK\n");
    }
    auto* adapter=AhciSystemGuardTestPrepare(device);
    FatPrivateMount* mount=nullptr;
    if(!adapter || !FatFsMountPrivateRecovery(1,adapter,&mount) ||
       !FatFsPrivateRecoveryGeometryMatches(mount,profile)) {
        AhciSystemGuardTestFinish(adapter);KPuts("GUARD_PENDING_INCOMPLETE mount\n");return;
    }
    const int inspection=FatFsPrivateRecoveryInspect(mount,records[0],records[1]);
#if defined(BROOK_SSD_AUTO_RECOVERY)
    int journalPin=FatFsPrivateRecoveryPinJournal(mount);
    if(!journalPin) {
        uint64_t sequence=0;uint32_t bytes=0,crc=0;
        const int read=FatFsPrivateRecoveryReadPanic(mount,g_recoveryPanicPacket,sizeof(g_recoveryPanicPacket),&sequence,&bytes,&crc);
        if(!read) {
            g_recoveryPanic={g_recoveryPanicPacket,sequence,bytes,crc};
            __atomic_store_n(&g_recoveryPanicReady,true,__ATOMIC_RELEASE);
            KPrintf("GUARD_PANIC_PRIOR sequence=%lu bytes=%u crc=%x\n",sequence,bytes,crc);
        }
    }
#endif
    // A damaged or unsupported data filesystem must not prevent retrieval of
    // an already validated crash journal from the independently checked ESP.
    if(autoInvalid) {AhciSystemGuardTestFinish(adapter);KPuts("GUARD_AUTO_INVALID\n");return;}
    bool resumePending=false;
#if defined(BROOK_SSD_AUTO_RECOVERY)
    resumePending=!inspection && BrookRecoveryGuardMatchesPending(records[0],128,records[1],128,identity);
#endif
    if(inspection || (!resumePending && !BrookRecoveryGuardAllowsNormal(records[0],128,records[1],128,identity))) {
        AhciSystemGuardTestFinish(adapter);KPrintf("GUARD_PENDING_INCOMPLETE records=%d\n",inspection);return;
    }
    if(autoClean && !resumePending) {
#if defined(BROOK_SSD_GUARDED_DESKTOP)
        // The checked private ESP and data reservation remain pinned for the
        // entire boot. A failure at any step leaves maintenance in control.
        uint64_t publication=0;
        int handoff=journalPin;
        if(!handoff)handoff=FatFsPrivateRecoveryPublishJournalTest(mount,false,&publication);
        if(!handoff && !AhciSystemGuardTestBeginWrites(adapter))handoff=-13;
        if(!handoff && !AhciSystemGuardTestAdoptSystemMount(adapter,1,publication))handoff=-13;
        if(!handoff)handoff=FatFsPrivateRecoveryArmJournal(mount);
        uint8_t drive=2;
        if(!handoff && !AhciSystemGuardTestMountDesktop(adapter,&drive))handoff=-5;
        if(handoff) {KPrintf("GUARD_DESKTOP_HANDOFF_FAILED status=%d\n",handoff);return;}
        GuardTestPrepareDesktopHome();
        const int script=ShellExecScript("/boot/INIT.RC");
        KPrintf("GUARD_DESKTOP_HANDOFF_SCRIPT status=%d\n",script);
        return;
#else
        AhciSystemGuardTestFinish(adapter);KPuts("GUARD_AUTO_NO_REPAIR\n");return;
#endif
    }
#if defined(BROOK_SSD_AUTO_RECOVERY)
    // Give a connected recovery bridge a bounded opportunity to retrieve the
    // retained crash before a successful retry restarts the machine again.
    if(resumePending && __atomic_load_n(&g_recoveryPanicReady,__ATOMIC_ACQUIRE) && NetGetIf() && NetGetIf()->ipAddr) {
        KPuts("GUARD_PANIC_RETRIEVAL_WINDOW 10s\n");SchedulerSleepMs(10000);
    }
#endif
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
    char journalMode[8]{};
    const auto journalSize=FwCfgReadFile("opt/guard-journal-pin",journalMode,sizeof(journalMode));
    if(journalSize) {
        const bool pinOnly=journalSize==6 && !memcmp(journalMode,"test-1",6);
        const bool capture=journalSize==6 && !memcmp(journalMode,"test-3",6);
        const bool denyCapture=journalSize==6 && !memcmp(journalMode,"test-4",6);
        const bool failedPublish=journalSize==6 && !memcmp(journalMode,"test-5",6);
        const bool desktopPanic=journalSize==6 && !memcmp(journalMode,"test-7",6);
        const bool desktopFailed=journalSize==6 && !memcmp(journalMode,"test-8",6);
        const bool desktopShutdown=journalSize==6 && !memcmp(journalMode,"test-9",6);
        const bool desktopDenied=journalSize==6 && !memcmp(journalMode,"test-a",6);
        const bool desktopScript=journalSize==6 && !memcmp(journalMode,"test-b",6);
        const bool desktop=desktopPanic || desktopFailed || desktopShutdown || desktopDenied || desktopScript || (journalSize==6 && !memcmp(journalMode,"test-6",6));
        const bool publish=capture || denyCapture || failedPublish || desktop || (journalSize==6 && !memcmp(journalMode,"test-2",6));
        if(!pinOnly && !publish) {
            AhciSystemGuardTestFinish(adapter);KPuts("GUARD_JOURNAL_PIN_INVALID\n");return;
        }
        const int pinned=FatFsPrivateRecoveryPinJournal(mount);
        const int repeated=FatFsPrivateRecoveryPinJournal(mount);
        KPrintf("GUARD_JOURNAL_PIN_RESULT status=%d repeated=%d\n",pinned,repeated);
        if(publish && !pinned && repeated==-22) {
            const int premature=FatFsPrivateRecoveryArmJournalTest(mount);
            KPrintf("GUARD_JOURNAL_PUBLISH_EARLY_ARM status=%d\n",premature);
            if(premature!=-22)return;
            if(failedPublish) {
                const int failed=FatFsPrivateRecoveryPublishJournalTest(mount,true);
                const int retried=FatFsPrivateRecoveryPublishJournalTest(mount);
                const int arm=FatFsPrivateRecoveryArmJournalTest(mount);
                KPrintf("GUARD_JOURNAL_PUBLISH_FAILED status=%d retry=%d arm=%d\n",failed,retried,arm);
                return;
            }
            uint64_t publication=0;
            int publishResult=FatFsPrivateRecoveryPublishJournalTest(mount,false,&publication);
            KPrintf("GUARD_JOURNAL_PUBLISH_MOUNT status=%d\n",publishResult);
            if(!publishResult && FatFsPrivateRecoveryPublishJournalTest(mount)!=-22)publishResult=-5;
            if(!publishResult && !AhciSystemGuardTestBeginWrites(adapter))publishResult=-13;
            if(desktop && !publishResult) {
                if(desktopDenied) {
                    const bool forged=AhciSystemGuardTestAdoptSystemMount(adapter,1,publication+1);
                    const bool retry=AhciSystemGuardTestAdoptSystemMount(adapter,1,publication);
                    KPrintf("GUARD_JOURNAL_DESKTOP_IDENTITY_DENIED forged=%u retry=%u publication=%lu\n",
                            forged?1u:0u,retry?1u:0u,AhciSystemVolumePublication());
                    return;
                }
                if(!AhciSystemGuardTestAdoptSystemMount(adapter,1,publication)) {
                    KPuts("GUARD_JOURNAL_DESKTOP_FAIL system-adoption\n");return;
                }
                VfsPath identityRoot{};int identityError=VfsPathRoot(&identityRoot);
                auto* rootNode=!identityError?VfsOpenAt(identityRoot,identityRoot,"/media/system",VFS_O_METADATA,0,0,nullptr):nullptr;
                auto* legacyRoot=VfsOpen("/media/system",VFS_O_METADATA);
                auto* memberNode=VfsOpen("/media/system/EFI/BOOT/BOOTX64.EFI",VFS_O_STREAM);
                auto* foreignNode=VfsOpen("/boot/FATFSCK.ELF",VFS_O_STREAM);
                const bool owns=rootNode && legacyRoot && memberNode && foreignNode && AhciSystemVolumeOwnsRoot(rootNode) &&
                    AhciSystemVolumeOwnsRoot(legacyRoot) &&
                    AhciSystemVolumeOwnsMember(memberNode) && !AhciSystemVolumeOwnsRoot(memberNode) &&
                    !AhciSystemVolumeOwnsMember(foreignNode);
                if(rootNode)VfsRelease(rootNode);if(legacyRoot)VfsRelease(legacyRoot);if(memberNode)VfsRelease(memberNode);if(foreignNode)VfsRelease(foreignNode);
                VfsPathRelease(identityRoot);
                KPrintf("GUARD_JOURNAL_DESKTOP_SYSTEM_IDENTITY status=%d\n",owns?0:-5);
                if(!owns){KPuts("GUARD_JOURNAL_DESKTOP_FAIL system-identity\n");return;}
                const int armed=FatFsPrivateRecoveryArmJournalTest(mount);
                uint8_t drive=2;
                const bool mounted=!armed && AhciSystemGuardTestMountDesktop(adapter,&drive);
                if(desktopFailed) {
                    const int denied=AhciSystemGuardTestFailedDesktopWrite();
                    KPrintf("GUARD_JOURNAL_DESKTOP_FAILED mounted=%u write=%d\n",mounted?1u:0u,denied);return;
                }
                if(!mounted) {
                    KPrintf("GUARD_JOURNAL_DESKTOP_FAIL arm=%d\n",armed);return;
                }
                // Kernel callers still use legacy absolute path lookup. Check
                // that it sees this same retained FAT instance after handoff.
                int legacyError=0;VnodeStat legacyStat{};
                const int legacyStatError=VfsLstatPath("/media/system/EFI/BOOT/BOOTX64.EFI",&legacyStat);
                auto* legacy=VfsOpenModeChecked("/media/system/EFI/BOOT/BOOTX64.EFI",VFS_O_STREAM,0666,&legacyError);
                char magic[2]{};uint64_t legacyOffset=0;
                const int legacyRead=legacy?VfsRead(legacy,magic,sizeof(magic),&legacyOffset):-1;
                const bool legacyOk=legacy && legacyRead==int(sizeof(magic)) &&
                    !memcmp(magic,"MZ",sizeof(magic));
                if(legacy)VfsRelease(legacy);
                KPrintf("GUARD_JOURNAL_DESKTOP_LEGACY status=%d stat=%d open=%d read=%d\n",legacyOk?0:-5,legacyStatError,legacyError,legacyRead);
                if(!legacyOk){KPuts("GUARD_JOURNAL_DESKTOP_FAIL legacy\n");return;}
                VfsPath root{};int error=VfsPathRoot(&root);
                int openError=0;auto* file=!error?VfsOpenAt(root,root,"/data/home/GUARD.TXT",VFS_O_WRITE|VFS_O_CREATE,0644,0,&openError):nullptr;
                constexpr char witness[]="guarded desktop handoff\n";uint64_t offset=0;
                if(!file || VfsWrite(file,witness,sizeof(witness)-1,&offset)!=int(sizeof(witness)-1) || VfsFsync(file))error=-5;
                if(file)VfsClose(file);VfsPathRelease(root);
                if(error){KPrintf("GUARD_JOURNAL_DESKTOP_FAIL write=%d open=%d\n",error,openError);return;}
                KPuts("GUARD_JOURNAL_DESKTOP_PASS\n");
                if(desktopScript) {
                    GuardTestPrepareDesktopHome();
                    const int scriptResult=ShellExecScript("/boot/INIT.RC");
                    KPrintf("GUARD_JOURNAL_NORMAL_SCRIPT status=%d\n",scriptResult);
                }
                if(desktopPanic)KernelPanic("guarded desktop handoff panic");
                if(desktopShutdown) {
                    char state[24]{};int shutdownError=0;
                    ShutdownCommand("dddddddddddddddd0000000000000001","shutdown",state,&shutdownError);
                    KPrintf("GUARD_JOURNAL_DESKTOP_SHUTDOWN state=%s error=%d\n",state,shutdownError);
                }
                return;
            }
            if(!publishResult) {
                VfsPath root{};publishResult=VfsPathRoot(&root);
                int aliasError=0;
                auto* alias=!publishResult?VfsOpenAt(root,root,"/media/system/PANIC.BIN",0,0,0,&aliasError):nullptr;
                KPrintf("GUARD_JOURNAL_PUBLISH_ALIAS opened=%u status=%d\n",alias?1u:0u,aliasError);
                if(alias) {VfsClose(alias);publishResult=-16;}
                else if(!publishResult && aliasError!=-16)publishResult=-5;
                if(!publishResult) {
                    const int unlinkResult=VfsRemoveAt(root,root,"/media/system/PANIC.BIN",false);
                    KPrintf("GUARD_JOURNAL_PUBLISH_UNLINK status=%d\n",unlinkResult);
                    if(unlinkResult!=-16)publishResult=-16;
                }
                if(!publishResult) {
                    int openError=0;
                    auto* temporary=VfsOpenAt(root,root,"/media/system/PINDEL.TXT",VFS_O_WRITE|VFS_O_CREATE,0644,0,&openError);
                    if(!temporary)publishResult=-5;
                    else {
                        const int whileOpen=VfsRemoveAt(root,root,"/media/system/PINDEL.TXT",false);
                        VfsClose(temporary);
                        const int afterClose=VfsRemoveAt(root,root,"/media/system/PINDEL.TXT",false);
                        KPrintf("GUARD_JOURNAL_PUBLISH_REMOVE open=%d closed=%d\n",whileOpen,afterClose);
                        if(whileOpen!=-16 || afterClose)publishResult=-5;
                    }
                }
                if(!publishResult) {
                    const int renameResult=VfsRenameAt(root,root,"/media/system/PANIC.BIN",root,"/media/system/PANIC2.BIN");
                    KPrintf("GUARD_JOURNAL_PUBLISH_RENAME status=%d\n",renameResult);
                    if(renameResult!=-16)publishResult=-16;
                }
                if(!publishResult) {
                    constexpr char witness[]="checked journal remains pinned\n";
                    int openError=0;
                    auto* file=VfsOpenAt(root,root,"/media/system/PINTMP.TXT",VFS_O_WRITE|VFS_O_CREATE,0644,0,&openError);
                    uint64_t offset=0;
                    if(!file)publishResult=-5;
                    else {
                        if(VfsWrite(file,witness,sizeof(witness)-1,&offset)!=int(sizeof(witness)-1) || VfsFsync(file))publishResult=-5;
                        const int whileOpen=VfsRenameAt(root,root,"/media/system/PINTMP.TXT",root,"/media/system/PINTST.TXT");
                        VfsClose(file);
                        const int journalTarget=VfsRenameAt(root,root,"/media/system/PINTMP.TXT",root,"/media/system/PANIC.BIN");
                        const int afterClose=VfsRenameAt(root,root,"/media/system/PINTMP.TXT",root,"/media/system/PINTST.TXT");
                        KPrintf("GUARD_JOURNAL_PUBLISH_MOVE open=%d journal_target=%d closed=%d\n",whileOpen,journalTarget,afterClose);
                        if(whileOpen!=-16 || journalTarget!=-16 || afterClose)publishResult=-5;
                    }
                    if(!publishResult && VfsSync(nullptr))publishResult=-5;
                }
                VfsPathRelease(root);
            }
            KPrintf("GUARD_JOURNAL_PUBLISH_RESULT status=%d\n",publishResult);
            if(denyCapture && !publishResult) {
                const int denied=FatFsPrivateRecoveryArmJournalTest(mount);
                KPrintf("GUARD_JOURNAL_PANIC_DENIED status=%d\n",denied);
            }
            if(capture && !publishResult) {
                const int armed=FatFsPrivateRecoveryArmJournalTest(mount);
                KPrintf("GUARD_JOURNAL_PANIC_ARM status=%d\n",armed);
                if(!armed)KernelPanic("checked journal publication panic test");
            }
        }
        // Keep the reservation, mount and leases for the rest of this boot.
        return;
    }
#endif
    if(!AhciSystemGuardTestBeginWrites(adapter)) {
        AhciSystemGuardTestFinish(adapter);KPuts("GUARD_PENDING_INCOMPLETE authorization\n");return;
    }
#if defined(BROOK_SSD_AUTO_RECOVERY)
    // The checker and disk reservation precede journal acquisition. Failure
    // to capture diagnostics must not by itself prevent filesystem repair.
    const int journalArm=journalPin?journalPin:FatFsPrivateRecoveryArmJournal(mount);
    KPrintf("GUARD_PANIC_JOURNAL pin=%d arm=%d\n",journalPin,journalArm);
#endif
    uint32_t failAt=0;
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
    char injection[3]{};const uint32_t rawLength=FwCfgReadFile("opt/guard-pending-fail",injection,sizeof(injection));
    const uint32_t length=rawLength && rawLength<=sizeof(injection) && !injection[rawLength-1]?rawLength-1:rawLength;
    for(uint32_t i=0;i<length && i<sizeof(injection);++i) {
        if(injection[i]<'0' || injection[i]>'9') {failAt=99;break;}
        failAt=failAt*10+uint32_t(injection[i]-'0');
    }
    if(rawLength>sizeof(injection) || length>2 || failAt>11 || (rawLength && !failAt)) {
        AhciSystemGuardTestFinish(adapter);KPuts("GUARD_PENDING_INCOMPLETE injection\n");return;
    }
#endif
    const int pending=resumePending?2:FatFsPrivateRecoveryTestPending(mount,identity,failAt);
    AhciSystemGuardTestFinish(adapter);
    if(!resumePending && FatFsPrivateRecoveryTestPending(mount,identity,failAt)!=0) {
        KPuts("GUARD_PENDING_INCOMPLETE repeated\n");return;
    }
    KPrintf("GUARD_PENDING_RESULT outcome=%d\n",pending);
#if defined(BROOK_SSD_GUARD_REPAIR)
    if(pending!=2)return;
#if defined(BROOK_SSD_AUTO_RECOVERY)
    if(!AhciSystemGuardBeginAttempt(adapter)) {
        KPuts("GUARD_ATTEMPT_INCOMPLETE authorization\n");return;
    }
    const int attempt=FatFsPrivateRecoveryBeginAttempt(mount,identity,records[0]);
    AhciSystemGuardTestFinish(adapter);
    KPrintf("GUARD_ATTEMPT_RESULT outcome=%d\n",attempt);
    if(attempt!=2)return;
    KPrintf("GUARD_ATTEMPT_NUMBER %u\n",BrookUpdateGet32(records[0]+20));
    KPrintf("GUARD_PANIC_RESET armed=%u\n",PanicRecoveryResetArm()?1u:0u);
#endif
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
#if defined(BROOK_SSD_GUARD_COMPLETION)
    // Derive the exact pending record from the pair admitted before repair.
    // The repair generation identifies the attempt across both transitions.
#if !defined(BROOK_SSD_AUTO_RECOVERY)
    BrookUpdatePut64(records[0]+8,BrookUpdateGet64(records[0]+8)+1);
    BrookUpdatePut32(records[0]+16,BROOK_RECOVERY_GUARD_PENDING);
    BrookUpdatePut32(records[0]+124,BrookUpdateCrc(records[0],124));
#endif
    uint32_t completeFail=0;
#if defined(BROOK_SSD_GUARD_COMPLETE_TEST)
    char completeInjection[3]{};
    const uint32_t completeRaw=FwCfgReadFile("opt/guard-complete-fail",completeInjection,sizeof(completeInjection));
    const uint32_t completeLength=completeRaw && completeRaw<=sizeof(completeInjection) && !completeInjection[completeRaw-1]?completeRaw-1:completeRaw;
    for(uint32_t i=0;i<completeLength && i<sizeof(completeInjection);++i) {
        if(completeInjection[i]<'0' || completeInjection[i]>'9') {completeFail=99;break;}
        completeFail=completeFail*10+uint32_t(completeInjection[i]-'0');
    }
    if(completeRaw>sizeof(completeInjection) || completeLength>2 || completeFail>11 || (completeRaw && !completeFail)) {
        KPuts("GUARD_COMPLETE_INCOMPLETE injection\n");return;
    }
#endif
    if(!AhciSystemGuardTestBeginCompletion(adapter,identityOk)) {
        KPuts("GUARD_COMPLETE_INCOMPLETE authorization\n");return;
    }
    const int complete=FatFsPrivateRecoveryTestComplete(mount,identity,records[0],completeFail);
    AhciSystemGuardTestFinish(adapter);
    if(FatFsPrivateRecoveryTestComplete(mount,identity,records[0],completeFail)!=0) {
        KPuts("GUARD_COMPLETE_INCOMPLETE repeated\n");return;
    }
    KPrintf("GUARD_COMPLETE_RESULT outcome=%d\n",complete);
    // Reset only after the durable clean pair. Failures, uncertain completion
    // and returned reset writes never retry; the test build requires opt-in.
    bool restartRequested=false;
#if defined(BROOK_SSD_AUTO_RECOVERY)
    restartRequested=true;
#else
    char restart[8]{};
    const auto restartSize=FwCfgReadFile("opt/guard-complete-restart",restart,sizeof(restart));
    restartRequested=restartSize==6 && !memcmp(restart,"test-1",6);
#endif
    if(complete==2 && restartRequested) {
        KPuts("GUARD_COMPLETE_RESTART\n");
        if(PlatformPowerIssue(true))SchedulerSleepMs(1000);
        KPuts("GUARD_COMPLETE_RESTART_RETURNED\n");
    }
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
