// syscall_sysv_ipc.cpp -- System V semaphore syscalls.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process_user_namespace.h"

#include "posix_timer.h"
#include "sysv_sem.h"
#include "timeout_value.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_time.h"
#include "syscall_handlers.h"

namespace brook {

// SysV semaphores: user memory is staged before entering the IPC service.
int64_t sys_semget(uint64_t key,uint64_t count,uint64_t flags,uint64_t,uint64_t,uint64_t)
{
    return SysvSemGet(ProcessCurrent(),int32_t(key),int(count),int(flags));
}
static int64_t SemUserOperation(uint64_t id,uint64_t address,uint64_t count,uint64_t timeoutAddress)
{
    if(!count)return -EINVAL;
    if(count>500)return -E2BIG;
    uint64_t deadline=~uint64_t{0};
    if(timeoutAddress) {
        PosixTimespec timeout{};uint64_t duration=0;
        if(!CopyFromUser(&timeout,timeoutAddress,sizeof(timeout)))return -EFAULT;
        if(!TimeoutNanoseconds(timeout.sec,timeout.nsec,&duration))return -EINVAL;
        deadline=TimeoutAddSafe(ClockNowNs(1),duration);
    }
    KernelOperationGuard guard(ProcessCurrent());
    auto* ops=static_cast<SysvSemOp*>(kmalloc(count*sizeof(SysvSemOp)));
    if(!ops)return -ENOMEM;
    int result=CopyFromUser(ops,address,count*sizeof(SysvSemOp))?
        SysvSemOperate(ProcessCurrent(),int(id),ops,unsigned(count),deadline):-EFAULT;
    kfree(ops);return result;
}
int64_t sys_semop(uint64_t id,uint64_t address,uint64_t count,uint64_t,uint64_t,uint64_t)
{ return SemUserOperation(id,address,count,0); }
int64_t sys_semtimedop(uint64_t id,uint64_t address,uint64_t count,uint64_t timeout,uint64_t,uint64_t)
{ return SemUserOperation(id,address,count,timeout); }
int64_t sys_semctl(uint64_t id,uint64_t number,uint64_t command,uint64_t arg,uint64_t,uint64_t)
{
    KernelOperationGuard guard(ProcessCurrent());
    const int cmd=int(command)&~0x100;
    SysvSemDs ds{};uint16_t* values=nullptr;int count=0;
    if(cmd==1) {
        if(!CopyFromUser(&ds,arg,sizeof(ds)))return -EFAULT;
        ds.permission.uid=ProcessResolveId(ProcessCurrent(),false,ds.permission.uid);
        ds.permission.gid=ProcessResolveId(ProcessCurrent(),true,ds.permission.gid);
        if(ds.permission.uid==UserIdInvalid || ds.permission.gid==UserIdInvalid)return -EINVAL;
    }
    if(cmd==13 || cmd==17) {
        count=SysvSemCount(ProcessCurrent(),int(id));if(count<0)return count;
        values=static_cast<uint16_t*>(kmalloc(unsigned(count)*sizeof(uint16_t)));
        if(!values)return -ENOMEM;
        if(cmd==17 && !CopyFromUser(values,arg,unsigned(count)*sizeof(uint16_t))) {
            kfree(values);return -EFAULT;
        }
    }
    int result=SysvSemControl(ProcessCurrent(),int(id),int(number),int(command),int(arg),&ds,values,unsigned(count));
    if(result>=0 && cmd==2) {
        ds.permission.uid=ProcessReportId(ProcessCurrent(),false,ds.permission.uid);
        ds.permission.cuid=ProcessReportId(ProcessCurrent(),false,ds.permission.cuid);
        ds.permission.gid=ProcessReportId(ProcessCurrent(),true,ds.permission.gid);
        ds.permission.cgid=ProcessReportId(ProcessCurrent(),true,ds.permission.cgid);
    }
    if(result>=0 && ((cmd==2 && !CopyToUser(arg,&ds,sizeof(ds))) ||
        (cmd==13 && !CopyToUser(arg,values,unsigned(count)*sizeof(uint16_t)))))result=-EFAULT;
    kfree(values);return result;
}

} // namespace brook
