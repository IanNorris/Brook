#pragma once
#include <stdint.h>
namespace brook {
// Diagnostic only: the most recent reason a GPU submission or handoff
// operation was refused, set at each refusal site immediately before the
// error is returned (all such sites run under the submission or handoff
// mutex). The syscall layer logs it, rate-limited, when the error reaches
// userspace - hasvk turns any execbuffer error into VK_ERROR_DEVICE_LOST, so
// without this the renderer only sees "device lost".
inline const char* g_bdwRefusal=nullptr;
inline void BdwNoteRefusal(const char* why){__atomic_store_n(&g_bdwRefusal,why,__ATOMIC_RELEASE);}
inline const char* BdwTakeRefusal(){return __atomic_exchange_n(&g_bdwRefusal,static_cast<const char*>(nullptr),__ATOMIC_ACQ_REL);}
// Log the first 16 refusals, then every power of two.
inline bool BdwRefusalLogDue(uint64_t& counter){const uint64_t n=++counter;return n<=16 || !(n&(n-1));}
}
