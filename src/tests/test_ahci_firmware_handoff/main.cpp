#include "../../kernel/src/ahci_firmware_handoff.h"
#include <cassert>
using namespace brook::ahci_ro;
struct Registers { uint32_t r[32]{};bool stuck=false,ignoreStop=false;unsigned writes=0,pointerWrites=0,restarts=0; };
struct Port {
    Registers* p;
    uint32_t read(uint32_t o){return p->r[o/4];}
    void write(uint32_t o,uint32_t v){++p->writes;if(o==CLB || o==CLBU || o==FB || o==FBU)++p->pointerWrites;
        if(o==CMD && ((v&~p->r[o/4])&(ST|FRE)))++p->restarts;
        if(o==CMD && p->ignoreStop)return;p->r[o/4]=v;}
    void fence(){}
    bool waitClear(uint32_t o,uint32_t mask){if(p->stuck)return false;p->r[o/4]&=~mask;return true;}
};
struct Controller {
    Registers regs[2]{};bool disable=true;unsigned disableCalls=0;
    Port port(uint32_t i){return Port{&regs[i]};}
    bool disableInterrupts(){++disableCalls;return disable;}
};
int main() {
    uint32_t failed=0;Controller c;
    c.regs[0].r[CMD/4]=ST|FRE|CR|FR;c.regs[0].r[0x28/4]=3;
    c.regs[1].r[CMD/4]=FRE|FR;
    assert(QuiesceFirmware(c,3,2,failed)==HandoffResult::Ok && failed==0xffffffffu);
    assert(!(c.regs[0].r[CMD/4]&(ST|FRE|CR|FR)) && !(c.regs[1].r[CMD/4]&(ST|FRE|CR|FR)));
    Controller pending;pending.regs[1].r[CI/4]=1;
    assert(QuiesceFirmware(pending,3,2,failed)==HandoffResult::CommandsActive && failed==1);
    assert(!pending.disableCalls && !pending.regs[0].writes && !pending.regs[1].writes);
    Controller ncq;ncq.regs[0].r[SACT/4]=1;assert(QuiesceFirmware(ncq,1,1,failed)==HandoffResult::CommandsActive && !ncq.disableCalls);
    Controller stale;stale.regs[0].r[0x28/4]=3;stale.regs[0].r[TFD/4]=0x7f;assert(QuiesceFirmware(stale,1,1,failed)==HandoffResult::Ok);
    Controller busy;busy.regs[0].r[CMD/4]=FRE|FR;busy.regs[0].r[0x28/4]=3;busy.regs[0].r[TFD/4]=0x80;assert(QuiesceFirmware(busy,1,1,failed)==HandoffResult::DeviceBusy && !busy.disableCalls);
    Controller missing;missing.regs[1].r[CMD/4]=0xffffffffu;assert(QuiesceFirmware(missing,3,2,failed)==HandoffResult::Invalid && !missing.disableCalls);
    Controller irq;irq.disable=false;assert(QuiesceFirmware(irq,1,1,failed)==HandoffResult::Interrupts && !irq.regs[0].writes);
    Controller stuck;stuck.regs[0].r[CMD/4]=ST|FRE;stuck.regs[1].r[CMD/4]=ST|FRE;stuck.regs[1].stuck=true;
    assert(QuiesceFirmware(stuck,3,2,failed)==HandoffResult::StopFailed && failed==1);
    assert(!(stuck.regs[0].r[CMD/4]&(ST|FRE)));
    assert(!stuck.regs[0].pointerWrites && !stuck.regs[1].pointerWrites && !stuck.regs[0].restarts && !stuck.regs[1].restarts);
    Controller ignored;ignored.regs[0].r[CMD/4]=ST|FRE;ignored.regs[0].ignoreStop=true;
    assert(QuiesceFirmware(ignored,1,1,failed)==HandoffResult::StopFailed);
}
