#include <cstdio>
#include <cstdlib>
#include "launch_history.h"
using namespace brook;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
int main() {
    LaunchFeedbackTable feedback;
    LaunchHistory history;
    int slot=feedback.Begin(10,20,"Editor","editor","editor");
    const auto token=feedback.entries[slot].token;
    CHECK(feedback.Take(12)==slot);
    CHECK(feedback.Window(token,21,"editor",nullptr,5,21,14));
    // Bridge adoption after the early window vanished retains the original fact.
    CHECK(history.Track(feedback.entries[slot],false));
    auto& r=history.records[history.Find(token)];
    CHECK(r.startedTick==12&&r.windowTick==14&&r.windowObserved&&r.windowId==5&&r.windowIncarnation==21&&r.viaToken);
    CHECK(!r.bridgeOnly&&!LaunchHistory::NeedsObservation(r,15));
    history.Complete(token,-2,16);
    CHECK(r.rawResult==-2&&r.dispatchTick==16&&r.windowObserved);
    CHECK(LaunchFeedbackTable::Exact(LaunchHistory::State(r,17),"failed"));
    CHECK(!LaunchHistory::NeedsObservation(r,17));

    LaunchFeedback entry{};entry.token=100;entry.requested=30;entry.windowCeiling=80;
    LaunchFeedbackTable::Copy(entry.appId,sizeof(entry.appId),"single");
    CHECK(history.Track(entry,true));history.Start(100,32);history.Complete(100,0,34);
    auto& single=history.records[history.Find(100)];
    history.Observe(999,true,70,"single",2,70,35);
    CHECK(single.windowObserved&&single.existingWindow&&!single.viaToken);
    CHECK(LaunchHistory::NeedsObservation(single,35));
    history.Observe(100,true,90,"single",3,90,36);
    CHECK(single.viaToken&&!single.existingWindow&&single.windowId==3);
    history.ShareWithUser(100);CHECK(!single.bridgeOnly);

    entry.token=101;CHECK(history.Track(entry,true));history.Start(101,32);
    auto& late=history.records[history.Find(101)];
    const auto deadline=entry.requested+LaunchFeedbackTable::TimeoutMs;
    CHECK(!LaunchHistory::NeedsObservation(late,deadline));
    history.Observe(101,false,100,"",9,100,deadline);CHECK(!late.windowObserved);
    history.Complete(101,-5,deadline+1);CHECK(late.rawResult==-5&&late.dispatchFinished);
    CHECK(LaunchFeedbackTable::Exact(LaunchHistory::State(late,deadline+2),"failed"));
    entry.token=102;CHECK(history.Track(entry,true));history.Start(102,32);
    auto& cancelled=history.records[history.Find(102)];cancelled.cancelled=true;
    CHECK(!LaunchHistory::NeedsObservation(cancelled,34));
    history.Observe(102,false,90,"",7,90,35);CHECK(!cancelled.windowObserved);
    CHECK(LaunchFeedbackTable::Exact(LaunchHistory::State(cancelled,35),"cancelled"));

    for(unsigned i=0;i<LaunchHistory::Capacity-4;++i){entry.token=1000+i;CHECK(history.Track(entry,true));}
    CHECK(!history.CanTrack());CHECK(history.CanTrack(token));
    CHECK(history.Track(feedback.entries[slot],true));CHECK(!r.bridgeOnly);
    entry.token=99999;CHECK(!history.Track(entry,true));CHECK(history.Find(token)>=0);
    std::puts("launch history tests passed");
}
