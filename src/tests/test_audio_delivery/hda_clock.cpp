// Exercise production HDA queue/setup/progress code with MMIO side effects.
// Hardware itself and codec timing still require isolated VM acceptance.
#include <algorithm>
#include <array>
#include <cstdio>
#include "../../drivers/intel_hda/intel_hda_mod.cpp"

namespace {
std::array<uint8_t,4096> registers{};
std::array<uint8_t,AUDIO_BUF_SIZE> dma{};
std::array<BdlEntry,NUM_FRAGMENTS> descriptors{};
uint64_t nowNs = 1000000000;
bool clockAvailable = true, failSetup = false;
int failures = 0;
void (*legacyWaitHook)() = nullptr;
void (*afterCopyHook)() = nullptr;
#define VERIFY(x) do { if(!(x)) { std::fprintf(stderr,"HDA FAIL line %d: %s\n",__LINE__,#x);++failures; } } while(0)
void store(uint32_t offset, uint32_t width, uint32_t value) {
    memcpy(registers.data()+offset,&value,width);
}
void initialize() {
    registers.fill(0);dma.fill(0);nowNs=1000000000;clockAvailable=true;failSetup=false;
    g_nativeClock=brook::AudioDmaClock{};g_nativeMode=false;g_nativeRingBase=0;
    g_nativeLastDataEnd=g_nativePrimedAt=0;
    g_streamRunning=g_streamConfigured=false;g_playLock=g_legacyActive=0;g_legacyEpoch=0;legacyWaitHook=afterCopyHook=nullptr;
    g_submittedBytes=g_playedBytes=0;g_lastLpib=0;
    g_initialized=true;g_dacNid=2;g_outStreamBase=0x80;
    g_audioBuf=dma.data();g_bdl=descriptors.data();
}
void move(uint32_t bytes,uint64_t elapsedNs) {
    nowNs+=elapsedNs;
    uint32_t position=HdaTestRead(g_outStreamBase+SD_LPIB,4);
    store(g_outStreamBase+SD_LPIB,4,(position+bytes)%AUDIO_BUF_SIZE);
}
brook::AudioDeviceProgress progress() {
    brook::AudioDeviceProgress value{};VERIFY(HdaNativeProgress(&value));return value;
}
}
namespace brook {
bool KvmClockAvailable() { return clockAvailable; }
uint64_t KvmClockReadNs() { return nowNs; }
bool KvmClockReadStableNs(uint64_t* result) { if (!clockAvailable) return false; *result=nowNs; return true; }
extern "C" void SchedulerSleepMs(uint32_t ms) {
    if (legacyWaitHook) { auto hook=legacyWaitHook;legacyWaitHook=nullptr;hook(); }
    nowNs+=uint64_t(ms)*1000000;
}
}
uint32_t HdaTestRead(uint32_t offset,uint32_t width) {
    uint32_t value=0;memcpy(&value,registers.data()+offset,width);return value;
}
void HdaTestWrite(uint32_t offset,uint32_t width,uint32_t value) {
    if(offset==g_outStreamBase+SD_STS && width==1) {
        registers[offset]&=~value;return; // RW1C stream error/completion status.
    }
    if(offset==ICS && width==2) {
        if(value&ICS_ICB)store(ICS,2,ICS_IRV); // Synchronous codec reply.
        else store(ICS,2,HdaTestRead(ICS,2)&~(value&ICS_IRV));
        return;
    }
    if(offset==g_outStreamBase+SD_CTL && width==4) {
        // Match QEMU: SRST preserves stale LPIB; RUN's rising edge parses a
        // fresh BDL and resets its position. Clearing on SRST hid restart loss.
        if((value&SD_CTL_SRST) && failSetup)value&=~SD_CTL_SRST;
        if((value&SD_CTL_RUN) && !(HdaTestRead(offset,width)&SD_CTL_RUN))
            store(g_outStreamBase+SD_LPIB,4,0);
    }
    store(offset,width,value);
}
namespace {
void prime() {
    if(g_nativeMode && !g_streamRunning && g_nativeClock.Pending()) {
        nowNs+=NATIVE_PRIME_NS;HdaService();VERIFY(g_streamRunning);
    }
}
void advanceNative(uint32_t bytes) {
    prime();
    while(bytes) {
        uint32_t chunk=std::min(bytes,4096u);
        move(chunk,uint64_t(chunk)*1000000000/176400+1000000);
        progress();bytes-=chunk;
    }
}
void finishPhysical() {
    advanceNative(g_nativeClock.Pending());
    if(g_streamRunning)advanceNative(NATIVE_IDLE_TRAILER);
    VERIFY(!g_streamRunning);
}
}
void HdaTestAfterCopy() { if(afterCopyHook) {auto hook=afterCopyHook;afterCopyHook=nullptr;hook();} }
int RunHdaClockTests() {
    using namespace brook;
    initialize();std::array<int16_t,AUDIO_BUF_SIZE/2> tone{};tone.fill(1234);
    // Consecutive fresh rings must not charge the stale pre-RUN position as a
    // wrap. Verify both native and legacy accounting before any DMA movement.
    for(unsigned i=0;i<3;++i) {
        store(g_outStreamBase+SD_LPIB,4,12348+i*4);
        const uint64_t completed=g_nativeClock.completed;
        auto restarted=HdaTrySubmit(tone.data(),4);
        VERIFY(restarted.accepted==4 && !restarted.error);
        VERIFY(HdaTestRead(g_outStreamBase+SD_LPIB,4)==12348+i*4);
        prime();VERIFY(progress().completed==completed);
        advanceNative(4);VERIFY(progress().completed==completed+4);
        finishPhysical();
    }
    initialize();
    for(unsigned i=0;i<3;++i) {
        store(g_outStreamBase+SD_LPIB,4,12348+i*4);
        VERIFY(HdaPlayPcm(tone.data(),4,44100,2,16,true)==4);
        VERIFY(HdaTestRead(g_outStreamBase+SD_LPIB,4)==12348+i*4);
        HdaService();VERIFY(g_streamRunning && g_playedBytes==0);
        HdaService();VERIFY(g_streamRunning && g_playedBytes==0);
        move(4,1000000);UpdatePlayedBytesLocked();VERIFY(g_playedBytes==4);
        HdaService();VERIFY(!g_streamRunning && !g_playedBytes && !g_submittedBytes);
    }
    initialize();
    auto receipt=HdaTrySubmit(tone.data(),4);
    VERIFY(receipt.epoch==1 && receipt.begin==0 && receipt.accepted==4 && !receipt.error);
    VERIFY(!g_streamRunning);prime();VERIFY(g_streamRunning && !progress().completed);
    VERIFY(!HdaNativeRestart(1));
    advanceNative(4);auto snapshot=progress();
    VERIFY(snapshot.completed==4 && snapshot.submitted==4 && snapshot.running);
    VERIFY(std::all_of(dma.begin(),dma.end(),[](uint8_t b){return b==0;}));
    // Logical application DMA is done; physical output must flush codec data.
    advanceNative(NATIVE_IDLE_TRAILER);snapshot=progress();
    VERIFY(!snapshot.running && snapshot.completed==4+NATIVE_IDLE_TRAILER);
    const uint64_t base=snapshot.completed;
    nowNs+=10000000000ULL;snapshot=progress();VERIFY(snapshot.completed==base && !snapshot.discontinuity);
    receipt=HdaTrySubmit(tone.data(),8192);
    VERIFY(receipt.epoch==1 && receipt.begin==base && receipt.accepted==8192);
    VERIFY(g_nativeRingBase==base);
    advanceNative(4096);snapshot=progress();VERIFY(snapshot.completed==base+4096 && !snapshot.discontinuity);
    VERIFY(std::all_of(dma.begin(),dma.begin()+4096,[](uint8_t b){return b==0;}));VERIFY(dma[4096]!=0);
    nowNs+=AudioDmaClock::MaxSampleNs;snapshot=progress();
    VERIFY(snapshot.discontinuity && snapshot.completed==base+4096);
    receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.error==-32 && !receipt.accepted);
    VERIFY(!HdaNativeRestart(snapshot.epoch+1));VERIFY(HdaNativeRestart(snapshot.epoch));
    receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.epoch==2 && receipt.begin==0 && receipt.accepted==4);
    VERIFY(!HdaNativeRestart(1));VERIFY(HdaPlayPcm(tone.data(),4,44100,2,16,true)==-16);
    finishPhysical();const auto beforeLegacy=progress();
    VERIFY(HdaPlayPcm(tone.data(),4,44100,2,16,true)==4);
    receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.error==-16 && !receipt.accepted);
    HdaService();VERIFY(g_streamRunning);move(4,1000000);HdaService();VERIFY(!g_streamRunning);
    snapshot=progress();VERIFY(snapshot.completed==beforeLegacy.completed);
    receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.begin==beforeLegacy.completed && receipt.accepted==4);

    initialize();receipt=HdaTrySubmit(tone.data(),49152);VERIFY(receipt.accepted==49152);
    receipt=HdaTrySubmit(tone.data(),4);VERIFY(!receipt.accepted && !receipt.error);
    advanceNative(32768);snapshot=progress();VERIFY(snapshot.completed==32768 && !snapshot.discontinuity);
    receipt=HdaTrySubmit(tone.data(),32768);VERIFY(receipt.begin==49152 && receipt.accepted==32768);
    advanceNative(32768);snapshot=progress();VERIFY(snapshot.completed==65536 && !snapshot.discontinuity);
    advanceNative(16384);snapshot=progress();VERIFY(snapshot.completed==81920 && snapshot.running);
    // An idle physical interval must be skipped by a new owned receipt.
    advanceNative(4);receipt=HdaTrySubmit(tone.data(),8192);
    VERIFY(receipt.begin==81924+NATIVE_WRITE_LEAD && receipt.accepted==8192);
    advanceNative(NATIVE_WRITE_LEAD+4);snapshot=progress();VERIFY(snapshot.completed==receipt.begin+4);
    store(g_outStreamBase+SD_STS,1,SD_STS_FIFOE);snapshot=progress();
    const uint64_t proven=snapshot.completed;VERIFY(snapshot.discontinuity);
    failSetup=true;VERIFY(!HdaNativeRestart(snapshot.epoch));
    auto failed=progress();VERIFY(failed.epoch==snapshot.epoch && failed.discontinuity && failed.completed==proven);
    failSetup=false;VERIFY(HdaNativeRestart(snapshot.epoch));
    snapshot=progress();VERIFY(snapshot.epoch==2 && !snapshot.completed && !snapshot.submitted && !snapshot.discontinuity);

    initialize();receipt=HdaTrySubmit(tone.data(),8);VERIFY(receipt.accepted==8);prime();
    move(1,1000000);VERIFY(progress().completed==0);move(3,1000000);VERIFY(progress().completed==4);
    nowNs-=10000000;VERIFY(progress().discontinuity);
    initialize();legacyWaitHook=[] {
        move(49152,280000000);HdaService();
        const int16_t sample[2]={1234,1234};auto attempted=HdaTrySubmit(sample,4);
        VERIFY(attempted.error==-16 && !attempted.accepted && !g_nativeMode);
    };
    VERIFY(HdaPlayPcm(tone.data(),65536,44100,2,16,false)==65536);
    VERIFY(!g_legacyActive && !legacyWaitHook);
    receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.error==-16 && !receipt.accepted);
    move(16384,100000000);HdaService();receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.accepted==4);
    initialize();legacyWaitHook=[] { HdaStop(); };
    VERIFY(HdaPlayPcm(tone.data(),65536,44100,2,16,false)==49152);
    VERIFY(!g_legacyActive && !g_streamRunning);receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.accepted==4);
    initialize();clockAvailable=false;AudioDeviceProgress unavailable{};VERIFY(!HdaNativeProgress(&unavailable));
    receipt=HdaTrySubmit(tone.data(),4);VERIFY(!receipt.accepted && receipt.error==-95);
    VERIFY(HdaPlayPcm(tone.data(),4,44100,2,16,true)==4);
    initialize();receipt=HdaTrySubmit(tone.data(),8);VERIFY(receipt.accepted==8);prime();
    clockAvailable=false;snapshot=progress();VERIFY(snapshot.discontinuity && snapshot.completed==0);
    initialize();receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.accepted==4 && !g_streamRunning);
    clockAvailable=false;HdaService();snapshot=progress();
    VERIFY(snapshot.discontinuity && !snapshot.completed && !g_streamRunning);
    initialize();receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.accepted==4);
    --nowNs;snapshot=progress();VERIFY(snapshot.discontinuity && !snapshot.completed);
    initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8192);
    afterCopyHook=[] { move(NATIVE_WRITE_LEAD+4,75000000); };
    receipt=HdaTrySubmit(tone.data(),4);snapshot=progress();
    VERIFY(receipt.accepted==4 && receipt.error==-32 && snapshot.discontinuity && snapshot.completed==8192);
    initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8188);
    afterCopyHook=[] { move(8,1000000); };
    receipt=HdaTrySubmit(tone.data(),4);snapshot=progress();
    VERIFY(receipt.accepted==4 && receipt.error==-32 && snapshot.discontinuity && snapshot.completed==8188);
    std::printf("HDA_CLOCK %s\n",failures?"FAIL":"PASS");return failures;
}

