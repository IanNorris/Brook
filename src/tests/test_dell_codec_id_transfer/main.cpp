#include "../../kernel/src/dell_codec_id_transfer.h"
#include <cassert>
#include <cstdint>
#include <deque>
#include <vector>
using namespace brook;
struct Fake {
    std::vector<uint16_t> commands;
    std::deque<uint8_t> pending;
    uint8_t response[4]={0x10,0xec,0x02,0x86};
    unsigned reads=0;bool failQueue=false,failReceive=false;
    bool queue(uint16_t command) {
        if(failQueue)return false;
        commands.push_back(command);
        if(command&0x100)pending.push_back(response[reads++]);
        return true;
    }
    bool receive(uint8_t& value) {
        if(failReceive)return false;
        assert(!pending.empty());value=pending.front();pending.pop_front();return true;
    }
};
static DellCodecIdState exercise(uint8_t depth,uint8_t last) {
    Fake io;io.response[3]=last;DellCodecIdTransfer transfer(depth);
    for(unsigned i=0;i<100 && transfer.State()==DellCodecIdState::Pending;++i) {
        const bool stop=io.commands.size()==8;
        transfer.Step({false,stop,true,!io.pending.empty()},io);
    }
    const std::vector<uint16_t> expected={0,15,0,0,0x500,0x100,0x100,0x300};
    assert(io.commands==expected && io.reads==4 && io.pending.empty());
    assert(transfer.Value()==(0x10ec0200u|last));
    const auto n=io.commands.size();transfer.Step({false,false,true,false},io);assert(io.commands.size()==n);
    return transfer.State();
}
int main() {
    assert(exercise(1,0x86)==DellCodecIdState::Rt286);
    assert(exercise(2,0x88)==DellCodecIdState::Rt288);
    assert(exercise(4,0x99)==DellCodecIdState::Unknown);
    Fake io;DellCodecIdTransfer invalid(0);assert(invalid.State()==DellCodecIdState::ProtocolError);
    DellCodecIdTransfer early(1);assert(early.Step({false,true,true,false},io)==DellCodecIdState::ProtocolError);
    DellCodecIdTransfer stray(1);assert(stray.Step({false,false,true,true},io)==DellCodecIdState::ProtocolError);
    DellCodecIdTransfer abort(1);assert(abort.Step({true,false,true,false},io)==DellCodecIdState::Aborted && io.commands.empty());
    DellCodecIdTransfer abortPriority(1);assert(abortPriority.Step({true,true,true,true},io)==DellCodecIdState::Aborted && io.commands.empty());
    DellCodecIdTransfer timeout(1);assert(timeout.Expire()==DellCodecIdState::TimedOut);
    assert(timeout.Step({false,false,true,false},io)==DellCodecIdState::TimedOut && io.commands.empty());
    Fake limitedIo;DellCodecIdTransfer limited(1);
    for(unsigned i=0;i<5;++i)assert(limited.Step({false,false,true,false},limitedIo)==DellCodecIdState::Pending);
    assert(limitedIo.commands.size()==5);
    for(unsigned i=0;i<8;++i)assert(limited.Step({false,false,true,false},limitedIo)==DellCodecIdState::Pending);
    assert(limitedIo.commands.size()==5); // One unread byte fills the RX budget.
    assert(limited.Step({false,false,true,true},limitedIo)==DellCodecIdState::Pending);
    assert(limited.Step({false,false,true,false},limitedIo)==DellCodecIdState::Pending && limitedIo.commands.size()==6);
    Fake stalledIo;DellCodecIdTransfer stalled(4);
    for(unsigned i=0;i<10;++i)assert(stalled.Step({false,false,false,false},stalledIo)==DellCodecIdState::Pending);
    assert(stalledIo.commands.empty());
    for(unsigned i=0;i<8;++i)assert(stalled.Step({false,false,true,false},stalledIo)==DellCodecIdState::Pending);
    for(unsigned i=0;i<4;++i)assert(stalled.Step({false,false,false,true},stalledIo)==DellCodecIdState::Pending);
    assert(stalled.Expire()==DellCodecIdState::TimedOut && stalled.Value()==0);
    Fake extraIo;DellCodecIdTransfer extra(4);
    for(unsigned i=0;i<8;++i)assert(extra.Step({false,false,true,false},extraIo)==DellCodecIdState::Pending);
    for(unsigned i=0;i<4;++i)assert(extra.Step({false,false,false,true},extraIo)==DellCodecIdState::Pending);
    assert(extra.Step({false,false,false,true},extraIo)==DellCodecIdState::ProtocolError);
    DellCodecIdTransfer queueFail(1);io.failQueue=true;
    assert(queueFail.Step({false,false,true,false},io)==DellCodecIdState::TransportError);io.failQueue=false;
    Fake partialIo;DellCodecIdTransfer partial(1);
    assert(partial.Step({false,false,true,false},partialIo)==DellCodecIdState::Pending);
    partialIo.failQueue=true;assert(partial.Step({false,false,true,false},partialIo)==DellCodecIdState::TransportError);
    assert(partialIo.commands.size()==1);
    DellCodecIdTransfer receiveFail(1);
    for(unsigned i=0;i<5;++i)assert(receiveFail.Step({false,false,true,false},io)==DellCodecIdState::Pending);
    io.failReceive=true;assert(receiveFail.Step({false,false,false,true},io)==DellCodecIdState::TransportError);
}
