#include "../../kernel/src/ssd_install_gate.h"
#include <cassert>
#include <thread>
using namespace brook;
int main(){
    SsdInstallGate active;assert(active.Begin());assert(!active.Begin());assert(!active.CloseReady());
    active.Finish();assert(active.CloseReady());assert(!active.Begin());
    SsdInstallGate finished;assert(finished.Begin());finished.Finish();assert(!finished.Begin());assert(finished.CloseReady());
    SsdInstallGate closed;assert(closed.CloseReady());assert(!closed.Begin());
    for(unsigned i=0;i<1000;++i){SsdInstallGate g;bool began=false,closedReady=false;
        std::thread a([&]{began=g.Begin();});std::thread b([&]{closedReady=g.CloseReady();});a.join();b.join();
        assert(began!=closedReady);if(began){assert(!g.CloseReady());g.Finish();}assert(g.CloseReady());assert(!g.Begin());}
}
