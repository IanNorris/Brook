#include "../../kernel/src/ahci_readonly_sequence.h"
#include <cassert>
#include <initializer_list>
using namespace brook::ahci_ro;
struct Mock {
    uint32_t regs[32]{};unsigned writes=0,fences=0;uint32_t transferredBytes=512;
    bool stuckCi=false,stuckCr=false,stuckFr=false,ignoreStop=false,ignoreFisStop=false,unavailableAfterWrite=false;
    uint32_t ignoredOffset=0xffffffffu;
    uint32_t injectIs=0, interruptOnAck=0;
    uint32_t lastCompletionFis=0, receivedFis=0, injectFis=0x00404034u;
    uint32_t taskOnCompletion=0xffffffffu;
    uint32_t taskOnSettle=0, fisOnSettle=0xffffffffu;
    bool allowStaleTaskBusy=false, postedWithStaleTaskBusy=false, lastDmaCompletedWithStaleTaskBusy=false;
    bool lastCommandCompletedOk=false;
    unsigned fisClears=0;
    uint32_t settleLink=0x133, isOnPause=0, serrOnPause=0, linkOnCompletion=0;
    uint64_t now=0, stepNs=1000000;
    unsigned commandIssues=0, polls=0, settleAfter=0;
    Mock(){regs[SSTS/4]=0x133;regs[SIG/4]=ATA_SIGNATURE;}
    uint32_t read(uint32_t off){return regs[off/4];}
    void write(uint32_t off,uint32_t value){
        ++writes;
        if(off==CI && value)++commandIssues;
        if(off==IS) {
            regs[off/4]|=interruptOnAck;
            regs[off/4]&=~value;
        } else if(off==SERR)regs[off/4]&=~value;
        else if(off!=ignoredOffset && !(off==CMD && ignoreStop && !(value&ST)) &&
                !(off==CMD && ignoreFisStop && !(value&FRE)))regs[off/4]=value;
        if(off==CMD && unavailableAfterWrite)regs[off/4]=0xffffffffu;
    }
    void fence(){++fences;}
    uint32_t receivedRegisterFis(){return receivedFis;}
    void clearReceivedRegisterFis(){receivedFis=0;++fisClears;}
    uint64_t nowNs(){return now;}
    void pause(){
        ++polls;
        now+=stepNs;
        regs[IS/4]|=isOnPause;
        regs[SERR/4]|=serrOnPause;
        if(polls==settleAfter){
            regs[SSTS/4]=settleLink;regs[TFD/4]=taskOnSettle;
            if(fisOnSettle!=0xffffffffu)receivedFis=fisOnSettle;
        }
    }
    bool waitClear(uint32_t off,uint32_t mask){
        if((off==CI&&stuckCi)||(mask==CR&&stuckCr)||(mask==FR&&stuckFr))return false;
        const bool issued=regs[off/4]&mask;
        regs[off/4]&=~mask;
        if(off==CI && issued){
            regs[IS/4]|=injectIs;
            if(injectIs&D2H_REGISTER_IRQ)receivedFis=injectFis;
            if(taskOnCompletion!=0xffffffffu)regs[TFD/4]=taskOnCompletion;
            if(linkOnCompletion)regs[SSTS/4]=linkOnCompletion;
        }
        return true;
    }
    bool waitClearFlush(uint32_t off,uint32_t mask){return waitClear(off,mask);}
    uint32_t transferred(){return transferredBytes;}
};
int main(){
    Mock m;
    assert(ControllerModeReady(0x80000000u));
    assert(!ControllerModeReady(0xffffffffu) && !ControllerModeReady(0x80000001u) && !ControllerModeReady(0x80000002u));
    assert(Prepare(m,0x1000,0x2000,false)==Result::Ok);
    assert(m.regs[CLB/4]==0x1000 && m.regs[FB/4]==0x2000 && m.fences==1);
    assert(Start(m)==Result::Ok && (m.regs[CMD/4]&(ST|FRE))==(ST|FRE));
    assert(Submit(m,512)==Result::Ok && m.fences==3);
    assert(Quiesce(m) && !(m.regs[CMD/4]&(ST|FRE)));
    Mock busy;busy.regs[CI/4]=1;assert(Prepare(busy,0x1000,0x2000,false)==Result::Busy && busy.writes==0);
    Mock invalid;assert(Prepare(invalid,0x1001,0x2000,false)==Result::InvalidDma && invalid.writes==0);
    for(uint32_t off:{CLB,CLBU,FB,FBU}){Mock ignored;ignored.regs[off/4]=0xdeadbeefu;ignored.ignoredOffset=off;assert(Prepare(ignored,0x1000,0x2000,false)==Result::DeviceError);}
    assert(Prepare(invalid,0x100000000ull,0x2000,false)==Result::InvalidDma && invalid.writes==0);
    Mock tfd;tfd.regs[TFD/4]=0x80;assert(Submit(tfd,512)==Result::DeviceBusy && tfd.writes==0);
    Mock timeout;timeout.stuckCi=true;assert(Submit(timeout,512)==Result::Timeout);
    timeout.stuckCr=true;assert(!Quiesce(timeout));
    Mock stop;stop.regs[CMD/4]=ST|FRE;stop.ignoreStop=true;assert(!Quiesce(stop));
    Mock fis;fis.regs[CMD/4]=FRE;fis.ignoreFisStop=true;assert(!Quiesce(fis));
    Mock fr;fr.regs[CMD/4]=FRE|FR;fr.stuckFr=true;assert(!Quiesce(fr));
    Mock unavailable;unavailable.unavailableAfterWrite=true;assert(Start(unavailable)==Result::DeviceError);
    Mock shortIo;shortIo.transferredBytes=511;assert(Submit(shortIo,512)==Result::ShortTransfer);
    Mock error;error.regs[TFD/4]=1;assert(Submit(error,512)==Result::DeviceError);
    Mock flush;flush.transferredBytes=0;flush.regs[CMD/4]=ST|FRE;flush.injectIs=1;assert(SubmitNoData(flush)==Result::Ok);
    Mock flushTimeout;flushTimeout.regs[CMD/4]=ST|FRE;flushTimeout.stuckCi=true;assert(SubmitNoData(flushTimeout)==Result::Timeout);
    Mock flushError;flushError.regs[CMD/4]=ST|FRE;flushError.injectIs=1|(1u<<30);assert(SubmitNoData(flushError)==Result::DeviceError);
    Mock flushUnexpected;flushUnexpected.regs[CMD/4]=ST|FRE;flushUnexpected.injectIs=1;assert(SubmitNoData(flushUnexpected)==Result::ShortTransfer);
    Mock flushBusy;flushBusy.regs[CMD/4]=ST|FRE;flushBusy.regs[CI/4]=1;assert(SubmitNoData(flushBusy)==Result::Busy && flushBusy.writes==0);
    Mock flushStopped;assert(SubmitNoData(flushStopped)==Result::DeviceError && flushStopped.writes==0);
    Mock flushMissing;flushMissing.regs[CMD/4]=ST|FRE;flushMissing.transferredBytes=0;assert(SubmitNoData(flushMissing)==Result::CompletionUnconfirmed);
    Mock flushIgnored;flushIgnored.regs[CMD/4]=ST|FRE;flushIgnored.ignoredOffset=CI;flushIgnored.transferredBytes=0;assert(SubmitNoData(flushIgnored)==Result::CompletionUnconfirmed);
    Mock flushFault;flushFault.regs[CMD/4]=ST|FRE;flushFault.regs[TFD/4]=0x20;flushFault.injectIs=1;assert(SubmitNoData(flushFault)==Result::DeviceError);
    // Workstation logged the old success FIS and the new zeroed PRDBC
    // before the current command was issued. Model an independently
    // verified previous command, then that exact pre-issue register tuple.
    const auto workstation=[] {
        Mock io;
        io.allowStaleTaskBusy=true;
        io.regs[CMD/4]=0x4c017;
        io.regs[SCTL/4]=0x300;
        io.injectIs=D2H_REGISTER_IRQ;
        io.injectFis=0x00404034;
        assert(Submit(io,512)==Result::Ok);
        io.regs[TFD/4]=0xc0;
        return io;
    };
    Mock stale=workstation();
    assert(Submit(stale,512)==Result::Ok && stale.commandIssues==2 && stale.fisClears==2);
    assert(stale.postedWithStaleTaskBusy && stale.regs[CI/4]==0 && stale.regs[TFD/4]==0xc0);

    Mock physical=workstation();physical.allowStaleTaskBusy=false;
    assert(Submit(physical,512)==Result::DeviceBusy && physical.commandIssues==1);
    Mock unverified=workstation();unverified.lastCompletionFis=0;unverified.lastCommandCompletedOk=false;
    assert(Submit(unverified,512)==Result::DeviceBusy && unverified.commandIssues==1);
    Mock changedFis=workstation();changedFis.receivedFis=0x00504034;
    assert(Submit(changedFis,512)==Result::DeviceBusy && changedFis.commandIssues==1);
    Mock recovered=workstation();recovered.allowStaleTaskBusy=false;recovered.settleAfter=1;
    assert(Submit(recovered,512)==Result::Ok && recovered.commandIssues==2);
    assert(!recovered.postedWithStaleTaskBusy);
    Mock stalledClock=workstation();stalledClock.allowStaleTaskBusy=false;stalledClock.stepNs=0;
    assert(Submit(stalledClock,512)==Result::DeviceBusy && stalledClock.polls==TASK_SETTLE_POLLS);

    for(uint32_t bytes:{0u,511u,513u}) {
        Mock incomplete=workstation();incomplete.transferredBytes=bytes;
        assert(Submit(incomplete,512)==Result::ShortTransfer && incomplete.commandIssues==2);
        assert(incomplete.lastCompletionFis==0);
    }
    // The Workstation restart saw CI/SACT clear, fresh DHRS and exact
    // PRDBC while TFD still held BSY and the received FIS slot was empty.
    for(uint32_t bytes:{512u,4096u,65536u}) {
        Mock lagged;
        lagged.allowStaleTaskBusy=true;
        lagged.regs[CMD/4]=0x4c017;
        lagged.regs[SCTL/4]=0x300;
        lagged.injectIs=D2H_REGISTER_IRQ;
        lagged.injectFis=0;
        lagged.taskOnCompletion=0xc0;
        lagged.transferredBytes=bytes;
        lagged.stepNs=1000;

        Mock settles=lagged;settles.settleAfter=2;settles.taskOnSettle=0x40;
        settles.fisOnSettle=0x00404034;
        assert(Submit(settles,bytes)==Result::Ok && settles.polls==2);
        assert(settles.lastCompletionFis==0x00404034 && settles.commandIssues==1);

        Mock signalOnly=lagged;
        assert(Submit(signalOnly,bytes)==Result::Ok);
        assert(signalOnly.now==TASK_SETTLE_NS && signalOnly.lastCompletionFis==0);

        // The next submission can use the late clean FIS once it appears.
        signalOnly.receivedFis=0x00404034;
        signalOnly.injectFis=0x00404034;
        assert(Submit(signalOnly,bytes)==Result::Ok && signalOnly.commandIssues==2);
        assert(signalOnly.postedWithStaleTaskBusy);

        Mock physicalLag=lagged;physicalLag.allowStaleTaskBusy=false;
        assert(Submit(physicalLag,bytes)==Result::CompletionUnconfirmed && !physicalLag.polls);
        Mock shortLag=lagged;--shortLag.transferredBytes;
        assert(Submit(shortLag,bytes)==Result::ShortTransfer);
        Mock drq=lagged;drq.taskOnCompletion=0xc8;
        assert(Submit(drq,bytes)==Result::CompletionUnconfirmed);
        Mock taskError=lagged;taskError.settleAfter=2;taskError.taskOnSettle=0x41;
        assert(Submit(taskError,bytes)==Result::DeviceError);
        Mock fisError=lagged;fisError.settleAfter=2;fisError.fisOnSettle=0x04414034;
        assert(Submit(fisError,bytes)==Result::DeviceError);
        Mock fatalIrq=lagged;fatalIrq.isOnPause=1u<<30;
        assert(Submit(fatalIrq,bytes)==Result::DeviceError && fatalIrq.polls==1);
        Mock exchange=lagged;exchange.serrOnPause=MEDIA_CHANGE_SERR;
        assert(Submit(exchange,bytes)==Result::MediaChanged && exchange.polls==1);
        Mock stalled=lagged;stalled.stepNs=0;
        assert(Submit(stalled,bytes)==Result::Ok && stalled.polls==TASK_SETTLE_POLLS);
    }
    // quickwins-25f0ea6a: after an accepted completion Workstation kept
    // PxTFD=0xc0 with DHRS set, CI/SACT clear and an empty received FIS
    // through the whole pre-issue wait; the port was quarantined.
    {
        const auto vm=[] {
            Mock io;io.allowStaleTaskBusy=true;
            io.regs[CMD/4]=0x4c017;io.regs[SCTL/4]=0x300;
            io.injectIs=D2H_REGISTER_IRQ;io.stepNs=1000;
            return io;
        };
        const auto expectStalePost=[](Mock& io, bool noData) {
            const unsigned issues=io.commandIssues;const uint64_t began=io.now;
            io.injectFis=0x00404034;io.taskOnCompletion=0x40;
            assert((noData?SubmitNoData(io):Submit(io,512))==Result::Ok);
            assert(io.commandIssues==issues+1 && io.postedWithStaleTaskBusy && io.lastCommandCompletedOk);
            // The bounded pre-issue wait ran before the shadow was accepted.
            assert(io.now-began>=TASK_SETTLE_NS);
        };
        // DMA completion accepted on DHRS + PRDBC with no FIS; FIS never lands.
        Mock neverFis=vm();neverFis.injectFis=0;neverFis.taskOnCompletion=0xc0;
        assert(Submit(neverFis,512)==Result::Ok && neverFis.lastDmaCompletedWithStaleTaskBusy);
        assert(neverFis.lastCommandCompletedOk && neverFis.receivedFis==0);
        expectStalePost(neverFis,false);
        // DMA completion accepted with TFD idle and no FIS; the shadow returns later.
        Mock idleThenShadow=vm();idleThenShadow.injectFis=0;idleThenShadow.taskOnCompletion=0x40;
        assert(Submit(idleThenShadow,512)==Result::Ok && !idleThenShadow.lastDmaCompletedWithStaleTaskBusy);
        assert(idleThenShadow.lastCompletionFis==0);
        idleThenShadow.regs[TFD/4]=0xc0;
        expectStalePost(idleThenShadow,false);
        // Clean DMA completion whose FIS was then cleared (ArmCommand) and not reposted.
        Mock clearedFis=vm();clearedFis.injectFis=0x00404034;
        assert(Submit(clearedFis,512)==Result::Ok && clearedFis.lastCompletionFis==0x00404034);
        clearedFis.receivedFis=0;clearedFis.regs[TFD/4]=0xc0;
        expectStalePost(clearedFis,false);
        // No-data FLUSH completion, then an empty FIS and the busy shadow.
        Mock flushed=vm();flushed.injectFis=0x00404034;flushed.transferredBytes=0;
        assert(SubmitNoData(flushed)==Result::Ok && flushed.lastCommandCompletedOk);
        flushed.receivedFis=0;flushed.regs[TFD/4]=0xc0;flushed.transferredBytes=512;
        expectStalePost(flushed,false);
        // ...and a flush posted over the same shadow.
        Mock beforeFlush=vm();beforeFlush.injectFis=0;beforeFlush.taskOnCompletion=0xc0;
        assert(Submit(beforeFlush,512)==Result::Ok);
        beforeFlush.transferredBytes=0;
        expectStalePost(beforeFlush,true);

        const auto shadow=[&] {
            Mock io=vm();io.injectFis=0;io.taskOnCompletion=0xc0;
            assert(Submit(io,512)==Result::Ok && io.commandIssues==1);
            return io;
        };
        // Real errors still fail; unexplained state still refuses to issue.
        Mock taskErr=shadow();taskErr.regs[TFD/4]=0xc1;
        assert(Submit(taskErr,512)==Result::DeviceError && taskErr.commandIssues==1);
        Mock tfes=shadow();tfes.regs[IS/4]|=1u<<30;
        assert(Submit(tfes,512)==Result::DeviceError && tfes.commandIssues==1);
        Mock ifs=shadow();ifs.isOnPause=1u<<27;
        assert(Submit(ifs,512)==Result::DeviceError && ifs.commandIssues==1);
        Mock serrCmd=shadow();serrCmd.regs[SERR/4]=1u<<10;
        assert(Submit(serrCmd,512)==Result::DeviceError && serrCmd.commandIssues==1);
        Mock serrRecovered=shadow();serrRecovered.regs[SERR/4]=1u<<1;
        assert(Submit(serrRecovered,512)==Result::DeviceBusy && serrRecovered.commandIssues==1);
        Mock linkEvent=shadow();linkEvent.regs[IS/4]|=1u<<22;
        assert(Submit(linkEvent,512)==Result::DeviceBusy && linkEvent.commandIssues==1);
        Mock drqShadow=shadow();drqShadow.regs[TFD/4]=0xc8;
        assert(Submit(drqShadow,512)==Result::DeviceBusy && drqShadow.commandIssues==1);
        Mock bsyOnly=shadow();bsyOnly.regs[TFD/4]=0x80;
        assert(Submit(bsyOnly,512)==Result::DeviceBusy && bsyOnly.commandIssues==1);
        Mock busyFisShadow=shadow();busyFisShadow.receivedFis=0x00c04034;
        assert(Submit(busyFisShadow,512)==Result::DeviceBusy && busyFisShadow.commandIssues==1);
        Mock priorFailed=shadow();priorFailed.lastCommandCompletedOk=false;
        assert(Submit(priorFailed,512)==Result::DeviceBusy && priorFailed.commandIssues==1);
        // A failed completion clears the evidence for the next issue.
        Mock failedThenShadow=shadow();failedThenShadow.transferredBytes=511;
        failedThenShadow.taskOnCompletion=0x40;
        assert(Submit(failedThenShadow,512)==Result::ShortTransfer && !failedThenShadow.lastCommandCompletedOk);
        failedThenShadow.regs[TFD/4]=0xc0;failedThenShadow.transferredBytes=512;
        assert(Submit(failedThenShadow,512)==Result::DeviceBusy && failedThenShadow.commandIssues==2);
        // A shadow that clears inside the wait issues normally.
        Mock clears=shadow();clears.settleAfter=clears.polls+2;clears.taskOnSettle=0x40;
        clears.taskOnCompletion=0x40;clears.injectFis=0x00404034;
        assert(Submit(clears,512)==Result::Ok && clears.commandIssues==2 && !clears.postedWithStaleTaskBusy);

        // Generic (non-VMware) HBAs: identical refusal, polls and register
        // writes whether or not the previous command completed.
        for(bool completedOk:{false,true}) for(bool noData:{false,true}) {
            Mock generic=shadow();
            generic.allowStaleTaskBusy=false;generic.lastCommandCompletedOk=completedOk;
            Mock reference=generic;reference.lastCommandCompletedOk=!completedOk;
            const auto a=noData?SubmitNoData(generic):Submit(generic,512);
            const auto b=noData?SubmitNoData(reference):Submit(reference,512);
            assert(a==Result::DeviceBusy && b==a);
            assert(generic.commandIssues==1 && generic.writes==reference.writes);
            assert(generic.polls==reference.polls && generic.fences==reference.fences);
            assert(generic.regs[IS/4]==reference.regs[IS/4] && generic.receivedFis==reference.receivedFis);
        }
    }
    Mock lostFis=workstation();lostFis.injectIs=0;lostFis.injectFis=0;
    assert(Submit(lostFis,512)==Result::CompletionUnconfirmed && lostFis.commandIssues==2);
    Mock lostIssue=workstation();lostIssue.ignoredOffset=CI;lostIssue.transferredBytes=0;
    assert(Submit(lostIssue,512)==Result::CompletionUnconfirmed && lostIssue.receivedFis==0);
    Mock badFis=workstation();badFis.injectFis=0x04414034;
    assert(Submit(badFis,512)==Result::DeviceError);
    Mock busyFis=workstation();busyFis.injectFis=0x00c04034;
    assert(Submit(busyFis,512)==Result::CompletionUnconfirmed);
    Mock active=workstation();active.stuckCi=true;
    assert(Submit(active,512)==Result::Timeout);
    Mock unretired=workstation();unretired.regs[SACT/4]=1;
    assert(Submit(unretired,512)==Result::Busy && unretired.commandIssues==1);
    Mock staleFlush=workstation();staleFlush.transferredBytes=0;
    assert(SubmitNoData(staleFlush)==Result::Ok && staleFlush.commandIssues==2);
    Mock missingFlush=workstation();missingFlush.transferredBytes=0;missingFlush.injectFis=0;
    assert(SubmitNoData(missingFlush)==Result::CompletionUnconfirmed);
    Mock busyExchange=workstation();busyExchange.serrOnPause=MEDIA_CHANGE_SERR;
    assert(Submit(busyExchange,512)==Result::MediaChanged && busyExchange.commandIssues==1);
    Mock corruptLink=workstation();corruptLink.serrOnPause=1u<<9;
    assert(Submit(corruptLink,512)==Result::DeviceError && corruptLink.commandIssues==1);
    Mock duringErrorAck;duringErrorAck.interruptOnAck=1u<<30;
    assert(Submit(duringErrorAck,512)==Result::DeviceError && duringErrorAck.commandIssues==0);

    assert(!NeedsQuarantine(Result::DeviceBusy));
    assert(!NeedsQuarantine(Result::CompletionUnconfirmed));
    assert(!NeedsQuarantine(Result::ShortTransfer));
    assert(NeedsQuarantine(Result::Busy) && NeedsQuarantine(Result::Timeout));
    assert(NeedsQuarantine(Result::DeviceError) && NeedsQuarantine(Result::MediaChanged));

    for(bool noData:{false,true}) {
        const auto submit=[noData](Mock& fault) {
            fault.regs[CMD/4]=ST|FRE;
            fault.transferredBytes=noData ? 0 : 512;
            return noData ? SubmitNoData(fault) : Submit(fault,512);
        };
        for(uint32_t change:{1u<<31,1u<<6}) {
            Mock pending;
            pending.regs[IS/4]=change;
            assert(submit(pending)==Result::MediaChanged && !pending.writes && !pending.polls);

            Mock duringAck;
            duringAck.interruptOnAck=change;
            assert(submit(duringAck)==Result::MediaChanged && !duringAck.commandIssues);
            assert(duringAck.regs[IS/4]==change);

            Mock inFlight;
            inFlight.injectIs=change|1;
            assert(submit(inFlight)==Result::MediaChanged && inFlight.commandIssues==1);
        }
        for(uint32_t link:{0x233u,0x633u}) { // Partial and Slumber, DET=3
            Mock lpm;
            lpm.regs[SSTS/4]=link;
            lpm.regs[IS/4]=1u<<22;   // PRCS
            lpm.regs[SERR/4]=1u<<16; // DIAG.N
            lpm.injectIs=1u|(1u<<22);
            assert(submit(lpm)==Result::Ok && lpm.commandIssues==1);
            assert(lpm.regs[SERR/4]==(1u<<16));

            // Device exchange while the PhyRdy latch is already set is still
            // a media change, even when the live link has returned to DET=3.
            lpm.regs[SERR/4]|=1u<<26;
            assert(submit(lpm)==Result::MediaChanged && lpm.commandIssues==1);
        }
        for(uint32_t link:{0x1u,0x801u}) { // Transient DET and DevSleep
            Mock transient;
            transient.regs[SSTS/4]=link;
            transient.regs[TFD/4]=0x80;
            transient.settleAfter=3;
            transient.injectIs=1;
            assert(submit(transient)==Result::Ok && transient.commandIssues==1 && transient.polls==3);

            Mock persistent;
            persistent.regs[SSTS/4]=link;
            assert(submit(persistent)==Result::MediaChanged && !persistent.writes);
            assert(persistent.now==MEDIA_SETTLE_NS);

            Mock exchange;
            exchange.regs[SSTS/4]=link;
            exchange.serrOnPause=1u<<26;
            exchange.settleAfter=1;
            assert(submit(exchange)==Result::MediaChanged && !exchange.writes && exchange.polls==1);

            Mock coldPort;
            coldPort.regs[SSTS/4]=link;
            coldPort.isOnPause=1u<<31;
            coldPort.settleAfter=1;
            assert(submit(coldPort)==Result::MediaChanged && !coldPort.writes && coldPort.polls==1);
        }
        Mock devSleepOnline;
        devSleepOnline.regs[SSTS/4]=0x833;
        devSleepOnline.injectIs=1;
        assert(submit(devSleepOnline)==Result::Ok && devSleepOnline.commandIssues==1 && devSleepOnline.polls==3);

        Mock disconnected;
        disconnected.regs[SSTS/4]=0;
        disconnected.stepNs=0; // A stalled panic/early boot clock must not hang.
        assert(submit(disconnected)==Result::MediaChanged && !disconnected.writes);
        assert(disconnected.polls==MEDIA_SETTLE_POLLS);

        Mock completion;
        completion.linkOnCompletion=0x801;
        completion.settleAfter=1;
        completion.injectIs=1;
        assert(submit(completion)==Result::Ok && completion.commandIssues==1 && completion.polls==1);

        Mock wrongDevice;
        wrongDevice.regs[SIG/4]=0xeb140101;
        assert(submit(wrongDevice)==Result::MediaChanged && !wrongDevice.writes);
    }
    for(uint32_t bit:{24u,27u,28u,29u,30u}){Mock fatal;fatal.injectIs=1u<<bit;assert(Submit(fatal,512)==Result::DeviceError);}
}
