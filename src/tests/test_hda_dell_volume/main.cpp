// Master volume split for the Dell speaker: DAC step + Q15 software residual.
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../drivers/intel_hda/hda_dell_audio_bridge.h"
#include "../../drivers/intel_hda/hda_dell_volume.h"
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
// Records the codec-visible order of gain and pin operations.
struct Owner {
    uint8_t cpu[65536]{},device[65536]{};uint64_t ns=1000000000;uint32_t pos=0;
    bool running=false,held=false,pinMuted=true,failGain=false;uint8_t gain=HdaDellDacInitialStep;
    std::vector<int> ops; // >=0: DAC step written; -1 unmute; -2 mute
    uint64_t nowNs(){ns+=1000000;return ns;}
    bool position(uint32_t& p,bool& error,bool& run){pos=(pos+192)%65536;p=pos;error=false;run=running;return true;}
    bool write(uint32_t o,const uint8_t* d,uint32_t n){if(d)std::memcpy(cpu+o,d,n);else std::memset(cpu+o,0,n);return true;}
    bool publish(uint32_t o,uint32_t n){if(n){auto first=o&~63u,last=(o+n+63)&~63u;std::memcpy(device+first,cpu+first,last-first);}return true;}
    bool fence(){return true;}bool start(){running=true;return true;}
    bool mute(){ops.push_back(-2);pinMuted=true;return true;}
    bool unmute(){ops.push_back(-1);pinMuted=false;return true;}
    bool setOutputGain(uint8_t step){assert(step<=HdaDellDacMaxStep);if(failGain)return false;if(step!=gain)ops.push_back(step);gain=step;return true;}
    bool stop(){running=false;return true;}bool retire(){return true;}void quarantine(){held=true;}
    bool retireRetained(){return true;}bool prepareRetained(){return true;}
};
static_assert(HdaDellAudioBridge<Owner,Lock,true,true>::HardwareVolume,"gain-capable owner drives the DAC");
struct PlainOwner : Owner {};
int main(){
    // 1. Tables equal the documented formula (recomputed with libm).
    for(unsigned v=1;v<=100;++v){
        const unsigned down=unsigned(std::floor(-40.0*std::log10(v/100.0)+1e-9));
        assert(HdaDellVolumeStepTable[v]==127-down);
        const long q=std::lround(32768.0*(v/100.0)/std::pow(10.0,-double(down)/40.0));
        assert(HdaDellVolumeResidualQ15[v]==(q>32768?32768:q));
        const auto s=HdaDellVolumeFor(v);
        assert(s.dacStep<=HdaDellDacMaxStep && s.residualQ15<=32768 && s.residualQ15>=30900);
        // Total gain tracks the linear-percent control within 0.01 dB.
        const double total=std::pow(10.0,(int(s.dacStep)-127)*0.5/20.0)*s.residualQ15/32768.0;
        assert(std::fabs(20*std::log10(total)-20*std::log10(v/100.0))<0.01);
        if(v>1){const auto p=HdaDellVolumeFor(v-1);
            const double prev=std::pow(10.0,(int(p.dacStep)-127)*0.5/20.0)*p.residualQ15/32768.0;assert(total>prev);}
    }
    assert(HdaDellVolumeFor(100).dacStep==127 && HdaDellVolumeFor(100).residualQ15==32768);
    assert(HdaDellVolumeFor(250).dacStep==127 && HdaDellVolumeFor(0).residualQ15==0);
    // Old fixed DAC step 80 is -23.5 dB; equal loudness now sits near 7%.
    assert((80-127)*0.5==-23.5 && HdaDellVolumeFor(7).dacStep==81);
    // Q15 residual never amplifies and saturates.
    assert(brook_audio_scale_q15(32767,100,32768)==32767 && brook_audio_scale_q15(-40000,100,32768)==-32768);
    assert(brook_audio_scale_q15(1000,BROOK_AUDIO_MUTED|100,32768)==0 && brook_audio_scale_q15(2000,30,31086)==1897);

    // 2. Bridge: the DAC step is written before the pin unmutes, follows each
    // volume change while audible, and is untouched while muted.
    {Owner o;Lock l;HdaDellAudioBridge<Owner,Lock,true,true> b(o,l,64,4096);
     uint8_t samples[256];std::memset(samples,0x11,sizeof(samples));assert(b.submit(samples,sizeof(samples)).accepted);
     HdaDellOutputRequest req{true,50};
     assert(b.reconcileOutput([&]{return req;}));
     assert(o.ops.size()==2 && o.ops[0]==HdaDellVolumeFor(50).dacStep && o.ops[1]==-1 && !o.pinMuted);
     req.control=100;assert(b.reconcileOutput([&]{return req;}) && o.ops.back()==127 && !o.pinMuted);
     req.control=7;assert(b.reconcileOutput([&]{return req;}) && o.ops.back()==81);
     const auto n=o.ops.size();assert(b.reconcileOutput([&]{return req;}) && o.ops.size()==n); // no redundant verbs
     req.control=BROOK_AUDIO_MUTED|7;assert(b.reconcileOutput([&]{return req;}) && o.ops.back()==-2 && o.pinMuted);
     req.control=BROOK_AUDIO_MUTED|100;const auto m=o.ops.size();assert(b.reconcileOutput([&]{return req;}) && o.ops.size()==m);
     req.control=60;assert(b.reconcileOutput([&]{return req;}));
     assert(o.ops[m]==HdaDellVolumeFor(60).dacStep && o.ops[m+1]==-1);
     // Disabled at runtime: the DAC is left alone (old fixed-gain behaviour).
     req.control=100;const auto k=o.ops.size();assert(b.reconcileOutput([&]{return req;},false) && o.ops.size()==k);
     // A failed gain write closes the bridge like a failed mute.
     o.failGain=true;req.control=20;assert(!b.reconcileOutput([&]{return req;}));
     assert(!b.reconcileOutput([&]{return req;}));}
    {Owner o;Lock l;HdaDellAudioBridge<Owner,Lock,true,true> b(o,l,64,4096);
     uint8_t samples[256]{};assert(b.submit(samples,sizeof(samples)).accepted);
     o.failGain=true;HdaDellOutputRequest req{true,50};assert(!b.reconcileOutput([&]{return req;}) && o.pinMuted);
     for(int op:o.ops)assert(op!=-1);} // never unmuted with an uncertain gain

    // 3. Mixer: with a residual table only the residual is applied.
    {static Owner o;static Lock l;static HdaDellAudioBridge<Owner,Lock> b(o,l,64,4096);
     AudioMixerInit();
     static const AudioDriver driver{"dell_hda_volume_test",
        [](const void* p,uint32_t n,uint32_t hz,uint8_t c,uint8_t bits,bool){return b.play(p,n,hz,c,bits);},
        []{b.stop();},[]{return b.isPlaying();},[]{return b.pending();},[]{b.service();},
        [](const void* p,uint32_t n){return b.submit(p,n);},[](AudioDeviceProgress* p){return b.progress(p);},
        [](uint64_t e){return b.restart(e);}};
     assert(AudioRegister(&driver));
     AudioSetHardwareVolumeResidual(HdaDellVolumeResidualQ15);assert(AudioHardwareVolumeResidual()==HdaDellVolumeResidualQ15);
     assert(AudioSetOutputControl(30));
     const auto a=AudioMixerOpen(),c=AudioMixerOpen();assert(a && c);
     int16_t s[128];for(auto& x:s)x=1000;
     assert(AudioMixerSubmit(s,64,a)==64 && AudioMixerSubmit(s,64,c)==64);AudioMixerFlush();
     // (1000+1000)*31086/32768 = 1897 = 0x0769, not the software 30% (600).
     bool found=false;for(unsigned i=0;i+1<sizeof(o.device);i+=2)if(o.device[i]==0x69 && o.device[i+1]==0x07){found=true;break;}
     assert(found);
     AudioSetHardwareVolumeResidual(nullptr);assert(!AudioHardwareVolumeResidual());
     assert(AudioMixerClose(a) && AudioMixerClose(c));AudioStop();}
}
