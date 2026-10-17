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
int failures=0;
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
    CHECK(brook::AudioMixerSubmit(samples.data(),samples.size()/2,0)==samples.size()/2);
    limit=28;brook::AudioMixerFlush();CHECK(output.size()==28);
    limit=7;brook::AudioMixerFlush();CHECK(output.size()==35);
    limit=0;brook::AudioMixerFlush();CHECK(output.size()==35);
    limit=-11;brook::AudioMixerFlush();CHECK(output.size()==35);
    finish(expected.size());CHECK(output==expected);
    return !failures;
}
void concurrentFlush() {
    std::vector<int16_t> first(80,1234),second(60,-2345);
    auto expected=raw(first),tail=raw(second);expected.insert(expected.end(),tail.begin(),tail.end());
    output.clear();calls=0;entered=false;releaseFirst=false;blockFirst=true;limit=4;
    CHECK(brook::AudioMixerSubmit(first.data(),first.size()/2,0)==first.size()/2);
    std::thread writer([] { brook::AudioMixerFlush(); });
    while(!entered.load())std::this_thread::yield();
    CHECK(brook::AudioMixerSubmit(second.data(),second.size()/2,0)==second.size()/2);
    std::thread racingFlush([] { brook::AudioMixerFlush(); });racingFlush.join();
    releaseFirst=true;writer.join();blockFirst=false;
    finish(expected.size());CHECK(output==expected);
}
void stopInvalidatesSuffix() {
    std::vector<int16_t> samples(100,5678);
    output.clear();limit=4;
    CHECK(brook::AudioMixerSubmit(samples.data(),50,0)==50);
    brook::AudioMixerFlush();CHECK(output.size()==4);
    brook::AudioStop();limit=-1;brook::AudioMixerFlush();CHECK(output.size()==4);

    // A batch created while stop is sleeping must also expire at completion.
    output.clear();blockStop=true;stopEntered=false;releaseStop=false;limit=4;
    std::thread stopping([] { brook::AudioStop(); });
    while(!stopEntered.load())std::this_thread::yield();
    CHECK(brook::AudioMixerSubmit(samples.data(),50,0)==50);
    brook::AudioMixerFlush();CHECK(output.size()==4);
    releaseStop=true;stopping.join();blockStop=false;
    limit=-1;brook::AudioMixerFlush();CHECK(output.size()==4);

    // Hardware acceptance that was already in flight cannot be retracted,
    // but its unaccepted suffix must not be retried after that stop.
    output.clear();calls=0;blockFirst=true;entered=false;releaseFirst=false;limit=4;
    CHECK(brook::AudioMixerSubmit(samples.data(),50,0)==50);
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
        CHECK(brook::AudioMixerSubmit(samples.data(),samples.size()/2,0)==samples.size()/2);
        limit=1000;brook::AudioMixerFlush();finish(expected.size());
    }
    CHECK(output==expected);
    output.clear();std::vector<int16_t> left(100),right(100),mixed(100);
    for(unsigned i=0;i<100;++i) {left[i]=right[i]=i%2?-30000:30000;mixed[i]=i%2?-32768:32767;}
    CHECK(brook::AudioMixerSubmit(left.data(),50,0)==50);
    CHECK(brook::AudioMixerSubmit(right.data(),50,1)==50);
    finish(200);CHECK(output==raw(mixed));
}
}
int main() {
    brook::AudioMixerInit();
    const brook::AudioDriver driver{"test",play,stop,nullptr,nullptr,nullptr};
    CHECK(brook::AudioRegister(&driver));
    // The baseline must fail cleanly on lost partial data before concurrency.
    if(!partialAndErrors())return 1;
    concurrentFlush();stopInvalidatesSuffix();ringAndClamping();
    std::printf("AUDIO_DELIVERY %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
