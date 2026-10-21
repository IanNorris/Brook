#pragma once
#include <stdint.h>

namespace brook {
inline constexpr unsigned AudioMixContributionSlots=32;
inline constexpr unsigned AudioMixPreparedSlots=8;
// Plain fields: the disabled mixer path need not initialize the temporary
// array. An enabled preparation initializes every slot under the mixer lock.
struct AudioMixPreparedStream {
    uint64_t identity, epoch;
    uint32_t contributionFrames, softwareFrames, inFlightFrames, capacityFrames;
};
struct AudioMixContributionRow {
    uint64_t identity=0, epoch=0, acceptedOutputFrames=0, contributedFrames=0;
    uint64_t missingEmptyFrames=0, missingShortFrames=0;
    // Missing-frame weights, not preparation counts. Software: 0, 1..440,
    // 441..1023, 1024+. In-flight: 0, positive below half capacity, half+.
    uint64_t missingSoftwareBins[4]{}, missingInFlightBins[3]{};
};
struct AudioMixContributionSnapshot {
    uint64_t epoch=0, observations=0, outputFrames=0;
    uint64_t omittedStreamFrames=0, omittedObservations=0, invalid=0;
    unsigned count=0;
    bool saturated=false, exhausted=false;
    AudioMixContributionRow rows[AudioMixContributionSlots]{};
};
inline void AudioMixSaturatingAdd(uint64_t& value,uint64_t amount,bool& saturated) {
    if(amount>~uint64_t(0)-value){value=~uint64_t(0);saturated=true;}
    else value+=amount;
}
// Caller serializes controls, preparation tokens and observations. Historical
// identities persist until reset; no Process/stream pointers, clocks or eviction.
class AudioMixContributionModel {
    AudioMixContributionSnapshot s_{};
    void Add(uint64_t& value,uint64_t amount) {
        AudioMixSaturatingAdd(value,amount,s_.saturated);
    }
    void Advance(bool on) {
        if(s_.exhausted || s_.epoch>~uint64_t(0)-2){s_.exhausted=true;s_.epoch=0;return;}
        s_.epoch=((s_.epoch&~uint64_t(1))+2)|(on?1:0);
    }
public:
    uint64_t Token() const {return s_.epoch;}
    void Enable(bool on) {Advance(on);}
    void Reset() {
        const auto epoch=s_.epoch;const bool exhausted=s_.exhausted;
        s_={};s_.epoch=epoch;s_.exhausted=exhausted;Advance(epoch&1);
    }
    AudioMixContributionSnapshot Snapshot() const {return s_;}
    bool Observe(uint64_t token,uint32_t requested,uint32_t accepted,
                 const AudioMixPreparedStream* streams,unsigned count) {
        if(!(token&1) || token!=s_.epoch || !accepted)return false;
        bool valid=requested && accepted<=requested && count<=AudioMixPreparedSlots && (!count || streams);
        if(valid)for(unsigned i=0;i<count;++i){
            const auto& p=streams[i];if(!p.identity)continue;
            if(!p.capacityFrames || p.softwareFrames>p.capacityFrames ||
               p.inFlightFrames>p.capacityFrames-p.softwareFrames ||
               p.contributionFrames>p.softwareFrames || p.contributionFrames>requested)valid=false;
        }
        if(!valid){Add(s_.invalid,1);return false;}
        Add(s_.observations,1);Add(s_.outputFrames,accepted);
        for(unsigned i=0;i<count;++i){
            const auto& p=streams[i];if(!p.identity)continue;
            unsigned slot=0;
            for(;slot<s_.count;++slot)if(s_.rows[slot].identity==p.identity && s_.rows[slot].epoch==p.epoch)break;
            if(slot==s_.count){
                if(slot==AudioMixContributionSlots){
                    Add(s_.omittedObservations,1);Add(s_.omittedStreamFrames,accepted);continue;
                }
                auto& r=s_.rows[s_.count++];r.identity=p.identity;r.epoch=p.epoch;
            }
            auto& r=s_.rows[slot];
            const uint64_t contribution=accepted<p.contributionFrames?accepted:p.contributionFrames;
            const uint64_t missing=accepted-contribution;
            Add(r.acceptedOutputFrames,accepted);Add(r.contributedFrames,contribution);
            Add(p.softwareFrames?r.missingShortFrames:r.missingEmptyFrames,missing);
            const unsigned softwareBin=!p.softwareFrames?0:p.softwareFrames<441?1:p.softwareFrames<1024?2:3;
            const unsigned flightBin=!p.inFlightFrames?0:uint64_t(p.inFlightFrames)*2<p.capacityFrames?1:2;
            Add(r.missingSoftwareBins[softwareBin],missing);Add(r.missingInFlightBins[flightBin],missing);
        }
        return true;
    }
};
} // namespace brook
