#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_audio_bridge.h"
using namespace brook;
namespace brook {
extern "C" void* kmalloc(uint64_t n){return std::malloc(n);}
extern "C" void kfree(void* p){std::free(p);}
extern "C" void SerialPrintf(const char*,...){}
extern "C" void SerialPuts(const char*){}
extern "C" void KPrintf(const char*,...){}
bool KvmClockReadStableNs(uint64_t* out){*out=1000000000;return true;}
}
struct Lock {std::atomic<bool> held{false};void lock(){while(held.exchange(true)){};}bool tryLock(){return !held.exchange(true);}void unlock(){held.store(false);}};
struct Owner {
    uint8_t cpu[65536]{},device[65536]{};uint64_t ns=1000000000;uint32_t pos=0;bool running=false,held=false;
    uint64_t nowNs(){ns+=1000000;return ns;}
    bool position(uint32_t& p,bool& error,bool& run){pos=(pos+192)%65536;p=pos;error=false;run=running;return true;}
    bool write(uint32_t o,const uint8_t* d,uint32_t n){if(d)std::memcpy(cpu+o,d,n);else std::memset(cpu+o,0,n);return true;}
    bool publish(uint32_t o,uint32_t n){if(n){auto first=o&~63u,last=(o+n+63)&~63u;std::memcpy(device+first,cpu+first,last-first);}return true;}
    bool fence(){return true;}bool start(){running=true;return true;}bool mute(){return true;}
    bool stop(){running=false;return true;}bool retire(){return true;}void quarantine(){held=true;}
};
static Owner owner;static Lock lock;static HdaDellAudioBridge<Owner,Lock> bridge(owner,lock,64,4096);
static unsigned progressCalls=0;
static int play(const void* p,uint32_t n,uint32_t hz,uint8_t c,uint8_t b,bool){return bridge.play(p,n,hz,c,b);}
static void stop(){bridge.stop();}static bool playing(){return bridge.isPlaying();}static uint32_t pending(){return bridge.pending();}
static void service(){bridge.service();}static AudioSubmitReceipt submit(const void* p,uint32_t n){return bridge.submit(p,n);}
static bool progress(AudioDeviceProgress* p){++progressCalls;return bridge.progress(p);}static bool restart(uint64_t e){return bridge.restart(e);}
int main(){
    AudioMixerInit();const AudioDriver driver{"dell_hda_test",play,stop,playing,pending,service,submit,progress,restart};assert(AudioRegister(&driver));
    assert(AudioSetOutputControl(BROOK_AUDIO_MUTED|20) && !AudioUserUnmuteRequested());
    assert(AudioSetOutputControl(20) && !AudioUserUnmuteRequested()); // boot/settings do not authorize output
    assert(AudioSetUserOutputControl(BROOK_AUDIO_MUTED|20) && !AudioUserUnmuteRequested());
    assert(!AudioSetUserOutputControl(101) && !AudioUserUnmuteRequested());
    assert(AudioSetUserOutputControl(20) && AudioUserUnmuteRequested());
    assert(AudioSetUserOutputControl(BROOK_AUDIO_MUTED|20) && !AudioUserUnmuteRequested());
    assert(AudioSetOutputControl(20) && !AudioUserUnmuteRequested());
    assert(AudioSetUserOutputControl(30) && !AudioUserUnmuteRequested()); // not an explicit mute->unmute
    assert(AudioSetUserOutputControl(30,true) && AudioUserUnmuteRequested());
    assert(AudioSetUserOutputControl(BROOK_AUDIO_MUTED|30) && AudioSetUserOutputControl(30) && AudioUserUnmuteRequested());
    const auto a=AudioMixerOpen(),b=AudioMixerOpen();assert(a && b);
    int16_t samples[128];for(auto& s:samples)s=1000;
    assert(AudioSetOutputControl(BROOK_AUDIO_MUTED|20));assert(AudioMixerSubmit(samples,64,a)==64);AudioMixerFlush();
    for(auto byte:owner.device)assert(byte==0);
    assert(AudioSetOutputControl(30));assert(AudioMixerSubmit(samples,64,a)==64 && AudioMixerSubmit(samples,64,b)==64);AudioMixerFlush();
    // Both active streams are mixed and the existing master volume scales the
    // output before the backend sees it: (1000+1000)*30%=600.
    bool found=false;for(unsigned i=0;i+1<sizeof(owner.device);i+=2)
        if(owner.device[i]==0x58 && owner.device[i+1]==0x02){found=true;break;}
    assert(found && !owner.held);
    for(unsigned i=0;i<20;++i){AudioMixerRefresh();AudioMixerTick();}
    AudioStreamProgress progressA{};assert(AudioMixerGetProgress(a,&progressA) && progressA.clocked && !progressA.discontinuity);
    assert(AudioMixerClose(a) && AudioMixerClose(b));AudioStop();assert(!owner.running && !owner.held);
    AudioPrepareShutdown();const auto calls=progressCalls;AudioMixerRefresh();AudioMixerFlush();AudioMixerTick();assert(progressCalls==calls);
}
