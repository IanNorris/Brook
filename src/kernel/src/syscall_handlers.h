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

// syscall_time.cpp
int64_t sys_clock_gettime(uint64_t clockid, uint64_t tsAddr, uint64_t,
                           uint64_t, uint64_t, uint64_t);
int64_t sys_clock_getres(uint64_t clockid, uint64_t tsAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t);
int64_t sys_gettimeofday(uint64_t tvAddr, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t);
int64_t sys_time(uint64_t tloc, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_nanosleep(uint64_t reqAddr, uint64_t remAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t);
int64_t sys_clock_nanosleep(uint64_t clockid, uint64_t flags, uint64_t reqAddr,
                             uint64_t remAddr, uint64_t, uint64_t);
int64_t sys_timer_create(uint64_t clock, uint64_t eventAddr, uint64_t idAddr,
                         uint64_t, uint64_t, uint64_t);
int64_t sys_timer_settime(uint64_t id, uint64_t flags, uint64_t valueAddr,
                          uint64_t oldAddr, uint64_t, uint64_t);
int64_t sys_timer_gettime(uint64_t id, uint64_t valueAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t);
int64_t sys_timer_getoverrun(uint64_t id, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t);
int64_t sys_timer_delete(uint64_t id, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t);

// syscall_poll.cpp
int64_t sys_poll(uint64_t fdsAddr, uint64_t nfds, uint64_t timeout,
                uint64_t, uint64_t, uint64_t);
int64_t sys_ppoll(uint64_t fdsAddr, uint64_t nfds, uint64_t tspecAddr,
                 uint64_t sigmaskAddr, uint64_t sigsetsize, uint64_t);
int64_t sys_select(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                  uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t);
int64_t sys_pselect6(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                   uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t sigmaskAddr);

// syscall_epoll.cpp
int64_t sys_epoll_create(uint64_t size, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t);
int64_t sys_epoll_create1(uint64_t flags, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t);
int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t watchfd,
                        uint64_t eventAddr, uint64_t, uint64_t);
int64_t sys_epoll_wait(uint64_t epfd, uint64_t eventsAddr,
                        uint64_t maxevents, uint64_t timeout_ms,
                        uint64_t, uint64_t);
int64_t sys_epoll_pwait(uint64_t epfd, uint64_t eventsAddr,
                         uint64_t maxevents, uint64_t timeout_ms,
                         uint64_t sigmaskAddr, uint64_t sigsetsize);

// syscall_fd.cpp
int64_t sys_close(uint64_t fd, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags,
                        uint64_t, uint64_t, uint64_t);
int64_t sys_dup(uint64_t oldfd, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t);
int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                   uint64_t, uint64_t, uint64_t);