namespace { bool mixerClockEnabled = false; }
brook::AudioSubmitReceipt TestMixerNativeSubmit(const void* samples,uint32_t bytes) { return HdaTrySubmit(samples,bytes); }
bool TestMixerNativeProgress(brook::AudioDeviceProgress* result) { return mixerClockEnabled && HdaNativeProgress(result); }
bool TestMixerNativeRestart(uint64_t epoch) { return HdaNativeRestart(epoch); }
int RunMixerClockTests() {
    using namespace brook;
    mixerClockEnabled=true;AudioStop();initialize();AudioMixerRefresh();
    auto a=AudioMixerOpen(),b=AudioMixerOpen();VERIFY(a && b);
    std::array<int16_t,12> first{};first.fill(10000);std::array<int16_t,8> second{};second.fill(20000);
    AudioStreamProgress pa{},pb{};
    VERIFY(AudioMixerSubmit(first.data(),6,a)==6);VERIFY(AudioMixerSubmit(second.data(),4,b)==4);AudioMixerFlush();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.clocked && pa.queuedFrames==6 && !pa.completedFrames);
    VERIFY(AudioMixerAvailableFrames(a)==MIXER_RING_FRAMES-6);
    advanceNative(4);AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==1 && pa.queuedFrames==5);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==1 && pb.queuedFrames==3);
    VERIFY(AudioMixerReset(a));VERIFY(AudioMixerSubmit(first.data(),2,a)==2);AudioMixerFlush();
    advanceNative(28);AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==2 && pa.submittedFrames==2 && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==4 && !pb.queuedFrames);
    AudioStop();initialize();AudioMixerRefresh();
    VERIFY(AudioMixerSubmit(first.data(),4,a)==4);VERIFY(AudioMixerSubmit(second.data(),4,b)==4);AudioMixerFlush();
    auto old=a;VERIFY(AudioMixerClose(a));a=AudioMixerOpen();VERIFY(a && a!=old);
    VERIFY(AudioMixerSubmit(first.data(),1,a)==1);AudioMixerFlush();advanceNative(20);AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==1 && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==4 && !pb.queuedFrames);
    AudioStop();initialize();AudioMixerRefresh();
    for(unsigned i=0;i<200;++i) {VERIFY(AudioMixerSubmit(first.data(),1,a)==1);AudioMixerFlush();}
    const uint32_t accepted=static_cast<uint32_t>(g_nativeClock.submitted);VERIFY(accepted>0 && accepted<800);
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.submittedFrames==200 && pa.queuedFrames==200 && !pa.completedFrames);
    advanceNative(accepted);AudioMixerRefresh();
    for(unsigned i=0;i<128;++i)AudioMixerFlush();
    // Unowned lead silence cannot credit application frames.
    advanceNative(NATIVE_WRITE_LEAD);AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==accepted/4);
    advanceNative(g_nativeClock.Pending());AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==200 && !pa.queuedFrames);
    AudioStop();initialize();AudioMixerRefresh();
    VERIFY(AudioMixerSubmit(first.data(),2,a)==2);VERIFY(AudioMixerSubmit(second.data(),4,b)==4);AudioMixerFlush();
    advanceNative(4);AudioMixerRefresh();nowNs+=AudioDmaClock::MaxSampleNs;AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==1 && pa.discontinuity);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==1 && pb.discontinuity);
    VERIFY(!AudioMixerSubmit(first.data(),1,a));VERIFY(AudioMixerReset(a));VERIFY(AudioMixerSubmit(first.data(),2,a)==2);
    AudioMixerFlush();advanceNative(8);AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==2 && !pa.discontinuity && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.discontinuity && pb.completedFrames==1);
    VERIFY(AudioMixerClose(a) && AudioMixerClose(b));mixerClockEnabled=false;AudioStop();
    std::printf("MIXER_CLOCK %s\n",failures?"FAIL":"PASS");return failures;
}
