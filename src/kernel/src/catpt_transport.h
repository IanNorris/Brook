#pragma once
#include "catpt_mailbox.h"

namespace brook::catpt {
// Platform-owned operations. They must enforce device ownership and suitable
// memory ordering. The caller must serialize methods with an IRQ-safe outer
// lock, prohibit callback re-entry, and route only a known delayed response to
// delayed(); asynchronous notifications are separate. When DONE and BUSY are
// both pending, dispatch DONE first without acknowledging BUSY early. A BUSY
// response observed while WaitingDone must remain pending until DONE is handled.
struct TransportIo {
    void* context;
    bool (*read)(void*,uint32_t,void*,uint32_t);
    bool (*write)(void*,uint32_t,const void*,uint32_t);
    bool (*publish)(void*,uint32_t);
    bool (*ackDone)(void*);
    bool (*ackBusy)(void*);
};
enum class TransportState : uint8_t { Unavailable, Idle, WaitingDone, WaitingDelayed, Complete, Failed };

class Transport {
public:
    bool bind(const Mailbox& mailbox,TransportIo io) {
        if(state_!=TransportState::Unavailable || mailbox.state()!=MailboxState::Ready ||
           !io.read || !io.write || !io.publish || !io.ackDone || !io.ackBusy)return false;
        cfg_=mailbox.config();io_=io;state_=TransportState::Idle;return true;
    }
    bool begin(uint32_t header,const void* data,uint32_t bytes,uint32_t replyBytes) {
        if(state_!=TransportState::Idle || bytes>cfg_.outboxSize || replyBytes>cfg_.outboxSize ||
           (bytes && !data) || (header&(Mailbox::BusyBit|(1u<<30)|Mailbox::ReadyBit)))return false;
        replyBytes_=replyBytes;
        if((bytes && !io_.write(io_.context,cfg_.outboxOffset,data,bytes)) ||
           !io_.publish(io_.context,header|Mailbox::BusyBit)){state_=TransportState::Failed;return false;}
        state_=TransportState::WaitingDone;return true;
    }
    // status 5 means the DSP will reply via its BUSY notification path.
    bool immediate(uint32_t header) {
        if(state_!=TransportState::WaitingDone || !(header&(1u<<30)))return false;
        const uint32_t status=header&31;
        if(status==5){
            if(!io_.ackDone(io_.context)){state_=TransportState::Failed;return false;}
            state_=TransportState::WaitingDelayed;return true;
        }
        return finish(header,false);
    }
    bool delayed(uint32_t header) {
        if(state_!=TransportState::WaitingDelayed || !(header&Mailbox::BusyBit))return false;
        return finish(header,true);
    }
    // The platform supplies and enforces its deadline; a late reply must never
    // be mistaken for a later request, so timeout poisons this instance.
    void timeout() { if(state_==TransportState::WaitingDone || state_==TransportState::WaitingDelayed)state_=TransportState::Failed; }
    TransportState state() const {return state_;}
    // status is meaningful only in Complete; failed instances require the
    // platform to quiesce/reinitialize the actual device before rebinding.
    uint32_t status() const {return status_;}
    const uint8_t* reply() const {return state_==TransportState::Complete && status_==0?reply_:nullptr;}
    uint32_t replySize() const {return reply()?replyBytes_:0;}
    bool consume() {if(state_!=TransportState::Complete)return false;state_=TransportState::Idle;return true;}
private:
    bool finish(uint32_t header,bool busy) {
        const uint32_t status=header&31;
        // Copy before acknowledging: the DSP may reuse the outbox after ACK.
        if(status==0 && replyBytes_ && !io_.read(io_.context,cfg_.outboxOffset,reply_,replyBytes_)){
            state_=TransportState::Failed;return false;
        }
        if(!(busy?io_.ackBusy(io_.context):io_.ackDone(io_.context))){state_=TransportState::Failed;return false;}
        status_=status;state_=TransportState::Complete;return true;
    }
    TransportIo io_{};MailboxConfig cfg_{};TransportState state_=TransportState::Unavailable;
    uint32_t replyBytes_=0,status_=0;uint8_t reply_[Mailbox::MailboxLimit]{};
};
}
