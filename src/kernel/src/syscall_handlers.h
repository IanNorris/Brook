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

} // namespace brook
