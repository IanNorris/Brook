#include "intel_bdw_render_gt_init.h"
#include "intel_bdw_render_gt_mmio.h"
#include <cassert>
#include <cstdint>
using namespace brook;
struct Fake {
    uint32_t regs[6]={0x12340000,0xffffffffu&~0x10000000u,0x200,0x100,0x200,1};
    uint32_t l3=0x12000034;unsigned writes=0,mcrWrites=0,delays=0,reads=0,failWrite=0;
    bool held=true,idle=true,safe=true,steered=true,failMcr=false,failDelay=false,unstable=false;
    uint32_t selector=0,fuse=1u<<25;bool unstableTopology=false;unsigned topologyReads=0;
    static int index(uint32_t reg){const uint32_t all[]={0x4090,0x20a0,0x2050,0x9430,0x9400,0x9424};
        for(unsigned i=0;i<6;++i)if(all[i]==reg)return i;
        return -1;}
    bool owned(){return held;}bool allEnginesQuiescent(){return idle;}
    bool displayAndFirmwareSafe(){return safe;}bool mcrSteeringReady(){return steered;}
    bool readMcrTopology(uint32_t& s,uint32_t& f){s=selector;f=fuse;if(unstableTopology && ++topologyReads==2)s^=1u<<24;return true;}
    bool readReg(uint32_t reg,uint32_t& value){const int i=index(reg);if(i<0)return false;
        value=regs[i];if(unstable && ++reads==2)value^=1;return true;}
    bool writeReg(uint32_t reg,uint32_t value){const int i=index(reg);if(i<0)return false;
        ++writes;if(i==2){const uint32_t mask=value>>16;regs[i]=(regs[i]&~mask)|(value&mask);}else regs[i]=value;
        return writes!=failWrite;}
    bool readMcr(uint32_t reg,uint32_t& value){if(reg!=0xb100)return false;value=l3;return true;}
    bool writeMcrBroadcast(uint32_t reg,uint32_t value){if(reg!=0xb100)return false;++mcrWrites;l3=value;return !failMcr;}
    bool delayAtLeastOneMicrosecond(){++delays;return !failDelay;}
};
int main(){
    // A valid selector must name an enabled slice and a globally enabled
    // subslice. Reserved selector values and inaccessible reads fail closed.
    {const uint32_t fuse=(5u<<25)|(2u<<21);
     assert(BdwMcrSelectionPresent(0,fuse));
     assert(BdwMcrSelectionPresent((2u<<26)|(2u<<24),fuse));
     assert(!BdwMcrSelectionPresent(1u<<26,fuse));
     assert(!BdwMcrSelectionPresent(1u<<24,fuse));
     assert(!BdwMcrSelectionPresent(3u<<26,fuse));
     assert(!BdwMcrSelectionPresent(3u<<24,fuse));
     assert(!BdwMcrSelectionPresent(0,0));
     assert(!BdwMcrSelectionPresent(0xffffffffu,fuse));
     assert(!BdwMcrSelectionPresent(0,0xffffffffu));}
    {Fake io;const auto result=BdwInitializeRenderGt(io);
     assert(result.status==BdwRenderGtInitStatus::Applied && result.stage==7 && result.writes==8);
     assert((io.regs[0]&0x40) && !(io.regs[1]&0x88000) && (io.regs[2]&0x1000));
     assert((io.regs[3]&0x4000) && (io.regs[4]&0x10000) && io.regs[5]==1);
     assert((io.l3&((0x1fu<<19)|(0x1fu<<14)))==((15u<<19)|(1u<<14)));
     assert(io.mcrWrites==1 && io.delays==1 && result.dopRestoreAttempted && result.dopRestored);}
    {Fake io;io.safe=false;const auto result=BdwInitializeRenderGt(io);
     assert(result.status==BdwRenderGtInitStatus::NotStarted && !result.writes && !io.writes);}
    {Fake io;io.unstable=true;const auto result=BdwInitializeRenderGt(io);
     assert(result.status==BdwRenderGtInitStatus::NotStarted && !io.writes);}
    {Fake io;io.failWrite=6;const auto result=BdwInitializeRenderGt(io);
     assert(result.status==BdwRenderGtInitStatus::Uncertain && result.dopRestoreAttempted && result.dopRestored);
     assert(!io.mcrWrites && !io.delays && io.regs[5]==1);}
    {Fake io;io.failMcr=true;const auto result=BdwInitializeRenderGt(io);
     assert(result.status==BdwRenderGtInitStatus::Uncertain && result.dopRestoreAttempted && result.dopRestored);
     assert(io.mcrWrites==1 && io.delays==1 && io.regs[5]==1);}
    {Fake io;io.failDelay=true;const auto result=BdwInitializeRenderGt(io);
     assert(result.status==BdwRenderGtInitStatus::Uncertain && !result.dopRestoreAttempted && io.delays==1 && io.regs[5]==0);}
    {Fake io;io.failWrite=7;const auto result=BdwInitializeRenderGt(io);
     assert(result.status==BdwRenderGtInitStatus::Uncertain && result.dopRestoreAttempted && !result.dopRestored);}
    {Fake owner;uint32_t engine[1024]{},gam[1024]{},gt[1024]{};
     engine[0xa0/4]=0x88000;gt[0x424/4]=1;
     BdwRenderGtMmio<Fake> mmio(owner,engine,gam,gt);
     assert(!mmio.writeReg(0x4090,0x80) && !mmio.writeReg(0x2050,0x10000000));
     assert(!mmio.writeReg(0x2209c,0) && !mmio.writeMcrBroadcast(0xb100,0));
     const auto result=BdwInitializeRenderGt(mmio);
     assert(result.status==BdwRenderGtInitStatus::Applied && gam[0x90/4]==0x40);
     assert(!(engine[0xa0/4]&0x88000) && engine[0x50/4]==0x10001000);
     assert(gt[0x400/4]==0x10000 && gt[0x430/4]==0x4000 && gt[0x424/4]==1);
     owner.held=false;uint32_t value=0;assert(!mmio.readReg(0x4090,value) && !mmio.writeReg(0x4090,0x40));}
    {Fake owner;uint32_t engine[1024]{},gam[1024]{},gt[1024]{};
     BdwRenderGtMmio<Fake> mmio(owner,engine,gam,gt);
     owner.fuse=0;assert(!mmio.mcrSteeringReady());
     owner.fuse=1u<<25;owner.unstableTopology=true;assert(!mmio.mcrSteeringReady());}
}
