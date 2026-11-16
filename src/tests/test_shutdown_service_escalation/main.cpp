#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "shutdown_service_escalation.h"
using namespace brook;
using A = ServiceStopPolicy::Action;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
int main(){
    ServiceStopPolicy p;
    CHECK(p.Poll(1000000)==A::None);            // nothing before Begin
    CHECK(p.MsUntilKill(0)==0);
    p.Begin(500);
    CHECK(p.Poll(500)==A::None);
    CHECK(p.MsUntilKill(500)==ServiceStopPolicy::KillMs);
    CHECK(p.Poll(500+ServiceStopPolicy::ResendTermMs-1)==A::None);
    CHECK(p.Poll(500+ServiceStopPolicy::ResendTermMs)==A::Term);
    CHECK(p.Poll(500+ServiceStopPolicy::ResendTermMs+1)==A::None); // once only
    CHECK(p.MsUntilKill(500+ServiceStopPolicy::KillMs-1000)==1000);
    CHECK(p.Poll(500+ServiceStopPolicy::KillMs)==A::Kill);
    CHECK(p.Poll(500+ServiceStopPolicy::KillMs+60000)==A::None);   // kill once, then wait
    CHECK(p.MsUntilKill(500+ServiceStopPolicy::KillMs)==0);
    // A late first poll (worker stalled) skips straight to the kill.
    ServiceStopPolicy q;q.Begin(0);
    CHECK(q.Poll(ServiceStopPolicy::KillMs+5)==A::Kill);
    CHECK(q.Poll(ServiceStopPolicy::KillMs+10)==A::None);
    // A new operation restarts the sequence.
    q.Begin(100000);CHECK(q.Poll(100000+ServiceStopPolicy::ResendTermMs)==A::Term);
    static_assert(ServiceStopPolicy::ResendTermMs<ServiceStopPolicy::KillMs,"order");
    // Windowless application escalation: grace -> SIGTERM -> SIGKILL (+rekill).
    using W = WindowlessStopPolicy::Action;
    WindowlessStopPolicy w;
    CHECK(w.Poll(0,false)==W::None && w.stage==WindowlessStopPolicy::Stage::Idle);
    CHECK(w.Poll(1000,true)==W::None && w.MsUntilNext(1000)==WindowlessStopPolicy::GraceMs);
    CHECK(w.Poll(1000+WindowlessStopPolicy::GraceMs-1,true)==W::None);
    CHECK(w.Poll(1000+WindowlessStopPolicy::GraceMs,true)==W::Term);
    const uint64_t t=1000+WindowlessStopPolicy::GraceMs;
    CHECK(w.MsUntilNext(t)==WindowlessStopPolicy::KillMs && !strcmp(w.StageName(),"sigterm"));
    CHECK(w.Poll(t+WindowlessStopPolicy::KillMs-1,true)==W::None);
    CHECK(w.Poll(t+WindowlessStopPolicy::KillMs,true)==W::Kill && w.MsUntilNext(t+WindowlessStopPolicy::KillMs)==0);
    CHECK(w.Poll(t+WindowlessStopPolicy::KillMs+1,true)==W::None);
    CHECK(w.Poll(t+WindowlessStopPolicy::KillMs+WindowlessStopPolicy::RekillMs,true)==W::Kill);
    // An open window (or no remaining blocker) returns to Idle at any stage;
    // the full grace period then restarts, so a save prompt is never killed.
    CHECK(w.Poll(t+60000,false)==W::None && w.stage==WindowlessStopPolicy::Stage::Idle);
    CHECK(w.Poll(t+60001,true)==W::None && !strcmp(w.StageName(),"grace"));
    CHECK(w.Poll(t+60001+WindowlessStopPolicy::GraceMs-1,true)==W::None);
    CHECK(w.Poll(t+60001+WindowlessStopPolicy::GraceMs,true)==W::Term);
    CHECK(w.Poll(t+60002+WindowlessStopPolicy::GraceMs,false)==W::None);
    CHECK(w.Poll(t+70000+WindowlessStopPolicy::GraceMs,true)==W::None); // back to grace, no kill
    // A stalled worker still steps one stage per poll (never TERM+KILL at once).
    WindowlessStopPolicy late;late.Poll(0,true);
    CHECK(late.Poll(1000000,true)==W::Term && late.Poll(1000000,true)==W::None);
    CHECK(late.Poll(2000000,true)==W::Kill);
    late.Reset();CHECK(late.stage==WindowlessStopPolicy::Stage::Idle && late.MsUntilNext(5)==0);
    std::puts("SHUTDOWN_SERVICE_ESCALATION_PASS");
}
