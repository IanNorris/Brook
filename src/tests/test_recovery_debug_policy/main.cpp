#include "../../kernel/src/recovery_debug_policy.h"
#include "../../kernel/src/debug_local_policy.h"
#include <cstring>
#include <cassert>
#include <initializer_list>
using namespace brook;
int main() {
    auto local=[](const char* command) { return DebugLocalRequestPolicy(command,std::strlen(command)); };
    assert(local("bridge hardware caps").allowed);
    assert(local("@1 bridge hardware caps").allowed && local("@1 bridge hardware caps").tokenBytes==1);
    assert(local("@12345678901234567890123 bridge hardware caps").allowed);
    assert(!local("@123456789012345678901234 bridge hardware caps").allowed);
    assert(!local("@ bridge hardware caps").allowed);
    assert(!local("@2 @1 bridge hardware caps").allowed);
    assert(!local("@2 bridge hardware caps trailing").allowed);
    assert(!local("@2 bridge stats").allowed && local("@2 bridge stats").tokenBytes==1);
    assert(!local("bridge job caps").allowed);
    constexpr char embedded[]="bridge hardware caps\0trailing";
    assert(!DebugLocalRequestPolicy(embedded,sizeof(embedded)-1).allowed);

    assert(RecoveryDebugClassify("bridge hardware caps")==RecoveryDebugCommand::Hardware);
    assert(RecoveryDebugClassify("bridge hardware recovery-panic 0 512")==RecoveryDebugCommand::Hardware);
    assert(RecoveryDebugClassify("bridge hardware recovery-panic-previous 0 512")==RecoveryDebugCommand::Hardware);
    assert(RecoveryDebugClassify("bridge hardware logs 0 10")==RecoveryDebugCommand::Hardware);
    assert(RecoveryDebugClassify("bridge guard 01234567890123456789012345678901 bridge hardware caps")==RecoveryDebugCommand::Guard);
    assert(RecoveryDebugClassify("bridge epoch init 01234567890123456789012345678901")==RecoveryDebugCommand::Epoch);
    assert(RecoveryDebugClassify("bridge job caps")==RecoveryDebugCommand::JobCaps);
    assert(RecoveryDebugClassify("bridge ui caps")==RecoveryDebugCommand::UiCaps);
    for(const char* command:{"bridge hardware power deadbeef restart","bridge hardware ahci-sector 0", "bridge job run", "reboot", "bridge hardware caps trailing", "bridge slot status", "bridge slot select 0"})
        assert(RecoveryDebugClassify(command)==RecoveryDebugCommand::Deny);
}
