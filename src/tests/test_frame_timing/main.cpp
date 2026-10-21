#include <cstdio>
#include "../../kernel/src/frame_timing.h"
using namespace brook;
static unsigned failures;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
int main() {
    FrameTimingModel<2> m;FrameTimingRow row;
    m.Begin(1,0);m.End(1,10,20);CHECK(m.Status().completed==0);
    CHECK(m.Control(1));const auto epoch=m.Status().epoch;
    m.Begin(1,1000);
    const auto prep=m.Enter(1,FramePhase::Preparation,1010);
    const auto command=m.Enter(1,FramePhase::QueueSetup,1015);
    m.Command(1,100,true,42);m.Command(2,999,true,43); // another task cannot charge this frame
    const auto wait=m.Enter(1,FramePhase::FenceWait,1020);
    m.Leave(1,wait,1070);m.Leave(1,command,1075);m.Leave(1,prep,1080);m.End(1,1100,1116);
    CHECK(!m.Read(epoch,0,&row)); // refuse live export
    CHECK(m.Control(0));CHECK(m.Read(epoch,0,&row));
    CHECK(row.phases[unsigned(FramePhase::Other)]==30);
    CHECK(row.phases[unsigned(FramePhase::Preparation)]==10);
    CHECK(row.phases[unsigned(FramePhase::QueueSetup)]==10);
    CHECK(row.phases[unsigned(FramePhase::FenceWait)]==50);
    uint64_t total=0;for(auto ns:row.phases)total+=ns;
    CHECK(total==row.endNs-row.beginNs && total==100);
    CHECK(row.commands==1 && row.fencedCommands==1 && row.commandBytes==100);
    CHECK(row.firstFence==42 && row.lastFence==42);
    CHECK(row.commandNsByParent[unsigned(FramePhase::Preparation)]==60);
    CHECK(row.commandsByParent[unsigned(FramePhase::Preparation)]==1);
    CHECK(row.commandNsByParent[unsigned(FramePhase::Preparation)] ==
          row.phases[unsigned(FramePhase::QueueSetup)]+row.phases[unsigned(FramePhase::FenceWait)]);
    CHECK(!row.previousEndNs && row.sleepDeadlineTicks==1116);

    CHECK(m.Control(1));CHECK(m.Status().epoch==epoch+1 && !m.Status().count);
    m.Begin(1,2000);m.End(2,2050,2060);CHECK(m.Active(1));m.End(1,2100,2116);
    m.Begin(1,3000);m.End(1,3100,3116);
    m.Begin(1,4000);m.End(1,4100,4116);
    CHECK(m.Control(0));CHECK(m.Status().completed==3 && m.Status().overwritten==1);
    CHECK(m.Read(epoch+1,0,&row) && row.frame==2 && row.previousEndNs==2100);
    CHECK(m.Read(epoch+1,1,&row) && row.frame==3 && row.previousEndNs==3100);
    CHECK(!m.Read(epoch+1,2,&row) && !m.Read(epoch,0,&row));

    CHECK(m.Control(1));m.Begin(1,0);
    const auto parent=m.Enter(1,FramePhase::Preparation,1);
    const auto child=m.Enter(1,FramePhase::AckWait,2);
    m.Leave(1,parent,3);CHECK(!m.Active(1) && m.Status().invalidFrames==1);
    m.Leave(1,child,4);m.End(1,5,10);CHECK(!m.Status().count);

    CHECK(m.Control(-1,true));m.Begin(1,100);
    const auto old=m.Enter(1,FramePhase::Preparation,110);
    CHECK(m.Control(-1,true));m.Begin(1,200);
    m.Leave(1,old,210);m.End(1,220,230);
    CHECK(m.Status().completed==1 && !m.Status().invalidFrames);
    CHECK(m.Control(1));CHECK(m.Status().completed==1); // idempotent on preserves epoch
    m.Begin(1,300);CHECK(m.Control(0));CHECK(m.Status().abortedFrames==1);
    m.Leave(1,old,400);CHECK(m.Status().abortedFrames==1);

    CHECK(m.Control(1));m.Begin(1,100);
    m.Enter(1,FramePhase::Preparation,99);CHECK(m.Status().invalidFrames==1);
    m.Begin(1,200);m.ClockFailure();CHECK(m.Status().clockFailures==1 && m.Status().invalidFrames==2);
    m.Begin(1,300);m.End(1,310,330);m.Begin(1,309); // reversed inter-frame clock
    CHECK(m.Status().invalidFrames==3);m.End(1,320,340);
    m.Begin(1,400);m.Enter(1,FramePhase::Count,401);CHECK(m.Status().invalidFrames==4);
    m.Begin(1,500);m.Enter(1,FramePhase::QueueSetup,501);m.Command(1,100,true,42);m.Command(1,100,true,42);
    CHECK(m.Status().invalidFrames==5);
    m.Begin(1,600);const auto first=m.Enter(1,FramePhase::QueueSetup,601);
    m.Command(1,100,true,42);m.Leave(1,first,602);
    m.Enter(1,FramePhase::QueueSetup,603);m.Command(1,100,true,42);
    CHECK(m.Status().invalidFrames==6); // reused fence across distinct requests

    CHECK(m.Control(-1,true));m.Begin(1,100);
    const auto priorFrame=m.Enter(1,FramePhase::Preparation,101);
    m.Begin(1,200); // explicitly invalidates the unfinished preceding frame
    m.Leave(1,priorFrame,210);m.End(1,220,230);
    CHECK(m.Status().invalidFrames==1 && m.Status().completed==1);
    std::printf("FRAME_TIMING_MODEL %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
