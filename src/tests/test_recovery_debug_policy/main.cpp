#include "../../kernel/src/recovery_debug_policy.h"
#include <cassert>
#include <initializer_list>
using namespace brook;
int main() {
    assert(RecoveryDebugClassify("bridge hardware caps")==RecoveryDebugCommand::Hardware);
    assert(RecoveryDebugClassify("bridge hardware recovery-panic 0 512")==RecoveryDebugCommand::Hardware);
    assert(RecoveryDebugClassify("bridge hardware logs 0 10")==RecoveryDebugCommand::Hardware);
    assert(RecoveryDebugClassify("bridge guard 01234567890123456789012345678901 bridge hardware caps")==RecoveryDebugCommand::Guard);
    assert(RecoveryDebugClassify("bridge epoch init 01234567890123456789012345678901")==RecoveryDebugCommand::Epoch);
    assert(RecoveryDebugClassify("bridge job caps")==RecoveryDebugCommand::JobCaps);
    assert(RecoveryDebugClassify("bridge ui caps")==RecoveryDebugCommand::UiCaps);
    for(const char* command:{"bridge hardware power deadbeef restart","bridge hardware ahci-sector 0", "bridge job run", "reboot", "bridge hardware caps trailing"})
        assert(RecoveryDebugClassify(command)==RecoveryDebugCommand::Deny);
}
