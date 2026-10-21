// audio.cpp — Brook audio subsystem registry + software mixer.
//
// Each /dev/dsp open owns a FIFO of resampled 44100 Hz stereo 16-bit PCM.
// Delivery snapshots mix live streams without sharing write cursors.
// A dedicated kernel thread (audio_mix) wakes every ~10ms and drains
// accumulated data to the hardware driver in consistent chunks,
// decoupling application write timing from hardware playback timing.

#include "audio.h"
#include "dsp_producer_timing.h"
#include "serial.h"
#include "kprintf.h"
#include "memory/heap.h"
#include "spinlock.h"
#include "string.h"
#include "scheduler.h"
#include "process.h"
#include "apic.h"
#include "kvmclock.h"

namespace brook {


extern "C" uint64_t AudioClockNowNs()
{
    // Delivered LAPIC ticks can undercount a CPU stall. Do not present that
    // fallback as a bound on elapsed DMA time. Add a hardware clock separately.
    uint64_t now = 0;
    return KvmClockReadStableNs(&now) ? now : 0;
}

static SpinLock g_dspProducerLock;
static DspProducerTimingModel g_dspProducerTiming;
static uint64_t g_dspProducerEpoch=0,g_dspProducerContention=0;
uint64_t DspProducerTimingToken(){return __atomic_load_n(&g_dspProducerEpoch,__ATOMIC_ACQUIRE);}
void DspProducerTimingEnable(bool on){
    SpinLockAcquire(&g_dspProducerLock);g_dspProducerTiming.Enable(on);
    __atomic_store_n(&g_dspProducerEpoch,g_dspProducerTiming.Token(),__ATOMIC_RELEASE);SpinLockRelease(&g_dspProducerLock);
}
void DspProducerTimingReset(){
    SpinLockAcquire(&g_dspProducerLock);g_dspProducerTiming.Reset();
    __atomic_store_n(&g_dspProducerEpoch,g_dspProducerTiming.Token(),__ATOMIC_RELEASE);SpinLockRelease(&g_dspProducerLock);
}
DspProducerSnapshot DspProducerTimingRead(){
    SpinLockAcquire(&g_dspProducerLock);auto s=g_dspProducerTiming.Snapshot();SpinLockRelease(&g_dspProducerLock);
    s.contended=__atomic_load_n(&g_dspProducerContention,__ATOMIC_RELAXED);return s;
}
void DspProducerTimingRecord(uint64_t token,const DspProducerEvent& event){
    // No diagnostic wait while returning from a user write. Drops remain visible.
    if(!SpinLockTryAcquire(&g_dspProducerLock)){
        auto old=__atomic_load_n(&g_dspProducerContention,__ATOMIC_RELAXED);
        while(old!=~uint64_t(0) && !__atomic_compare_exchange_n(&g_dspProducerContention,&old,old+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
        return;
    }
    g_dspProducerTiming.Observe(token,event);SpinLockRelease(&g_dspProducerLock);
}

static SpinLock g_deliveryTimingLock;
static AudioDeliveryTimingModel g_deliveryTiming;
static uint64_t g_deliveryTimingEpoch=0,g_deliveryTimingContention=0;
extern "C" uint64_t AudioDeliveryTimingToken(){return __atomic_load_n(&g_deliveryTimingEpoch,__ATOMIC_ACQUIRE);}
void AudioDeliveryTimingEnable(bool on){
    SpinLockAcquire(&g_deliveryTimingLock);g_deliveryTiming.Enable(on);
    __atomic_store_n(&g_deliveryTimingEpoch,g_deliveryTiming.Token(),__ATOMIC_RELEASE);SpinLockRelease(&g_deliveryTimingLock);
}
void AudioDeliveryTimingReset(){
    SpinLockAcquire(&g_deliveryTimingLock);g_deliveryTiming.Reset();
    __atomic_store_n(&g_deliveryTimingEpoch,g_deliveryTiming.Token(),__ATOMIC_RELEASE);SpinLockRelease(&g_deliveryTimingLock);
}
AudioDeliveryTimingSnapshot AudioDeliveryTimingRead(){
    SpinLockAcquire(&g_deliveryTimingLock);auto s=g_deliveryTiming.Snapshot();SpinLockRelease(&g_deliveryTimingLock);
    s.contendedRecords=__atomic_load_n(&g_deliveryTimingContention,__ATOMIC_RELAXED);return s;
}
extern "C" void AudioDeliveryTimingRecord(uint64_t token,const AudioDeliveryEvent* e){
    if(!e || !(token&1))return;
    // Driver owns its playback lock. Never wait for a diagnostic reader.
    if(!SpinLockTryAcquire(&g_deliveryTimingLock)){
        auto old=__atomic_load_n(&g_deliveryTimingContention,__ATOMIC_RELAXED);
        while(old!=~uint64_t(0) && !__atomic_compare_exchange_n(&g_deliveryTimingContention,&old,old+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
        return;
    }
    g_deliveryTiming.Observe(token,*e);SpinLockRelease(&g_deliveryTimingLock);
}

static const AudioDriver* g_audioDriver = nullptr;
static SpinLock g_mixerLock;
static SpinLock g_audioTimingLock;
static AudioServiceTimingModel g_audioTiming;
static uint64_t g_audioTimingEpoch=0,g_audioTimingContention=0;
void AudioServiceTimingEnable(bool enabled){
    SpinLockAcquire(&g_audioTimingLock);g_audioTiming.Enable(enabled);
    __atomic_store_n(&g_audioTimingEpoch,g_audioTiming.Token(),__ATOMIC_RELEASE);
    SpinLockRelease(&g_audioTimingLock);
}
void AudioServiceTimingReset(){
    SpinLockAcquire(&g_audioTimingLock);g_audioTiming.Reset();
    __atomic_store_n(&g_audioTimingEpoch,g_audioTiming.Token(),__ATOMIC_RELEASE);
    SpinLockRelease(&g_audioTimingLock);
}
AudioServiceTimingSnapshot AudioServiceTimingRead(){
    SpinLockAcquire(&g_audioTimingLock);auto result=g_audioTiming.Snapshot();SpinLockRelease(&g_audioTimingLock);
    result.contendedRecords=__atomic_load_n(&g_audioTimingContention,__ATOMIC_RELAXED);return result;
}
static void AudioServiceTimingRecord(uint64_t token,uint64_t before,uint64_t deadline,
                                    uint64_t wake,uint64_t end,bool worked){
    // The mixer never waits for a diagnostic reader/reset. Lifetime drop count
    // is separate from resettable interval counters and must be checked.
    if(!SpinLockTryAcquire(&g_audioTimingLock)){
        auto old=__atomic_load_n(&g_audioTimingContention,__ATOMIC_RELAXED);
        while(old!=~uint64_t(0) && !__atomic_compare_exchange_n(&g_audioTimingContention,
              &old,old+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
        return;
    }
    g_audioTiming.Observe(token,before,deadline,wake,end,worked);
    SpinLockRelease(&g_audioTimingLock);
}

// Independently owned FIFO rings. Samples remain charged to their stream
// until the driver accepts them; prepared mixes never own pointers into a ring.
static constexpr uint32_t MIXER_BUF_MASK = MIXER_RING_FRAMES - 1;
static constexpr uint32_t MIXER_BATCH_FRAMES = 1764;
struct MixerStream {
    int16_t* samples = nullptr;
    AudioStreamId identity = 0;
    uint64_t epoch = 0;
    uint64_t read = 0, write = 0;
    bool enabled = true;
    uint64_t submittedFrames = 0, completedFrames = 0;
    uint32_t inFlight = 0;
    uint32_t capacity = MIXER_RING_FRAMES;
    uint32_t writableFrames = 1;
    bool discontinuity = false;
    bool coalesceUsed = false; // one grace per empty software-queue episode
};
static MixerStream g_streams[MIXER_MAX_STREAMS];
static uint64_t g_nextStreamIdentity = 1;
static bool g_mixerReady = false;
static int16_t* g_flushBuf = nullptr;
static uint32_t g_flushBusy = 0;
static uint64_t g_deliveryEpoch = 0, g_flushEpoch = 0;
static uint32_t g_batchRemaining = 0;
struct Contribution {
    AudioStreamId identity = 0;
    uint64_t epoch = 0, read = 0;
    uint32_t frames = 0;
};
// If a generic driver accepts part of an encoded stereo frame, preserve the
// rest of that frame exactly. Later whole frames can be remixed after a close.
static uint8_t g_partialFrame[MIXER_FRAME_BYTES];
static uint32_t g_partialOffset = 0;
static Contribution g_partialContributions[MIXER_MAX_STREAMS];
static constexpr uint32_t COMPLETION_SLOTS = 128;
struct Completion {
    AudioSubmitReceipt receipt{};
    uint32_t credited = 0;
    Contribution parts[MIXER_MAX_STREAMS]{};
};
static Completion g_completions[COMPLETION_SLOTS];
static uint32_t g_completionHead = 0, g_completionCount = 0;
enum class DriverMode { Unknown, Legacy, Native };
static DriverMode g_driverMode = DriverMode::Unknown; // flush owner only
#ifdef BROOK_HOST_TEST
// Hosted commitment-horizon experiment only; no native state, API or branch.
static uint32_t g_testCommitQuantum = 0;
static uint64_t g_testCommitCount = 0;
void AudioMixerTestCommitQuantum(uint32_t frames) {
    SpinLockAcquire(&g_mixerLock);
    g_testCommitQuantum = frames <= MIXER_BATCH_FRAMES ? frames : 0;
    g_testCommitCount = 0;
    SpinLockRelease(&g_mixerLock);
}
uint64_t AudioMixerTestCommitCount() { return g_testCommitCount; }
#endif
static uint64_t g_hardwareEpoch = 0;
static bool g_clockSupported = false; // mixer lock
static AudioMixerSharedPrefixSnapshot g_sharedPrefix; // existing mixer lock
static bool g_sharedPrefixActive=false;
struct SharedPrefixAttempt {uint64_t token=0;uint32_t normal=0, requested=0;};
static void PrefixAdd(uint64_t& value,uint64_t amount=1) {
    AudioMixSaturatingAdd(value,amount,g_sharedPrefix.saturated);
}
static void PrefixAdvance(bool on) {
    if(g_sharedPrefix.exhausted || g_sharedPrefix.epoch>~uint64_t(0)-2){
        g_sharedPrefix.exhausted=true;g_sharedPrefix.epoch=0;on=false;
    }else g_sharedPrefix.epoch=((g_sharedPrefix.epoch&~uint64_t(1))+2)|(on?1:0);
    g_sharedPrefix.enabled=on;
    __atomic_store_n(&g_sharedPrefixActive,on,__ATOMIC_RELEASE);
}
void AudioMixerSharedPrefixEnable(bool enabled) {
    SpinLockAcquire(&g_mixerLock);PrefixAdvance(enabled);SpinLockRelease(&g_mixerLock);
}
void AudioMixerSharedPrefixReset() {
    SpinLockAcquire(&g_mixerLock);
    const auto old=g_sharedPrefix;g_sharedPrefix={};g_sharedPrefix.epoch=old.epoch;
    g_sharedPrefix.exhausted=old.exhausted;PrefixAdvance(old.enabled);
    SpinLockRelease(&g_mixerLock);
}
AudioMixerSharedPrefixSnapshot AudioMixerSharedPrefixRead() {
    SpinLockAcquire(&g_mixerLock);const auto result=g_sharedPrefix;
    SpinLockRelease(&g_mixerLock);return result;
}
// Called with the preparation lock held. Empty queues do not restrict the
// shared prefix. A tiny positive prefix can postpone surplus only while fresh
// physical lead covers snapshot age plus one nominal service opportunity.
static uint32_t SharedPrefixFrames(uint32_t normal,const AudioDeviceProgress& device,
                                   uint64_t sampledBefore,SharedPrefixAttempt& attempt) {
    if(!g_sharedPrefix.enabled || !normal)return normal;
    unsigned positive=0;uint32_t shortest=normal;
    for(const auto& stream:g_streams){
        if(!stream.identity || !stream.enabled || stream.discontinuity || stream.write==stream.read)continue;
        ++positive;const uint32_t available=static_cast<uint32_t>(stream.write-stream.read);
        if(available<shortest)shortest=available;
    }
    if(positive<2 || shortest==normal)return normal;
    PrefixAdd(g_sharedPrefix.candidates);
    if(g_driverMode!=DriverMode::Native || strcmp(g_audioDriver->name,"intel_hda")){
        PrefixAdd(g_sharedPrefix.unsupportedDriver);return normal;
    }
    constexpr uint64_t serviceNs=10000000;
    const uint64_t now=AudioClockNowNs();
    if(!sampledBefore || !now || now<sampledBefore || now-sampledBefore>serviceNs || now>~uint64_t(0)-serviceNs){
        PrefixAdd(g_sharedPrefix.invalidClock);return normal;
    }
    const uint64_t travelNs=now-sampledBefore+serviceNs;
    const uint64_t frames=(travelNs*MIXER_HW_RATE+999999999)/1000000000;
    const uint64_t requiredBytes=12288+frames*MIXER_FRAME_BYTES;
    if(!device.running || device.discontinuity || device.epoch!=g_hardwareEpoch ||
       device.submitted<device.completed || device.submitted-device.completed<requiredBytes){
        PrefixAdd(g_sharedPrefix.headroomBypasses);return normal;
    }
    PrefixAdd(g_sharedPrefix.shortenedAttempts);
    attempt={g_sharedPrefix.epoch,normal,shortest};return shortest;
}
static AudioMixContributionModel g_mixContributions; // existing mixer lock
void AudioMixerContributionsEnable(bool enabled) {
    SpinLockAcquire(&g_mixerLock);g_mixContributions.Enable(enabled);SpinLockRelease(&g_mixerLock);
}
void AudioMixerContributionsReset() {
    SpinLockAcquire(&g_mixerLock);g_mixContributions.Reset();SpinLockRelease(&g_mixerLock);
}
AudioMixContributionSnapshot AudioMixerContributionsRead() {
    SpinLockAcquire(&g_mixerLock);const auto result=g_mixContributions.Snapshot();
    SpinLockRelease(&g_mixerLock);return result;
}


// All coalescing state is protected by g_mixerLock, including configuration.
// A return from Flush still allows Tick to call the driver's service routine.
static AudioMixerCoalesceSnapshot g_coalesce;
static bool g_coalesceActive = false;
static bool g_coalescePending = false;
static uint64_t g_coalesceDeadline = 0;
static Contribution g_coalesceMissing[MIXER_MAX_STREAMS]{};
static void CoalesceClearPending()
{
    g_coalescePending = false;
    g_coalesceDeadline = 0;
    for (auto& part : g_coalesceMissing) part = {};
}
static void CoalesceCount(uint64_t& value)
{
    if (value == ~uint64_t(0)) g_coalesce.saturated = true;
    else ++value;
}
void AudioMixerCoalesceEnable(bool enabled)
{
    SpinLockAcquire(&g_mixerLock);
    g_coalesce.enabled = enabled;
    __atomic_store_n(&g_coalesceActive, enabled, __ATOMIC_RELEASE);
    CoalesceClearPending();
    for (auto& stream : g_streams) stream.coalesceUsed = false;
    SpinLockRelease(&g_mixerLock);
}
void AudioMixerCoalesceReset()
{
    SpinLockAcquire(&g_mixerLock);
    const bool enabled = g_coalesce.enabled;
    g_coalesce = {}; g_coalesce.enabled = enabled;
    CoalesceClearPending();
    for (auto& stream : g_streams) stream.coalesceUsed = false;
    SpinLockRelease(&g_mixerLock);
}
AudioMixerCoalesceSnapshot AudioMixerCoalesceRead()
{
    SpinLockAcquire(&g_mixerLock);
    const auto result = g_coalesce;
    SpinLockRelease(&g_mixerLock);
    return result;
}


static MixerStream* FindStream(AudioStreamId identity)
{
    const uint32_t slot = static_cast<uint32_t>(identity & 15);
    if (!slot || slot > MIXER_MAX_STREAMS) return nullptr;
    auto& stream = g_streams[slot - 1];
    return identity && stream.identity == identity ? &stream : nullptr;
}
static void ResetStream(MixerStream& stream)
{
    ++stream.epoch;
    stream.read = stream.write = 0;
    stream.submittedFrames = stream.completedFrames = stream.inFlight = 0;
    stream.discontinuity = false;
    stream.coalesceUsed = false;
}
static void ResetAllStreams()
{
    SpinLockAcquire(&g_mixerLock);
    for (auto& stream : g_streams) ResetStream(stream);
    CoalesceClearPending();
    SpinLockRelease(&g_mixerLock);
}
void AudioMixerInit()
{
    // Boot-only initialization: allocate outside the mixer lock.
    for (auto& stream : g_streams)
        stream.samples = static_cast<int16_t*>(kmalloc(MIXER_RING_FRAMES * MIXER_FRAME_BYTES));
    g_flushBuf = static_cast<int16_t*>(kmalloc(MIXER_BATCH_FRAMES * MIXER_FRAME_BYTES));
    bool ready = g_flushBuf != nullptr;
    for (const auto& stream : g_streams) ready = ready && stream.samples;
    if (!ready) {
        for (auto& stream : g_streams) { kfree(stream.samples); stream.samples = nullptr; }
        kfree(g_flushBuf); g_flushBuf = nullptr;
        return;
    }
    g_mixerReady = true;
}
AudioStreamId AudioMixerOpen()
{
    AudioMixerRefresh();
    SpinLockAcquire(&g_mixerLock);
    AudioStreamId identity = 0;
    if (g_mixerReady && g_nextStreamIdentity <= (~uint64_t(0) >> 4)) {
        for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
            auto& stream = g_streams[i];
            if (stream.identity) continue;
            identity = (g_nextStreamIdentity++ << 4) | (i + 1);
            ResetStream(stream); stream.enabled = true; stream.capacity = MIXER_RING_FRAMES;
            stream.writableFrames = 1; stream.identity = identity;
            break;
        }
    }
    SpinLockRelease(&g_mixerLock);
    return identity;
}
bool AudioMixerClose(AudioStreamId identity)
{
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (stream) { ResetStream(*stream); stream->identity = 0; }
    SpinLockRelease(&g_mixerLock);
    return stream != nullptr;
}
bool AudioMixerReset(AudioStreamId identity)
{
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (stream) ResetStream(*stream);
    SpinLockRelease(&g_mixerLock);
    return stream != nullptr;
}
bool AudioMixerSetEnabled(AudioStreamId identity, bool enabled)
{
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (stream && stream->enabled != enabled) {
        stream->enabled = enabled; stream->coalesceUsed = false;
    }
    SpinLockRelease(&g_mixerLock);
    return stream != nullptr;
}
bool AudioMixerSetCapacity(AudioStreamId identity, uint32_t frames, uint32_t writableFrames)
{
    if (!frames || frames > MIXER_RING_FRAMES || !writableFrames || writableFrames > frames) return false;
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    const bool empty = stream && stream->read == stream->write && !stream->inFlight;
    if (empty) { stream->capacity = frames; stream->writableFrames = writableFrames; }
    SpinLockRelease(&g_mixerLock);
    return empty;
}
bool AudioMixerSetWritableFrames(AudioStreamId identity, uint32_t frames)
{
    if (!frames) return false;
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    const bool valid = stream && frames <= stream->capacity;
    if (valid) stream->writableFrames = frames;
    SpinLockRelease(&g_mixerLock);
    return valid;
}
uint32_t AudioMixerAvailableFrames(AudioStreamId identity)
{
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    uint32_t available = stream && !stream->discontinuity ? stream->capacity - static_cast<uint32_t>(stream->write - stream->read) - stream->inFlight : 0;
    SpinLockRelease(&g_mixerLock);
    return available;
}
uint32_t AudioMixerSubmit(const int16_t* samples, uint32_t frames, AudioStreamId identity)
{
    if (!samples || !frames) return 0;
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (!stream || stream->discontinuity) { SpinLockRelease(&g_mixerLock); return 0; }
    const uint32_t available = stream->capacity - static_cast<uint32_t>(stream->write - stream->read) - stream->inFlight;
    if (frames > available) frames = available;
    // Empty queues may restart their indices, avoiding lifetime counter wrap.
    if (stream->read == stream->write) stream->read = stream->write = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t index = static_cast<uint32_t>(stream->write + i) & MIXER_BUF_MASK;
        stream->samples[index * 2] = samples[i * 2];
        stream->samples[index * 2 + 1] = samples[i * 2 + 1];
    }
    if (stream->submittedFrames > ~uint64_t(0) - frames) {
        stream->discontinuity = true; SpinLockRelease(&g_mixerLock); return 0;
    }
    stream->write += frames;
    stream->submittedFrames += frames;
    if (frames) stream->coalesceUsed = false;
    SpinLockRelease(&g_mixerLock);
    return frames;
}
static void Consume(const Contribution* contributions, uint32_t frames, bool clocked = false)
{
    for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
        const auto& part = contributions[i];
        auto* stream = FindStream(part.identity);
        if (!stream || stream->epoch != part.epoch || stream->read != part.read) continue;
        uint32_t consumed = frames < part.frames ? frames : part.frames;
        stream->read += consumed;
        if (clocked) stream->inFlight += consumed;
    }
}
// Snapshot a new whole-frame attempt. Rebuilding unaccepted frames removes
// closed/reset/paused contributors without subtracting already-clamped sums.
static uint32_t Prepare(Contribution* contributions, AudioMixPreparedStream* prepared,
                        const AudioDeviceProgress& device,uint64_t sampledBefore,SharedPrefixAttempt& prefix)
{
    uint32_t frames = 0;
    for (const auto& stream : g_streams)
        if (stream.identity && stream.enabled && !stream.discontinuity && stream.write - stream.read > frames)
            frames = static_cast<uint32_t>(stream.write - stream.read);
    if (frames > MIXER_BATCH_FRAMES) frames = MIXER_BATCH_FRAMES;
    if (g_batchRemaining && frames > g_batchRemaining) frames = g_batchRemaining;
    const uint32_t normalFrames=frames;
    frames=SharedPrefixFrames(frames,device,sampledBefore,prefix);
#ifdef BROOK_HOST_TEST
    if (g_testCommitQuantum && frames > g_testCommitQuantum &&
        g_driverMode == DriverMode::Native && !strcmp(g_audioDriver->name,"intel_hda")) {
        const uint64_t now=AudioClockNowNs();
        constexpr uint64_t serviceNs=10000000;
        if (sampledBefore && now>=sampledBefore && now-sampledBefore<=serviceNs &&
            now<=~uint64_t(0)-serviceNs && device.running && !device.discontinuity &&
            device.epoch==g_hardwareEpoch && device.submitted>=device.completed) {
            const uint64_t travelFrames=((now-sampledBefore+serviceNs)*MIXER_HW_RATE+999999999)/1000000000;
            if (device.submitted-device.completed>=12288+travelFrames*MIXER_FRAME_BYTES) {
                frames=g_testCommitQuantum;
                ++g_testCommitCount;
                // Captured proposal owns remainder clearing even if a test
                // changes configuration while the driver copies it.
                prefix={~uint64_t(0),normalFrames,frames};
            }
        }
    }
#endif
    // Unsubmitted surplus is still in the source FIFO, not owned by this
    // short proposal. Keep the ordinary remainder bound so a low-headroom
    // retry can restore ordinary batching after rejection/partial acceptance.
    g_batchRemaining = normalFrames;
    for (uint32_t slot = 0; slot < MIXER_MAX_STREAMS; ++slot) {
        const auto& stream = g_streams[slot];
        if (prepared) prepared[slot]={};
        if (!stream.identity || !stream.enabled || stream.discontinuity) continue;
        uint32_t count = static_cast<uint32_t>(stream.write - stream.read);
        if (count > frames) count = frames;
        contributions[slot] = {stream.identity, stream.epoch, stream.read, count};
        // An opened-but-never-written endpoint is not a producer. A previously
        // written drained-open endpoint remains observationally included; its
        // missing frames are not evidence that the app currently demands audio.
        if (prepared && stream.submittedFrames)
            prepared[slot]={stream.identity,stream.epoch,count,
                static_cast<uint32_t>(stream.write-stream.read),stream.inFlight,stream.capacity};
    }
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t left = 0, right = 0;
        for (uint32_t slot = 0; slot < MIXER_MAX_STREAMS; ++slot) {
            const auto& part = contributions[slot];
            if (i >= part.frames) continue;
            const uint32_t index = static_cast<uint32_t>(part.read + i) & MIXER_BUF_MASK;
            left += g_streams[slot].samples[index * 2];
            right += g_streams[slot].samples[index * 2 + 1];
        }
        if (left > 32767) left = 32767; else if (left < -32768) left = -32768;
        if (right > 32767) right = 32767; else if (right < -32768) right = -32768;
        g_flushBuf[i * 2] = static_cast<int16_t>(left);
        g_flushBuf[i * 2 + 1] = static_cast<int16_t>(right);
    }
    return frames;
}
// The sole flush owner owns the bounded completion ledger. Reserve before
// submitting; hardware acceptance can never outrun its ownership metadata.
static void Reconcile(const AudioDeviceProgress& device)
{
    SpinLockAcquire(&g_mixerLock);
    for (uint32_t n = 0; n < g_completionCount; ++n) {
        auto& entry = g_completions[(g_completionHead + n) % COMPLETION_SLOTS];
        const uint32_t total = entry.receipt.accepted / MIXER_FRAME_BYTES;
        const bool sameEpoch = entry.receipt.epoch == device.epoch;
        uint32_t proven = 0;
        if (sameEpoch && device.completed > entry.receipt.begin) {
            const uint64_t frames = (device.completed - entry.receipt.begin) / MIXER_FRAME_BYTES;
            proven = frames < total ? static_cast<uint32_t>(frames) : total;
        }
        // A backwards snapshot is not new completion evidence.
        if (proven < entry.credited) proven = entry.credited;
        const bool lost = !sameEpoch || device.discontinuity;
        for (const auto& part : entry.parts) {
            auto* stream = FindStream(part.identity);
            if (!stream || stream->epoch != part.epoch) continue;
            const uint32_t before = entry.credited < part.frames ? entry.credited : part.frames;
            const uint32_t after = proven < part.frames ? proven : part.frames;
            stream->inFlight -= after - before;
            stream->completedFrames += after - before;
            if (lost && part.frames > after) {
                stream->inFlight -= part.frames - after;
                stream->discontinuity = true;
            }
        }
        entry.credited = lost ? total : proven;
    }
    while (g_completionCount) {
        auto& entry = g_completions[g_completionHead];
        if (entry.credited != entry.receipt.accepted / MIXER_FRAME_BYTES) break;
        g_completionHead = (g_completionHead + 1) % COMPLETION_SLOTS;
        --g_completionCount;
    }
    SpinLockRelease(&g_mixerLock);
}
static void RefreshDevice(uint64_t epoch, AudioDeviceProgress* fresh = nullptr)
{
    if (fresh) *fresh = {};
    if (g_flushEpoch != epoch) {
        g_batchRemaining = g_partialOffset = g_completionHead = g_completionCount = 0;
        g_driverMode = DriverMode::Unknown;
        g_flushEpoch = epoch;
    }
    if (!g_audioDriver || g_driverMode == DriverMode::Legacy) return;
    AudioDeviceProgress device{};
    const bool supported = g_audioDriver->trySubmit && g_audioDriver->progress && g_audioDriver->restart &&
                           g_audioDriver->progress(&device);
    if (__atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE) != epoch) return;
    if (!supported) {
        if (g_driverMode == DriverMode::Native) {
            // Previously clocked data cannot silently switch to an unclocked
            // path. Preserve only already proven completion and report loss.
            device.epoch = g_hardwareEpoch; device.discontinuity = true;
            Reconcile(device);
            return;
        }
        g_driverMode = DriverMode::Legacy;
        SerialPuts("audio: native DMA clock unavailable; using legacy delivery\n");
        SpinLockAcquire(&g_mixerLock); g_clockSupported = false; SpinLockRelease(&g_mixerLock);
        return;
    }
    if (g_driverMode != DriverMode::Native) SerialPuts("audio: native DMA completion accounting enabled\n");
    g_driverMode = DriverMode::Native;
    g_hardwareEpoch = device.epoch;
    SpinLockAcquire(&g_mixerLock); g_clockSupported = true; SpinLockRelease(&g_mixerLock);
    Reconcile(device);
    if (fresh) *fresh = device;
    // The terminal snapshot was reconciled before retiring this epoch. A failed
    // restart leaves the driver's fault and its last proven watermark latched.
    if (device.discontinuity) g_audioDriver->restart(device.epoch);
}
void AudioMixerRefresh()
{
    if (__atomic_exchange_n(&g_flushBusy, 1u, __ATOMIC_ACQUIRE)) return;
    RefreshDevice(__atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE));
    __atomic_store_n(&g_flushBusy, 0u, __ATOMIC_RELEASE);
}
bool AudioMixerGetProgress(AudioStreamId identity, AudioStreamProgress* result)
{
    if (!result) return false;
    SpinLockAcquire(&g_mixerLock);
    auto* stream = FindStream(identity);
    if (stream) *result = {stream->epoch, stream->submittedFrames, stream->completedFrames,
        static_cast<uint32_t>(stream->write - stream->read) + stream->inFlight,
        g_clockSupported, stream->discontinuity, stream->capacity, stream->writableFrames};
    SpinLockRelease(&g_mixerLock);
    return stream != nullptr;
}
void AudioMixerDiagnosticSnapshot(AudioMixerDiagnostic* result)
{
    if (!result) return;
    *result = {};
    result->clockNs = AudioClockNowNs();
    SpinLockAcquire(&g_mixerLock);
    result->ready = g_mixerReady;
    result->clocked = g_clockSupported;
    for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
        const auto& stream = g_streams[i];
        if (!stream.identity) continue;
        result->streams[i] = {stream.identity, stream.epoch,
            stream.submittedFrames, stream.completedFrames,
            static_cast<uint32_t>(stream.write - stream.read), stream.inFlight,
            stream.capacity, stream.enabled, stream.discontinuity};
    }
    SpinLockRelease(&g_mixerLock);
}

