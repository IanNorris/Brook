#pragma once
#include <stdint.h>

namespace brook {
// Fixed RT286/RT288 root-node vendor parameter transaction over DesignWare
// I2C, factored from the hardware transport. It assumes exclusive ownership,
// a powered/configured master, target 0x1c, restart enabled, clean FIFOs and
// cleared abort/stop status. None of those preconditions is established here.
// This component never maps a controller, enables audio, or retries a failure.
// The transport must retain STOP until all RX bytes are drained. Terminal
// failure does not cancel queued hardware commands: the owner must retain
// exclusive ownership and establish controller quiescence before reuse.
// It must report FIFO overflows as transport errors and capture abort details
// before clearing status. Clamp a hardware RX depth to four before narrowing
// it to uint8_t (the hardware encoding can represent 256 entries).
// See Linux rl6347a_hw_read and i2c_dw_xfer_msg for the wire protocol.
struct DellCodecI2cStatus {bool aborted,stop,txSpace,rxReady;};
enum class DellCodecIdState : uint8_t { Pending, Rt286, Rt288, Unknown, Aborted, ProtocolError, TimedOut, TransportError };
class DellCodecIdTransfer {
    static constexpr uint16_t Read=0x100,Stop=0x200,Restart=0x400;
    static constexpr uint8_t Verb[4]={0x00,0x0f,0x00,0x00};
    uint8_t queued_=0,received_=0,rxDepth_;
    uint32_t value_=0;
    DellCodecIdState state_=DellCodecIdState::Pending;
public:
    explicit DellCodecIdTransfer(uint8_t rxDepth):rxDepth_(rxDepth) {
        if(!rxDepth)state_=DellCodecIdState::ProtocolError;
    }
    DellCodecIdState State() const {return state_;}
    // A value is meaningful only after a complete four-byte response + STOP.
    uint32_t Value() const {return state_==DellCodecIdState::Rt286 || state_==DellCodecIdState::Rt288 || state_==DellCodecIdState::Unknown ? value_ : 0;}
    DellCodecIdState Expire() {if(state_==DellCodecIdState::Pending)state_=DellCodecIdState::TimedOut;return state_;}
    template<class Io> DellCodecIdState Step(const DellCodecI2cStatus& status,Io& io) {
        if(state_!=DellCodecIdState::Pending)return state_;
        if(status.aborted)return state_=DellCodecIdState::Aborted;
        const unsigned readsQueued=queued_>4?unsigned(queued_-4):0;
        if(status.rxReady) {
            if(received_>=readsQueued || received_>=4)return state_=DellCodecIdState::ProtocolError;
            uint8_t byte=0;if(!io.receive(byte))return state_=DellCodecIdState::TransportError;
            value_=(value_<<8)|byte;++received_;
            // Drain one value per step before considering a latched STOP.
            return state_;
        }
        if(status.stop) {
            if(queued_!=8 || received_!=4)return state_=DellCodecIdState::ProtocolError;
            return state_=value_==0x10ec0286?DellCodecIdState::Rt286:
                          value_==0x10ec0288?DellCodecIdState::Rt288:DellCodecIdState::Unknown;
        }
        if(status.txSpace && queued_<8 && (queued_<4 || readsQueued-received_<rxDepth_)) {
            uint16_t command=queued_<4?Verb[queued_]:Read;
            if(queued_==4)command|=Restart;
            if(queued_==7)command|=Stop;
            if(!io.queue(command))return state_=DellCodecIdState::TransportError;
            ++queued_;
        }
        return state_;
    }
};
}
