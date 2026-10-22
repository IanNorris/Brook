#include <cstdio>
#include "../../kernel/src/audio_mix_contributions.h"
namespace {
unsigned failures;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"MIX line %d: %s\n",__LINE__,#x);++failures;}}while(0)
using namespace brook;
void conservation(const AudioMixContributionSnapshot& s) {
    CHECK(!s.saturated && !s.invalid);
    for(unsigned i=0;i<s.count;++i){
        const auto& r=s.rows[i];
        CHECK(r.acceptedOutputFrames==r.contributedFrames+r.missingEmptyFrames+r.missingShortFrames);
        uint64_t software=0,flight=0;
        for(auto n:r.missingSoftwareBins)software+=n;
        for(auto n:r.missingInFlightBins)flight+=n;
        CHECK(software==r.missingEmptyFrames+r.missingShortFrames && flight==software);
    }
}
}
int main() {
    AudioMixContributionModel m;
    AudioMixPreparedStream p[3]={{17,1,1024,2048,2048,4096},
                                {34,2,128,128,3000,4096},
                                {51,3,0,0,0,4096}};
    CHECK(!m.Token() && !m.Observe(m.Token(),1024,1024,p,3));
    m.Enable(true);auto token=m.Token();
    CHECK(!m.Observe(token,1024,0,p,3));CHECK(!m.Snapshot().observations);
    CHECK(m.Observe(token,1024,64,p,3));
    auto s=m.Snapshot();CHECK(s.outputFrames==64 && s.count==3);
    CHECK(s.rows[1].contributedFrames==64 && !s.rows[1].missingShortFrames);
    CHECK(s.rows[2].missingEmptyFrames==64 && s.rows[2].missingInFlightBins[0]==64);
    CHECK(m.Observe(token,1024,512,p,3));s=m.Snapshot();
    CHECK(s.outputFrames==576 && s.rows[0].contributedFrames==576);
    CHECK(s.rows[1].contributedFrames==192 && s.rows[1].missingShortFrames==384);
    CHECK(s.rows[1].missingSoftwareBins[1]==384 && s.rows[1].missingInFlightBins[2]==384);
    conservation(s);
    // A different captured epoch must not replace a historical row.
    p[1].epoch=3;CHECK(m.Observe(token,1024,512,p,3));
    CHECK(m.Snapshot().count==4 && m.Snapshot().rows[1].acceptedOutputFrames==576);
    m.Enable(false);CHECK(!m.Observe(token,1024,512,p,3));
    CHECK(m.Snapshot().outputFrames==1088);
    m.Enable(true);token=m.Token();m.Reset();
    CHECK(!m.Observe(token,1024,512,p,3) && !m.Snapshot().observations);
    token=m.Token();CHECK(m.Observe(token,1024,1024,p,3));conservation(m.Snapshot());
    // Full table does not evict or corrupt earlier identities. Omission units
    // are stream-frames, and can exceed unique physical output frames.
    m.Reset();token=m.Token();
    for(unsigned i=0;i<AudioMixContributionSlots;++i){
        AudioMixPreparedStream one{100+i,1,10,10,0,1024};
        CHECK(m.Observe(token,10,10,&one,1));
    }
    AudioMixPreparedStream overflow[3]={{999,1,0,0,1,1024},{998,1,0,0,1,1024},{100,1,1,1,0,1024}};
    CHECK(m.Observe(token,8,8,overflow,3));s=m.Snapshot();
    CHECK(s.count==32 && s.outputFrames==328 && s.omittedObservations==2 && s.omittedStreamFrames==16);
    CHECK(s.rows[0].acceptedOutputFrames==18 && s.rows[0].contributedFrames==11 && s.rows[0].missingShortFrames==7);
    conservation(s);
    // Invalid preparation/receipt input cannot partially count a batch.
    m.Reset();token=m.Token();p[0].softwareFrames=5000;
    CHECK(!m.Observe(token,1024,512,p,3));CHECK(!m.Observe(token,100,101,p,3));
    CHECK(!m.Observe(token,100,1,nullptr,1));s=m.Snapshot();
    CHECK(s.invalid==3 && !s.observations && !s.outputFrames && !s.count);
    // Exact bucket boundaries, including the in-flight half-capacity boundary.
    m.Reset();token=m.Token();
    const uint32_t queue[]={0,1,440,441,1023,1024};
    for(unsigned i=0;i<6;++i){
        AudioMixPreparedStream one{100+i,1,queue[i],queue[i],i==0?0u:i==1?1023u:1024u,2048};
        CHECK(m.Observe(token,1764,1764,&one,1));
        s=m.Snapshot();const auto& r=s.rows[i];
        const unsigned bin=i==0?0:i<3?1:i<5?2:3;
        CHECK(r.missingSoftwareBins[bin]==1764-queue[i]);
        CHECK(r.missingInFlightBins[i==0?0:i==1?1:2]==1764-queue[i]);
    }
    conservation(m.Snapshot());
    // Real downstream-staging geometry: almost full Firefox admission with
    // six reusable frames is valid even though in-flight exceeds client size.
    m.Reset();token=m.Token();
    AudioMixPreparedStream staged{49,7,0,0,8506,4416,4096};
    CHECK(m.Observe(token,1024,1024,&staged,1));
    s=m.Snapshot();CHECK(!s.invalid && s.rows[0].missingEmptyFrames==1024);
    CHECK(s.rows[0].missingInFlightBins[2]==1024);
    staged.contributionFrames=454;staged.softwareFrames=454;staged.inFlightFrames=8052;
    CHECK(m.Observe(token,1024,512,&staged,1));conservation(m.Snapshot());
    // Staging does not excuse overcommit or more software than client capacity.
    staged.inFlightFrames=8059;CHECK(!m.Observe(token,1024,512,&staged,1));
    staged.inFlightFrames=0;staged.stagingFrames=4417;
    CHECK(!m.Observe(token,1024,512,&staged,1));
    CHECK(m.Snapshot().invalid==2 && m.Snapshot().observations==2);
    uint64_t value=~uint64_t(0)-1;bool saturated=false;
    AudioMixSaturatingAdd(value,1,saturated);CHECK(value==~uint64_t(0) && !saturated);
    AudioMixSaturatingAdd(value,1,saturated);CHECK(value==~uint64_t(0) && saturated);
    value=0;AudioMixSaturatingAdd(value,3,saturated);CHECK(value==3 && saturated);
    std::printf("AUDIO_MIX_CONTRIBUTIONS %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
