#include "../../kernel/src/ahci_readonly_sequence.h"
#include <cassert>
#include <initializer_list>
using namespace brook::ahci_ro;
struct Mock {
    uint32_t regs[32]{};unsigned writes=0,fences=0;uint32_t transferredBytes=512;
    bool stuckCi=false,stuckCr=false,stuckFr=false,ignoreStop=false,ignoreFisStop=false,unavailableAfterWrite=false;
    uint32_t ignoredOffset=0xffffffffu;
    uint32_t injectIs=0;
    uint32_t read(uint32_t off){return regs[off/4];}
    void write(uint32_t off,uint32_t value){++writes;if(off==IS || off==SERR)regs[off/4]&=~value;else if(off==ignoredOffset){}else if(off==CMD && ignoreStop && !(value&ST)){}else if(off==CMD && ignoreFisStop && !(value&FRE)){}else regs[off/4]=value;if(off==CMD && unavailableAfterWrite)regs[off/4]=0xffffffffu;}
    void fence(){++fences;}
    bool waitClear(uint32_t off,uint32_t mask){if((off==CI&&stuckCi)||(mask==CR&&stuckCr)||(mask==FR&&stuckFr))return false;regs[off/4]&=~mask;if(off==CI)regs[IS/4]|=injectIs;return true;}
    uint32_t transferred(){return transferredBytes;}
};
int main(){
    Mock m;
    assert(ControllerModeReady(0x80000000u));
    assert(!ControllerModeReady(0xffffffffu) && !ControllerModeReady(0x80000001u) && !ControllerModeReady(0x80000002u));
    assert(Prepare(m,0x1000,0x2000,false)==Result::Ok);
    assert(m.regs[CLB/4]==0x1000 && m.regs[FB/4]==0x2000 && m.fences==1);
    assert(Start(m)==Result::Ok && (m.regs[CMD/4]&(ST|FRE))==(ST|FRE));
    assert(Submit(m,512)==Result::Ok && m.fences==3);
    assert(Quiesce(m) && !(m.regs[CMD/4]&(ST|FRE)));
    Mock busy;busy.regs[CI/4]=1;assert(Prepare(busy,0x1000,0x2000,false)==Result::Busy && busy.writes==0);
    Mock invalid;assert(Prepare(invalid,0x1001,0x2000,false)==Result::InvalidDma && invalid.writes==0);
    for(uint32_t off:{CLB,CLBU,FB,FBU}){Mock ignored;ignored.regs[off/4]=0xdeadbeefu;ignored.ignoredOffset=off;assert(Prepare(ignored,0x1000,0x2000,false)==Result::DeviceError);}
    assert(Prepare(invalid,0x100000000ull,0x2000,false)==Result::InvalidDma && invalid.writes==0);
    Mock tfd;tfd.regs[TFD/4]=0x80;assert(Submit(tfd,512)==Result::DeviceBusy && tfd.writes==0);
    Mock timeout;timeout.stuckCi=true;assert(Submit(timeout,512)==Result::Timeout);
    timeout.stuckCr=true;assert(!Quiesce(timeout));
    Mock stop;stop.regs[CMD/4]=ST|FRE;stop.ignoreStop=true;assert(!Quiesce(stop));
    Mock fis;fis.regs[CMD/4]=FRE;fis.ignoreFisStop=true;assert(!Quiesce(fis));
    Mock fr;fr.regs[CMD/4]=FRE|FR;fr.stuckFr=true;assert(!Quiesce(fr));
    Mock unavailable;unavailable.unavailableAfterWrite=true;assert(Start(unavailable)==Result::DeviceError);
    Mock shortIo;shortIo.transferredBytes=511;assert(Submit(shortIo,512)==Result::ShortTransfer);
    Mock error;error.regs[TFD/4]=1;assert(Submit(error,512)==Result::DeviceError);
    for(uint32_t bit:{24u,27u,28u,29u,30u}){Mock fatal;fatal.injectIs=1u<<bit;assert(Submit(fatal,512)==Result::DeviceError);}
}
