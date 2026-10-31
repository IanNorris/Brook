#pragma once
#include "dell_codec_id_transfer.h"
#include <stdint.h>

namespace brook {
// Polling adapter for an *already initialized and exclusively owned*
// DesignWare master. No mapping, reset, power, interrupt acknowledgement or
// ownership is performed here. The caller must preserve the owner on every
// outcome until the controller is known idle; a timeout does not cancel I/O.
// The supplied bus must provide read32(offset,value) and write32(offset,value).
// These operations may themselves hang on inaccessible physical MMIO; the
// software deadline cannot bound an individual stuck MMIO access.
struct DellCodecPollResult {
    DellCodecIdState state=DellCodecIdState::Pending;
    uint32_t abortSource=0;
    bool abortSourceValid=false;
    bool recoveryRequired=true;
    uint32_t value=0;
};
template<class Bus> class DellCodecI2cPoll {
    static constexpr uint32_t DataCmd=0x10,RawInterrupt=0x34,TxLevel=0x74,RxLevel=0x78,AbortSource=0x80;
    static constexpr uint32_t RxUnder=1u<<0,RxOver=1u<<1,TxOver=1u<<3,Abort=1u<<6,Stop=1u<<9;
    Bus& bus_;DellCodecIdTransfer transfer_;uint32_t txDepth_,rxDepth_;
    uint64_t start_,last_,timeout_;uint32_t steps_;
    DellCodecPollResult result_{};
    struct Adapter {
        Bus& bus;
        bool queue(uint16_t command){return bus.write32(DataCmd,command);}
        bool receive(uint8_t& value){uint32_t word=0;if(!bus.read32(DataCmd,word))return false;value=uint8_t(word);return true;}
    };
    DellCodecPollResult Fail(DellCodecIdState state){result_.state=state;return result_;}
public:
    DellCodecI2cPoll(Bus& bus,uint32_t txDepth,uint32_t rxDepth,uint64_t start,uint64_t timeout,uint32_t maxSteps)
        :bus_(bus),transfer_(uint8_t(rxDepth>4?4:rxDepth)),txDepth_(txDepth),rxDepth_(rxDepth),
         start_(start),last_(start),timeout_(timeout),steps_(maxSteps) {
        if(!txDepth || txDepth>256 || !rxDepth || rxDepth>256 || !timeout || !maxSteps)
            result_.state=DellCodecIdState::ProtocolError;
    }
    DellCodecPollResult Poll(uint64_t now) {
        if(result_.state!=DellCodecIdState::Pending)return result_;
        if(now<last_ || now<start_)return Fail(DellCodecIdState::ProtocolError);
        last_=now;
        if(now-start_>=timeout_ || !steps_)return Fail(transfer_.Expire());
        --steps_;
        uint32_t raw=0;if(!bus_.read32(RawInterrupt,raw) || raw==0xffffffffu)return Fail(DellCodecIdState::TransportError);
        if(raw&Abort) {
            result_.abortSourceValid=bus_.read32(AbortSource,result_.abortSource) && result_.abortSource!=0xffffffffu;
            return Fail(DellCodecIdState::Aborted);
        }
        if(raw&(RxUnder|RxOver|TxOver))return Fail(DellCodecIdState::TransportError);
        uint32_t tx=0,rx=0;
        if(!bus_.read32(TxLevel,tx) || !bus_.read32(RxLevel,rx) || tx>txDepth_ || rx>rxDepth_)
            return Fail(DellCodecIdState::TransportError);
        Adapter adapter{bus_};
        result_.state=transfer_.Step({false,(raw&Stop)!=0,tx<txDepth_,rx!=0},adapter);
        if(result_.state==DellCodecIdState::Rt286 || result_.state==DellCodecIdState::Rt288 || result_.state==DellCodecIdState::Unknown)
            result_.value=transfer_.Value();
        return result_;
    }
};
}
