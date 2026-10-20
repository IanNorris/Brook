#pragma once
#include <stdint.h>
namespace brook {
// Diagnostic-only observations. Driver watermarks are physical bytes, including
// silence; they must never be interpreted as application completion.
struct AudioDeliveryEvent {
    uint64_t hardwareEpoch=0, beforeCopyNs=0, afterCopyNs=0, afterPositionNs=0;
    uint64_t pendingBeforeReserve=0, reservedBytes=0, leadBeforeBytes=0;
    uint32_t acceptedBytes=0, advancedBytes=0;
    bool running=false, positionValid=false, publishSafe=false;
};
inline constexpr unsigned AudioDeliveryEventSlots=16;
struct AudioDeliveryTimingSnapshot {
    uint64_t epoch=0, observations=0, acceptedBytes=0, reservations=0, reservedBytes=0;
    uint64_t invalidClock=0, invalidPosition=0, unsafe=0, runningSamples=0;
    uint64_t maxCopyNs=0, maxSubmissionNs=0, maxAdvanceBytes=0;
    uint64_t minHeadroomBytes=~uint64_t(0), maxReservedBytes=0, contendedRecords=0;
    uint64_t copyBins[7]{};
    bool saturated=false;
    uint32_t retained=0, next=0;
    AudioDeliveryEvent events[AudioDeliveryEventSlots]{};
    AudioDeliveryEvent maxAdvanceEvent{}, minHeadroomEvent{};
};
// Caller serializes this model. Tokens reject controls racing an in-flight copy.
class AudioDeliveryTimingModel {
    AudioDeliveryTimingSnapshot s_{};
    bool exhausted_=false;
    void Add(uint64_t& v,uint64_t n){if(v>~uint64_t(0)-n){v=~uint64_t(0);s_.saturated=true;}else v+=n;}
    void Advance(bool on){
        if(exhausted_ || s_.epoch>~uint64_t(0)-2){exhausted_=true;s_.epoch=0;return;}
        s_.epoch=((s_.epoch&~uint64_t(1))+2)|(on?1:0);
    }
public:
    uint64_t Token() const{return s_.epoch;}
    void Enable(bool on){Advance(on);}
    void Reset(){auto epoch=s_.epoch;s_={};s_.epoch=epoch;Advance(epoch&1);}
    AudioDeliveryTimingSnapshot Snapshot() const{return s_;}
    bool Observe(uint64_t token,const AudioDeliveryEvent& e){
        if(!(token&1) || token!=s_.epoch)return false;
        Add(s_.observations,1);Add(s_.acceptedBytes,e.acceptedBytes);
        if(e.reservedBytes){Add(s_.reservations,1);Add(s_.reservedBytes,e.reservedBytes);if(e.reservedBytes>s_.maxReservedBytes)s_.maxReservedBytes=e.reservedBytes;}
        if(!e.publishSafe)Add(s_.unsafe,1);
        if(!e.beforeCopyNs || e.afterCopyNs<e.beforeCopyNs || (e.running && e.afterPositionNs<e.afterCopyNs))Add(s_.invalidClock,1);
        else {
            const auto copy=e.afterCopyNs-e.beforeCopyNs;
            const auto total=(e.running?e.afterPositionNs:e.afterCopyNs)-e.beforeCopyNs;
            if(copy>s_.maxCopyNs)s_.maxCopyNs=copy;
            if(total>s_.maxSubmissionNs)s_.maxSubmissionNs=total;
            unsigned bin=0;uint64_t edge=1000;
            while(bin<6 && copy>=edge){++bin;edge*=10;}
            Add(s_.copyBins[bin],1);
        }
        if(e.running){
            if(!e.positionValid)Add(s_.invalidPosition,1);
            else {
                Add(s_.runningSamples,1);
                if(s_.runningSamples==1 || e.advancedBytes>s_.maxAdvanceBytes){s_.maxAdvanceBytes=e.advancedBytes;s_.maxAdvanceEvent=e;}
                const auto room=e.advancedBytes>e.leadBeforeBytes?0:e.leadBeforeBytes-e.advancedBytes;
                if(room<s_.minHeadroomBytes){s_.minHeadroomBytes=room;s_.minHeadroomEvent=e;}
            }
        }
        s_.events[s_.next]=e;s_.next=(s_.next+1)%AudioDeliveryEventSlots;
        if(s_.retained<AudioDeliveryEventSlots)++s_.retained;
        return true;
    }
};
extern "C" uint64_t AudioDeliveryTimingToken();
extern "C" void AudioDeliveryTimingRecord(uint64_t token,const AudioDeliveryEvent* event);
void AudioDeliveryTimingEnable(bool enabled);
void AudioDeliveryTimingReset();
AudioDeliveryTimingSnapshot AudioDeliveryTimingRead();
}
