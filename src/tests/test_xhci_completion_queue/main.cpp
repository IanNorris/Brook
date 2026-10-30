#include "../../drivers/xhci/xhci_completion_queue.h"
#include <cassert>
struct Event {uint64_t param;uint32_t status,control;};
int main(){
    brook::XhciCompletionQueue<Event,3> q;Event out{};
    assert(q.Push({0x1000,1,32u<<10}));assert(q.Push({0x2000,2,33u<<10}));assert(q.Push({0x3000,3,32u<<10}));
    assert(!q.Push({0x4000,4,32u<<10}));
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
