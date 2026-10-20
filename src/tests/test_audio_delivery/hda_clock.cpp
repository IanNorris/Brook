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
    g_nativeClock=brook::AudioDmaClock{};g_nativeFaultEvidence={};g_nativeMode=false;g_nativeRingBase=0;
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
    AudioDmaClock reserved{};
    VERIFY(reserved.Accept(16) && reserved.Start(0,100));
    VERIFY(reserved.Observe(4,200,201,false) && reserved.completed==4);
    VERIFY(!reserved.ReserveStart(32) && reserved.submitted==16);
    VERIFY(reserved.ReserveGap(32) && reserved.completed==4 && reserved.Pending()==28);
    VERIFY(!reserved.ReserveGap(16) && !reserved.ReserveGap(34));
    VERIFY(!reserved.ReserveGap(reserved.completed+AudioDmaClock::RingBytes));
    reserved.Fault();VERIFY(!reserved.ReserveGap(36));
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
    VERIFY(g_nativeFaultEvidence.reason && !std::strcmp(g_nativeFaultEvidence.reason,"sample-gap"));
    VERIFY(g_nativeFaultEvidence.epoch==snapshot.epoch && g_nativeFaultEvidence.completed==snapshot.completed);
    VERIFY(g_nativeFaultEvidence.afterNs-g_nativeFaultEvidence.previousNs>=AudioDmaClock::MaxSampleNs);
    const auto retainedFault=g_nativeFaultEvidence;
    nowNs+=1000000;progress();
    VERIFY(g_nativeFaultEvidence.beforeNs==retainedFault.beforeNs && g_nativeFaultEvidence.reason==retainedFault.reason);
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
    VERIFY(!std::strcmp(g_nativeFaultEvidence.reason,"device-status"));
    VERIFY(g_nativeFaultEvidence.status==SD_STS_FIFOE && g_nativeFaultEvidence.completed==proven);
    failSetup=true;VERIFY(!HdaNativeRestart(snapshot.epoch));
    auto failed=progress();VERIFY(failed.epoch==snapshot.epoch && failed.discontinuity && failed.completed==proven);
    failSetup=false;VERIFY(HdaNativeRestart(snapshot.epoch));
    snapshot=progress();VERIFY(snapshot.epoch==2 && !snapshot.completed && !snapshot.submitted && !snapshot.discontinuity);

    initialize();receipt=HdaTrySubmit(tone.data(),8);VERIFY(receipt.accepted==8);prime();
    move(1,1000000);VERIFY(progress().completed==0);move(3,1000000);VERIFY(progress().completed==4);
    nowNs-=10000000;VERIFY(progress().discontinuity);
    VERIFY(!std::strcmp(g_nativeFaultEvidence.reason,"clock-backwards"));
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
    VERIFY(!std::strcmp(g_nativeFaultEvidence.reason,"clock-unavailable"));
    initialize();receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.accepted==4 && !g_streamRunning);
    clockAvailable=false;HdaService();snapshot=progress();
    VERIFY(snapshot.discontinuity && !snapshot.completed && !g_streamRunning);
    initialize();receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.accepted==4);
    --nowNs;snapshot=progress();VERIFY(snapshot.discontinuity && !snapshot.completed);
    initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8192);
    afterCopyHook=[] { move(NATIVE_WRITE_LEAD+4,75000000); };
    receipt=HdaTrySubmit(tone.data(),4);snapshot=progress();
    VERIFY(receipt.accepted==4 && receipt.error==-32 && snapshot.discontinuity && snapshot.completed==8192);
    VERIFY(g_nativeFaultEvidence.publish.failedChecks==PublishDmaCaughtWrite);
    VERIFY(g_nativeFaultEvidence.publish.sampled && g_nativeFaultEvidence.publish.begin==receipt.begin);
    VERIFY(g_nativeFaultEvidence.beforeNs==nowNs && g_nativeFaultEvidence.afterNs==nowNs);
    VERIFY(g_nativeFaultEvidence.rawPosition==HdaTestRead(g_outStreamBase+SD_LPIB,4));
    initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8188);
    afterCopyHook=[] { move(NATIVE_WRITE_LEAD+8,75000000); };
    receipt=HdaTrySubmit(tone.data(),4);snapshot=progress();
    VERIFY(receipt.accepted==4 && receipt.error==-32 && snapshot.discontinuity && snapshot.completed==8188);
    VERIFY(g_nativeFaultEvidence.publish.failedChecks==PublishDmaCaughtWrite);
    VERIFY(g_nativeFaultEvidence.publish.rawCompleted==8188 && g_nativeFaultEvidence.publish.delta==NATIVE_WRITE_LEAD+8);
    VERIFY(g_nativeFaultEvidence.publish.begin==8188+NATIVE_WRITE_LEAD && g_nativeFaultEvidence.publish.accepted==4);
    VERIFY(!g_streamRunning && std::all_of(dma.begin(),dma.end(),[](uint8_t b){return b==0;}));
    const auto publishFault=g_nativeFaultEvidence;
    receipt=HdaTrySubmit(tone.data(),4);VERIFY(receipt.error==-32 && !receipt.accepted);
    VERIFY(g_nativeFaultEvidence.afterNs==publishFault.afterNs &&
           g_nativeFaultEvidence.publish.failedChecks==publishFault.publish.failedChecks);
    // Exact native failure shape: 744 owned bytes left, then a 1024-byte
    // device fetch during a 49-us copy bracket. Preserve the old tail while
    // reserving lead; the next receipt must not be exposed at the old end.
    for(uint32_t previousEnd:{8192u,65000u}) {
        initialize();
        if(previousEnd==8192){receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8192-744);}
        else {
            receipt=HdaTrySubmit(tone.data(),49152);advanceNative(32768);
            receipt=HdaTrySubmit(tone.data(),65000-49152);advanceNative(65000-32768-744);
        }
        const auto priorCompleted=g_nativeClock.completed;
        VERIFY(g_nativeClock.submitted==previousEnd && g_nativeClock.Pending()==744);
        afterCopyHook=[] {move(1024,49000);};
        receipt=HdaTrySubmit(tone.data(),3072);
        VERIFY(receipt.accepted==3072 && !receipt.error && receipt.epoch==1);
        VERIFY(receipt.begin==priorCompleted+NATIVE_WRITE_LEAD && receipt.begin>previousEnd);
        VERIFY(dma[(previousEnd-1)%AUDIO_BUF_SIZE]!=0); // old owned tail intact
        snapshot=progress();VERIFY(!snapshot.discontinuity && snapshot.completed==priorCompleted+1024);
        for(uint64_t at=previousEnd;at<receipt.begin;++at)VERIFY(dma[at%AUDIO_BUF_SIZE]==0);
    }
    // These independent rejections used to look identical in native logs.
    initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8188);
    afterCopyHook=[] {nowNs+=AudioDmaClock::MaxSampleNs;};
    receipt=HdaTrySubmit(tone.data(),4);snapshot=progress();
    VERIFY(receipt.error==-32 && snapshot.discontinuity && snapshot.completed==8188);
    VERIFY(g_nativeFaultEvidence.publish.failedChecks==PublishSampleGap);
    VERIFY(g_nativeFaultEvidence.afterNs-g_nativeFaultEvidence.previousNs==AudioDmaClock::MaxSampleNs);
    initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8188);
    afterCopyHook=[] {--nowNs;};
    receipt=HdaTrySubmit(tone.data(),4);snapshot=progress();
    VERIFY(receipt.error==-32 && snapshot.discontinuity && snapshot.completed==8188);
    VERIFY(g_nativeFaultEvidence.publish.failedChecks==PublishClockOrder);
    initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8188);
    afterCopyHook=[] {clockAvailable=false;};
    receipt=HdaTrySubmit(tone.data(),4);snapshot=progress();
    VERIFY(receipt.error==-32 && snapshot.discontinuity && snapshot.completed==8188);
    VERIFY((g_nativeFaultEvidence.publish.failedChecks&(PublishCopyClock|PublishPositionClock))==
           (PublishCopyClock|PublishPositionClock));
    initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8188);
    afterCopyHook=[] {store(g_outStreamBase+SD_LPIB,4,AUDIO_BUF_SIZE+4);};
    receipt=HdaTrySubmit(tone.data(),4);snapshot=progress();
    VERIFY(receipt.error==-32 && snapshot.discontinuity && snapshot.completed==8188);
    VERIFY(g_nativeFaultEvidence.publish.failedChecks&PublishPosition);
    VERIFY(g_nativeFaultEvidence.rawPosition==AUDIO_BUF_SIZE+4);
    std::printf("HDA_CLOCK %s\n",failures?"FAIL":"PASS");return failures;
}

