#include "../../drivers/xhci/xhci_ownership.h"
#include <cassert>
#include "../../drivers/xhci/xhci_msi.h"
#include "../../drivers/xhci/xhci_root_port.h"
#include "../../drivers/xhci/xhci_intel_routing.h"
#include <vector>
#include <map>
struct Registers {
    std::map<unsigned,unsigned> values;unsigned byteWrites=0,fullWrites=0,polls=0,releaseAt=1;
    unsigned read(unsigned offset){return values[offset];}
    void writeByte(unsigned offset,uint8_t v){assert(offset==0x43);values[0x40]=(values[0x40]&0x00ffffffu)|(unsigned(v)<<24);++byteWrites;}
    void write(unsigned offset,unsigned value){assert(offset==0x44);values[offset]=value&~(7u<<29);++fullWrites;}
    bool wait(){if(++polls==releaseAt)values[0x40]&=~(1u<<16);return polls<8;}
};
struct RootPort {
    uint32_t value=1u|(1u<<9)|(3u<<10)|(2u<<14)|(5u<<25);
    uint64_t tick=0,completeAt=50;bool stuck=false,disconnect=false,inaccessible=false;
    std::vector<uint32_t> writes;
    uint32_t read(){return inaccessible?0xffffffffu:value;}
    uint64_t now(){return tick;}
    bool wait(){++tick;if(disconnect && tick==10)value&=~1u;
        if(!stuck && tick==completeAt && (value&1))value=(value&~(1u<<4))|2u|(1u<<21);return true;}
    void write(uint32_t v){writes.push_back(v);
        assert(!(v&2)); // writing PED=1 would disable the port
        value=(value&~((15u<<5)|(1u<<9)|(3u<<14)|(7u<<25)))|brook::XhciPortPersistent(v);
        value&=~(v&(1u<<21));if(v&(1u<<4))value|=1u<<4;}
};
struct Routing {
    std::map<unsigned,unsigned> values{{0xd0,0x80000002},{0xd4,0x15},{0xd8,0x40000001},{0xdc,0xa}};
    std::vector<std::pair<unsigned,unsigned>> writes;unsigned reads=0,ignoreOffset=0;bool ignore=false;
    uint32_t read(unsigned offset){++reads;return values.at(offset);}
    void write(unsigned offset,unsigned value){assert(offset==0xd8 || offset==0xd0);writes.push_back({offset,value});if(!ignore && offset!=ignoreOffset)values[offset]=value;}
};
int main(){
    brook::XhciIntelRouteState route{};Routing routing;
    assert(brook::XhciEnableIntelRouting(0x1b36,0x000d,0x0c0330,routing,route)==brook::XhciIntelRouteResult::NotApplicable && !routing.reads && routing.writes.empty());
    assert(brook::XhciEnableIntelRouting(0x8086,0x1234,0x0c0330,routing,route)==brook::XhciIntelRouteResult::NotApplicable && !routing.reads);
    assert(brook::XhciEnableIntelRouting(0x8086,0x9cb1,0x0c0330,routing,route)==brook::XhciIntelRouteResult::Ready);
    assert((routing.writes==std::vector<std::pair<unsigned,unsigned>>{{0xd8,0x4000000b},{0xd0,0x80000017}}));
    routing.writes.clear();assert(brook::XhciEnableIntelRouting(0x8086,0x9cb1,0x0c0330,routing,route)==brook::XhciIntelRouteResult::Ready && routing.writes.empty());
    Routing wrongClass;assert(brook::XhciEnableIntelRouting(0x8086,0x9cb1,0x0c0320,wrongClass,route)==brook::XhciIntelRouteResult::NotApplicable && !wrongClass.reads);
    Routing unavailable;unavailable.values[0xdc]=~0u;assert(brook::XhciEnableIntelRouting(0x8086,0x9cb1,0x0c0330,unavailable,route)==brook::XhciIntelRouteResult::Unavailable && unavailable.writes.empty());
    Routing denied;denied.values[0xd4]=denied.values[0xdc]=0;assert(brook::XhciEnableIntelRouting(0x8086,0x9cb1,0x0c0330,denied,route)==brook::XhciIntelRouteResult::Ready && denied.writes.empty());
    Routing failed;failed.ignore=true;assert(brook::XhciEnableIntelRouting(0x8086,0x9cb1,0x0c0330,failed,route)==brook::XhciIntelRouteResult::NotLatched && failed.writes.size()==1 && failed.writes[0].first==0xd8 && failed.values[0xd0]==0x80000002);
    Routing partial;partial.ignoreOffset=0xd0;
    assert(brook::XhciEnableIntelRouting(0x8086,0x9cb1,0x0c0330,partial,route)==brook::XhciIntelRouteResult::NotLatched && partial.writes.size()==2 && partial.values[0xd8]==0x4000000b && partial.values[0xd0]==0x80000002);
    Routing reserved;reserved.values[0xd4]|=0x10000000;reserved.values[0xdc]|=0x10000000;
    assert(brook::XhciEnableIntelRouting(0x8086,0x9cb1,0x0c0330,reserved,route)==brook::XhciIntelRouteResult::Ready && (reserved.values[0xd0]&0x10000000)==0 && (reserved.values[0xd8]&0x10000000)==0);
    static_assert(!(brook::XhciPortPersistent(~0u)&((1u<<1)|(1u<<4)|(1u<<16)|(127u<<17)|(1u<<31))));
    RootPort port;auto persistent=brook::XhciPortPersistent(port.value);
    assert(brook::XhciResetRootPort(port,1000) && port.tick==50 && (port.value&2));
    assert(port.writes.size()==2 && port.writes[0]==(persistent|(1u<<4)) && port.writes[1]==(persistent|(1u<<21)));
    RootPort stale;stale.value|=1u<<21;
    assert(brook::XhciResetRootPort(stale,1000) && stale.tick==50 && stale.writes.size()==3);
    assert(stale.writes.front()==(persistent|(1u<<21)));
    RootPort timeout;timeout.stuck=true;assert(!brook::XhciResetRootPort(timeout,100) && timeout.tick==100 && timeout.writes.size()==1);
    RootPort removed;removed.disconnect=true;assert(!brook::XhciResetRootPort(removed,100) && removed.tick==10);
    RootPort invalid;invalid.inaccessible=true;assert(!brook::XhciResetRootPort(invalid,100) && invalid.writes.empty());
    RootPort absent;absent.value&=~1u;assert(!brook::XhciResetRootPort(absent,100) && absent.writes.empty());
    unsigned legacy;
    brook::XhciMsiLayout layout;
    assert(brook::XhciMsiRegisters(0x80,0,layout) && layout.data==0x88 && !layout.upper);
    assert(brook::XhciMsiRegisters(0x80,0x1f0,layout) && layout.data==0x8c && layout.upper==0x88 && layout.mask==0x90 && !(layout.enabled&0x70));
    assert(!brook::XhciMsiRegisters(0xfc,0x180,layout));
    assert(brook::XhciScratchpadCount(3u<<27)==3);
    assert(brook::XhciScratchpadCount(2u<<21)==64);
    assert(brook::XhciScratchpadCount((31u<<21)|(31u<<27))==1023);
    Registers r;r.values[0x20]=2|(8<<8);r.values[0x40]=1|(1u<<16);r.values[0x44]=0xe011;
    assert(brook::XhciAcquireFirmwareOwnership(r,0x20,65536,legacy));assert(legacy==0x40 && r.byteWrites==1 && r.fullWrites==1);
    assert((r.values[0x40]&(1u<<24)) && !(r.values[0x40]&(1u<<16)));
    r={};r.values[0x40]=1|(1u<<16);r.releaseAt=100;
    assert(!brook::XhciAcquireFirmwareOwnership(r,0x40,65536,legacy));assert(!r.fullWrites);
    r={};r.values[0x40]=1|(1u<<16)|(255<<8);
    assert(!brook::XhciAcquireFirmwareOwnership(r,0x40,128,legacy));assert(!r.byteWrites);
    r={};assert(brook::XhciAcquireFirmwareOwnership(r,0,65536,legacy));assert(!legacy && !r.byteWrites);
    assert(!brook::XhciAcquireFirmwareOwnership(r,3,65536,legacy));
    r.values[0x40]=0xffffffff;assert(!brook::XhciAcquireFirmwareOwnership(r,0x40,65536,legacy));
}
