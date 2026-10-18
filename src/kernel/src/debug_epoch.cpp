#include "debug_epoch.h"
#include "spinlock.h"
#include "string.h"
#include "net.h"

namespace brook {
namespace {
char epoch[33]="00000000000000000000000000000000";
SpinLock epochLock{};
bool Valid(const char* token) {
    bool nonzero=false;
    for(unsigned i=0;i<32;++i) {
        if(!token[i]||!((token[i]>='0'&&token[i]<='9')||(token[i]>='a'&&token[i]<='f')))return false;
        nonzero=nonzero||token[i]!='0';
    }
    return nonzero&&!token[32];
}
}
bool DebugEpochInitialize(const char* proposal) {
    if(!Valid(proposal))return false;
    SpinLockAcquire(&epochLock);
    if(!Valid(epoch))memcpy(epoch,proposal,33);
    SpinLockRelease(&epochLock);return true;
}
void DebugEpochCopy(char out[33]) {
    SpinLockAcquire(&epochLock);memcpy(out,epoch,33);SpinLockRelease(&epochLock);
}
bool DebugEpochMatches(const char* expected) {
    if(!Valid(expected))return false;
    char current[33];DebugEpochCopy(current);return !strcmp(current,expected);
}
bool DebugEpochHandleCommand(const char* command) {
    const char* prefix="bridge epoch";unsigned i=0;
    while(prefix[i]&&prefix[i]==command[i])++i;
    if(prefix[i]||(command[i]&&command[i]!=' '))return false;
    if(command[i]) {
        const char* suffix=" init ";unsigned n=0;
        while(suffix[n]&&command[i+n]==suffix[n])++n;
        if(suffix[n]||!Valid(command+i+n)) {
            DebugChannelSend("{\"epoch_error\":\"Invalid epoch proposal\"}\n");return true;
        }
        DebugEpochInitialize(command+i+n);
    }
    char current[33];DebugEpochCopy(current);
    DebugChannelSend("{\"boot_id\":\"");DebugChannelSend(current);DebugChannelSend("\"}\n");return true;
}
}
