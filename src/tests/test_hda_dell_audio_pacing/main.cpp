// Real mixer (audio.cpp) + Dell HDA bridge/queue under a simulated 44.1 kHz DMA
// clock, a 12.8 ms mixer period (observed on the Dell) and 10 ms client callbacks.
//
// Reproduces the gen 371 Chromium symptom: a 1920-frame client FIFO whose
// hardware-resident copy is charged against it cannot keep the Dell queue's
// ~5.2 KiB publication lead, so the queue pads silence and the source drains
// slowly. With the downstream allowance and a downstream-aware client (the
// BROOK_OSS_DOWNSTREAM ALSA plugin), the source keeps real time with no gaps.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include "../../drivers/intel_hda/hda_dell_audio_bridge.h"
using namespace brook;
namespace brook {
extern "C" void* kmalloc(uint64_t n){return std::malloc(n);}
extern "C" void kfree(void* p){std::free(p);}
extern "C" void SerialPrintf(const char*,...){}
extern "C" void SerialPuts(const char*){}
extern "C" void KPrintf(const char*,...){}
static uint64_t g_now=1000000000ULL;
bool KvmClockReadStableNs(uint64_t* out){*out=g_now;return true;}
}
struct Lock {bool held=false;void lock(){assert(!held);held=true;}bool tryLock(){if(held)return false;held=true;return true;}void unlock(){held=false;}};
struct Owner {
    uint8_t cpu[65536]{},device[65536]{};uint64_t startNs=0;bool running=false,held=false;
    uint64_t nowNs(){return ++g_now;}
    bool position(uint32_t& p,bool& error,bool& run){
        error=false;run=running;if(!running){p=0;return true;}
        const uint64_t bytes=(g_now-startNs)*176400ULL/1000000000ULL;
        p=uint32_t((bytes&~uint64_t(63))%65536);return true;
    }
    bool write(uint32_t o,const uint8_t* d,uint32_t n){if(d)std::memcpy(cpu+o,d,n);else std::memset(cpu+o,0,n);return true;}
    bool publish(uint32_t o,uint32_t n){if(n){auto first=o&~63u,last=(o+n+63)&~63u;std::memcpy(device+first,cpu+first,last-first);}return true;}
    bool fence(){return true;}bool start(){running=true;startNs=g_now;return true;}bool mute(){return true;}
    bool stop(){running=false;return true;}bool retire(){return true;}void quarantine(){held=true;}
};
// Dell runtime geometry: 64-byte line, 4096-byte guard, 1024-byte burst, 20 us clock uncertainty.
static Owner owner;static Lock lock;static HdaDellAudioBridge<Owner,Lock> bridge(owner,lock,64,4096,1024,20000,true);
static int play(const void* p,uint32_t n,uint32_t hz,uint8_t c,uint8_t b,bool){return bridge.play(p,n,hz,c,b);}
static void stop(){bridge.stop();}static bool playing(){return bridge.isPlaying();}static uint32_t pending(){return bridge.pending();}
static void service(){bridge.service();}static AudioSubmitReceipt submit(const void* p,uint32_t n){return bridge.submit(p,n);}
static bool progress(AudioDeviceProgress* p){return bridge.progress(p);}static bool restart(uint64_t e){return bridge.restart(e);}