// Called under the mixer lock, before preparing or consuming any samples.
// The native device snapshot was taken outside this lock by this flush owner.
static bool CoalesceRefill(const AudioDeviceProgress& device, uint64_t sampledBefore)
{
    if (!g_coalesce.enabled) return false;
    constexpr uint64_t graceNs = 10000000; // one nominal service opportunity
    const uint64_t now = AudioClockNowNs();
    if (g_coalescePending) {
        // Never defer two consecutive opportunities, even if another stream
        // becomes empty or this invocation arrives long after the deadline.
        CoalesceCount(g_coalesce.forcedResumes);
        if (now && now > g_coalesceDeadline) CoalesceCount(g_coalesce.lateResumes);
        for (const auto& part : g_coalesceMissing) {
            auto* stream = FindStream(part.identity);
            if (stream && stream->epoch == part.epoch && stream->enabled &&
                !stream->discontinuity && stream->write > stream->read)
                CoalesceCount(g_coalesce.recoveredContributors);
        }
        CoalesceClearPending();
        return false;
    }
    bool ready = false, missing = false;
    for (const auto& stream : g_streams) {
        if (!stream.identity || !stream.enabled || stream.discontinuity) continue;
        ready |= stream.write > stream.read;
        missing |= stream.write == stream.read && stream.inFlight && !stream.coalesceUsed;
    }
    if (!ready || !missing) return false;
    if (g_driverMode != DriverMode::Native || strcmp(g_audioDriver->name, "intel_hda")) {
        CoalesceCount(g_coalesce.unsupportedDriver); return false;
    }
    if (!sampledBefore || !now || now < sampledBefore || now - sampledBefore > graceNs ||
        now > ~uint64_t(0) - graceNs) {
        CoalesceCount(g_coalesce.invalidClock); return false;
    }
    // Fixed-HDA experiment only: retain the full unchanged 12-KiB safe write
    // lead plus sample age and one prospective 10-ms interval. This is an
    // opportunity bound, not a promise that the thread will run within 10 ms.
    const uint64_t travelNs = now - sampledBefore + graceNs;
    const uint64_t travelFrames = (travelNs * MIXER_HW_RATE + 999999999) / 1000000000;
    const uint64_t requiredBytes = 12288 + travelFrames * MIXER_FRAME_BYTES;
    if (!device.running || device.discontinuity || device.epoch != g_hardwareEpoch ||
        device.submitted < device.completed || device.submitted - device.completed < requiredBytes) {
        CoalesceCount(g_coalesce.headroomBypasses); return false;
    }
    for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
        auto& stream = g_streams[i];
        if (stream.identity && stream.enabled && !stream.discontinuity &&
            stream.write == stream.read && stream.inFlight && !stream.coalesceUsed) {
            stream.coalesceUsed = true;
            g_coalesceMissing[i] = {stream.identity, stream.epoch, 0, 0};
        }
    }
    g_coalescePending = true;
    g_coalesceDeadline = now + graceNs;
    CoalesceCount(g_coalesce.deferrals);
    return true;
}
void AudioMixerFlush()
{
    if (__atomic_exchange_n(&g_flushBusy, 1u, __ATOMIC_ACQUIRE)) return;
    struct FlushOwner { ~FlushOwner() { __atomic_store_n(&g_flushBusy, 0u, __ATOMIC_RELEASE); } } owner;
    const uint64_t epoch = __atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE);
    // Default-off avoids any additional clock reads on the normal path.
    bool controlled = __atomic_load_n(&g_coalesceActive, __ATOMIC_ACQUIRE) ||
                            __atomic_load_n(&g_sharedPrefixActive, __ATOMIC_ACQUIRE);
