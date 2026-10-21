// Exercise production HDA queue/setup/progress code with MMIO side effects.
// Hardware itself and codec timing still require isolated VM acceptance.
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>
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
void (*beforeSubmitHook)() = nullptr;
#define VERIFY(x) do { if(!(x)) { std::fprintf(stderr,"HDA FAIL line %d: %s\n",__LINE__,#x);++failures; } } while(0)
void store(uint32_t offset, uint32_t width, uint32_t value) {
    memcpy(registers.data()+offset,&value,width);
}
void initialize() {
    registers.fill(0);dma.fill(0);nowNs=1000000000;clockAvailable=true;failSetup=false;
    g_nativeClock=brook::AudioDmaClock{};g_nativeFaultEvidence={};g_nativeMode=false;g_nativeRingBase=0;
    g_nativeLastDataEnd=g_nativePrimedAt=0;
    g_streamRunning=g_streamConfigured=false;g_playLock=g_legacyActive=0;g_legacyEpoch=0;legacyWaitHook=afterCopyHook=beforeSubmitHook=nullptr;
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
    // The reservation trigger is strict, with no gap when the full lead is
    // already owned. Exercise this using the selected production lead so the
    // same safety checks can validate an isolated alternative lead.
    for(uint32_t pending:{NATIVE_WRITE_LEAD-4,NATIVE_WRITE_LEAD,NATIVE_WRITE_LEAD+4}) {
        initialize();receipt=HdaTrySubmit(tone.data(),49152);
        VERIFY(receipt.accepted==49152);
        advanceNative(49152-pending);
        const auto completed=g_nativeClock.completed;
        receipt=HdaTrySubmit(tone.data(),4);
        const uint64_t expected=pending<NATIVE_WRITE_LEAD?completed+NATIVE_WRITE_LEAD:49152;
        VERIFY(!receipt.error && receipt.accepted==4 && receipt.begin==expected);
        VERIFY(g_nativeClock.completed==completed && dma[49151]!=0);
        for(uint64_t at=49152;at<expected;++at)VERIFY(dma[at%AUDIO_BUF_SIZE]==0);
    }
    // Completion is frame-aligned, but the raw DMA position is not. Fetching
    // exactly up to the new receipt is accepted; one byte past it is detected
    // and faults without crediting those exposed application bytes.
    for(uint32_t rawOffset:{1u,2u,3u})for(bool beyond:{false,true}) {
        initialize();receipt=HdaTrySubmit(tone.data(),8192);advanceNative(8192-744);
        move(rawOffset,1000);progress();
        const auto completed=g_nativeClock.completed;
        static uint32_t fetchBytes;
        fetchBytes=NATIVE_WRITE_LEAD-rawOffset+(beyond?1:0);
        afterCopyHook=[] {move(fetchBytes,49000);};
        receipt=HdaTrySubmit(tone.data(),4);
        VERIFY(receipt.begin==completed+NATIVE_WRITE_LEAD && receipt.accepted==4);
        VERIFY(receipt.error==(beyond?-32:0));
        VERIFY(g_nativeClock.completed==completed && g_nativeClock.discontinuity==beyond);
        if(beyond)VERIFY(g_nativeFaultEvidence.publish.failedChecks==PublishDmaCaughtWrite);
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

namespace { bool mixerClockEnabled = false; uint64_t resetDuringCopy = 0, mixerProgressDelayNs = 0; }
brook::AudioSubmitReceipt TestMixerNativeSubmit(const void* samples,uint32_t bytes) {
    if(beforeSubmitHook){auto hook=beforeSubmitHook;beforeSubmitHook=nullptr;hook();}
    return HdaTrySubmit(samples,bytes);
}
bool TestMixerNativeProgress(brook::AudioDeviceProgress* result) {
    const bool valid=mixerClockEnabled && HdaNativeProgress(result);
    nowNs+=mixerProgressDelayNs;return valid;
}
bool TestMixerNativeRestart(uint64_t epoch) { return HdaNativeRestart(epoch); }
namespace { unsigned nativeServiceCalls = 0; }
void TestMixerNativeService() { if (mixerClockEnabled) { ++nativeServiceCalls; HdaService(); } }

namespace {
// Drive the production mixer and HDA bodies. An owned 16-KiB device queue
// leaves enough lead for the explicit 12-KiB + 10-ms experiment. Neither
// producer has software samples after priming, but both still own DMA frames.
void coalesceSetup(brook::AudioStreamId a, brook::AudioStreamId b) {
    using namespace brook;
    AudioMixerCoalesceEnable(false);AudioMixerSharedPrefixEnable(false);
    AudioStop(); initialize(); AudioMixerRefresh();
    VERIFY(AudioMixerSetEnabled(a,true) && AudioMixerSetEnabled(b,true));
    std::vector<int16_t> left(4096*2), right(4096*2);
    for(unsigned i=0;i<4096;++i) {left[i*2]=1000;right[i*2+1]=2000;}
    VERIFY(AudioMixerSubmit(left.data(),4096,a)==4096);
    VERIFY(AudioMixerSubmit(right.data(),4096,b)==4096);
    for(unsigned i=0;i<3;++i)AudioMixerFlush();
    VERIFY(g_nativeClock.submitted==16384 && !g_nativeClock.completed);
    prime(); AudioMixerRefresh(); AudioMixerCoalesceReset();
}
void queueChannel(brook::AudioStreamId id, bool right, uint32_t frames=1024) {
    std::vector<int16_t> samples(frames*2);
    for(unsigned i=0;i<frames;++i)samples[i*2+(right?1:0)]=right?2000:1000;
    VERIFY(brook::AudioMixerSubmit(samples.data(),frames,id)==frames);
}
void verifyDma(uint32_t byteOffset,uint32_t frames,bool right) {
    for(unsigned i=0;i<frames;++i) {
        int16_t pair[2]{};memcpy(pair,dma.data()+byteOffset+i*4,4);
        VERIFY(pair[0]==1000 && pair[1]==(right?2000:0));
    }
}
const brook::AudioMixContributionRow* contributionRow(const brook::AudioMixContributionSnapshot& s,
                                                     uint64_t id,uint64_t epoch) {
    for(unsigned i=0;i<s.count;++i)if(s.rows[i].identity==id && s.rows[i].epoch==epoch)return &s.rows[i];
    return nullptr;
}
void contributionConservation(const brook::AudioMixContributionSnapshot& s) {
    VERIFY(!s.invalid && !s.saturated && !s.omittedObservations);
    for(unsigned i=0;i<s.count;++i){
        const auto& r=s.rows[i];uint64_t software=0,flight=0;
        for(auto n:r.missingSoftwareBins)software+=n;
        for(auto n:r.missingInFlightBins)flight+=n;
        VERIFY(r.acceptedOutputFrames==r.contributedFrames+r.missingEmptyFrames+r.missingShortFrames);
        VERIFY(software==r.missingEmptyFrames+r.missingShortFrames && flight==software);
    }
}
void contributionTests(brook::AudioStreamId a,brook::AudioStreamId& b) {
    using namespace brook;
    VERIFY(!(AudioMixerContributionsRead().epoch&1));
    coalesceSetup(a,b);AudioMixerContributionsEnable(true);AudioMixerContributionsReset();
    AudioStreamProgress pa{},pb{};VERIFY(AudioMixerGetProgress(a,&pa) && AudioMixerGetProgress(b,&pb));
    queueChannel(a,false);queueChannel(b,true,128);AudioMixerFlush();
    auto s=AudioMixerContributionsRead();auto* r=contributionRow(s,b,pb.epoch);
    VERIFY(s.outputFrames==1024 && s.observations==1 && s.count==2);
    VERIFY(r && r->contributedFrames==128 && r->missingShortFrames==896 && !r->missingEmptyFrames);
    VERIFY(r && r->missingSoftwareBins[1]==896 && r->missingInFlightBins[1]==896);
    contributionConservation(s);verifyDma(16384,128,true);verifyDma(16384+512,896,false);

    // A drained-open producer is observable, not proof of current app demand.
    coalesceSetup(a,b);finishPhysical();AudioMixerRefresh();AudioMixerContributionsReset();
    VERIFY(AudioMixerGetProgress(b,&pb));queueChannel(a,false);AudioMixerFlush();
    s=AudioMixerContributionsRead();r=contributionRow(s,b,pb.epoch);
    VERIFY(r && r->missingEmptyFrames==1024 && r->missingInFlightBins[0]==1024);
    contributionConservation(s);

    coalesceSetup(a,b);AudioMixerContributionsReset();VERIFY(AudioMixerSetEnabled(b,false));
    auto unused=AudioMixerOpen();VERIFY(unused);queueChannel(a,false);AudioMixerFlush();
    s=AudioMixerContributionsRead();VERIFY(s.count==1 && s.rows[0].identity==a && s.outputFrames==1024);
    VERIFY(AudioMixerClose(unused));contributionConservation(s);

    // Only the accepted prefix counts. Full-ring retries contribute nothing;
    // the later accepted suffix is rebuilt and sees B's now-empty software FIFO.
    coalesceSetup(a,b);AudioMixerContributionsReset();VERIFY(AudioMixerGetProgress(b,&pb));
    std::array<int16_t,16384> silence{};
    VERIFY(HdaTrySubmit(silence.data(),31744).accepted==31744);
    queueChannel(a,false);queueChannel(b,true,128);AudioMixerFlush();
    s=AudioMixerContributionsRead();r=contributionRow(s,b,pb.epoch);
    VERIFY(s.outputFrames==256 && r && r->contributedFrames==128 && r->missingShortFrames==128);
    AudioMixerFlush();VERIFY(AudioMixerContributionsRead().outputFrames==256);
    advanceNative(4096);AudioMixerFlush();s=AudioMixerContributionsRead();r=contributionRow(s,b,pb.epoch);
    VERIFY(s.outputFrames==1024 && s.observations==2 && r && r->missingEmptyFrames==768);
    contributionConservation(s);

    // A refill during copying cannot retroactively alter the prepared mix.
    coalesceSetup(a,b);AudioMixerContributionsReset();VERIFY(AudioMixerGetProgress(b,&pb));
    queueChannel(a,false);resetDuringCopy=b;
    afterCopyHook=[] {queueChannel(resetDuringCopy,true);};AudioMixerFlush();
    s=AudioMixerContributionsRead();r=contributionRow(s,b,pb.epoch);
    VERIFY(r && r->missingEmptyFrames==1024 && !r->contributedFrames);
    contributionConservation(s);verifyDma(16384,1024,false);

    // Epoch reset/close/reuse after preparation changes completion ownership,
    // but the accepted PCM retains the old identity and epoch for this diagnostic.
    for(unsigned action=0;action<3;++action){
        coalesceSetup(a,b);AudioMixerContributionsReset();VERIFY(AudioMixerGetProgress(b,&pb));
        const auto old=b;queueChannel(a,false);queueChannel(b,true,128);resetDuringCopy=b;
        if(action==0)afterCopyHook=[] {VERIFY(AudioMixerReset(resetDuringCopy));queueChannel(resetDuringCopy,true);};
        if(action==1)afterCopyHook=[] {VERIFY(AudioMixerClose(resetDuringCopy));resetDuringCopy=AudioMixerOpen();VERIFY(resetDuringCopy);queueChannel(resetDuringCopy,true);};
        if(action==2)afterCopyHook=[] {VERIFY(AudioMixerSetEnabled(resetDuringCopy,false));};
        AudioMixerFlush();b=resetDuringCopy;s=AudioMixerContributionsRead();r=contributionRow(s,old,pb.epoch);
        VERIFY(s.count==2 && r && r->contributedFrames==128 && r->missingShortFrames==896);
        contributionConservation(s);verifyDma(16384,128,true);
    }

    // A diagnostic control racing the copy fences it out without modifying PCM.
    coalesceSetup(a,b);AudioMixerContributionsReset();queueChannel(a,false);queueChannel(b,true,128);
    afterCopyHook=[] {AudioMixerContributionsReset();};AudioMixerFlush();
    s=AudioMixerContributionsRead();VERIFY(!s.outputFrames && !s.count && !s.observations);
    verifyDma(16384,128,true);verifyDma(16384+512,896,false);
    AudioMixerContributionsEnable(false);AudioStop();initialize();AudioMixerRefresh();
}
void sharedPrefixTests(brook::AudioStreamId a,brook::AudioStreamId b) {
    using namespace brook;
    VERIFY(!AudioMixerSharedPrefixRead().enabled);
    // Same completion-driven refill: baseline commits the unmatched tail and
    // later submits B alone. Candidate retains A's surplus and mixes it with B.
    for(bool enabled:{false,true}){
        coalesceSetup(a,b);AudioMixerSharedPrefixEnable(enabled);AudioMixerSharedPrefixReset();
        AudioMixerContributionsEnable(true);AudioMixerContributionsReset();
        AudioStreamProgress pa{},pb{};VERIFY(AudioMixerGetProgress(a,&pa) && AudioMixerGetProgress(b,&pb));
        queueChannel(a,false);queueChannel(b,true,128);AudioMixerFlush();
        VERIFY(g_nativeClock.submitted==16384+(enabled?128:1024)*4);
        const auto services=nativeServiceCalls;advanceNative(1764);queueChannel(b,true,896);AudioMixerTick();
        VERIFY(nativeServiceCalls==services+1);
        const auto diag=AudioMixerContributionsRead();contributionConservation(diag);
        const auto* ar=contributionRow(diag,a,pa.epoch);const auto* br=contributionRow(diag,b,pb.epoch);
        VERIFY(ar && br && ar->contributedFrames==1024 && br->contributedFrames==1024);
        VERIFY(diag.outputFrames==(enabled?1024:1920));
        VERIFY(ar && ar->missingEmptyFrames==(enabled?0:896));
        VERIFY(br && br->missingShortFrames==(enabled?0:896));
        if(enabled)verifyDma(16384,1024,true);
        finishPhysical();AudioMixerRefresh();
        VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==5120 && !pa.queuedFrames);
        VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==5120 && !pb.queuedFrames);
        const auto st=AudioMixerSharedPrefixRead();
        VERIFY(st.shortenedAttempts==(enabled?1:0) && st.shortenedReceipts==(enabled?1:0));
        VERIFY(st.acceptedPrefixFrames==(enabled?128:0) && st.requestedTailFrames==(enabled?896:0));
    }
    AudioMixerContributionsEnable(false);
    // Fully accepted 512 -> 448 shortening must not impose a 64-frame cap
    // after both producers refill. Reset/off during copy fences counters only.
    for(unsigned control=0;control<3;++control){
        coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
        queueChannel(a,false,512);queueChannel(b,true,448);
        if(control==1)afterCopyHook=[] {AudioMixerSharedPrefixReset();};
        if(control==2)afterCopyHook=[] {AudioMixerSharedPrefixEnable(false);};
        AudioMixerFlush();VERIFY(g_nativeClock.submitted==16384+448*4);
        advanceNative(1764);queueChannel(a,false,448);queueChannel(b,true,512);AudioMixerFlush();
        VERIFY(g_nativeClock.submitted==16384+960*4);verifyDma(16384,960,true);
        finishPhysical();AudioMixerRefresh();AudioStreamProgress pa{},pb{};
        VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==5056 && !pa.queuedFrames);
        VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==5056 && !pb.queuedFrames);
    }
    // Never-refilling producer and tiny shared prefixes cannot retain surplus
    // forever: empty queues do not restrict, and inadequate lead bypasses.
    coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
    queueChannel(a,false);queueChannel(b,true,128);AudioMixerFlush();
    advanceNative(1764);AudioMixerFlush();
    VERIFY(g_nativeClock.submitted==20480 && AudioMixerSharedPrefixRead().shortenedAttempts==1);
    finishPhysical();AudioMixerRefresh();AudioStreamProgress pa{},pb{};
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==5120 && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==4224 && !pb.queuedFrames);

    coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
    queueChannel(a,false);
    for(unsigned i=0;i<4;++i){
        queueChannel(b,true,1);advanceNative(1764);AudioMixerTick();
        AudioMixerDiagnostic d{};AudioMixerDiagnosticSnapshot(&d);bool aQueued=false;
        for(const auto& row:d.streams)if(row.identity==a)aQueued=row.softwareFrames>0;
        if(!aQueued)break;
    }
    auto st=AudioMixerSharedPrefixRead();VERIFY(st.shortenedAttempts && st.headroomBypasses);
    finishPhysical();AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==pa.submittedFrames && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==pb.submittedFrames && !pb.queuedFrames);

    constexpr uint32_t threshold=12288+441*4;
    for(uint32_t pending:{threshold-4,threshold,threshold+4}){
        coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
        advanceNative(16384-pending);queueChannel(a,false);queueChannel(b,true,128);
        const auto before=g_nativeClock.submitted;AudioMixerFlush();st=AudioMixerSharedPrefixRead();
        VERIFY(st.shortenedAttempts==(pending>=threshold?1:0));
        VERIFY(st.headroomBypasses==(pending<threshold?1:0));
        VERIFY(g_nativeClock.submitted==before+(pending<threshold?4096:512));
    }
    coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
    queueChannel(a,false);queueChannel(b,true,128);mixerProgressDelayNs=10000001;AudioMixerFlush();mixerProgressDelayNs=0;
    st=AudioMixerSharedPrefixRead();VERIFY(!st.shortenedAttempts && st.invalidClock==1 && g_nativeClock.submitted==20480);
    coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
    advanceNative(16384-threshold);queueChannel(a,false);queueChannel(b,true,128);
    mixerProgressDelayNs=1000000;AudioMixerFlush();mixerProgressDelayNs=0;
    st=AudioMixerSharedPrefixRead();VERIFY(!st.shortenedAttempts && st.headroomBypasses==1);

    // Partial acceptance owns only a prefix; retry retains the ordinary
    // remainder budget and rebuilds its next proposal. Full-ring rejection cannot manufacture receipt counts.
    coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
    std::array<int16_t,16384> silence{};VERIFY(HdaTrySubmit(silence.data(),31744).accepted==31744);
    queueChannel(a,false);queueChannel(b,true,512);AudioMixerFlush();
    st=AudioMixerSharedPrefixRead();VERIFY(st.shortenedReceipts==1 && st.acceptedPrefixFrames==256);
    VERIFY(st.requestedTailFrames==512 && g_nativeClock.submitted==49152);
    AudioMixerFlush();VERIFY(AudioMixerSharedPrefixRead().shortenedReceipts==1);
    advanceNative(4096);AudioMixerFlush();st=AudioMixerSharedPrefixRead();
    VERIFY(st.shortenedReceipts==2 && g_nativeClock.submitted==50176);
    AudioMixerFlush();finishPhysical();AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==5120 && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==4608 && !pb.queuedFrames);

    // A rejected/partially accepted short proposal must not leave a tiny cap
    // preventing the next low-headroom call from restoring ordinary batching.
    for(bool partial:{false,true}){
        coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
        AudioDeliveryTimingEnable(true);AudioDeliveryTimingReset();
        VERIFY(HdaTrySubmit(silence.data(),partial?32512:32768).accepted==(partial?32512u:32768u));
        queueChannel(a,false);queueChannel(b,true,partial?128:1);AudioMixerFlush();
        VERIFY(g_nativeClock.submitted==49152);
        advanceNative(g_nativeClock.Pending()-(threshold-4));
        const auto before=g_nativeClock.submitted;AudioMixerFlush();
        st=AudioMixerSharedPrefixRead();VERIFY(st.headroomBypasses==1);
        VERIFY(g_nativeClock.submitted==before+(partial?960u:1024u)*4);
        const auto delivery=AudioDeliveryTimingRead();VERIFY(!delivery.reservations && !delivery.unsafe && !delivery.invalidClock && !delivery.invalidPosition);
        finishPhysical();AudioMixerRefresh();
        VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==5120 && !pa.queuedFrames);
        VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==(partial?4224u:4097u) && !pb.queuedFrames);
        AudioDeliveryTimingEnable(false);
    }

    coalesceSetup(a,b);AudioMixerSharedPrefixEnable(true);AudioMixerSharedPrefixReset();
    queueChannel(a,false);queueChannel(b,true,128);
    afterCopyHook=[] {AudioMixerSharedPrefixReset();};AudioMixerFlush();
    st=AudioMixerSharedPrefixRead();VERIFY(!st.shortenedReceipts && !st.acceptedPrefixFrames && !st.shortenedAttempts);
    verifyDma(16384,128,true);AudioMixerSharedPrefixEnable(false);AudioStop();initialize();AudioMixerRefresh();
}
void coalesceTests(brook::AudioStreamId a,brook::AudioStreamId b) {
    using namespace brook;
    VERIFY(!AudioMixerCoalesceRead().enabled);
    // Baseline locks in the exact opportunity: A is irrevocably submitted
    // without B, which arrives before the following service invocation.
    coalesceSetup(a,b);queueChannel(a,false);AudioMixerFlush();
    VERIFY(g_nativeClock.submitted==20480);verifyDma(16384,1024,false);
    queueChannel(b,true);AudioMixerTick();
    VERIFY(g_nativeClock.submitted==24576 && !AudioMixerCoalesceRead().deferrals);
    finishPhysical();AudioMixerRefresh();
    AudioStreamProgress pa{},pb{};
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==5120 && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==5120 && !pb.queuedFrames);

    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);queueChannel(a,false);
    const auto services=nativeServiceCalls;AudioMixerTick();
    VERIFY(nativeServiceCalls==services+1); // deferred Flush still services DMA
    VERIFY(g_nativeClock.submitted==16384 && AudioMixerCoalesceRead().deferrals==1);
    advanceNative(1764);queueChannel(b,true);AudioMixerTick();
    VERIFY(g_nativeClock.submitted==20480);verifyDma(16384,1024,true);
    auto status=AudioMixerCoalesceRead();
    VERIFY(status.deferrals==1 && status.forcedResumes==1 && status.recoveredContributors==1);
    VERIFY(status.lateResumes==1); // a delayed invocation must still submit
    finishPhysical();AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==5120 && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==5120 && !pb.queuedFrames);

    // B's finite source never refills: at most one grace for that empty
    // episode, and A progresses on the next invocation without duplication.
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);queueChannel(a,false);
    AudioMixerFlush();AudioMixerFlush();
    VERIFY(g_nativeClock.submitted==20480);verifyDma(16384,1024,false);
    VERIFY(AudioMixerSetEnabled(b,true)); // idempotent trigger cannot rearm grace
    queueChannel(a,false);AudioMixerFlush();VERIFY(g_nativeClock.submitted==24576);
    status=AudioMixerCoalesceRead();VERIFY(status.deferrals==1 && status.forcedResumes==1);
    finishPhysical();AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==6144 && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==4096 && !pb.queuedFrames);

    // No hold for paused, reset, closed/replaced, or fully drained peers.
    for(unsigned action=0;action<4;++action) {
        coalesceSetup(a,b);AudioMixerCoalesceEnable(true);
        if(action==0)VERIFY(AudioMixerSetEnabled(b,false));
        if(action==1)VERIFY(AudioMixerReset(b));
        if(action==2){auto old=b;VERIFY(AudioMixerClose(b));b=AudioMixerOpen();VERIFY(b && b!=old);}
        if(action==3){finishPhysical();AudioMixerRefresh();}
        queueChannel(a,false);AudioMixerFlush();
        VERIFY(!AudioMixerCoalesceRead().deferrals);
    }

    // Losing a missing identity during an existing grace cannot recover its
    // replacement or introduce another consecutive deferred opportunity.
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);queueChannel(a,false);AudioMixerFlush();
    VERIFY(AudioMixerReset(b));queueChannel(b,true);AudioMixerFlush();
    status=AudioMixerCoalesceRead();VERIFY(status.deferrals==1 && status.forcedResumes==1 && !status.recoveredContributors);
    verifyDma(16384,1024,true); // new epoch's PCM still mixes normally

    // A short but positive B queue is deliberately outside this policy:
    // retain the existing maximum-length mix and its silent suffix.
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);
    queueChannel(a,false);queueChannel(b,true,128);AudioMixerFlush();
    VERIFY(!AudioMixerCoalesceRead().deferrals && g_nativeClock.submitted==20480);
    verifyDma(16384,128,true);verifyDma(16384+128*4,1024-128,false);

    // Changing the missing participant cannot extend an existing grace.
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);queueChannel(a,false);AudioMixerFlush();
    VERIFY(AudioMixerReset(a));queueChannel(b,true);AudioMixerFlush();
    VERIFY(AudioMixerCoalesceRead().deferrals==1 && g_nativeClock.submitted==20480);

    // Close and reuse while deferred must not count the replacement identity
    // as recovery of the previous contributor, even when it supplies PCM.
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);queueChannel(a,false);AudioMixerFlush();
    const auto old=b;VERIFY(AudioMixerClose(b));b=AudioMixerOpen();VERIFY(b && b!=old);
    queueChannel(b,true);AudioMixerFlush();
    VERIFY(!AudioMixerCoalesceRead().recoveredContributors);
    verifyDma(16384,1024,true);

    // Partial native acceptance retains both unaccepted source suffixes.
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);
    std::array<int16_t,16384> silence{};
    VERIFY(HdaTrySubmit(silence.data(),31744).accepted==31744);
    queueChannel(a,false);AudioMixerFlush();queueChannel(b,true);AudioMixerFlush();
    VERIFY(g_nativeClock.submitted==49152);verifyDma(48128,256,true);
    AudioMixerDiagnostic diagnostic{};AudioMixerDiagnosticSnapshot(&diagnostic);
    for(const auto& stream:diagnostic.streams)if(stream.identity==a || stream.identity==b)
        VERIFY(stream.softwareFrames==768 && stream.inFlightFrames==4352);
    AudioMixerFlush();VERIFY(g_nativeClock.submitted==49152); // full, no loss
    advanceNative(4096);AudioMixerFlush();
    VERIFY(g_nativeClock.submitted==52224);verifyDma(49152,768,true);
    finishPhysical();AudioMixerRefresh();
    VERIFY(AudioMixerGetProgress(a,&pa) && pa.completedFrames==5120 && !pa.queuedFrames);
    VERIFY(AudioMixerGetProgress(b,&pb) && pb.completedFrames==5120 && !pb.queuedFrames);

    // Headroom boundary includes a full additional service interval.
    constexpr uint32_t threshold=12288+441*4;
    for(uint32_t pending:{threshold-4,threshold,threshold+4}) {
        coalesceSetup(a,b);AudioMixerCoalesceEnable(true);
        advanceNative(16384-pending);queueChannel(a,false);const auto before=g_nativeClock.submitted;
        AudioMixerFlush();status=AudioMixerCoalesceRead();
        VERIFY(status.deferrals==(pending>=threshold?1:0));
        VERIFY(status.headroomBypasses==(pending<threshold?1:0));
        VERIFY(g_nativeClock.submitted==before+(pending<threshold?4096:0));
    }
    // Age begins before the progress callback. A slow callback cannot present
    // an old queue snapshot as fresh headroom, or renew grace after a delay.
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);queueChannel(a,false);
    mixerProgressDelayNs=10000001;AudioMixerFlush();mixerProgressDelayNs=0;
    status=AudioMixerCoalesceRead();
    VERIFY(!status.deferrals && status.invalidClock==1 && g_nativeClock.submitted==20480);
    // Even an allowed snapshot age consumes part of the lead budget.
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);
    advanceNative(16384-threshold);queueChannel(a,false);
    mixerProgressDelayNs=1000000;AudioMixerFlush();mixerProgressDelayNs=0;
    status=AudioMixerCoalesceRead();VERIFY(!status.deferrals && status.headroomBypasses==1);
    coalesceSetup(a,b);AudioMixerCoalesceEnable(true);queueChannel(a,false);
    clockAvailable=false;AudioMixerFlush();
    VERIFY(!AudioMixerCoalesceRead().deferrals);
    clockAvailable=true;AudioMixerCoalesceEnable(false);AudioStop();initialize();AudioMixerRefresh();
    // b may have been replaced above; retire its current local identity.
    VERIFY(AudioMixerClose(b));
    std::printf("MIXER_COALESCE %s\n",failures?"FAIL":"PASS");
}
}
// Deterministic finite-source experiment, using actual mixer/HDA transactions.
// DMA movement does not reconcile mixer credits. Producers inspect the same
// cached capacity as a write; an explicit refresh variant models ioctl/poll.
static void commitQuantumGuards(brook::AudioStreamId a,brook::AudioStreamId b) {
    using namespace brook;
    for(unsigned action=0;action<5;++action) {
        AudioMixerTestCommitQuantum(0);coalesceSetup(a,b);
        AudioMixerTestCommitQuantum(441);
        if(action==2)advanceNative(16384-(12288+441*4-4));
        if(action>=3) {
            std::array<int16_t,16384> silence{};
            const uint32_t bytes=action==3?32768:31744;
            VERIFY(HdaTrySubmit(silence.data(),bytes).accepted==bytes);
        }
        queueChannel(a,false);queueChannel(b,true);
        const auto initial=g_nativeClock.submitted;
        if(action==0)afterCopyHook=[] {AudioMixerTestCommitQuantum(0);};
        if(action==1)mixerProgressDelayNs=10000001;
        AudioMixerFlush();mixerProgressDelayNs=0;
        if(action==0) {
            VERIFY(g_nativeClock.submitted==initial+441*4);
            queueChannel(a,false,441);queueChannel(b,true,441);
            AudioMixerFlush();VERIFY(g_nativeClock.submitted==initial+(441+1024)*4);
        } else if(action<3)VERIFY(g_nativeClock.submitted==initial+1024*4);
        else {
            VERIFY(g_nativeClock.submitted==49152);
            VERIFY(AudioMixerTestCommitCount()==1);
            advanceNative(g_nativeClock.Pending()-(12288+441*4-4));
            AudioMixerFlush();
            VERIFY(g_nativeClock.submitted==49152+(action==3?1024u:768u)*4);
        }
        finishPhysical();AudioMixerRefresh();
        for(auto id:{a,b}) {
            AudioStreamProgress p{};VERIFY(AudioMixerGetProgress(id,&p));
            VERIFY(p.completedFrames==p.submittedFrames && !p.queuedFrames && !p.discontinuity);
        }
        AudioMixerTestCommitQuantum(0);
    }
}
static void commitHorizonGuards(brook::AudioStreamId a,brook::AudioStreamId b) {
    using namespace brook;
    for(unsigned action=0;action<6;++action) {
        AudioMixerTestCommitQuantum(0);coalesceSetup(a,b);AudioMixerTestCommitHorizon(4096);
        queueChannel(a,false);queueChannel(b,true);
        AudioMixerFlush();VERIFY(g_nativeClock.submitted==16384);
        VERIFY(AudioMixerTestCommitCount()==1); // Full horizon owns no receipt.
        if(action==0 || action==5) {
            advanceNative(441*4);
            if(action==0)afterCopyHook=[] {AudioMixerTestCommitQuantum(0);};
            else afterCopyHook=[] {AudioMixerHorizonReset();};
            AudioMixerFlush();VERIFY(!AudioMixerHorizonRead().shortenedReceipts);
            AudioMixerHorizonEnable(false);
            VERIFY(g_nativeClock.submitted==16384+441*4);
            queueChannel(a,false,441);queueChannel(b,true,441);AudioMixerFlush();
            VERIFY(g_nativeClock.submitted==16384+(441+1024)*4);
        } else if(action<3) {
            if(action==1)mixerProgressDelayNs=10000001;
            else advanceNative(16384-(12288+441*4-4));
            AudioMixerFlush();mixerProgressDelayNs=0;
            VERIFY(g_nativeClock.submitted==16384+1024*4); // Ordinary recovery may exceed target.
        } else {
            advanceNative(441*4);
            const uint64_t full=g_nativeClock.completed+49152;
            if(action==3)beforeSubmitHook=[] {
                std::array<int16_t,AUDIO_BUF_SIZE/2> silence{};const auto bytes=49152-g_nativeClock.Pending();
                VERIFY(HdaTrySubmit(silence.data(),bytes).accepted==bytes);
            };
            else beforeSubmitHook=[] {
                std::array<int16_t,AUDIO_BUF_SIZE/2> silence{};const auto bytes=49152-g_nativeClock.Pending()-1024;
                VERIFY(HdaTrySubmit(silence.data(),bytes).accepted==bytes);
            };
            AudioMixerFlush();VERIFY(g_nativeClock.submitted==full);
            advanceNative(g_nativeClock.Pending()-(12288+441*4-4));AudioMixerFlush();
            VERIFY(g_nativeClock.submitted==full+(action==3?1024u:768u)*4);
        }
        finishPhysical();AudioMixerRefresh();
        for(auto id:{a,b}) {
            AudioStreamProgress p{};VERIFY(AudioMixerGetProgress(id,&p));
            VERIFY(p.completedFrames==p.submittedFrames && !p.queuedFrames && !p.discontinuity);
        }
        AudioMixerTestCommitQuantum(0);
    }
}
static void horizonControlContracts() {
    using namespace brook;
    AudioMixerTestCommitQuantum(0);AudioMixerCoalesceEnable(false);AudioMixerSharedPrefixEnable(false);
    const auto id=AudioMixerOpen();VERIFY(id);VERIFY(AudioMixerSetCapacity(id,4096,1024));
    const std::array<int16_t,64> samples{};VERIFY(AudioMixerSubmit(samples.data(),32,id)==32);
    VERIFY(AudioMixerSetEnabled(id,false));AudioStreamProgress before{},after{};
    VERIFY(AudioMixerGetProgress(id,&before));VERIFY(AudioMixerHorizonEnable(true));
    VERIFY(AudioMixerGetProgress(id,&after));
    VERIFY(after.capacityFrames==before.capacityFrames && after.epoch==before.epoch &&
        after.submittedFrames==before.submittedFrames && after.completedFrames==before.completedFrames &&
        after.queuedFrames==before.queuedFrames && after.fragmentFrames==1024);
    VERIFY(AudioMixerHorizonRead().unqualifiedStreams>0);
    VERIFY(AudioMixerSetWritableFrames(id,1));VERIFY(AudioMixerGetProgress(id,&after));
    VERIFY(after.writableFrames==1 && after.fragmentFrames==1024); // LOW_WATER cannot fake qualification.
    VERIFY(AudioMixerHorizonRead().unqualifiedStreams>0);
    VERIFY(!AudioMixerSetCapacity(id,8192,2048)); // Paused is not empty.
    for(auto tuple:{std::array<uint32_t,2>{6016,128},{5120,128},{16384,8192},{6144,1024}}) {
        const auto previous=AudioMixerHorizonRead();const auto probe=AudioMixerOpen();VERIFY(probe);
        VERIFY(AudioMixerSetCapacity(probe,tuple[0],tuple[1]));const auto next=AudioMixerHorizonRead();
        const bool qualified=AudioRefillQualified(tuple[0],tuple[1]);
        VERIFY(next.qualifiedStreams==previous.qualifiedStreams+unsigned(qualified));
        VERIFY(next.unqualifiedStreams==previous.unqualifiedStreams+unsigned(!qualified));
        VERIFY(AudioMixerClose(probe));
    }
    AudioMixerSharedPrefixEnable(true);AudioMixerCoalesceEnable(true);
    VERIFY(!AudioMixerSharedPrefixRead().enabled && !AudioMixerCoalesceRead().enabled);
    AudioMixerHorizonEnable(false);AudioMixerSharedPrefixEnable(true);
    VERIFY(!AudioMixerHorizonEnable(true));AudioMixerSharedPrefixEnable(false);
    AudioMixerCoalesceEnable(true);VERIFY(!AudioMixerHorizonEnable(true));AudioMixerCoalesceEnable(false);
    VERIFY(AudioMixerClose(id));AudioMixerHorizonReset();
}
static void steadyCommitExperiment() {
    using namespace brook;
    constexpr uint32_t total=44100*8;
    for(unsigned shape:{0u,1u,2u,3u,4u,5u,6u,7u,8u,9u})
    for(unsigned producerPeriod:{0u,1u,5u,9u,10u,19u,20u,21u,39u,40u,41u,50u,75u})
    for(uint32_t capacity:{4096u,5120u,6144u,8192u})
    for(unsigned mode=0;mode<4;++mode)for(uint32_t fragment:{1024u,2048u})for(bool wholeFragment:{false,true})
    for(bool refreshProducer:{false,true})for(unsigned offset:{0u,1u,4u})for(bool reverse:{false,true})
    for(unsigned scenario=0;scenario<4;++scenario) {
        const bool delayed=shape>=3;
        const unsigned budgetMs=(shape==5 || shape>=7)?40:20;
        // Preserve the original symmetric matrix. Additional controls model
        // observed Firefox 4416/64 versus Quake 8192/2048 endpoints; shape 2
        // changes only Firefox capacity. No scheduler or browser is simulated.
        if(!shape && producerPeriod!=5)continue;
        if(shape && !delayed && producerPeriod!=1 && producerPeriod!=5 && producerPeriod!=10)continue;
        if(shape && !delayed && (capacity!=8192 || (mode!=0 && mode!=3) || fragment!=1024 ||
                     !wholeFragment || scenario))continue;
        if(!delayed && scenario==3)continue;
        if(delayed && (producerPeriod==5 || capacity!=8192 || mode!=3 || fragment!=1024 ||
                       !wholeFragment || !refreshProducer))continue;
        if(delayed && scenario && (producerPeriod!=budgetMs || offset!=1))continue;
        // Honest four-fragment/eight-KiB-fragment geometry control: larger
        // fragments, rather than silently exceeding an application's count.
        if(fragment==2048 && ((capacity!=6144 && capacity!=8192) || mode!=3))continue;
        // Additional absence/late-service probes use one fixed phase/geometry,
        // not an unbounded Cartesian sweep. Scenario 0 is ordinary cadence.
        if(scenario && ((capacity!=4096 && mode!=3) || !wholeFragment || !refreshProducer || offset!=1))continue;
        AudioMixerTestCommitQuantum(0);AudioMixerSharedPrefixEnable(false);
        AudioMixerCoalesceEnable(false);AudioStop();initialize();AudioMixerRefresh();
        AudioStreamId ids[2]={AudioMixerOpen(),AudioMixerOpen()};VERIFY(ids[0] && ids[1]);
        uint32_t capacities[2]={shape==1?4416u:capacity,capacity};
        uint32_t fragments[2]={shape?64u:fragment,shape?2048u:fragment};
        if(delayed) {
            // Smallest total geometry at the first viable power-of-two size
            // under the original69-fragment maximum: F + ceil(rate*budget)
            // beyond4096, not an increase in the allowed fragment count.
            const uint32_t chosen=(budgetMs==20)?5120:6016;
            capacities[0]=shape==3?4416:(shape>=6?8192:chosen);
            fragments[0]=shape==3?64:(shape>=6?2048:128);
            capacities[1]=shape>=6?chosen:8192;
            fragments[1]=shape>=6?128:2048;
            if(shape==8){capacities[0]=16384;fragments[0]=8192;}
            if(shape==9){capacities[0]=6016;fragments[0]=128;capacities[1]=16384;fragments[1]=8192;}
            const uint32_t travel=(44100*budgetMs+999)/1000;
            VERIFY(chosen%128==0 && chosen/128<=69 && chosen>=4096+128+travel);
            VERIFY(69*64<4096+64+travel && chosen-128<4096+128+travel);
        }
        const uint32_t maximumCapacity=std::max(capacities[0],capacities[1]);
        // Prime a supported driver amount, not the full software capacity:
        // a16384-frame source FIFO exceeds the HDA ring's writable lead.
        const uint32_t primeFrames=std::min(maximumCapacity,8192u);
        const uint32_t primeCalls=(primeFrames+1763)/1764;
        const uint32_t primedFrames=std::min(maximumCapacity,primeCalls*1764);
        const uint32_t targets[2]={total,scenario==1?capacities[1]:total};
        std::vector<int16_t> pcm[2];uint32_t written[2]{},played[2]{};
        for(unsigned s=0;s<2;++s) {
            VERIFY(AudioMixerSetCapacity(ids[s],capacities[s],fragments[s]));
            pcm[s].resize(total*2);
            for(uint32_t f=0;f<total;++f) {
                const uint64_t code=(uint64_t(f)*48271+uint64_t(s)*65537) ^ (uint64_t(f)*f>>3);
                pcm[s][f*2+s]=int16_t(1000+code%28000);
            }
        }
        auto refill=[&](unsigned s) {
            // A finite exhausted producer is dormant, not a hidden periodic
            // completion observer that helps the still-active source refill.
            if(written[s]==targets[s])return;
            if(refreshProducer)AudioMixerRefresh();
            for(;;) {
                const uint32_t free=AudioMixerAvailableFrames(ids[s]);
                if(!free || written[s]==targets[s] || (wholeFragment && free<fragments[s]))break;
                const uint32_t count=std::min({free,fragments[s],targets[s]-written[s]});
                const auto accepted=AudioMixerSubmit(pcm[s].data()+written[s]*2,count,ids[s]);
                VERIFY(accepted==count);written[s]+=accepted;
                if(!accepted)break;
            }
        };
        refill(0);refill(1);
        for(unsigned i=0;i<primeCalls;++i)AudioMixerFlush();
        VERIFY(g_nativeClock.submitted==primedFrames*4);prime();AudioMixerRefresh();
        if(scenario==3)VERIFY(AudioMixerSetEnabled(ids[1],false));
        AudioMixerSharedPrefixEnable(mode==1);AudioMixerTestCommitQuantum(mode==2?441:0);
        if(mode==3)AudioMixerTestCommitHorizon(4096);
        AudioMixerContributionsEnable(true);AudioMixerContributionsReset();
        AudioDeliveryTimingEnable(true);AudioDeliveryTimingReset();
        uint64_t before[2]{},after[2]{},physicalBefore[2]{},physicalAfter[2]{};
        uint64_t metricsBefore[3]{},metricsAfter[3]{};
        uint64_t dmaBytes=0;
        uint32_t minPending=~uint32_t(0),maxPending=0;
        uint64_t minPhysicalPending=~uint64_t(0),maxPhysicalPending=0;
        uint64_t maxHorizonExcess=0;unsigned overHorizonSamples=0,recoveryMs=~0u;
        unsigned recoveryServiceResumeMs=~0u;
        unsigned elapsed=0;bool finished=false;
        uint32_t eligibleAt[2]{},opportunities[2]{},maxRefillDelay[2]{};
        unsigned firstDataMs[2]{};
        for(unsigned ms=1;ms<=25000;++ms) {
            // Exact 44.1-kHz physical advancement, independent of driver reads.
            const uint32_t frames=(ms*441u/10)-((ms-1)*441u/10);
            if(g_streamRunning) {
                const auto pos=HdaTestRead(g_outStreamBase+SD_LPIB,4);
                for(uint32_t f=0;f<frames;++f) {
                    int16_t pair[2]{};memcpy(pair,dma.data()+(pos+f*4)%AUDIO_BUF_SIZE,4);
                    for(unsigned s=0;s<2;++s)if(pair[s]) {
                        if(!firstDataMs[s])firstDataMs[s]=ms;
                        VERIFY(played[s]<targets[s]);
                        if(played[s]<targets[s])VERIFY(pair[s]==pcm[s][played[s]*2+s]);
                        ++played[s];
                    }
                }
                move(frames*4,1000000);
                dmaBytes+=frames*4;
            } else nowNs+=1000000;
            if(delayed)AudioMixerRefresh(); // Explicit, instantaneous discovery; delayed admission follows.
            if(scenario==3 && ms==6500)VERIFY(AudioMixerSetEnabled(ids[1],true));
            // Sweep order independently of polling phase, without an extra
            // reconciliation or flush between the two producer opportunities.
            for(unsigned order=0;order<2;++order) {
                const unsigned s=reverse?1-order:order;
                if(delayed) {
                    if(written[s]==targets[s] || (scenario==3 && s==1 && ms<6500))continue;
                    if(!eligibleAt[s] && AudioMixerAvailableFrames(ids[s])>=fragments[s])eligibleAt[s]=ms;
                    if(eligibleAt[s] && ms-eligibleAt[s]>=(s?0:producerPeriod)) {
                        maxRefillDelay[s]=std::max(maxRefillDelay[s],ms-eligibleAt[s]);
                        ++opportunities[s];refill(s);eligibleAt[s]=0;
                    }
                } else if(ms%(s?5:producerPeriod)==(s?offset:0))refill(s);
            }
            // Four missed service opportunities, followed by the original
            // cadence; never compensate with an immediate extra flush.
            if(ms%10==(delayed?offset:0) && !(scenario==2 && ms>=3000 && ms<3040)) {
                if(scenario==2 && ms>=3040 && recoveryServiceResumeMs==~0u)recoveryServiceResumeMs=ms;
                AudioMixerTick();
            }
            if(ms==2000 || ms==6000) {
                // Use existing cached completion, avoiding an observer-driven
                // reconciliation or submission at the measurement endpoints.
                AudioStreamProgress p[2]{};
                for(unsigned s=0;s<2;++s)VERIFY(AudioMixerGetProgress(ids[s],&p[s]));
                if(ms==2000)for(unsigned s=0;s<2;++s){before[s]=p[s].completedFrames;physicalBefore[s]=played[s];}
                else for(unsigned s=0;s<2;++s){after[s]=p[s].completedFrames;physicalAfter[s]=played[s];if(!(scenario==1 && s==1))VERIFY(written[s]<targets[s]);}
                const auto delivery=AudioDeliveryTimingRead();
                const auto contributions=AudioMixerContributionsRead();
                VERIFY(contributions.count==(scenario==3?1u:2u));contributionConservation(contributions);
                auto* metrics=ms==2000?metricsBefore:metricsAfter;
                metrics[0]=delivery.reservations;
                for(unsigned i=0;i<contributions.count;++i) {
                    metrics[1]+=contributions.rows[i].missingShortFrames;
                    metrics[2]+=contributions.rows[i].missingEmptyFrames;
                }
            }
            if(ms>=2000 && ms<=6000) {
                minPending=std::min(minPending,g_nativeClock.Pending());
                maxPending=std::max(maxPending,g_nativeClock.Pending());
                const uint64_t physicalPending=g_nativeClock.submitted>dmaBytes?g_nativeClock.submitted-dmaBytes:0;
                minPhysicalPending=std::min(minPhysicalPending,physicalPending);
                maxPhysicalPending=std::max(maxPhysicalPending,physicalPending);
                if(physicalPending>4096*4) {
                    ++overHorizonSamples;
                    maxHorizonExcess=std::max(maxHorizonExcess,physicalPending-4096*4);
                }
                if(scenario==2 && recoveryServiceResumeMs!=~0u && recoveryMs==~0u && physicalPending<=4096*4)
                    recoveryMs=ms-recoveryServiceResumeMs;
            }
            AudioStreamProgress p[2]{};
            for(unsigned s=0;s<2;++s)VERIFY(AudioMixerGetProgress(ids[s],&p[s]));
            if(p[0].completedFrames==targets[0] && p[1].completedFrames==targets[1]) {
                elapsed=ms;finished=true;break;
            }
        }
        VERIFY(finished && elapsed>6000);
        for(unsigned s=0;s<2;++s) {
            AudioStreamProgress p{};VERIFY(AudioMixerGetProgress(ids[s],&p));
            VERIFY(written[s]==targets[s] && played[s]==targets[s] && p.submittedFrames==targets[s] &&
                p.completedFrames==targets[s] && !p.queuedFrames && !p.discontinuity);
        }
        const auto delivery=AudioDeliveryTimingRead();const auto contributions=AudioMixerContributionsRead();
        VERIFY(!delivery.unsafe && !delivery.invalidClock && !delivery.invalidPosition && !g_nativeClock.discontinuity);
        contributionConservation(contributions);
        VERIFY(after[0]>before[0]); // An absent peer cannot stall the supplier.
        if(mode==3 && capacity>=6144 && scenario==2)VERIFY(recoveryMs<100);
        for(unsigned s=0;s<2;++s)VERIFY(after[s]-before[s]==physicalAfter[s]-physicalBefore[s]);
        if(delayed && shape!=3 && producerPeriod<=budgetMs && scenario!=2) {
            VERIFY(after[0]-before[0]==176400 && metricsAfter[0]==metricsBefore[0]);
            if(!scenario) {
                VERIFY(after[1]-before[1]==176400);
                VERIFY(metricsAfter[1]==metricsBefore[1] && metricsAfter[2]==metricsBefore[2]);
            }
        }
        // Extrema are sampled after the producer/service actions each ms, not
        // a continuous minimum immediately before every replenishment.
        if(delayed)std::printf("MIXER_REFILL_BUDGET shape=%u budget_ms=%u delay_ms=%u capacities=%u,%u fragments=%u,%u opportunities=%u,%u max_refill_delay_ms=%u,%u recovery_service_resume_ms=%u first_data_ms=%u,%u ",
                            shape,budgetMs,producerPeriod,capacities[0],capacities[1],fragments[0],fragments[1],
                            opportunities[0],opportunities[1],maxRefillDelay[0],maxRefillDelay[1],recoveryServiceResumeMs,firstDataMs[0],firstDataMs[1]);
        else if(shape)std::printf("MIXER_ASYMMETRIC shape=%u producer_period_ms=%u capacities=%u,%u fragments=%u,%u ",
                            shape,producerPeriod,capacities[0],capacities[1],fragments[0],fragments[1]);
        std::printf(shape?
            "mode=%u legacy_capacity_anchor=%u legacy_fragment_anchor=%u fragment=%u refresh=%u offset=%u reverse=%u scenario=%u completion=%llu,%llu physical=%llu,%llu interval_frames=176400 drain_ms=%u cached_pending=%u,%u sampled_physical_pending=%llu,%llu reservations=%llu steady_reservations=%llu missing_short=%llu missing_empty=%llu caps=%llu over_horizon_samples=%u max_horizon_excess_bytes=%llu recovery_ms=%u\n":
            "MIXER_STEADY mode=%u capacity=%u fragment_frames=%u fragment=%u refresh=%u offset=%u reverse=%u scenario=%u completion=%llu,%llu physical=%llu,%llu interval_frames=176400 drain_ms=%u cached_pending=%u,%u sampled_physical_pending=%llu,%llu reservations=%llu steady_reservations=%llu missing_short=%llu missing_empty=%llu caps=%llu over_horizon_samples=%u max_horizon_excess_bytes=%llu recovery_ms=%u\n",
            mode,capacity,fragment,unsigned(wholeFragment),unsigned(refreshProducer),offset,unsigned(reverse),scenario,
            (unsigned long long)(after[0]-before[0]),(unsigned long long)(after[1]-before[1]),
            (unsigned long long)(physicalAfter[0]-physicalBefore[0]),(unsigned long long)(physicalAfter[1]-physicalBefore[1]),
            elapsed,minPending,maxPending,
            (unsigned long long)minPhysicalPending,(unsigned long long)maxPhysicalPending,
            (unsigned long long)delivery.reservations,
            (unsigned long long)(metricsAfter[0]-metricsBefore[0]),
            (unsigned long long)(metricsAfter[1]-metricsBefore[1]),
            (unsigned long long)(metricsAfter[2]-metricsBefore[2]),
            (unsigned long long)AudioMixerTestCommitCount(),overHorizonSamples,
            (unsigned long long)maxHorizonExcess,recoveryMs);
        AudioMixerTestCommitQuantum(0);AudioMixerSharedPrefixEnable(false);
        AudioMixerContributionsEnable(false);AudioDeliveryTimingEnable(false);
        VERIFY(AudioMixerClose(ids[0]) && AudioMixerClose(ids[1]));AudioStop();
    }
}
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
    VERIFY(timing.observations==2 && timing.reservations==1 && timing.reservedBytes==NATIVE_WRITE_LEAD-4);
    VERIFY(timing.maxCopyNs==49000 && timing.maxAdvanceBytes==4 && timing.minHeadroomBytes==NATIVE_WRITE_LEAD-4);
    VERIFY(!timing.invalidClock && !timing.invalidPosition && !timing.unsafe);
    VERIFY(timing.reservationsRetained==1 && !timing.reservationsOverwritten);
    const auto& reservePair=timing.reservationEvents[0];
    VERIFY(reservePair.hasPrevious && reservePair.observation==2 && reservePair.controlEpoch==timing.epoch);
    VERIFY(reservePair.previous.acceptedBytes==8 && reservePair.current.acceptedBytes==8);
    VERIFY(reservePair.previous.hardwareEpoch==reservePair.current.hardwareEpoch);
    VERIFY(!reservePair.previous.running && reservePair.current.running);
    VERIFY(reservePair.current.reservedBytes==NATIVE_WRITE_LEAD-4);
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
    contributionTests(a,b);
    sharedPrefixTests(a,b);
    horizonControlContracts();
    commitQuantumGuards(a,b);
    commitHorizonGuards(a,b);
    steadyCommitExperiment();
    coalesceTests(a,b);
    VERIFY(AudioMixerClose(a));mixerClockEnabled=false;AudioStop();
    std::printf("MIXER_CLOCK %s\n",failures?"FAIL":"PASS");return failures;
}
