#include "../../kernel/src/ahci_readonly_sequence.h"
#include <cassert>
#include <initializer_list>
using namespace brook::ahci_ro;
struct Mock {
    uint32_t regs[32]{};unsigned writes=0,fences=0;uint32_t transferredBytes=512;
    bool stuckCi=false,stuckCr=false,stuckFr=false,ignoreStop=false,ignoreFisStop=false,unavailableAfterWrite=false;
    uint32_t ignoredOffset=0xffffffffu;
    uint32_t injectIs=0, interruptOnAck=0;
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
    uint64_t nowNs(){return now;}
    void pause(){
        ++polls;
        now+=stepNs;
        regs[IS/4]|=isOnPause;
        regs[SERR/4]|=serrOnPause;
        if(polls==settleAfter){regs[SSTS/4]=settleLink;regs[TFD/4]=0;}
    }
    bool waitClear(uint32_t off,uint32_t mask){
        if((off==CI&&stuckCi)||(mask==CR&&stuckCr)||(mask==FR&&stuckFr))return false;
        regs[off/4]&=~mask;
        if(off==CI){
            regs[IS/4]|=injectIs;
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
    Mock flushMissing;flushMissing.regs[CMD/4]=ST|FRE;flushMissing.transferredBytes=0;assert(SubmitNoData(flushMissing)==Result::DeviceError);
    Mock flushIgnored;flushIgnored.regs[CMD/4]=ST|FRE;flushIgnored.ignoredOffset=CI;flushIgnored.transferredBytes=0;assert(SubmitNoData(flushIgnored)==Result::DeviceError);
    Mock flushFault;flushFault.regs[CMD/4]=ST|FRE;flushFault.regs[TFD/4]=0x20;flushFault.injectIs=1;assert(SubmitNoData(flushFault)==Result::DeviceError);
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
