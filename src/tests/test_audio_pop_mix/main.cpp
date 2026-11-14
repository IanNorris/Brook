// A short cue (the volume pop) must be summed over continuously playing audio,
// never replace it: the output is tone + pop (saturated) with no missing or
// delayed tone frames before, during or after the cue.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdio>
#include "../../drivers/intel_hda/hda_dell_audio_bridge.h"
using namespace brook;
static uint64_t g_nowNs=1000000000;
namespace brook {
extern "C" void* kmalloc(uint64_t n){return std::malloc(n);}
extern "C" void kfree(void* p){std::free(p);}
extern "C" void SerialPrintf(const char*,...){}
extern "C" void SerialPuts(const char*){}
extern "C" void KPrintf(const char*,...){}
bool KvmClockReadStableNs(uint64_t* out){*out=g_nowNs;return true;}
}
struct Lock {std::atomic<bool> held{false};void lock(){while(held.exchange(true)){};}bool tryLock(){return !held.exchange(true);}void unlock(){held.store(false);}};
// Device model: consumes 441 frames (1764 bytes) per 10 ms tick and records
// the mixed bytes the queue publishes, in order.
struct Owner {
    uint8_t cpu[65536]{},device[65536]{};uint32_t pos=0;bool running=false,held=false;
    std::vector<int16_t> out;
    uint64_t nowNs(){return g_nowNs;}
    bool position(uint32_t& p,bool& error,bool& run){p=pos;error=false;run=running;return true;}
    bool write(uint32_t o,const uint8_t* d,uint32_t n){
        if(d)std::memcpy(cpu+o,d,n);else std::memset(cpu+o,0,n);
        // Mixed data only; zero fills (completed-area clears and gap padding)
        // are checked through the queue's gap counters instead.
        if(d)for(uint32_t i=0;i+1<n;i+=2)out.push_back(int16_t(uint16_t(d[i])|uint16_t(d[i+1])<<8));return true;}
    bool publish(uint32_t o,uint32_t n){if(n){auto first=o&~63u,last=(o+n+63)&~63u;std::memcpy(device+first,cpu+first,last-first);}return true;}
    bool fence(){return true;}bool start(){running=true;return true;}bool mute(){return true;}
    bool stop(){running=false;return true;}bool retire(){return true;}void quarantine(){held=true;}
};
static Owner owner;static Lock lock;static HdaDellAudioBridge<Owner,Lock> bridge(owner,lock,64,4096,1024,0,true);
static int play(const void* p,uint32_t n,uint32_t hz,uint8_t c,uint8_t b,bool){return bridge.play(p,n,hz,c,b);}
static void stop(){bridge.stop();}static bool playing(){return bridge.isPlaying();}static uint32_t pending(){return bridge.pending();}
static void service(){bridge.service();}static AudioSubmitReceipt submit(const void* p,uint32_t n){return bridge.submit(p,n);}
static bool progress(AudioDeviceProgress* p){return bridge.progress(p);}static bool restart(uint64_t e){return bridge.restart(e);}
int main(){
    AudioMixerInit();const AudioDriver driver{"dell_hda_pop_test",play,stop,playing,pending,service,submit,progress,restart};
    assert(AudioRegister(&driver));assert(AudioSetOutputControl(100));
    const auto tone=AudioMixerOpen();assert(tone);
    constexpr int16_t ToneValue=1000;constexpr uint32_t PopFrames=3540;
    std::vector<int16_t> pop(PopFrames*2);for(uint32_t i=0;i<PopFrames*2;++i)pop[i]=int16_t(i%2?-(int(i)%31000):int(i)%31000);
    pop[2000]=32767;pop[2001]=-32768; // saturation against the tone
    std::vector<int16_t> chunk(441*2,ToneValue);uint64_t toneFrames=0,gapsBeforeCue=0,gapBytesBeforeCue=0;
    for(unsigned tick=0;tick<60;++tick){
        g_nowNs+=10000000;owner.pos=(owner.pos+1764*(owner.running?1:0))%65536;
        assert(AudioMixerSubmit(chunk.data(),441,tone)==441);toneFrames+=441;
        if(tick==20){const auto q=bridge.diagnostics();gapsBeforeCue=q.laterGapEvents;gapBytesBeforeCue=q.laterGapBytes;AudioStreamId cue=AudioMixerOpenCue();assert(cue);assert(AudioMixerSubmit(pop.data(),PopFrames,cue)==PopFrames);}
        AudioMixerFlush();
    }
    // Reconstruct: strip leading silence (startup lead), then expect exactly
    // toneFrames frames of tone, with the pop summed over PopFrames of them.
    const auto& o=owner.out;size_t first=0;while(first<o.size() && o[first]==0)++first;
    assert(first<o.size() && first%2==0);
    const size_t frames=(o.size()-first)/2;
    assert(frames==toneFrames); // nothing missing and no cue-only frames ...
    size_t popAt=SIZE_MAX;
    for(size_t f=0;f<toneFrames;++f){
        const int16_t l=o[first+2*f],r=o[first+2*f+1];
        if(popAt==SIZE_MAX && (l!=ToneValue || r!=ToneValue))popAt=f;
        if(popAt!=SIZE_MAX && f-popAt<PopFrames){
            const auto sat=[](int v){return int16_t(v>32767?32767:v<-32768?-32768:v);};
            if(!(l==sat(ToneValue+pop[2*(f-popAt)]) && r==sat(ToneValue+pop[2*(f-popAt)+1]))){
                std::fprintf(stderr,"mismatch f=%zu popAt=%zu got %d,%d want %d,%d toneFrames=%llu frames=%zu\n",f,popAt,l,r,
                    sat(ToneValue+pop[2*(f-popAt)]),sat(ToneValue+pop[2*(f-popAt)+1]),(unsigned long long)toneFrames,frames);
                for(size_t k=f>4?f-4:0;k<f+4;++k)std::fprintf(stderr," [%zu] %d,%d\n",k,o[first+2*k],o[first+2*k+1]);assert(false);}
        } else assert(l==ToneValue && r==ToneValue); // ... and no gap before/after the cue
    }
    assert(popAt!=SIZE_MAX && popAt+PopFrames<=toneFrames);
    // The cue caused no padding in the hardware queue: no new later gaps from
    // the moment it was submitted to the end (startup lead gaps excepted).
    {const auto q=bridge.diagnostics();assert(q.enabled && q.laterGapEvents==gapsBeforeCue && q.laterGapBytes==gapBytesBeforeCue);}
    // The tone's own accounting is untouched: no discontinuity, every frame it
    // submitted was accepted (all were published, checked above).
    {AudioStreamProgress tp{};assert(AudioMixerGetProgress(tone,&tp) && !tp.discontinuity && tp.submittedFrames==toneFrames);}
    // With no live stream (the tone stopped submitting > 50 ms ago) the cue
    // plays on its own, in full.
    const size_t before=owner.out.size();
    for(unsigned tick=0;tick<10;++tick){g_nowNs+=10000000;owner.pos=(owner.pos+1764)%65536;AudioMixerFlush();}
    {AudioStreamId cue=AudioMixerOpenCue();assert(cue);assert(AudioMixerSubmit(pop.data(),PopFrames,cue)==PopFrames);
     for(unsigned tick=0;tick<20;++tick){g_nowNs+=10000000;owner.pos=(owner.pos+1764)%65536;AudioMixerFlush();}
     assert(owner.out.size()-before==PopFrames*2);
     for(uint32_t i=0;i<PopFrames*2;++i)assert(owner.out[before+i]==pop[i]);
     assert(AudioMixerClose(cue));}
    assert(AudioMixerClose(tone));
    AudioStop();
}
