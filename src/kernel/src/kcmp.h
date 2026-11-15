#pragma once
// kcmp.h -- Linux kcmp(2) object ordering, independent of the process layer.
//
// kcmp compares kernel objects named by two tasks and returns 0 (same object),
// 1 (first orders before second) or 2 (after). Like Linux, raw kernel
// pointers are never revealed: each type has a boot-random xor/odd-multiplier
// cookie, a bijection on 64 bits, so equality is exact and the order is
// consistent but meaningless across types or boots.
#include <stdint.h>

namespace brook {

enum KcmpType : uint32_t {
    KcmpFile = 0,      // idx1/idx2: descriptors; compares open file descriptions
    KcmpVm = 1,        // address space
    KcmpFiles = 2,     // descriptor table
    KcmpFs = 3,        // root/cwd/umask context
    KcmpSighand = 4,   // signal handler table
    KcmpIo = 5,        // I/O context (Brook has none: always equal, as Linux
                       // tasks that never allocated one)
    KcmpSysvsem = 6,   // SEM_UNDO list, 0 until created
    KcmpEpollTfd = 7,  // epoll target; not supported (EOPNOTSUPP)
    KcmpTypes = 8,
};

struct KcmpCookies {
    uint64_t xorValue[KcmpTypes];
    uint64_t multiplier[KcmpTypes]; // always odd
};

// Fill from an entropy source. Multipliers are forced odd so obfuscation
// stays a bijection.
template<typename Random>
inline void KcmpCookiesInit(KcmpCookies& cookies, Random&& random)
{
    for (uint32_t i = 0; i < KcmpTypes; ++i) {
        cookies.xorValue[i] = random();
        cookies.multiplier[i] = random() | 1;
    }
}

inline int64_t KcmpObfuscate(const KcmpCookies& cookies, uint32_t type, uint64_t value)
{
    return static_cast<int64_t>((value ^ cookies.xorValue[type]) * cookies.multiplier[type]);
}

// Linux kcmp_ptr: (t1 < t2) | ((t1 > t2) << 1) on signed obfuscated values.
inline int64_t KcmpOrder(const KcmpCookies& cookies, uint32_t type, uint64_t first, uint64_t second)
{
    const int64_t a = KcmpObfuscate(cookies, type, first);
    const int64_t b = KcmpObfuscate(cookies, type, second);
    return (a < b ? 1 : 0) | (a > b ? 2 : 0);
}

} // namespace brook
