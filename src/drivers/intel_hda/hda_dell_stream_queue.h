#pragma once
#include "hda_dell_stream_publish.h"
#include "../../kernel/src/audio_dma_clock.h"

namespace brook {
// Injected streaming core; not yet registered as an AudioDriver. The owner
// serializes all calls and owns the controller and all ring pages. Io methods:
// nowNs(), position(raw,error,running), write(offset,src,n), publish(offset,n),
// fence(), start(), mute(), stop(), quarantine(). write(nullptr) writes silence.
// Before start(), the caller has prepared and published an immutable BDL and
// configured the validated 44.1kHz route with the speaker muted. The physical
// guard and a service deadline must be established before hardware use.
// mute() is bounded best-effort; stop() verifies RUN-clear and disables DMA.
// A missed service deadline can be detected but cannot undo stale audio which
// hardware already fetched; callers must not claim a hard real-time guarantee.
struct HdaDellReceipt {uint64_t epoch=0,begin=0;uint32_t accepted=0;int error=0;};
template<class Io> class HdaDellStreamQueue {
    static constexpr uint32_t Ring=AudioDmaClock::RingBytes;
    Io& io_;AudioDmaClock clock_{};uint64_t cleared_=0,lastMovementNs_=0;uint32_t line_,guard_,maxBurst_;
    bool initialized_=false,failed_=false,closed_=false;
    struct Publisher {
        HdaDellStreamQueue& q;
        bool sample(uint64_t& c){return q.sample(c);}
        bool write(uint32_t o,const uint8_t* p,uint32_t n){return q.io_.write(o,p,n);}
        bool publish(uint32_t o,uint32_t n){return q.io_.publish(o,n);}
        bool fence(){return q.io_.fence();}
    };
    bool fail(){
        if(!failed_){failed_=true;clock_.Fault();io_.mute();io_.stop();io_.quarantine();}
        return false;
    }
    bool sample(uint64_t& raw){
        const uint64_t before=io_.nowNs();uint32_t position=0;bool error=false,running=false;
        if(!before || !io_.position(position,error,running) || !running)return false;
        const uint64_t after=io_.nowNs();
        const uint64_t previous=clock_.RawCompleted();
        if(position>Ring || after<clock_.SampleBeforeNs() ||
           after-clock_.SampleBeforeNs()>=AudioDmaClock::MaxSampleNs)return false;
        const uint32_t normalized=position==Ring?0:position;
        const uint32_t delta=(normalized+Ring-clock_.Position())%Ring;
        const uint64_t elapsed=after-clock_.SampleBeforeNs();
        const uint64_t allowed=elapsed*AudioDmaClock::BytesPerSecond/1000000000ULL+maxBurst_;
        if(delta>allowed)return false; // impossible jump or unobserved lap
        if(!clock_.Observe(normalized,before,after,error))return false;
        raw=clock_.RawCompleted();
        if(raw!=previous)lastMovementNs_=after;
        else if(after<lastMovementNs_ || after-lastMovementNs_>=150000000ULL)return false;
        return true;
    }
    bool reclaim(uint64_t cursor){
        if(cursor<guard_)return true;
        const uint64_t end=(cursor-guard_)&~uint64_t(line_-1);
        if(end<=cleared_)return true;
        if(end>~uint64_t(0)-Ring || cleared_>~uint64_t(0)-Ring)return false;
        uint64_t first=cleared_+Ring,last=end+Ring;
        if(first<clock_.submitted)first=clock_.submitted;
        if(first<last){
            const uint64_t count=last-first;
            if(count>Ring || (first&3) || (count&3))return false;
            Publisher p{*this};
            if(HdaStreamPublish(p,0,first,nullptr,uint32_t(count),Ring,line_,guard_)!=HdaStreamPublishResult::Published)return false;
        }
        cleared_=end;return true;
    }
public:
    HdaDellStreamQueue(Io& io,uint32_t line,uint32_t guard,uint32_t maxBurst=1024):io_(io),line_(line),guard_(guard),maxBurst_(maxBurst){}
    HdaDellStreamQueue(const HdaDellStreamQueue&)=delete;
    HdaDellStreamQueue& operator=(const HdaDellStreamQueue&)=delete;
    HdaDellStreamQueue(HdaDellStreamQueue&&)=delete;
    HdaDellStreamQueue& operator=(HdaDellStreamQueue&&)=delete;
    bool start(){
        if(initialized_ || failed_ || closed_ || line_<16 || line_>4096 || (line_&(line_-1)) ||
           (4096%line_) || !maxBurst_ || maxBurst_>guard_ || guard_-maxBurst_<line_ || guard_>=Ring/4)return false;
        // Initial zeroes must be visible to DMA before RUN; no partial
        // initialization can register or expose an active driver.
        if(!io_.write(0,nullptr,Ring) || !io_.publish(0,Ring) || !io_.fence())return fail();
        const uint64_t now=io_.nowNs();if(!clock_.Start(0,now))return fail();lastMovementNs_=now;
        if(!io_.start())return fail();initialized_=true;return true;
    }
    bool service(){
        if(!initialized_ || failed_)return false;
        uint64_t cursor=0;if(!sample(cursor) || !reclaim(cursor))return fail();return true;
    }
    // RUN/BME stop is not descriptor retirement. The owner must still reset
    // and retire the stream before freeing pages or relinquishing ownership.
    bool shutdown(){
        if(!initialized_ || failed_ || closed_)return false;
        const bool muted=io_.mute(),stopped=io_.stop();
        if(!muted || !stopped){failed_=true;clock_.Fault();io_.quarantine();return false;}
        initialized_=false;closed_=true;
        if(clock_.Pending())clock_.Fault(); // outstanding receipts were cancelled
        else clock_.running=false;
        return true;
    }
    HdaDellReceipt submit(const void* samples,uint32_t bytes){
        HdaDellReceipt r{clock_.epoch,clock_.submitted,0,0};
        if(!samples || !bytes || (bytes&3)){r.error=-22;return r;}
        if(!initialized_ || failed_){r.error=-32;return r;}
        if(!service()){r.error=-32;return r;}
        const uint64_t cursor=clock_.RawCompleted();
        if(cursor>~uint64_t(0)-guard_-maxBurst_-2*line_ || cleared_>~uint64_t(0)-Ring){fail();r.error=-32;return r;}
        // Leave a whole expected fetch burst between this sample and the
        // publication guard; the publisher samples again before copying.
        const uint64_t minBegin=(cursor+guard_+maxBurst_+2*line_-1)&~uint64_t(line_-1);
        uint64_t begin=clock_.submitted;if(begin<minBegin)begin=minBegin;
        const uint64_t clearedThrough=cleared_+Ring;
        if(cursor>~uint64_t(0)-Ring){fail();r.error=-32;return r;}
        uint64_t limit=cursor+Ring-guard_-line_;if(limit>clearedThrough)limit=clearedThrough;
        if(begin>=limit){r.begin=begin;return r;} // ordinary backpressure
        uint64_t available=(limit-begin)&~uint64_t(3);if(bytes>available)bytes=uint32_t(available);
        if(!bytes){r.begin=begin;return r;}
        if(begin!=clock_.submitted && !clock_.ReserveGap(begin)){fail();r.error=-32;return r;}
        r.begin=begin;Publisher p{*this};
        // Reserve before writes so an uncertain exposure stays represented in
        // the terminal epoch even if DMA passes it during publication.
        if(!clock_.Accept(bytes)){fail();r.error=-32;return r;}
        const auto published=HdaStreamPublish(p,0,begin,samples,bytes,Ring,line_,guard_);
        if(published!=HdaStreamPublishResult::Published){
            // An uncertain publication may already have reached the device.
            // Callers must not transparently retry it as if never exposed.
            if(published==HdaStreamPublishResult::Uncertain)r.accepted=bytes;
            fail();r.error=-32;return r;}
        r.accepted=bytes;return r;
    }
    const AudioDmaClock& progress()const{return clock_;}
    bool failed()const{return failed_;}
};
}
