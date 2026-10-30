#include "../../drivers/xhci/xhci_completion_queue.h"
#include <cassert>
struct Event {uint64_t param;uint32_t status,control;};
struct Consumer {
    Event ring[256]{};unsigned cursor=0,completed=0;bool cycle=true;
    Consumer(){brook::XhciInitializeLink(ring[255],0x1000);}
    void Poll(){
        for(unsigned limit=0;limit<1024;++limit) {
            auto& trb=ring[cursor];if(bool(trb.control&1)!=cycle)return;
            auto type=(trb.control>>10)&63;
            if(type==6){assert(cursor==255 && trb.param==0x1000);if(trb.control&2)cycle=!cycle;cursor=0;}
            else {assert(type==1 && cursor<255);++completed;++cursor;}
        }
        assert(false && "consumer entered an owned-ring loop");
    }
    void Initial(unsigned count){for(unsigned i=0;i<count;++i)ring[i].control=(1u<<10)|1u;Poll();}
};
static void PublicationTests(){
    // Complete entry254 before software has queued the following TD. The
    // initial Link must stop traversal before a future Link-first submission.
    Consumer c;c.Initial(255);assert(c.cursor==255 && c.cycle && c.completed==255);
    for(unsigned lap=0;lap<2;++lap) {
        const bool next=!c.cycle;brook::XhciTdPublication gate;
        gate.Stage(c.ring[255].control,brook::XhciLinkControl(c.cycle,false));
        gate.Stage(c.ring[0].control,(1u<<10)|(next?1u:0u));
        c.Poll();assert(c.cursor==255);gate.Publish();c.Poll();
        assert(c.cursor==1 && c.cycle==next);
        for(unsigned i=1;i<255;++i) {
            brook::XhciTdPublication single;single.Stage(c.ring[i].control,(1u<<10)|(next?1u:0u));
            c.Poll();assert(c.cursor==i);single.Publish();c.Poll();
        }
        assert(c.cursor==255 && c.cycle==next);
    }
    Consumer split;split.Initial(254);brook::XhciTdPublication gate;
    gate.Stage(split.ring[254].control,(1u<<10)|16u|1u);
    gate.Stage(split.ring[255].control,brook::XhciLinkControl(true,true));
    gate.Stage(split.ring[0].control,1u<<10);
    split.Poll();assert(split.cursor==254 && split.completed==254);
    gate.Publish();split.Poll();assert(split.cursor==1 && !split.cycle && split.completed==256);
    Consumer afterLink;afterLink.Initial(255);brook::XhciTdPublication linkGate;
    linkGate.Stage(afterLink.ring[255].control,brook::XhciLinkControl(true,false));
    linkGate.Stage(afterLink.ring[0].control,(1u<<10)|16u);
    linkGate.Stage(afterLink.ring[1].control,1u<<10);
    afterLink.Poll();assert(afterLink.cursor==255);
    linkGate.Publish();afterLink.Poll();assert(afterLink.cursor==2 && !afterLink.cycle);
}
int main(){
    PublicationTests();
    assert(brook::XhciTdPackets(65536,32768,1024)==31);
    assert(brook::XhciTdPackets(65536,61440,1024)==4);
    assert(brook::XhciTdPackets(65536,65536,1024)==0);
    assert(brook::XhciTdPackets(1500,512,1024)==2); // partial packet crosses TRBs
    assert(brook::XhciTdPackets(1500,1024,1024)==1);
    assert(brook::XhciTdPackets(13,13,1024)==0);
    assert(brook::XhciTdPackets(512,256,0)==0);
    assert(brook::XhciTdPackets(65536,61440,512)==8);
    assert(brook::XhciTdPackets(65536,61440,64)==31);
    brook::XhciCompletionQueue<Event,3> q;Event out{};
    assert(q.Push({0x1000,1,32u<<10}));assert(q.Push({0x2000,2,33u<<10}));assert(q.Push({0x3000,3,32u<<10}));
    assert(!q.Push({0x4000,4,32u<<10}));
    const uint64_t td[]{0x5000,0x3000};
    assert(q.TakeAny(32u<<10,td,2,out) && out.param==0x3000);
    assert(q.Push({0x3000,3,32u<<10}));
    assert(!q.Take(32u<<10,0x2000,out));
    assert(q.Take(32u<<10,0x3000,out) && out.status==3);
    assert(q.Take(33u<<10,0x2000,out) && out.status==2);
    assert(q.Take(32u<<10,0x1000,out) && out.status==1);
    assert(!q.Take(0,0,out));
    for(uint64_t p=0;p<65536;p+=4096) {
        const auto first=brook::XhciDmaChunk(p,65536);
        assert(first && first<=65536 && p+first<=65536);
        const auto rest=65536-first;if(rest)assert(brook::XhciDmaChunk(p+first,rest)==rest);
    }
}
