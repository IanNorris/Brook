#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include "../../kernel/src/frame_timing.h"
#include "../../kernel/src/frame_timing_reply.h"
static unsigned failures,tries,clockReads,ownerReads,sends;
static uintptr_t currentOwner=1;
static uint64_t now=100;
static bool refuseTry,clockValid=true;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
namespace brook {
struct SpinLock {bool held=false;};
static void SpinLockAcquire(SpinLock* lock){CHECK(!lock->held);lock->held=true;}
static bool SpinLockTryAcquire(SpinLock* lock){++tries;if(refuseTry || lock->held)return false;lock->held=true;return true;}
static void SpinLockRelease(SpinLock* lock){CHECK(lock->held);lock->held=false;}
static void* ProcessCurrent(){++ownerReads;return reinterpret_cast<void*>(currentOwner);}
static bool KvmClockReadStableNs(uint64_t* output){++clockReads;*output=now++;return clockValid;}
}
#include "../../kernel/src/frame_timing_runtime.inc"
using namespace brook;
static std::string serialized;
static void send(const char* text){++sends;CHECK(std::strlen(text)<=767);serialized+=text;}
int main() {
    FrameScopeToken token;
    FrameTimingBegin();FrameTimingEnter(unsigned(FramePhase::Preparation),&token);
    FrameTimingCommand(10,false,0);FrameTimingEnd(10);
    CHECK(!tries && !clockReads && !ownerReads && !token.id);
    CHECK(FrameTimingControl(1));FrameTimingBegin();
    FrameTimingEnter(unsigned(FramePhase::Preparation),&token);CHECK(token.id);
    const auto oldTries=tries,oldClocks=clockReads;
    currentOwner=2;refuseTry=true;
    FrameScopeToken foreign;
    FrameTimingEnter(unsigned(FramePhase::QueueSetup),&foreign);
    FrameTimingCommand(99,true,99);FrameTimingLeave(&token);FrameTimingEnd(20);
    CHECK(!foreign.id && tries==oldTries && clockReads==oldClocks);
    CHECK(FrameTimingReadStatus().contendedRecords==0);
    currentOwner=1;refuseTry=false;
    FrameTimingLeave(&token);FrameTimingEnd(30);CHECK(FrameTimingControl(0));
    auto status=FrameTimingReadStatus();FrameTimingRow row;
    CHECK(status.completed==1 && !status.invalidFrames && !status.contendedRecords);
    CHECK(FrameTimingReadRow(status.epoch,0,&row) && row.commands==0);
    // Genuine owner losses remain visible and fail the capture quality gate.
    CHECK(FrameTimingControl(1));FrameTimingBegin();refuseTry=true;
    FrameTimingEnter(unsigned(FramePhase::Preparation),&token);CHECK(!token.id);
    CHECK(FrameTimingReadStatus().contendedRecords==1);
    refuseTry=false;FrameTimingEnd(40);FrameTimingControl(0);
    CHECK(FrameTimingReadStatus().contendedRecords==1);
    // Reset/stop clear the hint, and stale tokens still require model epochs.
    FrameTimingControl(1);FrameTimingBegin();FrameTimingEnter(unsigned(FramePhase::Preparation),&token);
    FrameTimingControl(-1,true);const auto prior=tries;FrameTimingLeave(&token);CHECK(tries==prior);
    FrameTimingBegin();FrameTimingLeave(&token);FrameTimingEnd(50);FrameTimingControl(0);
    CHECK(!FrameTimingReadStatus().invalidFrames);
    // An unavailable clock cannot publish a recorder owner.
    FrameTimingControl(1);clockValid=false;FrameTimingBegin();const auto afterFailure=tries;
    FrameTimingEnter(unsigned(FramePhase::Preparation),&token);CHECK(tries==afterFailure && !token.id);
    clockValid=true;FrameTimingControl(0);

    // Exact serialized bytes, including values that cross fixed-size chunks.
    row={};const uint64_t maximum=~uint64_t(0);
    row.frame=row.beginNs=row.endNs=row.previousEndNs=row.sleepDeadlineTicks=maximum;
    row.commands=row.commandBytes=row.fencedCommands=row.firstFence=row.lastFence=row.commandPreparationNs=maximum;
    for(unsigned i=0;i<FramePhaseCount;++i)row.phases[i]=row.commandNsByParent[i]=row.commandsByParent[i]=maximum;
    FrameTimingWriteRow<send>(row,true);
    std::string expected=",{";
    const char* fields[]={"frame","begin_ns","end_ns","previous_end_ns","sleep_deadline_ticks","commands","command_bytes","fenced_commands","first_fence","last_fence","command_preparation_ns"};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i){if(i)expected+=',';expected+='"';expected+=fields[i];expected+="\":";expected+=std::to_string(maximum);}
    for(const char* name:{"phase_ns","command_ns_by_parent","commands_by_parent"}){
        expected+=",\"";expected+=name;expected+="\":[";
        for(unsigned i=0;i<FramePhaseCount;++i){if(i)expected+=',';expected+=std::to_string(maximum);}
        expected+=']';
    }
    expected+='}';CHECK(serialized==expected && sends<=3);
    std::printf("FRAME_TIMING_RUNTIME %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
