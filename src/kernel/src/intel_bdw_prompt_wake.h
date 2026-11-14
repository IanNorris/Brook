#pragma once
#include <stdint.h>

namespace brook {
// Runtime gate for M4a prompt GPU wakeups (wait registry arming and RCS
// completion spin slices). Off by default: every boot starts with the
// previous 1 ms polling behaviour, which is known safe on the Dell. Turn it
// on with the `gpu wakeups on` debug command (or /etc/dell-gpu-prompt-wake
// at boot). Off restores polling for waits armed afterwards; waits already
// armed still end at their one-tick timeout.
struct BdwPromptWakeState {
    uint32_t enabled=0;
    uint64_t ifRestores=0;   // IF found clear after a worker wait and re-enabled
    uint64_t armed=0,stale=0,refused=0; // registry outcomes while enabled / refusals while off
};
inline BdwPromptWakeState& BdwPromptWake(){static BdwPromptWakeState s{};return s;}
inline bool BdwPromptWakeEnabled(){return __atomic_load_n(&BdwPromptWake().enabled,__ATOMIC_ACQUIRE)!=0;}
inline void BdwPromptWakeSet(bool on){__atomic_store_n(&BdwPromptWake().enabled,on?1u:0u,__ATOMIC_RELEASE);}
inline void BdwPromptWakeCount(uint64_t& field){__atomic_fetch_add(&field,uint64_t{1},__ATOMIC_RELAXED);}
}