#ifdef BROOK_HOST_TEST
    controlled = controlled || g_testCommitQuantum;
#endif
    const uint64_t sampledBefore = controlled ? AudioClockNowNs() : 0;
    AudioDeviceProgress device{};
    RefreshDevice(epoch, controlled ? &device : nullptr);
    Contribution contributions[MIXER_MAX_STREAMS]{};
    AudioMixPreparedStream prepared[MIXER_MAX_STREAMS]; // initialized only when recording
    const bool partial = g_partialOffset != 0;
    SpinLockAcquire(&g_mixerLock);
    if (!g_mixerReady || !g_audioDriver || !g_audioDriver->play) {
        SpinLockRelease(&g_mixerLock); return;
    }
    if (!partial && CoalesceRefill(device, sampledBefore)) {
        SpinLockRelease(&g_mixerLock); return;
    }
    const uint64_t contributionToken=g_driverMode==DriverMode::Native?g_mixContributions.Token():0;
    SharedPrefixAttempt prefix;
    const uint32_t frames = partial ? 0 : Prepare(contributions,(contributionToken&1)?prepared:nullptr,device,sampledBefore,prefix);
    SpinLockRelease(&g_mixerLock);
    if (!partial && !frames) return;
    if (__atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE) != epoch) {
        g_batchRemaining = g_partialOffset = 0; return;
    }
    if (g_driverMode == DriverMode::Native) {
        if (g_completionCount == COMPLETION_SLOTS) return;
        const auto receipt = g_audioDriver->trySubmit(g_flushBuf, frames * MIXER_FRAME_BYTES);
        if (__atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE) != epoch) return;
        if (!receipt.accepted) return;
        if (receipt.accepted > frames * MIXER_FRAME_BYTES || receipt.accepted % MIXER_FRAME_BYTES ||
            receipt.begin % MIXER_FRAME_BYTES) {
            SerialPuts("audio: invalid native receipt\n");
            AudioStop(); return;
        }
        const uint32_t acceptedFrames = receipt.accepted / MIXER_FRAME_BYTES;
        SpinLockAcquire(&g_mixerLock);
        if(prefix.token && prefix.token==g_sharedPrefix.epoch && g_sharedPrefix.enabled){
            PrefixAdd(g_sharedPrefix.shortenedReceipts);
            PrefixAdd(g_sharedPrefix.acceptedPrefixFrames,acceptedFrames);
            // This is planned/requested surplus, not counterfactual driver
            // acceptance or proven avoided missing contributions.
            PrefixAdd(g_sharedPrefix.requestedTailFrames,prefix.normal-prefix.requested);
        }
        if (contributionToken&1)
            g_mixContributions.Observe(contributionToken,frames,acceptedFrames,prepared,MIXER_MAX_STREAMS);
        auto& entry = g_completions[(g_completionHead + g_completionCount) % COMPLETION_SLOTS];
        entry.receipt = receipt; entry.credited = 0;
        for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
            entry.parts[i] = contributions[i];
            if (entry.parts[i].frames > acceptedFrames) entry.parts[i].frames = acceptedFrames;
        }
        ++g_completionCount;
        Consume(contributions, acceptedFrames, true);
        g_batchRemaining -= acceptedFrames;
        // A fully accepted shortened proposal is complete. Its unsubmitted
        // surplus has no ownership claim on a later batch. This decision uses
        // the captured attempt, even if controls changed during the copy.
        if(prefix.token && acceptedFrames==prefix.requested)g_batchRemaining=0;
        SpinLockRelease(&g_mixerLock);
        return;
    }
    const uint32_t bytes = partial ? MIXER_FRAME_BYTES - g_partialOffset : frames * MIXER_FRAME_BYTES;
    const void* data = partial ? static_cast<const void*>(g_partialFrame + g_partialOffset) : g_flushBuf;
    const int accepted = g_audioDriver->play(data, bytes,
        MIXER_HW_RATE, MIXER_HW_CHANNELS, MIXER_HW_BITS, false);
    if (__atomic_load_n(&g_deliveryEpoch, __ATOMIC_ACQUIRE) != epoch) {
        g_batchRemaining = g_partialOffset = 0; return;
    }
    if (accepted <= 0) return;
    if (static_cast<uint32_t>(accepted) > bytes) {
        SerialPrintf("audio: driver returned %d for %u pending bytes\n", accepted, bytes); return;
    }
    SpinLockAcquire(&g_mixerLock);
    if (partial) {
        g_partialOffset += static_cast<uint32_t>(accepted);
        if (g_partialOffset == MIXER_FRAME_BYTES) {
            Consume(g_partialContributions, 1);
            g_partialOffset = 0; --g_batchRemaining;
        }
    } else {
        const uint32_t whole = static_cast<uint32_t>(accepted) / MIXER_FRAME_BYTES;
        Consume(contributions, whole);
        g_batchRemaining -= whole;
        const uint32_t tail = static_cast<uint32_t>(accepted) % MIXER_FRAME_BYTES;
        if (tail) {
            memcpy(g_partialFrame, reinterpret_cast<uint8_t*>(g_flushBuf) + whole * MIXER_FRAME_BYTES, MIXER_FRAME_BYTES);
            g_partialOffset = tail;
            for (uint32_t i = 0; i < MIXER_MAX_STREAMS; ++i) {
                const auto& part = contributions[i];
                g_partialContributions[i] = part.frames > whole
                    ? Contribution{part.identity, part.epoch, part.read + whole, 1} : Contribution{};
            }
        }
    }
    SpinLockRelease(&g_mixerLock);
}

