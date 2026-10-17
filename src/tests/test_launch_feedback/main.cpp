#include <cstdio>
#include <cstdlib>
#include "launch_feedback.h"
using namespace brook;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
int main(){
    {
        LaunchFeedbackTable legacy;
        int slot=legacy.Begin(0,0,"Legacy shortcut","","");
        CHECK(legacy.Begin(1,0,"Legacy shortcut","","")==slot);
        CHECK(legacy.Begin(2,0,"Different shortcut","","")!=slot);
    }
    LaunchFeedbackTable t;
    int a=t.Begin(10,20,"Browser","org.brook.browser","browser");CHECK(a==0);
    uint64_t first=t.entries[a].token;
    CHECK(t.Begin(11,20,"Browser","ORG.BROOK.BROWSER","browser")==a);
    CHECK(!t.Window(0,20,"org.brook.browser"));CHECK(!t.Window(0,21,"other"));
    CHECK(!t.Window(0,21,"org.brook.browser")); // a queued request must still execute
    CHECK(t.Take(12)==a);CHECK(t.entries[a].phase==LaunchPhase::Dispatching);
    CHECK(!t.Window(first+1,21,"org.brook.browser"));
    CHECK(t.Window(0,21,"ORG.BROOK.BROWSER")); // delayed app_id, before dispatch completion
    CHECK(t.Find(first)==a);CHECK(t.entries[a].windowObserved);CHECK(!t.entries[a].present());
    CHECK(t.Begin(13,20,"Browser","org.brook.browser","browser")==a); // no second dispatch
    t.Complete(first,false,13);CHECK(t.Find(first)==a);CHECK(t.entries[a].phase==LaunchPhase::Failed);
    CHECK(t.entries[a].present());CHECK(!t.Window(first,21,"org.brook.browser"));
    t.Dismiss(a);CHECK(t.Find(first)<0);
    a=t.Begin(14,30,"Editor","editor","editor");uint64_t second=t.entries[a].token;CHECK(second!=first);
    t.Complete(first,false,15);CHECK(t.entries[a].phase==LaunchPhase::Queued); // slot reuse
    CHECK(t.Take(16)==a);t.Complete(second,true,17);t.Advance(second,18,true);
    CHECK(t.entries[a].phase==LaunchPhase::Waiting); // wrapper may exit; descendants remain
    CHECK(t.Window(0,1,"editor")); // existing single-instance application after dispatch
    a=t.Begin(20,31,"Broken","broken","broken");uint64_t broken=t.entries[a].token;
    CHECK(t.Take(21)==a);t.Complete(broken,false,22);CHECK(t.entries[a].phase==LaunchPhase::Failed);
    CHECK(!t.Window(0,1,"broken")); // an existing window cannot hide dispatch failure
    t.Advance(broken,22+LaunchFeedbackTable::NoticeMs,false);CHECK(t.Find(broken)<0);
    a=t.Begin(40,40,"Slow","slow","slow");uint64_t slow=t.entries[a].token;
    CHECK(t.Take(41)==a);t.Advance(slow,40+LaunchFeedbackTable::TimeoutMs,true);
    CHECK(t.entries[a].phase==LaunchPhase::TimedOut);
    t.Complete(slow,true,41+LaunchFeedbackTable::TimeoutMs);CHECK(t.entries[a].phase==LaunchPhase::TimedOut);
    CHECK(t.Window(slow,42,"")); // timeout does not cancel process; late success clears notice
    a=t.Begin(50,0,"Queued","queued","queued");auto queued=t.entries[a].token;
    CHECK(t.Take(50+LaunchFeedbackTable::TimeoutMs)<0);CHECK(t.entries[a].phase==LaunchPhase::TimedOut);
    t.Dismiss(a);CHECK(t.Find(queued)<0);
    a=t.Begin(60,0,"No window","none","none");auto empty=t.entries[a].token;
    CHECK(t.Take(61)==a);t.Complete(empty,true,62);t.Advance(empty,63,false);
    CHECK(t.entries[a].phase==LaunchPhase::Waiting); // no live tag is not a failed handoff
    CHECK(t.Window(0,1,"none")); // late single-instance window resolves it
    for(unsigned i=0;i<t.Capacity;++i){char id[]={'a',char('0'+i),0};CHECK(t.Begin(70,0,id,id,id)>=0);}
    CHECK(t.Begin(71,0,"Extra","extra","extra")<0);
    {
        LaunchFeedbackTable exact;bool created=false;
        int slot=exact.Begin(0,0,"Editor","org.editor","editor","exact-id","spec-a",&created,1);
        CHECK(slot>=0&&created);
        CHECK(exact.Begin(1,0,"Editor changed","org.editor","editor2","exact-id","spec-a",&created,1)==slot&&!created);
        CHECK(exact.Begin(2,0,"Editor","org.editor","editor","exact-id","spec-b",&created,1)==-2&&!created);
        CHECK(exact.Begin(3,0,"Editor","org.editor","editor","exact-id","spec-a",&created,2)==-2&&!created);
        CHECK(exact.Begin(4,0,"Editor","org.editor","editor","EXACT-ID","spec-a",&created,1)!=slot&&created);
        auto token=exact.entries[slot].token;CHECK(exact.Take(5)==slot);
        CHECK(exact.Window(token,1,"org.editor"));CHECK(exact.entries[slot].inFlight());
        exact.Advance(token,LaunchFeedbackTable::TimeoutMs+1,true);
        exact.Dismiss(slot);CHECK(exact.Find(token)==slot); // timeout/dismiss cannot forget an active worker
        CHECK(exact.Begin(LaunchFeedbackTable::TimeoutMs+2,0,"Editor","org.editor","editor","exact-id","spec-a",&created,1)==slot&&!created);
        exact.Complete(token,true,LaunchFeedbackTable::TimeoutMs+3);CHECK(exact.Find(token)<0);
    }
    std::puts("launch feedback: identity, coalescing, completion races, reuse, exhaustion, timeout PASS");
}
