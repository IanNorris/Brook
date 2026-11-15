// H-04 hardening: a second process cannot silently take over input from a
// live grabber; the grabber itself, root, or anyone after it exits can.
#include "harness.h"
#include "input_grab_policy.h"
#include <stdlib.h>

using namespace harness;
static brook::Process* NewProcess(uint16_t pid,uint32_t euid) {
    auto* p=static_cast<brook::Process*>(calloc(1,sizeof(brook::Process)));
    p->magic=brook::PROCESS_MAGIC;p->pid=pid;p->euid=euid;p->state=brook::ProcessState::Running;
    return p;
}
void RunGrabTests() {
    auto* server=NewProcess(20,1000);auto* other=NewProcess(21,1000);auto* root=NewProcess(22,0);
    CHECK(brook::InputGrabTakeoverAllowed(nullptr,other));   // nobody grabbing
    CHECK(!brook::InputGrabTakeoverAllowed(server,other));   // live grabber
    CHECK(brook::InputGrabTakeoverAllowed(server,server));   // re-grab by the owner
    CHECK(brook::InputGrabTakeoverAllowed(server,root));     // root takeover
    server->state=brook::ProcessState::Terminated;           // a dead grabber
    CHECK(brook::InputGrabTakeoverAllowed(server,other));
}
