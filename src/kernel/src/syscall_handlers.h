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

} // namespace brook