enum class Client {Stock,Downstream};
struct Result {double sourceRate,gapFraction;uint64_t gapEvents;};
// Stock OSS plugin: outstanding source (delay = software + in flight) is
// bounded by its logical ring. Downstream-aware plugin: writes the kernel's
// reported available capacity (BROOK_DSP_GETQUEUE).
static Result Run(bool staging,Client client,uint64_t wakeNs,uint32_t period,uint32_t Capacity){
    AudioMixerInit();
    const AudioDriver driver{"dell_hda",play,stop,playing,pending,service,submit,progress,restart};
    assert(AudioRegister(&driver));assert(AudioSetOutputControl(30));
    if(staging){assert(AudioMixerStagingSetFrameLimit(4096));AudioMixerStagingEnable(true);}
    const auto stream=AudioMixerOpen();assert(stream);
    AudioMixerRefresh(); // native clock admission, as the first ioctl does
    constexpr uint32_t Fragment=64;
    assert(AudioMixerSetCapacity(stream,Capacity,Fragment));
    static int16_t samples[2*16384];for(auto& s:samples)s=1000;
    constexpr uint64_t StepNs=1000000,TickNs=12800000,PrimeNs=500000000,RunNs=5500000000ULL;
    const uint64_t origin=g_now;uint64_t nextTick=origin+TickNs,nextWake=origin;
    HdaDellQueueDiagnostics primed{};uint64_t primedFrames=0;bool measured=false;
    for(uint64_t t=origin;t<origin+RunNs;t+=StepNs){
        if(g_now<t)g_now=t;
        if(!measured && t>=origin+PrimeNs){
            primed=bridge.diagnostics();AudioStreamProgress p{};assert(AudioMixerGetProgress(stream,&p));
            primedFrames=p.completedFrames;measured=true;
        }
        if(t>=nextTick){AudioMixerTick();nextTick+=TickNs;}
        if(t<nextWake)continue;
        nextWake+=wakeNs;
        AudioMixerRefresh();
        AudioStreamProgress p{};assert(AudioMixerGetProgress(stream,&p) && p.clocked && !p.discontinuity);
        uint32_t space=client==Client::Stock?(p.queuedFrames<Capacity?Capacity-p.queuedFrames:0):p.availableFrames;
        space-=space%period;
        if(space)assert(AudioMixerSubmit(samples,space,stream)==space);
    }
    const auto end=bridge.diagnostics();AudioStreamProgress p{};assert(AudioMixerGetProgress(stream,&p));
    const double seconds=double(RunNs-PrimeNs)/1e9;
    const double published=double(end.publishedBytes-primed.publishedBytes),gaps=double(end.laterGapBytes-primed.laterGapBytes);
    assert(!owner.held && end.faults==0);
    return {double(p.completedFrames-primedFrames)/seconds,gaps/(published+gaps),end.laterGapEvents-primed.laterGapEvents};
}
// Each scenario needs a fresh mixer and bridge, so run it in a child process.
static Result Scenario(bool staging,Client client,uint32_t capacity){
    constexpr uint64_t wakeNs=10000000;constexpr uint32_t period=441; // 10 ms audio callbacks
    int fds[2];assert(pipe(fds)==0);
    const pid_t pid=fork();assert(pid>=0);
    if(!pid){close(fds[0]);const auto r=Run(staging,client,wakeNs,period,capacity);assert(write(fds[1],&r,sizeof r)==sizeof r);_exit(0);}
    close(fds[1]);Result r{};const auto n=read(fds[0],&r,sizeof r);close(fds[0]);
    int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0 && n==sizeof r);
    return r;
}
int main(){
    // Gen 371 geometries: Chromium negotiates 1920 frames / 64-frame fragments,
    // Firefox 4416 frames. Both use the stock ALSA OSS plugin today.
    const auto chrome=Scenario(false,Client::Stock,1920);
    const auto firefox=Scenario(false,Client::Stock,4416);
    const auto chromeStaged=Scenario(true,Client::Stock,1920);
    const auto fixed=Scenario(true,Client::Downstream,1920);
    const auto print=[](const char* name,const Result& r){
        std::printf("%-28s %6.0f frames/s gaps %5.1f%% (%llu events)\n",name,r.sourceRate,100*r.gapFraction,(unsigned long long)r.gapEvents);};
    print("chrome stock 1920",chrome);print("firefox stock 4416",firefox);
    print("chrome stock 1920 + staging",chromeStaged);print("chrome downstream + staging",fixed);
    std::fflush(stdout);
    // The gen 371 failure: silence padding and a slow source (Dell: 34.5k/s, 22%).
    assert(chrome.gapEvents>0 && chrome.gapFraction>0.10 && chrome.sourceRate<0.90*44100);
    // Firefox's larger FIFO covers the Dell lead plus one callback: the healthy control.
    assert(firefox.gapEvents==0 && firefox.sourceRate>0.995*44100);
    // Kernel staging alone cannot help a client whose plugin bounds delay by its own ring.
    assert(chromeStaged.gapFraction>0.10);
    // The fix: allowance plus downstream-aware plugin keeps real time with no padding.
    assert(fixed.gapEvents==0 && fixed.sourceRate>0.995*44100 && fixed.sourceRate<1.005*44100);
}
