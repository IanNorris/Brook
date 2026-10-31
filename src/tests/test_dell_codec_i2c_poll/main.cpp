#include "../../kernel/src/dell_codec_i2c_poll.h"
#include <cassert>
#include <cstdint>
#include <deque>
#include <vector>
using namespace brook;
struct FakeBus {
    uint32_t raw=0,tx=0,depth=2,abort=0x1234,failReg=0xffffffffu,rxOverride=0xffffffffu;bool fail=false,failWrite=false,suppressStop=false;
    std::vector<uint32_t> writes,reads;std::deque<uint8_t> rx;
    const uint8_t response[4]={0x10,0xec,0x02,0x86};unsigned readCommands=0;
    bool read32(uint32_t reg,uint32_t& value) {
        reads.push_back(reg);if(fail || reg==failReg)return false;
        switch(reg){case 0x34:value=raw;return true;case 0x74:value=tx;return true;
        case 0x78:value=rxOverride==0xffffffffu?uint32_t(rx.size()):rxOverride;return true;case 0x80:value=abort;return true;
        case 0x10:if(rx.empty())return false;value=rx.front();rx.pop_front();return true;default:assert(false);return false;}
    }
    bool write32(uint32_t reg,uint32_t value) {
        assert(reg==0x10);if(fail || failWrite)return false;writes.push_back(value);
        if(value&0x100)rx.push_back(response[readCommands++]);
        if(writes.size()==8 && !suppressStop)raw|=1u<<9;
        return true;
    }
};
int main(){
    FakeBus bus;DellCodecI2cPoll<FakeBus> poll(bus,2,2,100,1000,100);
    DellCodecPollResult result;
    for(uint64_t now=100;now<200;++now){result=poll.Poll(now);if(result.state!=DellCodecIdState::Pending)break;}
    assert(result.state==DellCodecIdState::Rt286 && result.value==0x10ec0286 && result.recoveryRequired);
    const std::vector<uint32_t> expected={0,15,0,0,0x500,0x100,0x100,0x300};assert(bus.writes==expected);
    auto writes=bus.writes.size(),reads=bus.reads.size();poll.Poll(9999);assert(bus.writes.size()==writes && bus.reads.size()==reads);
    FakeBus aborted;aborted.raw=(1u<<6)|(1u<<9);DellCodecI2cPoll<FakeBus> bad(aborted,2,2,0,100,10);
    result=bad.Poll(1);assert(result.state==DellCodecIdState::Aborted && result.abortSourceValid && result.abortSource==0x1234 && aborted.writes.empty());
    assert((aborted.reads==std::vector<uint32_t>{0x34,0x80})); // No clear-register reads.
    FakeBus overflow;overflow.raw=1u<<1;DellCodecI2cPoll<FakeBus> over(overflow,2,2,0,100,10);
    assert(over.Poll(1).state==DellCodecIdState::TransportError && overflow.writes.empty());
    FakeBus invalid;invalid.tx=3;DellCodecI2cPoll<FakeBus> levels(invalid,2,2,0,100,10);
    assert(levels.Poll(1).state==DellCodecIdState::TransportError && invalid.writes.empty());
    FakeBus inaccessible;inaccessible.raw=0xffffffff;DellCodecI2cPoll<FakeBus> unmapped(inaccessible,2,2,0,100,10);
    assert(unmapped.Poll(1).state==DellCodecIdState::TransportError && inaccessible.writes.empty());
    FakeBus stuck;stuck.tx=2;DellCodecI2cPoll<FakeBus> timeout(stuck,2,2,0,4,100);
    assert(timeout.Poll(1).state==DellCodecIdState::Pending);assert(timeout.Poll(4).state==DellCodecIdState::TimedOut && stuck.writes.empty());
    FakeBus frozen;frozen.tx=2;DellCodecI2cPoll<FakeBus> bounded(frozen,2,2,0,100,2);
    assert(bounded.Poll(0).state==DellCodecIdState::Pending);assert(bounded.Poll(0).state==DellCodecIdState::Pending);
    assert(bounded.Poll(0).state==DellCodecIdState::TimedOut);
    FakeBus backwards;DellCodecI2cPoll<FakeBus> clock(backwards,2,2,10,100,10);
    assert(clock.Poll(9).state==DellCodecIdState::ProtocolError && backwards.writes.empty());
    FakeBus wide;DellCodecI2cPoll<FakeBus> maxDepth(wide,256,256,0,100,100);
    assert(maxDepth.Poll(1).state==DellCodecIdState::Pending);
    for(uint32_t reg:{0x34u,0x74u,0x78u}) {
        FakeBus failed;failed.failReg=reg;DellCodecI2cPoll<FakeBus> p(failed,2,2,0,100,10);
        assert(p.Poll(1).state==DellCodecIdState::TransportError && failed.writes.empty());
    }
    FakeBus failedWrite;failedWrite.failWrite=true;DellCodecI2cPoll<FakeBus> writeError(failedWrite,2,2,0,100,10);
    assert(writeError.Poll(1).state==DellCodecIdState::TransportError);
    FakeBus failedData;failedData.failReg=0x10;DellCodecI2cPoll<FakeBus> dataError(failedData,2,2,0,100,20);
    for(uint64_t t=1;t<=5;++t)assert(dataError.Poll(t).state==DellCodecIdState::Pending);
    assert(dataError.Poll(6).state==DellCodecIdState::TransportError);
    FakeBus failedAbort;failedAbort.raw=1u<<6;failedAbort.failReg=0x80;DellCodecI2cPoll<FakeBus> abortError(failedAbort,2,2,0,100,10);
    result=abortError.Poll(1);assert(result.state==DellCodecIdState::Aborted && !result.abortSourceValid);
    FakeBus excessive;excessive.rxOverride=3;DellCodecI2cPoll<FakeBus> rxLevel(excessive,2,2,0,100,10);
    assert(rxLevel.Poll(1).state==DellCodecIdState::TransportError && excessive.writes.empty());
    FakeBus badConfig;DellCodecI2cPoll<FakeBus> zeroDepth(badConfig,0,2,0,100,10),zeroTimeout(badConfig,2,2,0,0,10),hugeDepth(badConfig,2,257,0,100,10);
    assert(zeroDepth.Poll(1).state==DellCodecIdState::ProtocolError && zeroTimeout.Poll(1).state==DellCodecIdState::ProtocolError && hugeDepth.Poll(1).state==DellCodecIdState::ProtocolError && badConfig.reads.empty());
    FakeBus noStop;noStop.suppressStop=true;DellCodecI2cPoll<FakeBus> missingStop(noStop,2,2,0,30,100);
    for(uint64_t now=0;now<30;++now)assert(missingStop.Poll(now).state==DellCodecIdState::Pending);
    assert(missingStop.Poll(30).state==DellCodecIdState::TimedOut && noStop.writes.size()==8);
}
