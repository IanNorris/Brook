#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define BROOK_HOST_TEST
#include "../../kernel/src/spinlock.h"
#include "../../kernel/src/ioapic_route_policy.h"
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line%d %s\n",__LINE__,#x);std::abort();}}while(0)
namespace brook {
static uint64_t g_ioApicVirt=1;
static uint32_t g_ioApicGsiBase=0;
static uint32_t g_ioApicEntries=24;
static IoApicRoutePolicy g_ioApicRoutePolicy;
static IrqSpinLock g_ioApicRouteLock;
static uint32_t regs[256];
static std::vector<unsigned> writes;
static uint8_t destination=2;
static uint8_t ApicGetId(){return destination;}
static uint32_t IoApicRead(uint8_t reg){return regs[reg];}
static void IoApicWrite(uint8_t reg,uint32_t value){regs[reg]=value;writes.push_back(reg);}
#include "../../kernel/src/ioapic_route.inc"
}
using namespace brook;
int main(){
 for(auto& r:regs)r=0x10000;
 CHECK(IoApicSelectBootLevelPin(11));
 const auto unrelated=regs[0x10+2*10];
 IoApicUnmaskIrq(11,50);
 CHECK(regs[0x10+2*11]==(0x8000|50)&&regs[0x11+2*11]==(2u<<24));
 CHECK(regs[0x10+2*10]==unrelated);
 CHECK(writes==std::vector<unsigned>({0x11+22,0x10+22}));
 CHECK(!IoApicSelectBootLevelPin(11));
 IoApicMaskIrq(11);CHECK(regs[0x10+22]==(0x18000|50));
 destination=7;IoApicUnmaskIrq(11,50);
 CHECK(regs[0x10+22]==(0x8000|50)&&regs[0x11+22]==(2u<<24));
 // Preserve polarity/delivery/reserved/read-only observation bits exactly.
 const uint32_t old=0x182000|0x10000|0x2000|0x4000|0x1000|0x700|40;
 regs[0x10+22]=old;const auto high=regs[0x11+22];IoApicUnmaskIrq(11,51);
 CHECK(regs[0x10+22]==((old&~uint32_t(0xff|0x10000))|0x8000|51));
 CHECK(regs[0x11+22]==high);
 // Unselected ISA/other PCI lines retain init's edge/high policy.
 IoApicUnmaskIrq(10,48);CHECK(regs[0x10+20]==48);
 CHECK(!IoApicSelectBootLevelPin(10));
 CHECK(!IoApicSelectBootLevelPin(24)&&!IoApicSelectBootLevelPin(255));
 g_ioApicGsiBase=32;uint8_t pin=255;
 CHECK(!IoApicSelectBootLevelGsi(31,true,pin) && !IoApicSelectBootLevelGsi(56,true,pin));
 CHECK(!IoApicSelectBootLevelGsi(43,true,pin) && pin==255); // already published
 const auto before=writes.size();
 CHECK(IoApicSelectBootLevelGsi(41,false,pin) && pin==9 && writes.size()==before);
 CHECK(!IoApicSelectBootLevelGsi(41,true,pin));
 regs[0x10+18]=0x12000; // stale active-low from earlier firmware
 IoApicUnmaskIrq(pin,49);
 CHECK(regs[0x10+18]==(0x8000|49));
 CHECK(IoApicSelectBootLevelGsi(44,true,pin) && pin==12);
 IoApicUnmaskIrq(pin,52);IoApicMaskIrq(pin);IoApicUnmaskIrq(pin,52);
 CHECK(regs[0x10+24]==(0xa000|52));
 g_ioApicVirt=0;CHECK(!IoApicSelectBootLevelPin(12));
 std::puts("IOAPIC_ROUTE_ACTUAL_BODY_PASS");
}