// syscall_open.cpp
int64_t sys_open(uint64_t path,uint64_t flags,uint64_t mode,uint64_t,uint64_t,uint64_t);
int64_t sys_creat(uint64_t path,uint64_t mode,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_openat(uint64_t dirfd,uint64_t path,uint64_t flags,uint64_t mode,uint64_t,uint64_t);

// syscall_rw.cpp
int64_t sys_write(uint64_t fd, uint64_t bufAddr, uint64_t count,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_read(uint64_t fd, uint64_t bufAddr, uint64_t count,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_readv(uint64_t fd, uint64_t iovAddr, uint64_t iovcnt,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_writev(uint64_t fd, uint64_t iovAddr, uint64_t iovcnt,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd, uint64_t offsetAddr,
                      uint64_t count, uint64_t, uint64_t);
int64_t sys_pwrite64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                      uint64_t offset, uint64_t, uint64_t);
int64_t sys_pread64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                     uint64_t offset, uint64_t, uint64_t);

// syscall_stat.cpp
int64_t sys_access(uint64_t pathAddr, uint64_t mode, uint64_t,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_stat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_lstat(uint64_t pathAddr, uint64_t statAddr, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_fstat(uint64_t fd,uint64_t statAddr,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_newfstatat(uint64_t dirfd, uint64_t pathAddr, uint64_t statAddr,
                        uint64_t flags, uint64_t, uint64_t);
int64_t sys_statx(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                   uint64_t mask, uint64_t bufAddr, uint64_t);
int64_t sys_statfs(uint64_t pathAddr, uint64_t bufAddr, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_fstatfs(uint64_t fd, uint64_t bufAddr, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_faccessat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_faccessat2(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                      uint64_t flags, uint64_t, uint64_t);

// syscall_dir.cpp
int64_t sys_getdents64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                        uint64_t, uint64_t, uint64_t);

// syscall_path.cpp
int64_t sys_getcwd(uint64_t bufAddr, uint64_t size, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_readlink(uint64_t pathAddr, uint64_t bufAddr, uint64_t bufsiz,
                      uint64_t, uint64_t, uint64_t);
int64_t sys_readlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t bufAddr,
                        uint64_t bufsiz, uint64_t, uint64_t);
int64_t sys_symlink(uint64_t targetAddr, uint64_t linkpathAddr, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_symlinkat(uint64_t targetAddr, uint64_t newdirfd, uint64_t linkpathAddr,
                       uint64_t, uint64_t, uint64_t);
int64_t sys_umask(uint64_t newMask, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_chdir(uint64_t pathAddr,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_fchdir(uint64_t fd,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_rmdir(uint64_t pathAddr, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_unlink(uint64_t pathAddr, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_rename(uint64_t oldAddr, uint64_t newAddr, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_mkdir(uint64_t pathAddr, uint64_t mode, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_mknodat(uint64_t dirfd,uint64_t pathAddr,uint64_t mode,uint64_t device,uint64_t,uint64_t);
int64_t sys_mknod(uint64_t path,uint64_t mode,uint64_t device,uint64_t,uint64_t,uint64_t);
int64_t sys_mkdirat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_unlinkat(uint64_t dirfd, uint64_t pathAddr, uint64_t flags,
                      uint64_t, uint64_t, uint64_t);
int64_t sys_renameat(uint64_t olddirfd, uint64_t oldAddr,
                      uint64_t newdirfd, uint64_t newAddr,
                      uint64_t, uint64_t);
int64_t sys_renameat2(uint64_t olddirfd, uint64_t oldAddr,
                       uint64_t newdirfd, uint64_t newAddr,
                       uint64_t flags, uint64_t);
int64_t sys_linkat(uint64_t olddirfd, uint64_t oldAddr,
                    uint64_t newdirfd, uint64_t newAddr,
                    uint64_t flags, uint64_t);
int64_t sys_link(uint64_t oldAddr, uint64_t newAddr, uint64_t,
                  uint64_t, uint64_t, uint64_t);

// syscall_file_ops.cpp
int64_t sys_sync(uint64_t, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t);
int64_t sys_fsync(uint64_t fd, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t,
                       uint64_t, uint64_t, uint64_t);
int64_t sys_truncate(uint64_t pathAddr, uint64_t length, uint64_t,
                      uint64_t, uint64_t, uint64_t);
int64_t sys_flock(uint64_t fd, uint64_t operation, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_chmod(uint64_t pathAddr, uint64_t mode, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_fchmodat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                      uint64_t, uint64_t, uint64_t);
int64_t sys_chown(uint64_t pathAddr, uint64_t uid, uint64_t gid,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_lchown(uint64_t pathAddr, uint64_t uid, uint64_t gid,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_fchownat(uint64_t dirfd, uint64_t pathAddr, uint64_t uid,
                      uint64_t gid, uint64_t flags, uint64_t);
int64_t sys_utimensat(uint64_t dirfd, uint64_t pathAddr, uint64_t timesAddr,
                       uint64_t flags, uint64_t, uint64_t);
int64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset,
                       uint64_t length, uint64_t, uint64_t);

// syscall_mount.cpp
int64_t sys_mount(uint64_t sourceAddr,uint64_t targetAddr,uint64_t fsAddr,
                  uint64_t flags,uint64_t dataAddr,uint64_t);
int64_t sys_umount2(uint64_t targetAddr,uint64_t flags,uint64_t,
                    uint64_t,uint64_t,uint64_t);
int64_t sys_pivot_root(uint64_t newRootAddr,uint64_t putOldAddr,uint64_t,uint64_t,uint64_t,uint64_t);

// syscall_ioctl.cpp
int64_t sys_ioctl(uint64_t fd, uint64_t cmd_raw, uint64_t arg,
                   uint64_t, uint64_t, uint64_t);

// syscall_socket.cpp
int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_bind(uint64_t fdVal, uint64_t addrVal, uint64_t addrLen,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_connect(uint64_t fdVal, uint64_t addrVal, uint64_t addrLen,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_sendto(uint64_t fdVal, uint64_t bufVal, uint64_t lenVal,
                    uint64_t flagsVal, uint64_t destVal, uint64_t addrLenVal);
int64_t sys_recvfrom(uint64_t fdVal, uint64_t bufVal, uint64_t lenVal,
                      uint64_t flagsVal, uint64_t srcVal, uint64_t addrLenVal);
int64_t sys_setsockopt(uint64_t fdVal, uint64_t levelVal, uint64_t optionVal,
                        uint64_t valueAddr, uint64_t length, uint64_t);
int64_t sys_getsockopt(uint64_t fdVal, uint64_t levelVal, uint64_t optnameVal,
                        uint64_t optvalAddr, uint64_t optlenAddr, uint64_t);
int64_t sys_getsockname(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                         uint64_t, uint64_t, uint64_t);
int64_t sys_getpeername(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                         uint64_t, uint64_t, uint64_t);
int64_t sys_shutdown(uint64_t fdVal, uint64_t howVal, uint64_t,
                      uint64_t, uint64_t, uint64_t);
int64_t sys_listen(uint64_t fdVal, uint64_t backlog, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_accept(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_accept4(uint64_t fdVal, uint64_t addrVal, uint64_t addrLenVal,
                     uint64_t flags, uint64_t, uint64_t);
int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol,
                        uint64_t svAddr, uint64_t, uint64_t);
int64_t sys_sendmsg(uint64_t fdVal, uint64_t msgVal, uint64_t flagsVal,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_recvmsg(uint64_t fdVal, uint64_t msgVal, uint64_t flagsVal,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_sendmmsg(uint64_t fdVal, uint64_t msgvecVal, uint64_t vlenVal,
                      uint64_t flagsVal, uint64_t, uint64_t);
int64_t sys_recvmmsg(uint64_t fdVal, uint64_t msgvecVal, uint64_t vlenVal,
                      uint64_t flagsVal, uint64_t timeoutVal, uint64_t);

// syscall_process.cpp
int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t,
                        uint64_t, uint64_t, uint64_t);
int64_t sys_exit(uint64_t status, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_exit_group(uint64_t status, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t);
int64_t sys_getpid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_getppid(uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_fork(uint64_t, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t);
int64_t sys_clone(uint64_t flags, uint64_t newStack, uint64_t parentTidAddr,
                   uint64_t childTidAddr, uint64_t tlsAddr, uint64_t);
int64_t sys_vfork(uint64_t, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_clone3(uint64_t argsPtr, uint64_t size, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_wait4(uint64_t pidArg, uint64_t statusAddr, uint64_t rawOptions,
                  uint64_t usageAddr, uint64_t, uint64_t);
int64_t sys_waitid(uint64_t rawSelector, uint64_t rawId, uint64_t infoAddr,
                   uint64_t rawOptions, uint64_t usageAddr, uint64_t);
int64_t sys_set_tid_address(uint64_t tidptr, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
int64_t sys_getpgrp(uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_getpgid(uint64_t pid, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_getsid(uint64_t pid, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_setsid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_gettid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t);
int64_t sys_rseq(uint64_t, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t);

// syscall_exec.cpp
int64_t sys_execve(uint64_t pathAddr, uint64_t argvAddr, uint64_t envpAddr,
                    uint64_t, uint64_t, uint64_t);

// syscall_sched.cpp
int64_t sys_sched_yield(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t);
int64_t sys_membarrier(uint64_t command,uint64_t flags,uint64_t,
                       uint64_t,uint64_t,uint64_t);
int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
                               uint64_t maskAddr, uint64_t, uint64_t, uint64_t);
int64_t sys_sched_setaffinity(uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t);
int64_t sys_sched_setparam(uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t);
int64_t sys_sched_getparam(uint64_t, uint64_t paramAddr, uint64_t,
                            uint64_t, uint64_t, uint64_t);
int64_t sys_sched_setscheduler(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t);
int64_t sys_sched_getscheduler(uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t);
int64_t sys_sched_get_priority_max(uint64_t policy, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);
int64_t sys_sched_get_priority_min(uint64_t policy, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);
int64_t sys_getpriority(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t);
int64_t sys_setpriority(uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t);

// syscall_futex.cpp
int64_t sys_set_robust_list(uint64_t head,uint64_t length,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_get_robust_list(uint64_t pid,uint64_t headOut,uint64_t lengthOut,uint64_t,uint64_t,uint64_t);
int64_t sys_futex(uint64_t uaddrVal, uint64_t opVal, uint64_t val,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);

// syscall_cred.cpp
int64_t sys_getuid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_getgid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t);
int64_t sys_geteuid(uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_getegid(uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_setuid(uint64_t id, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setgid(uint64_t id, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setreuid(uint64_t real, uint64_t effective, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setregid(uint64_t real, uint64_t effective, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_setresuid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t);
int64_t sys_setresgid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t);
int64_t sys_capget(uint64_t header, uint64_t data, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_capset(uint64_t header, uint64_t data, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_getgroups(uint64_t size, uint64_t listAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t);
int64_t sys_setgroups(uint64_t size, uint64_t listAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t);
int64_t sys_getresuid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t);
int64_t sys_getresgid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t);

// syscall_sysinfo.cpp
int64_t sys_uname(uint64_t bufAddr, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t newlimitAddr,
                        uint64_t oldlimitAddr, uint64_t, uint64_t);
int64_t sys_getrandom(uint64_t bufAddr, uint64_t count, uint64_t,
                        uint64_t, uint64_t, uint64_t);
int64_t sys_getrusage(uint64_t who, uint64_t usageAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t);
int64_t sys_sysinfo(uint64_t infoAddr, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t);
int64_t sys_times(uint64_t bufAddr, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t);
int64_t sys_getrlimit(uint64_t resource, uint64_t rlimAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t);

// syscall_sysv_ipc.cpp
int64_t sys_semget(uint64_t key,uint64_t count,uint64_t flags,uint64_t,uint64_t,uint64_t);
int64_t sys_semop(uint64_t id,uint64_t address,uint64_t count,uint64_t,uint64_t,uint64_t);
int64_t sys_semtimedop(uint64_t id,uint64_t address,uint64_t count,uint64_t timeout,uint64_t,uint64_t);
int64_t sys_semctl(uint64_t id,uint64_t number,uint64_t command,uint64_t arg,uint64_t,uint64_t);

// syscall_brook_system.cpp
int64_t sys_brook_system_message(uint64_t after,uint64_t output,uint64_t size,
                                 uint64_t a,uint64_t b,uint64_t c);
int64_t sys_brook_pageable_memory(uint64_t operation,uint64_t value,
                                  uint64_t a,uint64_t b,uint64_t c,uint64_t d);
int64_t sys_brook_profile(uint64_t op, uint64_t a1, uint64_t, uint64_t,
                            uint64_t, uint64_t);
int64_t sys_brook_set_crash_entry(uint64_t entry,
                                   uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t);
int64_t sys_brook_crash_complete(uint64_t exitCode,
                                  uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t);
int64_t sys_brook_input_pop(uint64_t bufAddr, uint64_t maxCount,
                              uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_brook_input_grab(uint64_t enable, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);
int64_t sys_brook_audio_control(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_brook_ahci_export(uint64_t op,uint64_t destination,uint64_t length,uint64_t offset,uint64_t r0,uint64_t r1);

// syscall_brook_wm.cpp
int64_t sys_brook_wm_create_window(uint64_t cW, uint64_t cH,
                                    uint64_t titlePtr, uint64_t outPtr,
                                    uint64_t flags, uint64_t);
int64_t sys_brook_wm_destroy_window(uint64_t wmId, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_signal_dirty(uint64_t wmId, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_publish_source_commit(uint64_t wmId,uint64_t incarnation,uint64_t commit,
                                            uint64_t a,uint64_t b,uint64_t c);
int64_t sys_brook_wm_begin_source_write(uint64_t wmId,uint64_t incarnation,uint64_t a,
                                         uint64_t b,uint64_t c,uint64_t d);
int64_t sys_brook_wm_query_source_commit(uint64_t wmId,uint64_t incarnation,uint64_t generation,
                                          uint64_t output,uint64_t a,uint64_t b);
int64_t sys_brook_wm_present_gres(uint64_t wmId, uint64_t gres,
                                  uint64_t w, uint64_t h,
                                  uint64_t, uint64_t);
int64_t sys_brook_wm_place_window(uint64_t window, uint64_t incarnation,
                                  uint64_t x, uint64_t y, uint64_t flags, uint64_t reserved);
int64_t sys_brook_wm_window_state(uint64_t window, uint64_t incarnation,
                                  uint64_t output, uint64_t size,
                                  uint64_t flags, uint64_t reserved);
int64_t sys_brook_wm_refresh_apps(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_theme(uint64_t op, uint64_t theme,
                          uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_appearance(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_brook_pointer_speed(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_brook_wm_taskbar_metrics(uint64_t op,uint64_t value,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_brook_wm_scale(uint64_t op,uint64_t scale,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_brook_wm_set_client_socket(uint64_t wmId,uint64_t fdValue,
    uint64_t,uint64_t,uint64_t,uint64_t);
int64_t sys_brook_wm_set_app_id(uint64_t wmId, uint64_t appIdPtr,
                               uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_set_title(uint64_t wmId, uint64_t titlePtr,
                                uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_pointer_capture(uint64_t wmId,uint64_t operation,uint64_t token,
                                    uint64_t,uint64_t,uint64_t);
int64_t sys_brook_wm_pop_input(uint64_t wmId, uint64_t bufPtr,
                                uint64_t max, uint64_t,
                                uint64_t, uint64_t);
int64_t sys_brook_wm_resize_vfb(uint64_t wmId, uint64_t newW,
                                 uint64_t newH, uint64_t outPtr,
                                 uint64_t, uint64_t);
int64_t sys_brook_wm_set_decoration_mode(uint64_t wmId, uint64_t csd,
                                          uint64_t, uint64_t,
                                          uint64_t, uint64_t);
int64_t sys_brook_wm_set_window_properties(uint64_t wmId, uint64_t mask,
                                            uint64_t opacity, uint64_t blurRadius,
                                            uint64_t, uint64_t);
int64_t sys_brook_wm_begin_move(uint64_t wmId, uint64_t,
                                 uint64_t, uint64_t,
                                 uint64_t, uint64_t);
int64_t sys_brook_wm_move_relative(uint64_t wmId, uint64_t parentWmId,
                                    uint64_t relX, uint64_t relY,
                                    uint64_t, uint64_t);
int64_t sys_brook_wm_set_maximized(uint64_t wmId, uint64_t enable,
                                    uint64_t, uint64_t,
                                    uint64_t, uint64_t);
int64_t sys_brook_wm_set_minimized(uint64_t wmId, uint64_t,
                                    uint64_t, uint64_t,
                                    uint64_t, uint64_t);
int64_t sys_brook_wm_begin_resize(uint64_t wmId, uint64_t edges,
                                   uint64_t, uint64_t,
                                   uint64_t, uint64_t);
int64_t sys_brook_wm_set_cursor_visible(uint64_t visible, uint64_t,
                                         uint64_t, uint64_t,
                                         uint64_t, uint64_t);
int64_t sys_brook_wm_set_cursor_image(uint64_t pixelsAddr, uint64_t w,
                                       uint64_t h, uint64_t hotX,
                                       uint64_t hotY, uint64_t);
int64_t sys_brook_wm_get_screen_info(uint64_t outAddr, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_scanout_map(uint64_t outAddr, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_scanout_flip(uint64_t minY, uint64_t maxY, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_get_windows(uint64_t outAddr, uint64_t maxCount, uint64_t,
                                  uint64_t, uint64_t, uint64_t);
int64_t sys_brook_wm_map_window_vfb(uint64_t wmId, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);

// syscall_brook_gpu.cpp
int64_t sys_brook_gpu_output_alloc(uint64_t bytes,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d);
int64_t sys_brook_gpu_buffer_try_cpu_read(uint64_t fd,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d);
int64_t sys_brook_gpu_buffer_snapshot_fence(uint64_t fd,uint64_t flags,uint64_t a,uint64_t b,uint64_t c,uint64_t d);
int64_t sys_brook_gpu_handoff_ctl(uint64_t op,uint64_t arg,uint64_t a,uint64_t b,uint64_t c,uint64_t d);
int64_t sys_brook_gpu_window_validate(uint64_t fdValue,uint64_t window,uint64_t incarnation,
                                       uint64_t a,uint64_t b,uint64_t c);
int64_t sys_brook_gpu_window_peer_recovery(uint64_t fdValue,uint64_t op,uint64_t bufferFd,
                                            uint64_t a,uint64_t b,uint64_t c);
int64_t sys_brook_gpu_ctx_create(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t);
int64_t sys_brook_gpu_resource_create(uint64_t ctx, uint64_t format,
                                      uint64_t bind, uint64_t w, uint64_t h,
                                      uint64_t);
int64_t sys_brook_gpu_attach_window(uint64_t ctx, uint64_t resId,
                                    uint64_t wmId, uint64_t, uint64_t, uint64_t);
int64_t sys_brook_gpu_upload_buffer(uint64_t ctx, uint64_t srcPtr,
                                    uint64_t bytes, uint64_t,
                                    uint64_t, uint64_t);
int64_t sys_brook_gpu_submit(uint64_t ctx, uint64_t cmdPtr, uint64_t nDwords,
                             uint64_t, uint64_t, uint64_t);
int64_t sys_brook_gpu_transfer(uint64_t ctx, uint64_t resId, uint64_t dir,
                               uint64_t w, uint64_t h, uint64_t);
int64_t sys_brook_gpu_ctx_destroy(uint64_t ctx, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t);

} // namespace brook