namespace { bool mixerClockEnabled = false; uint64_t resetDuringCopy = 0; }
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
    const uint64_t nextBegin=g_nativeClock.submitted-8;VERIFY(nextBegin>24);
    advanceNative(28);AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && !pa.completedFrames && pa.queuedFrames==2);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==4 && !pb.queuedFrames);
    // Completing old receipts and silent lead must not credit the new epoch.
    advanceNative(static_cast<uint32_t>(nextBegin-g_nativeClock.completed));AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && !pa.completedFrames && pa.queuedFrames==2);
    advanceNative(8);AudioMixerRefresh();
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
    VERIFY(AudioMixerReset(a));finishPhysical();AudioMixerRefresh();
    VERIFY(!AudioMixerSetCapacity(a,0) && !AudioMixerSetCapacity(a,MIXER_RING_FRAMES+1));
    VERIFY(!AudioMixerSetCapacity(a,4,0) && !AudioMixerSetCapacity(a,4,5));
    VERIFY(AudioMixerSetCapacity(a,4,2));VERIFY(AudioMixerAvailableFrames(a)==4);
    VERIFY(AudioMixerSubmit(first.data(),6,a)==4);AudioMixerFlush();
    VERIFY(!AudioMixerAvailableFrames(a) && !AudioMixerSetCapacity(a,8));
    advanceNative(4);AudioMixerRefresh();VERIFY(AudioMixerAvailableFrames(a)==1);
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.capacityFrames==4 && pa.queuedFrames==3 && pa.writableFrames==2);
    VERIFY(pa.capacityFrames-pa.queuedFrames < pa.writableFrames);
    advanceNative(4);AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.capacityFrames-pa.queuedFrames==pa.writableFrames);
    advanceNative(8);AudioMixerRefresh();VERIFY(AudioMixerAvailableFrames(a)==4);
    VERIFY(AudioMixerReset(a) && AudioMixerAvailableFrames(a)==4);
    VERIFY(AudioMixerClose(a));a=AudioMixerOpen();VERIFY(a && AudioMixerAvailableFrames(a)==MIXER_RING_FRAMES);
    // A reset during the device copy must not credit an old receipt to the
    // replacement stream, even if it queues new data before reconciliation.
    AudioStop();initialize();AudioMixerRefresh();
    VERIFY(AudioMixerSubmit(first.data(),2,a)==2);
    resetDuringCopy=a;
    afterCopyHook=[] {
        VERIFY(AudioMixerReset(resetDuringCopy));
        const int16_t replacement[4]={321,321,321,321};
        VERIFY(AudioMixerSubmit(replacement,2,resetDuringCopy)==2);
        AudioMixerFlush(); // nested owner must leave the in-flight batch alone
    };
    AudioMixerFlush();VERIFY(g_nativeClock.submitted==8);
    advanceNative(8);AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && !pa.completedFrames && pa.queuedFrames==2);
    AudioMixerFlush();advanceNative(g_nativeClock.Pending());AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==2 && !pa.queuedFrames);
    // Probe actual production reserve/copy samples without changing the
    // publication predicate. MMIO advances during the synthetic copy hook.
    AudioStop();initialize();AudioMixerRefresh();
    AudioDeliveryTimingEnable(true);AudioDeliveryTimingReset();
    VERIFY(AudioMixerSubmit(first.data(),2,a)==2);AudioMixerFlush();
    advanceNative(4);AudioMixerRefresh();
    afterCopyHook=[] {move(4,49000);};
    VERIFY(AudioMixerSubmit(second.data(),2,a)==2);AudioMixerFlush();
    auto timing=AudioDeliveryTimingRead();
    VERIFY(timing.observations==2 && timing.reservations==1 && timing.reservedBytes==12284);
    VERIFY(timing.maxCopyNs==49000 && timing.maxAdvanceBytes==4 && timing.minHeadroomBytes==12284);
    VERIFY(!timing.invalidClock && !timing.invalidPosition && !timing.unsafe);
    afterCopyHook=[] {move(20000,49000);};
    VERIFY(AudioMixerSubmit(first.data(),2,a)==2);AudioMixerFlush();
    timing=AudioDeliveryTimingRead();
    VERIFY(timing.observations==3 && timing.unsafe==1 && timing.minHeadroomBytes==0);
    VERIFY(g_nativeClock.discontinuity); // instrumentation never relaxes guard
    AudioDeliveryTimingEnable(false);AudioStop();initialize();AudioMixerRefresh();
    VERIFY(AudioMixerSubmit(first.data(),2,a)==2);AudioMixerFlush();
    VERIFY(AudioDeliveryTimingRead().observations==3); // off means no recording
    // A stale device sample may have a valid raw position but cannot establish
    // headroom. Keep it out of extrema while preserving the publication fault.
    AudioStop();initialize();AudioMixerRefresh();
    AudioDeliveryTimingEnable(true);AudioDeliveryTimingReset();
    VERIFY(AudioMixerSubmit(first.data(),2,a)==2);AudioMixerFlush();
    advanceNative(4);AudioMixerRefresh();
    afterCopyHook=[] {nowNs+=AudioDmaClock::MaxSampleNs;};
    VERIFY(AudioMixerSubmit(second.data(),2,a)==2);AudioMixerFlush();
    timing=AudioDeliveryTimingRead();
    VERIFY(timing.observations==2 && timing.unsafe==1 && timing.invalidPosition==1);
    VERIFY(!timing.runningSamples && !timing.maxAdvanceEvent.hardwareEpoch && !timing.minHeadroomEvent.hardwareEpoch);
    VERIFY(g_nativeClock.discontinuity);
    AudioDeliveryTimingEnable(false);
    VERIFY(AudioMixerClose(a) && AudioMixerClose(b));mixerClockEnabled=false;AudioStop();
    std::printf("MIXER_CLOCK %s\n",failures?"FAIL":"PASS");return failures;
}
