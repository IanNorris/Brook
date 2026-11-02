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
int main(){
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
}
