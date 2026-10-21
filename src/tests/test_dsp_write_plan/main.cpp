#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../../kernel/src/dsp_write_plan.h"
#include "../../kernel/src/dsp_geometry.h"

#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();} }while(0)
using brook::DspWritePlan;

static void prefixesMatchWholeConversion() {
    for(uint32_t rate:{4000u,8000u,44100u,48000u,96000u,192000u})
    for(uint8_t channels:{1,2})for(uint8_t bits:{8,16}){
        const uint32_t bytes=7000*channels*(bits/8);
        std::vector<int16_t> storage((bytes+1)/2);
        auto* input=reinterpret_cast<uint8_t*>(storage.data());
        for(uint32_t i=0;i<bytes;++i)input[i]=uint8_t(i*37+19);
        DspWritePlan plan;CHECK(plan.Begin(bytes,rate,channels,bits,4096,7,9));
        const uint32_t total=plan.Remaining();
        std::vector<int16_t> whole(total*2),split;
        CHECK(plan.Convert(input,whole.data(),total));
        const uint64_t inputFrames=plan.InputBytes()/(channels*(bits/8));
        // Independent whole-buffer nearest-neighbour reference.
        for(uint32_t i=0;i<total;++i)for(unsigned c=0;c<2;++c){
            const auto src=std::min(uint64_t(i)*rate/44100,inputFrames-1)*channels+(channels==2?c:0);
            const int16_t expected=bits==8?int16_t((int(input[src])-128)*256):storage[src];
            CHECK(whole[i*2+c]==expected);
        }
        unsigned attempt=0;uint64_t previous=0;
        while(plan.Remaining()){
            const uint32_t offered=plan.Offer(1+(attempt++*137)%1024);
            std::vector<int16_t> part(offered*2);CHECK(plan.Convert(input,part.data(),offered));
            // Simulate a driver accepting less than offered. The retry must
            // regenerate the suffix from the original conversion position.
            const uint32_t accepted=std::max(1u,offered/2);
            split.insert(split.end(),part.begin(),part.begin()+accepted*2);
            CHECK(plan.Accept(accepted));CHECK(plan.ConsumedBytes()>=previous);
            previous=plan.ConsumedBytes();CHECK(previous<=plan.InputBytes());
        }
        CHECK(split==whole);CHECK(plan.ConsumedBytes()==plan.InputBytes());
        CHECK(!plan.Accept(1));CHECK(plan.Remaining()==0);
    }
}
static void refillBeforeEmpty() {
    DspWritePlan plan;CHECK(plan.Begin(4096*4,44100,2,16,4096,1,1));
    // The old whole-chunk admission would reject these 1024 free frames even
    // though the queue still contains playable data. The prefix fits now.
    CHECK(plan.Offer(1024)==1024);CHECK(plan.Accept(1024));
    CHECK(plan.ConsumedBytes()==4096 && plan.Remaining()==3072);
    CHECK(plan.Offer(0)==0);CHECK(plan.Accept(0));CHECK(plan.Remaining()==3072);
    CHECK(plan.Offer(99999)==3072);CHECK(plan.Accept(3072));
    CHECK(plan.ConsumedBytes()==4096*4);
}
static void epochsAndBounds() {
    DspWritePlan plan;CHECK(plan.Begin(~uint64_t(0),48000,2,16,16384,11,13));
    CHECK(plan.InputBytes()<=65536 && plan.Remaining()<=16384);
    CHECK(plan.Matches(48000,2,16,11,13));
    CHECK(!plan.Matches(44100,2,16,11,13));CHECK(!plan.Matches(48000,1,16,11,13));
    CHECK(!plan.Matches(48000,2,8,11,13));CHECK(!plan.Matches(48000,2,16,12,13));
    CHECK(!plan.Matches(48000,2,16,11,14));
    CHECK(plan.Accept(3));CHECK(plan.ConsumedBytes()==12);
    const auto remaining=plan.Remaining();CHECK(!plan.Accept(remaining+1));CHECK(plan.Remaining()==remaining);
    int16_t out[2]={123,456};CHECK(!plan.Convert(nullptr,out,1));CHECK(out[0]==123 && out[1]==456);
    CHECK(!plan.Begin(0,44100,2,16,4096,1,1));CHECK(!plan.Active());
    CHECK(!plan.Begin(100,0,2,16,4096,1,1));CHECK(!plan.Begin(100,44100,3,16,4096,1,1));
    CHECK(!plan.Begin(100,44100,2,24,4096,1,1));CHECK(!plan.Begin(100,44100,2,16,0,1,1));
    CHECK(!plan.Begin(100,44100,2,16,16385,1,1));
    CHECK(!plan.Begin(3,44100,2,16,4096,1,1));
}
static void writersRestageSharedBuffer() {
    DspWritePlan a,b;CHECK(a.Begin(16,44100,2,16,4,1,1));CHECK(b.Begin(16,44100,2,16,4,1,1));
    const int16_t sources[2][8]={{1,2,3,4,5,6,7,8},{11,12,13,14,15,16,17,18}};
    int16_t sharedInput[8],sharedOutput[2];std::vector<int16_t> emitted[2];
    for(unsigned frame=0;frame<4;++frame)for(unsigned writer=0;writer<2;++writer){
        auto& plan=writer?b:a;
        std::copy(std::begin(sources[writer]),std::end(sources[writer]),sharedInput);
        CHECK(plan.Convert(reinterpret_cast<const uint8_t*>(sharedInput),sharedOutput,1));
        emitted[writer].insert(emitted[writer].end(),sharedOutput,sharedOutput+2);CHECK(plan.Accept(1));
    }
    for(unsigned i=0;i<2;++i)CHECK(emitted[i]==std::vector<int16_t>(sources[i],sources[i]+8));
}
static void geometryNegotiation() {
    using namespace brook;
    DspGeometry g{};
    CHECK(DspPlanGeometry((4u<<16)|12,4,16384,false,&g));
    CHECK(g.fragments==4 && g.fragmentBytes==4096 && g.totalBytes==16384 && !g.changed && !g.horizonQualified);
    CHECK(DspPlanGeometry((4u<<16)|12,4,16384,true,&g));
    CHECK(g.fragments==4 && g.fragmentBytes==8192 && g.capacityFrames==8192 && g.writableFrames==2048 && g.changed && g.horizonQualified);
    CHECK(DspPlanGeometry((6u<<16)|12,4,16384,true,&g));
    CHECK(g.fragments==4 && g.fragmentBytes==8192 && g.changed && g.horizonQualified);
    CHECK(DspPlanGeometry((2u<<16)|12,4,16384,true,&g));
    CHECK(g.fragments==2 && g.fragmentBytes==32768 && g.capacityFrames==16384 && g.changed && g.horizonQualified);
    CHECK(DspPlanGeometry((69u<<16)|8,4,16384,true,&g));
    CHECK(g.fragments==47 && g.fragmentBytes==512 && g.capacityFrames==6016 && g.writableFrames==128 && g.changed);
    CHECK(DspPlanGeometry((47u<<16)|9,4,16384,true,&g));
    CHECK(g.fragments==47 && g.fragmentBytes==512 && !g.changed && g.horizonQualified);
    CHECK(DspPlanGeometry((1u<<16)|12,4,16384,true,&g));
    CHECK(g.fragments==1 && g.fragmentBytes==4096 && !g.changed && !g.horizonQualified);
    CHECK(!AudioRefillQualified(5987,128) && AudioRefillQualified(5988,128));
    CHECK(!AudioRefillQualified(5120,128) && !AudioRefillQualified(4416,64));
    CHECK(AudioRefillQualified(8192,2048) && !AudioRefillQualified(6144,1024));
    CHECK(!AudioRefillQualified(16384,0) && !AudioRefillQualified(1,2));
    CHECK(AudioRefillRequiredFrames(~uint32_t(0))>~uint32_t(0));
    CHECK(DspPlanGeometry(12,4,16384,true,&g));
    CHECK(g.fragments==16 && g.fragmentBytes==4096 && g.totalBytes==65536);
    for(uint32_t frame:{1u,2u,4u})for(uint32_t count:{0u,1u,2u,3u,4u,6u,16u,47u,65u,69u,65535u})
    for(uint32_t exp:{7u,8u,10u,12u,14u,16u,17u})for(bool enabled:{false,true}) {
        const uint32_t request=(count<<16)|exp;
        const uint32_t originalCount=count?count:16384*frame/(1u<<exp);
        const bool valid=exp>=8 && exp<=16 && originalCount && uint64_t(originalCount)*(1u<<exp)<=16384*frame;
        const DspGeometry previous=g;
        CHECK(DspPlanGeometry(request,frame,16384,enabled,&g)==valid);
        if(!valid){CHECK(g.fragmentBytes==previous.fragmentBytes && g.totalBytes==previous.totalBytes);continue;}
        CHECK((!count || g.fragments<=count) && g.fragments && g.fragmentBytes>=(1u<<exp));
        CHECK((g.fragmentBytes&(g.fragmentBytes-1))==0);
        CHECK(g.totalBytes==g.fragments*g.fragmentBytes && g.totalBytes<=16384*frame);
        CHECK(g.capacityFrames*frame==g.totalBytes && g.writableFrames*frame==g.fragmentBytes);
        if(!enabled)CHECK(!g.changed && g.fragments==originalCount && g.fragmentBytes==(1u<<exp));
        CHECK(g.horizonQualified==AudioRefillQualified(g.capacityFrames,g.writableFrames));
        if(g.changed) {
            CHECK(g.horizonQualified);
            CHECK(g.capacityFrames-g.writableFrames<AudioRefillRequiredFrames(g.writableFrames));
            for(uint32_t smaller=1u<<exp;smaller<g.fragmentBytes;smaller*=2) {
                uint32_t available=16384*frame/smaller;
                if(count && available>count)available=count;
                CHECK(!AudioRefillQualified(available*smaller/frame,smaller/frame));
            }
        }
    }
    CHECK(!DspPlanGeometry(12,3,16384,true,&g));CHECK(!DspPlanGeometry(12,4,0,true,&g));
    CHECK(!DspPlanGeometry(12,4,16385,true,&g));CHECK(!DspPlanGeometry(12,4,16384,true,nullptr));
}
static void largeNegotiatedWrites() {
    // A32KiB fragment and a request exceeding the64KiB copy staging limit.
    // Preserve source order across partial FIFO admission and bounded syscalls.
    std::vector<int16_t> source(102400),result;
    for(size_t i=0;i<source.size();++i)source[i]=int16_t((i*48271) ^ (i>>3));
    uint64_t offset=0;unsigned transactions=0;
    while(offset<source.size()*2) {
        DspWritePlan plan;CHECK(plan.Begin(source.size()*2-offset,44100,2,16,16384,1,1));
        CHECK(plan.InputBytes()<=65536 && plan.Remaining()<=16384);++transactions;
        while(plan.Remaining()) {
            const uint32_t offered=plan.Offer(137+(plan.Accepted()*17)%8192);
            std::vector<int16_t> output(offered*2);CHECK(plan.Convert(reinterpret_cast<const uint8_t*>(source.data())+offset,output.data(),offered));
            const uint32_t accepted=std::max(1u,offered/2);
            result.insert(result.end(),output.begin(),output.begin()+accepted*2);CHECK(plan.Accept(accepted));
        }
        CHECK(plan.ConsumedBytes()==plan.InputBytes());offset+=plan.ConsumedBytes();
    }
    CHECK(transactions==4 && result==source);
}
int main(){prefixesMatchWholeConversion();refillBeforeEmpty();epochsAndBounds();writersRestageSharedBuffer();geometryNegotiation();largeNegotiatedWrites();std::puts("DSP write plan checks passed");}
