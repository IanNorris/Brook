#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "audio.h"

namespace brook {
extern "C" void* kmalloc(uint64_t size) { return std::malloc(size); }
extern "C" void kfree(void* memory) { std::free(memory); }
extern "C" void SerialPrintf(const char*,...) {}
extern "C" void SerialPuts(const char*) {}
extern "C" void KPrintf(const char*,...) {}
}
namespace {
std::vector<unsigned char> output;
std::mutex outputLock;
std::atomic<int> limit{-1},calls{0};
std::atomic<bool> blockFirst{false},entered{false},releaseFirst{false};
std::atomic<bool> blockStop{false},stopEntered{false},releaseStop{false};
std::atomic<int> failures{0};
brook::AudioStreamId primary=0,secondary=0;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);++failures; } } while(0)
int play(const void* data,uint32_t bytes,uint32_t rate,uint8_t channels,uint8_t bits,bool) {
    CHECK(rate==44100 && channels==2 && bits==16);
    int call=calls.fetch_add(1);
    if(blockFirst.load()) {
        if(call) { CHECK(false);return 0; }
        entered.store(true);
        while(!releaseFirst.load())std::this_thread::yield();
    }
    int amount=limit.load();
    if(amount==-1)amount=static_cast<int>(bytes);
    if(amount<0)return amount;
    amount=std::min(amount,static_cast<int>(bytes));
    auto* first=static_cast<const unsigned char*>(data);
    std::lock_guard<std::mutex> held(outputLock);
    output.insert(output.end(),first,first+amount);
    return amount;
}
void stop() {
    if(blockStop.load()) {
        stopEntered=true;
        while(!releaseStop.load())std::this_thread::yield();
    }
}
std::vector<unsigned char> raw(const std::vector<int16_t>& samples) {
    auto* first=reinterpret_cast<const unsigned char*>(samples.data());
    return {first,first+samples.size()*sizeof(int16_t)};
}
void finish(size_t bytes) {
    limit=-1;
    for(unsigned i=0;i<128 && output.size()<bytes;++i)brook::AudioMixerFlush();
    CHECK(output.size()==bytes);
}
bool partialAndErrors() {
    std::vector<int16_t> samples;
    for(int i=0;i<200;++i)samples.push_back(static_cast<int16_t>(i*37-3000));
    auto expected=raw(samples);output.clear();
    CHECK(brook::AudioMixerSubmit(samples.data(),samples.size()/2,primary)==samples.size()/2);
    limit=28;brook::AudioMixerFlush();CHECK(output.size()==28);
    limit=7;brook::AudioMixerFlush();CHECK(output.size()==35);
    limit=0;brook::AudioMixerFlush();CHECK(output.size()==35);
    limit=-11;brook::AudioMixerFlush();CHECK(output.size()==35);
    finish(expected.size());CHECK(output==expected);
    return !failures;
}
void diagnosticSnapshot() {
    using namespace brook;
    AudioMixerDiagnosticSnapshot(nullptr);
    auto probe = AudioMixerOpen(); CHECK(probe);
    std::vector<int16_t> samples(64, 123);
    CHECK(AudioMixerSubmit(samples.data(), 32, probe) == 32);
    CHECK(AudioMixerSetEnabled(probe, false));
    const auto available = AudioMixerAvailableFrames(probe);
    const auto driverCalls = calls.load();
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        AudioMixerDiagnostic snapshot{};
        AudioMixerDiagnosticSnapshot(&snapshot);
        CHECK(snapshot.ready);
        unsigned found = 0;
        for (const auto& row : snapshot.streams) {
            if (row.identity != probe) continue;
            ++found;
            CHECK(!row.enabled && !row.discontinuity);
            CHECK(row.submittedFrames == 32 && row.completedFrames == 0);
            CHECK(row.softwareFrames == 32 && row.inFlightFrames == 0);
            CHECK(row.capacityFrames == MIXER_RING_FRAMES);
        }
        CHECK(found == 1);
        CHECK(AudioMixerAvailableFrames(probe) == available);
        CHECK(calls.load() == driverCalls); // Observation never invokes driver.
    }
    CHECK(AudioMixerClose(probe));
    auto replacement = AudioMixerOpen(); CHECK(replacement && replacement != probe);
    AudioMixerDiagnostic snapshot{};
    AudioMixerDiagnosticSnapshot(&snapshot);
    unsigned found = 0;
    for (const auto& row : snapshot.streams) {
        CHECK(row.identity != probe);
        if (row.identity == replacement) {
            ++found;
            CHECK(!row.submittedFrames && !row.completedFrames && !row.softwareFrames);
        }
    }
    CHECK(found == 1);
    CHECK(AudioMixerClose(replacement));
}
void writableThreshold() {
    using namespace brook;
    auto stream=AudioMixerOpen();CHECK(stream);
    CHECK(AudioMixerSetCapacity(stream,256,64));
    std::vector<int16_t> samples(128,42);
    CHECK(AudioMixerSubmit(samples.data(),64,stream)==64);
    AudioStreamProgress before{},after{};
    CHECK(AudioMixerGetProgress(stream,&before));
    CHECK(before.capacityFrames==256 && before.writableFrames==64);
    CHECK(AudioMixerSetWritableFrames(stream,193));
    CHECK(AudioMixerGetProgress(stream,&after));
    CHECK(after.writableFrames==193 && after.queuedFrames==before.queuedFrames);
    CHECK(after.epoch==before.epoch && after.submittedFrames==before.submittedFrames &&
          after.completedFrames==before.completedFrames);
    CHECK(after.capacityFrames-after.queuedFrames<after.writableFrames);
    CHECK(AudioMixerSetWritableFrames(stream,192));
    CHECK(AudioMixerGetProgress(stream,&after));
    CHECK(after.capacityFrames-after.queuedFrames>=after.writableFrames);
    CHECK(!AudioMixerSetWritableFrames(stream,0));
    CHECK(!AudioMixerSetWritableFrames(stream,257));
    CHECK(AudioMixerGetProgress(stream,&after) && after.writableFrames==192);
    CHECK(AudioMixerReset(stream));
    CHECK(AudioMixerGetProgress(stream,&after) && after.writableFrames==192 && !after.queuedFrames);
    CHECK(AudioMixerSetCapacity(stream,128,32));
    CHECK(AudioMixerGetProgress(stream,&after) && after.writableFrames==32);
    CHECK(AudioMixerClose(stream));CHECK(!AudioMixerSetWritableFrames(stream,1));
}
void masterVolume() {
    using namespace brook;
    CHECK(AudioOutputControl()==BROOK_AUDIO_DEFAULT);
    for(auto bad:{101u,127u,128u,0x200u,0xffffffffu}){
        CHECK(!AudioSetOutputControl(bad));CHECK(AudioOutputControl()==BROOK_AUDIO_DEFAULT);
    }
    CHECK(brook_audio_control_setting(42,"output=356\r")==356);
    CHECK(brook_audio_control_setting(42,"output=357")==42);
    CHECK(brook_audio_control_setting(42,"output=100oops")==42);
    CHECK(brook_audio_control_setting(42,"output=00000")==42);
    const std::vector<int16_t> source={-32768,32767,-10001,10001};
    for(auto setting:{0u,25u,50u,100u,static_cast<unsigned>(BROOK_AUDIO_MUTED|75)}){
        CHECK(AudioSetOutputControl(setting));output.clear();limit=-1;
        CHECK(AudioMixerSubmit(source.data(),2,primary)==2);finish(8);
        std::vector<int16_t> expected;
        for(auto s:source)expected.push_back(brook_audio_scale(s,setting));
        CHECK(output==raw(expected));
        output.clear();
        CHECK(AudioPlay(source.data(),8,44100,2,16,true)==8);
        CHECK(output==raw(expected));
    }
    // Attenuate the sum before saturation, retaining quiet multi-app detail.
    CHECK(AudioSetOutputControl(50));output.clear();
    const std::vector<int16_t> loud={30000,-30000};
    CHECK(AudioMixerSubmit(loud.data(),1,primary)==1);
    CHECK(AudioMixerSubmit(loud.data(),1,secondary)==1);finish(4);
    CHECK(output==raw(loud));
    CHECK(source==std::vector<int16_t>({-32768,32767,-10001,10001}));
    CHECK(AudioSetOutputControl(BROOK_AUDIO_DEFAULT));output.clear();
}
void concurrentFlush() {
    std::vector<int16_t> first(80,1234),second(60,-2345);
    auto expected=raw(first),tail=raw(second);expected.insert(expected.end(),tail.begin(),tail.end());
    output.clear();calls=0;entered=false;releaseFirst=false;blockFirst=true;limit=4;
    CHECK(brook::AudioMixerSubmit(first.data(),first.size()/2,primary)==first.size()/2);
    std::thread writer([] { brook::AudioMixerFlush(); });
    while(!entered.load())std::this_thread::yield();
    CHECK(brook::AudioMixerSubmit(second.data(),second.size()/2,primary)==second.size()/2);
    std::thread racingFlush([] { brook::AudioMixerFlush(); });racingFlush.join();
    releaseFirst=true;writer.join();blockFirst=false;
    finish(expected.size());CHECK(output==expected);
}
void stopInvalidatesSuffix() {
    std::vector<int16_t> samples(100,5678);
    output.clear();limit=4;
    CHECK(brook::AudioMixerSubmit(samples.data(),50,primary)==50);
    brook::AudioMixerFlush();CHECK(output.size()==4);
    brook::AudioStop();limit=-1;brook::AudioMixerFlush();CHECK(output.size()==4);

    // A batch created while stop is sleeping must also expire at completion.
    output.clear();blockStop=true;stopEntered=false;releaseStop=false;limit=4;
    std::thread stopping([] { brook::AudioStop(); });
    while(!stopEntered.load())std::this_thread::yield();
    CHECK(brook::AudioMixerSubmit(samples.data(),50,primary)==50);
    brook::AudioMixerFlush();CHECK(output.size()==4);
    releaseStop=true;stopping.join();blockStop=false;
    limit=-1;brook::AudioMixerFlush();CHECK(output.size()==4);

    // Hardware acceptance that was already in flight cannot be retracted,
    // but its unaccepted suffix must not be retried after that stop.
    output.clear();calls=0;blockFirst=true;entered=false;releaseFirst=false;limit=4;
    CHECK(brook::AudioMixerSubmit(samples.data(),50,primary)==50);
    std::thread writing([] { brook::AudioMixerFlush(); });
    while(!entered.load())std::this_thread::yield();
    brook::AudioStop();releaseFirst=true;writing.join();blockFirst=false;
    limit=-1;brook::AudioMixerFlush();CHECK(output.size()==4);
}
void ringAndClamping() {
    output.clear();std::vector<unsigned char> expected;
    for(unsigned batch=0;batch<20;++batch) {
        std::vector<int16_t> samples(8000);
        for(unsigned i=0;i<samples.size();++i)samples[i]=static_cast<int16_t>(batch*109+i*13);
        auto bytes=raw(samples);expected.insert(expected.end(),bytes.begin(),bytes.end());
        CHECK(brook::AudioMixerSubmit(samples.data(),samples.size()/2,primary)==samples.size()/2);
        limit=1000;brook::AudioMixerFlush();finish(expected.size());
    }
    CHECK(output==expected);
    output.clear();std::vector<int16_t> left(100),right(100),mixed(100);
    for(unsigned i=0;i<100;++i) {left[i]=right[i]=i%2?-30000:30000;mixed[i]=i%2?-32768:32767;}
    CHECK(brook::AudioMixerSubmit(left.data(),50,primary)==50);
    CHECK(brook::AudioMixerSubmit(right.data(),50,secondary)==50);
    finish(200);CHECK(output==raw(mixed));
}
void ownedQueues() {
    using namespace brook;
    std::vector<int16_t> a(8,30000), b(12,10000);
    AudioStop(); output.clear(); limit=4;
    CHECK(AudioMixerSubmit(a.data(),4,primary)==4);
    CHECK(AudioMixerSubmit(b.data(),6,secondary)==6);
    AudioMixerFlush();
    CHECK(output==raw(std::vector<int16_t>(2,32767)));
    // Unaccepted clipped frames must be remixed, not subtracted after clipping.
    CHECK(AudioMixerReset(primary));
    finish(24);
    auto expected=raw(std::vector<int16_t>(2,32767));
    auto rest=raw(std::vector<int16_t>(10,10000));
    expected.insert(expected.end(),rest.begin(),rest.end());CHECK(output==expected);

    AudioStop(); output.clear(); calls=0; limit=4;
    CHECK(AudioMixerSubmit(a.data(),4,primary)==4);
    CHECK(AudioMixerSubmit(b.data(),6,secondary)==6);
    blockFirst=true;entered=false;releaseFirst=false;
    std::thread flushing([] { AudioMixerFlush(); });
    while(!entered.load())std::this_thread::yield();
    auto stale=primary; CHECK(AudioMixerClose(stale));primary=AudioMixerOpen();
    CHECK(primary && primary!=stale);
    CHECK(!AudioMixerClose(stale) && !AudioMixerReset(stale));
    CHECK(!AudioMixerSubmit(a.data(),4,stale));
    std::vector<int16_t> replacement(4,-2000);
    CHECK(AudioMixerSubmit(replacement.data(),2,primary)==2);
    releaseFirst=true;flushing.join();blockFirst=false;
    CHECK(AudioMixerAvailableFrames(primary)==MIXER_RING_FRAMES-2);
    finish(24);
    expected=raw(std::vector<int16_t>(2,32767));
    rest=raw(std::vector<int16_t>(4,8000));expected.insert(expected.end(),rest.begin(),rest.end());
    rest=raw(std::vector<int16_t>(6,10000));expected.insert(expected.end(),rest.begin(),rest.end());
    CHECK(output==expected);

    // Pause while a driver call is in flight still consumes accepted frames.
    AudioStop();output.clear();calls=0;limit=4;blockFirst=true;entered=false;releaseFirst=false;
    CHECK(AudioMixerSubmit(b.data(),6,primary)==6);
    std::thread pausing([] { AudioMixerFlush(); });
    while(!entered.load())std::this_thread::yield();
    CHECK(AudioMixerSetEnabled(primary,false));
    releaseFirst=true;pausing.join();blockFirst=false;
    CHECK(AudioMixerAvailableFrames(primary)==MIXER_RING_FRAMES-5);
    limit=-1;AudioMixerFlush();CHECK(output.size()==4);
    CHECK(AudioMixerSetEnabled(primary,true));finish(24);CHECK(output==raw(b));

    // A sub-frame tail is immutable across close/reuse, with no consumption
    // charged to the replacement generation when the old frame finishes.
    AudioStop();output.clear();limit=1;
    CHECK(AudioMixerSubmit(a.data(),4,primary)==4);
    AudioMixerFlush();auto old=primary;
    CHECK(AudioMixerClose(old));primary=AudioMixerOpen();
    CHECK(primary && primary!=old);
    CHECK(AudioMixerSubmit(replacement.data(),2,primary)==2);
    limit=-1;AudioMixerFlush();CHECK(output.size()==4);
    CHECK(AudioMixerAvailableFrames(primary)==MIXER_RING_FRAMES-2);
    finish(12);
    expected=raw(std::vector<int16_t>(2,30000));rest=raw(replacement);
    expected.insert(expected.end(),rest.begin(),rest.end());CHECK(output==expected);

    // Completing an immutable partial frame while paused must consume that
    // frame once, leaving only its unaccepted successors for resume.
    AudioStop();output.clear();limit=1;
    CHECK(AudioMixerSubmit(b.data(),6,primary)==6);
    AudioMixerFlush();CHECK(AudioMixerSetEnabled(primary,false));
    limit=-1;AudioMixerFlush();CHECK(output.size()==4);
    CHECK(AudioMixerAvailableFrames(primary)==MIXER_RING_FRAMES-5);
    AudioMixerFlush();CHECK(output.size()==4);
    CHECK(AudioMixerSetEnabled(primary,true));finish(24);CHECK(output==raw(b));

    // Reset preserves identity but changes its commit epoch, protecting new
    // samples from completion of the old partially accepted frame.
    AudioStop();output.clear();limit=1;
    CHECK(AudioMixerSubmit(a.data(),4,primary)==4);AudioMixerFlush();
    CHECK(AudioMixerReset(primary));
    CHECK(AudioMixerSubmit(replacement.data(),2,primary)==2);
    limit=-1;AudioMixerFlush();
    CHECK(AudioMixerAvailableFrames(primary)==MIXER_RING_FRAMES-2);
    finish(12);expected=raw(std::vector<int16_t>(2,30000));rest=raw(replacement);
    expected.insert(expected.end(),rest.begin(),rest.end());CHECK(output==expected);

    AudioStop();output.clear();
    std::vector<AudioStreamId> handles;
    for(unsigned i=2;i<MIXER_MAX_STREAMS;++i) {auto id=AudioMixerOpen();CHECK(id);handles.push_back(id);}
    CHECK(!AudioMixerOpen());
    for(auto id:handles)CHECK(AudioMixerClose(id));
    auto recycled=AudioMixerOpen();CHECK(recycled);CHECK(AudioMixerClose(recycled));
    std::vector<int16_t> full(MIXER_RING_FRAMES*2,37);
    CHECK(AudioMixerSubmit(full.data(),MIXER_RING_FRAMES,primary)==MIXER_RING_FRAMES);
    CHECK(AudioMixerAvailableFrames(primary)==0);
    CHECK(!AudioMixerSubmit(a.data(),1,primary));
    limit=0;AudioMixerFlush();CHECK(AudioMixerAvailableFrames(primary)==0);
    limit=-11;AudioMixerFlush();CHECK(AudioMixerAvailableFrames(primary)==0);
    finish(full.size()*2);CHECK(output==raw(full));
    CHECK(AudioMixerAvailableFrames(primary)==MIXER_RING_FRAMES);
}

}
int RunHdaClockTests();
int RunMixerClockTests();
brook::AudioSubmitReceipt TestMixerNativeSubmit(const void*,uint32_t);
bool TestMixerNativeProgress(brook::AudioDeviceProgress*);
bool TestMixerNativeRestart(uint64_t);
void TestMixerNativeService();
int main() {
    const auto timing=brook::AudioServiceTimingRead();CHECK(timing.epoch==0 && !timing.cycles && !timing.contendedRecords);
    brook::AudioServiceTimingEnable(true);brook::AudioServiceTimingReset();
    CHECK(brook::AudioServiceTimingRead().epoch&1);
    brook::AudioServiceTimingEnable(false);CHECK(!(brook::AudioServiceTimingRead().epoch&1));
    brook::AudioMixerInit();
    const brook::AudioDriver driver{"intel_hda",play,stop,nullptr,nullptr,TestMixerNativeService,
        TestMixerNativeSubmit,TestMixerNativeProgress,TestMixerNativeRestart};
    CHECK(brook::AudioRegister(&driver));
    primary=brook::AudioMixerOpen();secondary=brook::AudioMixerOpen();
    CHECK(primary && secondary && primary!=secondary);
    masterVolume();
    diagnosticSnapshot();
    writableThreshold();
    // The baseline must fail cleanly on lost partial data before concurrency.
    if(!partialAndErrors())return 1;
    concurrentFlush();stopInvalidatesSuffix();ringAndClamping();ownedQueues();
    failures+=RunHdaClockTests();
    failures+=RunMixerClockTests();
    std::printf("AUDIO_DELIVERY %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
