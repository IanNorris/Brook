#pragma once

// syscall_handlers.h -- syscall handler entry points registered in the dispatch table (syscall.cpp).
// Internal to the syscall layer; split from syscall.cpp
// (see docs/agent/syscall-split-plan.md).

#include <stdint.h>

namespace brook {

// syscall_signal.cpp
int64_t sys_alarm(uint64_t seconds, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_pause(uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_rt_sigsuspend(uint64_t maskAddr, uint64_t sigsetsize, uint64_t,
                            uint64_t, uint64_t, uint64_t);
int64_t sys_rt_sigtimedwait(uint64_t setAddr, uint64_t infoAddr,
                            uint64_t timeoutAddr, uint64_t sigsetsize,
                            uint64_t, uint64_t);
int64_t sys_rt_sigaction(uint64_t signum, uint64_t actAddr, uint64_t oldactAddr,
                          uint64_t sigsetsize, uint64_t, uint64_t);
int64_t sys_rt_sigpending(uint64_t setAddr, uint64_t sigsetsize,
                         uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_rt_sigprocmask(uint64_t how, uint64_t setAddr, uint64_t oldAddr,
                            uint64_t sigsetsize, uint64_t, uint64_t);
int64_t sys_signalfd4(uint64_t descriptor,uint64_t maskAddress,uint64_t size,uint64_t flags,uint64_t,uint64_t);
int64_t sys_signalfd(uint64_t descriptor,uint64_t mask,uint64_t size,uint64_t,uint64_t,uint64_t);
int64_t sys_sigaltstack(uint64_t ssAddr, uint64_t oldSsAddr, uint64_t,
                         uint64_t, uint64_t, uint64_t);
int64_t sys_rt_sigreturn(uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t);
int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t,
                  uint64_t, uint64_t, uint64_t);

// syscall_eventfd.cpp
int64_t sys_eventfd2(uint64_t initval, uint64_t flagsVal, uint64_t,
                       uint64_t, uint64_t, uint64_t);

// syscall_timerfd.cpp
int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags,
                            uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_timerfd_settime(uint64_t fd, uint64_t flagsVal,
                             uint64_t newValAddr, uint64_t oldValAddr,
                             uint64_t, uint64_t);
int64_t sys_timerfd_gettime(uint64_t fd, uint64_t currValAddr,
                             uint64_t, uint64_t, uint64_t, uint64_t);

// syscall_memfd.cpp
int64_t sys_memfd_create(uint64_t nameAddr, uint64_t flags,
                           uint64_t, uint64_t, uint64_t, uint64_t);

// syscall_pipe.cpp
int64_t sys_pipe(uint64_t address,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_pipe2(uint64_t pipefdAddr, uint64_t flags, uint64_t,
                   uint64_t, uint64_t, uint64_t);

// syscall_memory.cpp
int64_t sys_brk(uint64_t requested,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset);
int64_t sys_mprotect(uint64_t addr,uint64_t length,uint64_t prot,uint64_t,uint64_t,uint64_t);
int64_t sys_msync(uint64_t addr,uint64_t length,uint64_t flags,uint64_t,uint64_t,uint64_t);
int64_t sys_munmap(uint64_t addr,uint64_t length,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_mremap(uint64_t source,uint64_t oldLength,uint64_t newLength,uint64_t flags,uint64_t destination,uint64_t);
int64_t sys_mincore(uint64_t addr, uint64_t length, uint64_t vecAddr,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_madvise(uint64_t address, uint64_t length, uint64_t advice,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_fadvise64(uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t);
int64_t sys_mbind(uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);

} // namespace brook
