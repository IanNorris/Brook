#include "../../kernel/src/ahci_observe.h"
#include <cassert>
using namespace brook;
struct Mock {
    uint16_t cmd=0;unsigned writes=0,reads=0;bool refuse=false;bool busMasterChanged=false;
    bool lostReadback=false,failRestore=false;
    uint16_t command() {if(lostReadback && writes==1){lostReadback=false;return 0xffff;}return cmd;}
    void command(uint16_t v) {++writes;if ((v^cmd)&4)busMasterChanged=true;if((!refuse || !(v&2)) && !(failRestore && writes==2))cmd=v;}
    uint32_t read(uint32_t off) {++reads;if(off==0)return 5;if(off==0x0c)return 1;if(off==0x10)return 0x10300;if(off==0x128)return 0x103;if(off==0x124)return 0x101;return 0;}
};
int main() {
    uint64_t base=0;
    assert(AhciObserveBar(0xf7422000,base) && base==0xf7422000);
    assert(!AhciObserveBar(0,base));
    assert(!AhciObserveBar(0xf7422001,base));
    assert(!AhciObserveBar(0xf7422004,base));
    assert(!AhciObserveBar(0xfffff001,base));
    assert(AhciObservePortMap(5,0x3f));
    assert(!AhciObservePortMap(5,0x40));
    assert(!AhciObservePortMap(31,0x80000000));
    assert(!AhciObservePortMap(0xffffffff,1));
    assert(!AhciObservePortMap(5,0xffffffff));
    Mock good;AhciObservation observed{};AhciObserveRegisters(good,observed);
    assert(observed.valid && observed.portCount==1 && observed.ports[0].status==0x103);
    assert(observed.ports[0].signature==0x101 && good.cmd==0 && good.writes==2 && !good.busMasterChanged);
    Mock fail;fail.refuse=true;AhciObservation rejected{};AhciObserveRegisters(fail,rejected);
    assert(!rejected.valid && fail.reads==0 && fail.cmd==0 && fail.writes==2 && !fail.busMasterChanged);
    Mock active;active.cmd=4;AhciObservation held{};AhciObserveRegisters(active,held);
    assert(!held.valid && active.reads==0 && active.writes==0 && active.cmd==4);
    Mock enabled;enabled.cmd=6;AhciObservation existing{};AhciObserveRegisters(enabled,existing);
    assert(existing.valid && enabled.writes==0 && enabled.cmd==6);
    Mock unavailable;unavailable.lostReadback=true;AhciObservation missing{};AhciObserveRegisters(unavailable,missing);
    assert(!missing.valid && unavailable.reads==0 && unavailable.cmd==0 && unavailable.writes==2);
    Mock restore;restore.failRestore=true;AhciObservation unclean{};AhciObserveRegisters(restore,unclean);
    assert(!unclean.valid && restore.cmd==2 && restore.writes==2);
}
