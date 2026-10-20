#pragma once
#include <stdint.h>

namespace brook {
// One bounded write transaction. Prefixes retain a single conversion origin,
// so splitting a converted chunk cannot accumulate resampler rounding errors.
// Caller serializes each staging/submit attempt, but need not retain pointers
// or hold the open-description mutex while waiting for space.
class DspWritePlan {
    uint64_t inputFrames_=0,control_=0,stream_=0;
    uint32_t rate_=0,total_=0,accepted_=0;
    uint8_t channels_=0,bits_=0;
public:
    static constexpr uint32_t HardwareRate=44100,InputBytesLimit=65536;
    bool Begin(uint64_t bytes,uint32_t rate,uint8_t channels,uint8_t bits,
               uint32_t capacity,uint64_t control,uint64_t stream) {
        *this={};
        if(!rate || (channels!=1 && channels!=2) || (bits!=8 && bits!=16) ||
           !capacity || capacity>InputBytesLimit/4)return false;
        const uint32_t frameBytes=channels*(bits/8);
        uint64_t frames=bytes/frameBytes;
        const uint64_t inputCap=InputBytesLimit/frameBytes;
        const uint64_t outputCap=uint64_t(capacity)*rate/HardwareRate;
        if(frames>inputCap)frames=inputCap;
        if(frames>outputCap)frames=outputCap;
        if(!frames)return false;
        inputFrames_=frames;rate_=rate;channels_=channels;bits_=bits;
        control_=control;stream_=stream;
        total_=uint32_t((frames*HardwareRate+rate-1)/rate);
        return total_ && total_<=capacity;
    }
    bool Active()const{return inputFrames_!=0;}
    bool Matches(uint32_t rate,uint8_t channels,uint8_t bits,
                 uint64_t control,uint64_t stream)const {
        return rate_==rate && channels_==channels && bits_==bits &&
               control_==control && stream_==stream;
    }
    uint32_t Accepted()const{return accepted_;}
    uint32_t Remaining()const{return total_-accepted_;}
    uint32_t Offer(uint32_t available)const{return available<Remaining()?available:Remaining();}
    uint64_t InputBytes()const{return inputFrames_*channels_*(bits_/8);}
    uint64_t ConsumedBytes()const {
        if(!accepted_)return 0;
        uint64_t frames=accepted_==total_?inputFrames_:
            uint64_t(accepted_-1)*rate_/HardwareRate+1;
        if(frames>inputFrames_)frames=inputFrames_;
        return frames*channels_*(bits_/8);
    }
    bool Accept(uint32_t frames) {
        if(frames>Remaining())return false;
        accepted_+=frames;return true;
    }
    bool Convert(const uint8_t* input,int16_t* output,uint32_t frames)const {
        if(!Active() || !input || !output || frames>Remaining())return false;
        const auto* words=reinterpret_cast<const int16_t*>(input);
        for(uint32_t i=0;i<frames;++i){
            uint64_t source=uint64_t(accepted_+i)*rate_/HardwareRate;
            if(source>=inputFrames_)source=inputFrames_-1;
            for(uint32_t channel=0;channel<2;++channel){
                const uint64_t index=source*channels_+(channels_==2?channel:0);
                output[i*2+channel]=bits_==8?
                    int16_t((int(input[index])-128)*256):words[index];
            }
        }
        return true;
    }
};
}
