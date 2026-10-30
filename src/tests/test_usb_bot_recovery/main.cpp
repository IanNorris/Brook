#include "../../drivers/xhci/usb_bot_recovery.h"
#include "../../drivers/xhci/xhci_completion_queue.h"
#include <cassert>
#include <string>
using namespace brook;
struct Io {
    UsbBotReceive first=UsbBotReceive::Stall,second=UsbBotReceive::Complete;
    unsigned calls=0,fail=0;std::string order;
    UsbBotReceive receive(){order+='R';return calls++?second:first;}
    bool clearHalt(){order+='C';return fail!=1;}
    bool resetEndpoint(){order+='E';return fail!=2;}
    bool setDequeue(){order+='D';return fail!=3;}
};
int main() {
    Io good;assert(UsbBotReceiveCsw(good)&&good.order=="RCEDR"&&good.calls==2);
    Io complete;complete.first=UsbBotReceive::Complete;assert(UsbBotReceiveCsw(complete)&&complete.order=="R");
    Io timeout;timeout.first=UsbBotReceive::Failure;assert(!UsbBotReceiveCsw(timeout)&&timeout.order=="R");
    for(unsigned failed=1;failed<=3;failed++) {
        Io step;step.fail=failed;assert(!UsbBotReceiveCsw(step));
        assert(step.calls==1&&step.order==std::string("RCED").substr(0,failed+1));
    }
    Io twice;twice.second=UsbBotReceive::Stall;assert(!UsbBotReceiveCsw(twice)&&twice.calls==2);
    Io secondError;secondError.second=UsbBotReceive::Failure;assert(!UsbBotReceiveCsw(secondError)&&secondError.calls==2);
    for(bool cycle:{false,true}) {
        for(unsigned index:{0u,1u,254u,255u}) {
            auto next=UsbBotNextDequeue(0x1000,index,cycle,256);assert(next.valid);
            assert(next.index==(index==255?0:index)&&next.skipLink==(index==255));
            assert(next.cycle==(index==255?!cycle:cycle));
            assert(next.pointer==0x1000+uint64_t(next.index)*16+(next.cycle?1:0));
            if(next.skipLink) {
                uint32_t link=XhciLinkControl(!next.cycle,false);
                assert(bool(link&1)!=next.cycle); // cannot cross the next wrap early
            }
        }
    }
    assert(!UsbBotNextDequeue(0x1001,1,true,256).valid);
    assert(!UsbBotNextDequeue(0x1000,256,true,256).valid);
    assert(!UsbBotNextDequeue(~uint64_t{0}-63,1,true,256).valid);
    assert(UsbBotStatusValid(71,71,0,false,4096,4096,0));
    assert(!UsbBotStatusValid(71,72,0,false,4096,4096,0));
    assert(!UsbBotStatusValid(71,71,2,false,4096,4096,0));
    assert(!UsbBotStatusValid(71,71,0,false,4096,4096,4));
    assert(UsbBotStatusValid(71,71,0,true,192,192,168)); // padding ->24 relevant bytes
    assert(UsbBotStatusValid(71,71,1,true,192,0,192));
    assert(!UsbBotStatusValid(71,71,0,true,192,4,0));
    assert(!UsbBotStatusValid(71,71,0,true,192,193,0));
    assert(!UsbBotStatusValid(71,71,0,true,192,0,193));
}
