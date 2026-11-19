#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "../../drivers/intel_hda/hda_dell_stream_queue.h"
using namespace brook;
struct Fake {
    static constexpr uint32_t Ring=AudioDmaClock::RingBytes;
    uint8_t cpu[Ring]{},device[Ring]{};uint64_t now=1000000000,absolute=0;
    unsigned writes=0,flushes=0,stops=0,mutes=0,quarantines=0;bool started=false,failFlush=false,failStart=false,moveOnFlush=false,stopFails=false,running=true;
    uint64_t nowNs(){return now;}
    bool position(uint32_t& p,bool& error,bool& run){p=uint32_t(absolute%Ring);error=false;run=running;return true;}
    bool write(uint32_t at,const uint8_t* data,uint32_t n){assert(at<=Ring && n<=Ring-at);++writes;
        if(data)std::memcpy(cpu+at,data,n);else std::memset(cpu+at,0,n);return true;}
    bool publish(uint32_t at,uint32_t n){assert(at<=Ring && n<=Ring-at);++flushes;if(failFlush)return false;
        const uint32_t first=at&~63u,last=(at+n+63)&~63u;std::memcpy(device+first,cpu+first,last-first);
        if(moveOnFlush){absolute+=8192;now+=47000000;moveOnFlush=false;}return true;}
    bool fence(){return true;}bool start(){started=true;return !failStart;}
    bool mute(){++mutes;return true;}bool stop(){++stops;return !stopFails;}void quarantine(){++quarantines;}
    void advance(uint32_t n,const std::vector<uint8_t>& expected){
        for(uint32_t i=0;i<n;++i)assert(device[(absolute+i)%Ring]==expected[absolute+i]);
        absolute+=n;now+=uint64_t(n)*1000000000ULL/AudioDmaClock::BytesPerSecond;
    }
};
// Register-capable fake: stop() clears RUN and BME, as the owner verifies.
struct RegFake:Fake {
    uint8_t status=0;bool muteFails=false;
    bool mute(){++mutes;return !muteFails;}bool stop(){++stops;if(stopFails)return false;running=false;return true;}
    bool start(){running=true;return Fake::start();}
    void faultRegisters(HdaDellStreamRegisters& r){r={};r.valid=true;r.command=running?6:2;r.control=running?0x100002:0x100000;
        r.status=status;r.lpib=uint32_t(absolute%Ring);r.cbl=Ring;}
};
static void FaultRecords(){
    uint8_t samples[128];std::memset(samples,0x11,sizeof(samples));
    // Late service: elapsed since the last accepted sample exceeds the budget.
    Fake late;HdaDellStreamQueue<Fake> l(late,64,4096,1024,20000);assert(l.start() && !l.faultCount());
    late.now+=AudioDmaClock::MaxSampleNs;assert(!l.service() && late.quarantines==1);
    auto f=l.lastFault();assert(f.recorded && f.ordinal==1 && l.faultCount()==1 && f.stage==HdaDellFaultStage::Service);
    assert(f.reason==HdaDellFaultReason::ServiceLate && f.sampleReason==HdaDellFaultReason::ServiceLate && f.muted && f.stopped);
    assert(!f.recoverable && !f.atFault.valid && f.sampleAfterNs-f.previousSampleNs==AudioDmaClock::MaxSampleNs && f.epoch==1);
    assert(l.firstFault().ordinal==1 && !l.recoverable());
    assert(!l.service() && l.faultCount()==1 && late.quarantines==1); // terminal: no second record or retry
    // RUN observed clear by hardware.
    Fake halted;HdaDellStreamQueue<Fake> h(halted,64,4096);assert(h.start());halted.running=false;
    assert(!h.service() && h.lastFault().reason==HdaDellFaultReason::NotRunning && !h.lastFault().sampledRunning);
    // DMA position frozen for 150 ms.
    Fake stalled;HdaDellStreamQueue<Fake> st(stalled,64,4096);assert(st.start());
    for(unsigned i=0;i<4;++i){stalled.now+=40000000;if(i<3)assert(st.service());}
    assert(!st.service() && st.lastFault().reason==HdaDellFaultReason::Stalled && st.lastFault().lastMovementNs);
    // Impossible jump: recorded with the observed delta and allowance.
    Fake jump;HdaDellStreamQueue<Fake> j(jump,64,4096);assert(j.start());jump.absolute=8192;jump.now+=1000000;
    assert(!j.service());f=j.lastFault();assert(f.reason==HdaDellFaultReason::PositionJump && f.delta==8192 && f.allowed<8192 && f.position==8192);
    // Clock unavailable (owner clock failed).
    Fake zero;HdaDellStreamQueue<Fake> z(zero,64,4096);assert(z.start());zero.now=0;
    assert(!z.service() && z.lastFault().reason==HdaDellFaultReason::ClockUnavailable && !z.lastFault().atNs);
    // Device error bit reported by the position read.
    struct ErrorFake:Fake{bool position(uint32_t& p,bool& e,bool& r){Fake::position(p,e,r);e=true;return true;}};
    ErrorFake err;HdaDellStreamQueue<ErrorFake> e(err,64,4096);assert(e.start());err.now+=1000;
    assert(!e.service() && e.lastFault().reason==HdaDellFaultReason::DeviceError && e.lastFault().sampledError);
    // Publication: a cache flush failure is I/O, a moving cursor is uncertain.
    Fake flush;HdaDellStreamQueue<Fake> fl(flush,64,4096);assert(fl.start());flush.failFlush=true;
    assert(fl.submit(samples,sizeof(samples)).error);f=fl.lastFault();
    assert(f.stage==HdaDellFaultStage::Publish && f.reason==HdaDellFaultReason::PublishIo);
    Fake moving;HdaDellStreamQueue<Fake> mv(moving,64,4096);assert(mv.start());moving.moveOnFlush=true;
    assert(mv.submit(samples,sizeof(samples)).error);f=mv.lastFault();
    assert(f.stage==HdaDellFaultStage::Publish && f.reason==HdaDellFaultReason::PublishUncertain && f.completed>f.submitted); // DMA overran the copy
    // Shutdown whose stop is not verified, and an external abort.
    Fake closing;HdaDellStreamQueue<Fake> sh(closing,64,4096);assert(sh.start());closing.stopFails=true;
    assert(!sh.shutdown() && sh.lastFault().stage==HdaDellFaultStage::Shutdown && !sh.lastFault().stopped && closing.quarantines==1);
    Fake aborted;HdaDellStreamQueue<Fake> ab(aborted,64,4096);assert(ab.start() && !ab.abort());
    assert(ab.lastFault().reason==HdaDellFaultReason::ExternalAbort && aborted.quarantines==1);
    // Register snapshots before and after the bounded stop.
    RegFake regs;HdaDellStreamQueue<RegFake> rq(regs,64,4096);assert(rq.start());regs.status=8;regs.now+=AudioDmaClock::MaxSampleNs;
    assert(!rq.service());f=rq.lastFault();
    assert(f.atFault.valid && (f.atFault.control&2) && f.atFault.status==8 && f.atFault.command==6);
    assert(f.afterStop.valid && !(f.afterStop.control&2) && f.afterStop.command==2 && !f.recoverable && regs.quarantines==1);
}
static void Recovery(){
    uint8_t samples[128];std::memset(samples,0x22,sizeof(samples));
    // Admitted: late service with verified mute/stop, no DESE. Left stopped
    // and not quarantined until the owner retires and reprepares it.
    RegFake a;HdaDellStreamQueue<RegFake> q(a,64,4096);q.allowRecovery(true);assert(q.start());
    assert(q.submit(samples,sizeof(samples)).accepted);a.now+=AudioDmaClock::MaxSampleNs;
    assert(!q.service() && q.failed() && q.recoverable() && !a.quarantines && !a.running && a.mutes==1 && a.stops==1);
    assert(q.lastFault().recoverable && q.progress().discontinuity);
    const auto epoch=q.progress().epoch;assert(!q.recoverAfterVerifiedFault(epoch+1) && q.recoverable());
    assert(q.recoverAfterVerifiedFault(epoch) && !q.failed() && !q.recoverable() && q.progress().epoch==epoch+1);
    assert(!q.recoverAfterVerifiedFault(epoch+1)); // one use per fault
    a.absolute=0;assert(q.start() && a.running);auto r=q.submit(samples,sizeof(samples));assert(r.accepted && r.epoch==epoch+1);
    assert(q.lastFault().ordinal==1 && q.faultCount()==1 && !a.quarantines);
    // A second fault is recorded separately; the first is retained.
    a.now+=AudioDmaClock::MaxSampleNs;assert(!q.service() && q.faultCount()==2 && q.lastFault().ordinal==2 && q.firstFault().ordinal==1);
    q.abandonRecovery();assert(a.quarantines==1 && !q.recoverable() && !q.lastFault().recoverable);
    assert(!q.recoverAfterVerifiedFault(q.progress().epoch));
    // Never admitted: latched DESE, RUN cleared by hardware, unverified mute,
    // failed stop, flush I/O failure, or recovery not enabled.
    RegFake dese;HdaDellStreamQueue<RegFake> d(dese,64,4096);d.allowRecovery(true);assert(d.start());dese.status=0x18;dese.now+=1000;
    struct ErrorRegFake:RegFake{bool position(uint32_t& p,bool& e,bool& r){Fake::position(p,e,r);e=(status&0x18)!=0;return true;}};
    ErrorRegFake desePos;HdaDellStreamQueue<ErrorRegFake> dp(desePos,64,4096);dp.allowRecovery(true);assert(dp.start());desePos.status=0x10;desePos.now+=1000;
    assert(!dp.service() && dp.lastFault().reason==HdaDellFaultReason::DeviceError && !dp.recoverable() && desePos.quarantines==1);
    ErrorRegFake fifo;HdaDellStreamQueue<ErrorRegFake> fq(fifo,64,4096);fq.allowRecovery(true);assert(fq.start());fifo.status=0x08;fifo.now+=1000;
    assert(!fq.service() && fq.lastFault().reason==HdaDellFaultReason::DeviceError && fq.recoverable() && !fifo.quarantines); // FIFOE only
    dese.now+=AudioDmaClock::MaxSampleNs;assert(!d.service() && !d.recoverable() && dese.quarantines==1);
    RegFake halted;HdaDellStreamQueue<RegFake> h(halted,64,4096);h.allowRecovery(true);assert(h.start());halted.running=false;
    assert(!h.service() && !h.recoverable() && halted.quarantines==1);
    RegFake loud;HdaDellStreamQueue<RegFake> m(loud,64,4096);m.allowRecovery(true);assert(m.start());loud.muteFails=true;loud.now+=AudioDmaClock::MaxSampleNs;
    assert(!m.service() && !m.recoverable() && loud.quarantines==1 && !m.lastFault().muted);
    RegFake running;HdaDellStreamQueue<RegFake> rs(running,64,4096);rs.allowRecovery(true);assert(rs.start());running.stopFails=true;running.now+=AudioDmaClock::MaxSampleNs;
    assert(!rs.service() && !rs.recoverable() && running.quarantines==1 && !rs.lastFault().stopped);
    RegFake io;HdaDellStreamQueue<RegFake> fi(io,64,4096);fi.allowRecovery(true);assert(fi.start());io.failFlush=true;
    assert(fi.submit(samples,sizeof(samples)).error && !fi.recoverable() && io.quarantines==1);
    RegFake off;HdaDellStreamQueue<RegFake> o(off,64,4096);assert(o.start());off.now+=AudioDmaClock::MaxSampleNs;
    assert(!o.service() && !o.recoverable() && off.quarantines==1);
    // A publication overrun is a timing judgement: admitted.
    RegFake moving;HdaDellStreamQueue<RegFake> mv(moving,64,4096);mv.allowRecovery(true);assert(mv.start());moving.moveOnFlush=true;
    assert(mv.submit(samples,sizeof(samples)).error && mv.recoverable() && !moving.quarantines);
    // abort() on a pending recovery quarantines.
    assert(!mv.abort() && moving.quarantines==1 && !mv.recoverable());
}
int main(){
    FaultRecords();Recovery();
    Fake f;HdaDellStreamQueue<Fake> q(f,64,4096);assert(q.start() && f.started);
    std::vector<uint8_t> expected(200000,0);uint8_t samples[128];
    for(unsigned step=0;step<140;++step){
        if(step%5==0){const uint32_t size=(step%3==0)?4:(step%3==1?68:128);
            std::memset(samples,1+(step%200),sizeof(samples));auto r=q.submit(samples,size);
            assert(!r.error && r.accepted==size);assert(r.begin+size<expected.size());
            std::memcpy(expected.data()+r.begin,samples,r.accepted);}
        f.advance(1024,expected);assert(q.service());
    }
    assert(!q.failed() && q.progress().RawCompleted()==f.absolute && f.absolute>2*Fake::Ring);
    assert(q.shutdown() && f.mutes==1 && f.stops==1 && !f.quarantines && !q.start());
    Fake late;HdaDellStreamQueue<Fake> l(late,64,4096);assert(l.start());late.moveOnFlush=true;
    auto bad=l.submit(samples,sizeof(samples));assert(bad.error && bad.accepted==sizeof(samples) && l.failed() && late.mutes==1 && late.stops==1 && late.quarantines==1);
    assert(l.progress().submitted>=bad.begin+bad.accepted);
    bad=l.submit(samples,sizeof(samples));assert(bad.error && !bad.accepted && late.stops==1);
    Fake broken;HdaDellStreamQueue<Fake> b(broken,64,4096);assert(b.start());broken.failFlush=true;
    bad=b.submit(samples,sizeof(samples));assert(bad.error && b.failed() && broken.stops==1 && broken.quarantines==1);
    Fake clock;HdaDellStreamQueue<Fake> c(clock,64,4096);assert(c.start());clock.now+=AudioDmaClock::MaxSampleNs;
    assert(!c.service() && c.failed() && clock.quarantines==1);
    Fake skew;HdaDellStreamQueue<Fake> skewed(skew,64,4096,1024,20000);assert(skewed.start());
    skew.now+=AudioDmaClock::MaxSampleNs-20000;assert(!skewed.service() && skew.quarantines==1);
    Fake halted;HdaDellStreamQueue<Fake> h(halted,64,4096);assert(h.start());halted.running=false;halted.stopFails=true;
    assert(!h.service() && h.failed() && halted.mutes==1 && halted.stops==1 && halted.quarantines==1);
    Fake stalled;HdaDellStreamQueue<Fake> st(stalled,64,4096);assert(st.start());stalled.now+=150000000;
    assert(!st.service() && st.failed() && stalled.mutes==1 && stalled.quarantines==1);
    Fake jump;HdaDellStreamQueue<Fake> j(jump,64,4096);assert(j.start());jump.absolute=8192;jump.now+=1000000;
    assert(!j.service() && j.failed() && jump.mutes==1 && jump.quarantines==1);
    Fake invalid;HdaDellStreamQueue<Fake> x(invalid,64,4096);assert(x.start());
    bad=x.submit(samples,3);assert(bad.error==-22 && !x.failed());
    Fake bounded;HdaDellStreamQueue<Fake> bq(bounded,64,4096);assert(bq.start());
    std::vector<uint8_t> big(Fake::Ring,0x6d);auto partial=bq.submit(big.data(),big.size());
    assert(!partial.error && partial.accepted>0 && partial.accepted<big.size());
    auto full=bq.submit(samples,sizeof(samples));assert(!full.error && !full.accepted && !bq.failed());
    Fake badGuard;HdaDellStreamQueue<Fake> g(badGuard,64,4096,4096);assert(!g.start() && !badGuard.started);
    Fake unused;HdaDellStreamQueue<Fake> neverStarted(unused,64,4096);assert(neverStarted.closeBeforeStart());
    assert(!neverStarted.closeBeforeStart() && neverStarted.progress().discontinuity && !unused.started);
    assert(neverStarted.restartAfterCleanStop(1) && neverStarted.progress().epoch==2 && neverStarted.start());
    assert(!neverStarted.closeBeforeStart() && neverStarted.shutdown());
    Fake closing;HdaDellStreamQueue<Fake> shut(closing,64,4096);assert(shut.start());closing.stopFails=true;
    assert(!shut.shutdown() && shut.failed() && closing.mutes==1 && closing.stops==1 && closing.quarantines==1);
    Fake pending;HdaDellStreamQueue<Fake> pq(pending,64,4096);assert(pq.start());
    auto outstanding=pq.submit(samples,sizeof(samples));assert(outstanding.accepted==sizeof(samples) && !outstanding.error);
    assert(pq.shutdown() && pq.progress().discontinuity && !pending.quarantines);
    const auto oldEpoch=pq.progress().epoch;const auto oldSubmitted=pq.progress().submitted;
    assert(!pq.restartAfterCleanStop(oldEpoch+1) && pq.progress().epoch==oldEpoch && pq.progress().submitted==oldSubmitted);
    assert(pq.restartAfterCleanStop(oldEpoch) && pq.progress().epoch==oldEpoch+1 && !pq.progress().discontinuity);
    assert(!pq.restartAfterCleanStop(oldEpoch) && pq.progress().completed==0 && pq.progress().submitted==0);
    pending.absolute=0;assert(pq.start());for(auto byte:pending.device)assert(byte==0);
    auto fresh=pq.submit(samples,sizeof(samples));assert(fresh.accepted==sizeof(samples) && fresh.epoch==oldEpoch+1);
    assert(pq.shutdown());
    assert(!b.restartAfterCleanStop(b.progress().epoch) && broken.quarantines==1);
    Fake randomized;HdaDellStreamQueue<Fake> rq(randomized,64,4096);assert(rq.start());
    std::vector<uint8_t> timeline(1800000,0);uint32_t rng=0x38a7;
    for(unsigned step=0;step<1000;++step){
        rng=rng*1664525u+1013904223u;const uint32_t length=4*((rng%32)+1);
        std::memset(samples,1+(step%251),sizeof(samples));auto r=rq.submit(samples,length);
        assert(!r.error && r.accepted==length && r.begin+length<timeline.size());
        std::memcpy(timeline.data()+r.begin,samples,length);
        rng=rng*1664525u+1013904223u;const uint32_t n=4*((rng%448)+64);
        randomized.advance(n,timeline);assert(rq.service());
    }
    assert(randomized.absolute>10*Fake::Ring && !rq.failed() && rq.shutdown());

    // Boot-opt-in counters observe the already-sampled DMA timeline. Startup
    // guard reservations and later empty-tail movement are distinct and may
    // overlap, so neither is asserted to be audible loss.
    Fake trace;HdaDellStreamQueue<Fake> observed(trace,64,4096,1024,0,true);assert(observed.start());
    std::vector<uint8_t> observedTimeline(200000,0);
    trace.advance(1024,observedTimeline);assert(observed.service());
    auto stats=observed.diagnostics();assert(stats.enabled && stats.starts==1 && stats.emptyBeforeBytes==1024 && !stats.emptyAfterBytes);
    std::memset(samples,0x47,sizeof(samples));auto first=observed.submit(samples,sizeof(samples));
    assert(first.accepted==sizeof(samples) && !first.error);
    std::memcpy(observedTimeline.data()+first.begin,samples,first.accepted);
    stats=observed.diagnostics();assert(stats.startupGapBytes==first.begin && stats.publishedBytes==sizeof(samples) && !stats.laterGapEvents);
    trace.advance(uint32_t(first.begin+sizeof(samples)),observedTimeline);assert(observed.service());
    stats=observed.diagnostics();assert(stats.emptyAfterBytes==1024);
    auto second=observed.submit(samples,sizeof(samples));assert(second.accepted==sizeof(samples) && !second.error);
    stats=observed.diagnostics();assert(stats.laterGapEvents==1 && stats.laterGapBytes==second.begin-(first.begin+first.accepted));
    assert(stats.publishedBytes==2*sizeof(samples) && !stats.faults && !stats.publicationFailures);
    auto contiguous=observed.submit(samples,sizeof(samples));assert(contiguous.accepted==sizeof(samples));
    assert(observed.diagnostics().laterGapEvents==1); // no new gap while the tail remains ahead
    assert(observed.shutdown() && observed.restartAfterCleanStop(observed.progress().epoch));
    trace.absolute=0;assert(observed.start());trace.advance(1024,observedTimeline);assert(observed.service());
    stats=observed.diagnostics();assert(stats.starts==2 && stats.stops==1 && stats.emptyBeforeBytes==2048);
    assert(observed.shutdown());
    assert(!q.diagnostics().enabled && !q.diagnostics().starts && !q.diagnostics().publishedBytes);
    Fake tracedFailure;HdaDellStreamQueue<Fake> failedTrace(tracedFailure,64,4096,1024,0,true);assert(failedTrace.start());
    tracedFailure.failFlush=true;assert(failedTrace.submit(samples,sizeof(samples)).error);
    stats=failedTrace.diagnostics();assert(stats.faults==1 && stats.publicationFailures==1 && !stats.publishedBytes);
    Fake tracedFull;HdaDellStreamQueue<Fake> fullTrace(tracedFull,64,4096,1024,0,true);assert(fullTrace.start());
    assert(fullTrace.submit(big.data(),big.size()).accepted);
    assert(!fullTrace.submit(samples,sizeof(samples)).accepted && fullTrace.diagnostics().backpressure==1);
    Fake tracedUncertain;HdaDellStreamQueue<Fake> uncertainTrace(tracedUncertain,64,4096,1024,0,true);assert(uncertainTrace.start());
    tracedUncertain.moveOnFlush=true;auto uncertain=uncertainTrace.submit(samples,sizeof(samples));
    assert(uncertain.error && uncertain.accepted==sizeof(samples));
    stats=uncertainTrace.diagnostics();assert(stats.publicationFailures==1 && stats.faults==1 && !stats.publishedBytes);
}