void AudioMixerTick()
{
    if (!g_mixerReady) return;
    AudioMixerFlush();
    if (g_audioDriver && g_audioDriver->service)
        g_audioDriver->service();
}

// --- Driver registration ---

extern "C" bool AudioRegister(const AudioDriver* drv)
{
    if (g_audioDriver)
    {
        SerialPrintf("audio: driver '%s' already registered, rejecting '%s'\n",
                     g_audioDriver->name, drv->name);
        return false;
    }
    g_audioDriver = drv;
    KPrintf("audio: registered driver '%s'\n", drv->name);
    return true;
}

bool AudioAvailable()
{
    return g_audioDriver != nullptr;
}

int AudioPlay(const void* samples, uint32_t byteCount,
              uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample,
              bool nonblock)
{
    if (!g_audioDriver || !g_audioDriver->play) return -1;
    return g_audioDriver->play(samples, byteCount, sampleRate, channels, bitsPerSample, nonblock);
}

void AudioStop()
{
    // Invalidate retained delivery at both boundaries: a batch can be formed
    // while the sleeping driver stop is in progress. Only its flush owner
    // mutates the batch; this does not retract an in-flight hardware write.
    __atomic_fetch_add(&g_deliveryEpoch, 1ULL, __ATOMIC_ACQ_REL);
    ResetAllStreams();
    if (g_audioDriver && g_audioDriver->stop)
        g_audioDriver->stop();
    ResetAllStreams();
    __atomic_fetch_add(&g_deliveryEpoch, 1ULL, __ATOMIC_ACQ_REL);
}

