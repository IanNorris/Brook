#include <cstdio>
#include <cstdlib>
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
    std::puts("SHUTDOWN_SERVICE_ESCALATION_PASS");
}