bool AudioIsPlaying()
{
    if (!g_audioDriver || !g_audioDriver->isPlaying) return false;
    return g_audioDriver->isPlaying();
}

uint32_t AudioGetPosition()
{
    if (!g_audioDriver || !g_audioDriver->getPosition) return 0;
    return g_audioDriver->getPosition();
}

// --- Mixer kernel thread ---
// Wakes every 10ms, flushes accumulated mix data to hardware.

static void AudioMixerThreadFn(void* /*arg*/)
{
    SerialPuts("audio: mixer thread running\n");
    for (;;)
    {
        const uint64_t timingToken=__atomic_load_n(&g_audioTimingEpoch,__ATOMIC_ACQUIRE);
        const uint64_t beforeSleep=(timingToken&1)?AudioClockNowNs():0;
        // Sleep ~10ms
        Process* self = ProcessCurrent();
        self->wakeupTick = ApicTickCount() + AudioServicePeriodNs/1000000;
        const uint64_t afterDeadline=(timingToken&1)?AudioClockNowNs():0;
        SchedulerBlock(self);
        const uint64_t wake=(timingToken&1)?AudioClockNowNs():0;

        // Flush whatever has accumulated
        const bool worked=g_mixerReady && g_audioDriver && g_audioDriver->play;
        if (worked)
            AudioMixerTick();
        if(timingToken&1)AudioServiceTimingRecord(timingToken,beforeSleep,afterDeadline,
            wake,AudioClockNowNs(),worked);
    }
}

void AudioMixerThreadStart()
{
    Process* thread = KernelThreadCreate("audio_mix", AudioMixerThreadFn, nullptr);
    if (!thread)
    {
        SerialPuts("audio: failed to create mixer thread\n");
        return;
    }
    SpinLockAcquire(&g_audioTimingLock);g_audioTiming.Worker(thread->pid,thread->incarnation);SpinLockRelease(&g_audioTimingLock);
    SchedulerAddProcess(thread);
    SerialPrintf("audio: mixer thread started (pid %u)\n", thread->pid);
}

} // namespace brook
