#pragma once
#include "fd_table.h"
#include "record_locks.h"
namespace brook {
struct Process;
RecordLockOwner FileLockOwner(Process* process);
struct FileLockCloseContext {
    RecordLockOwner owner;
    RecordLockNode* retired=nullptr;
    explicit FileLockCloseContext(Process* process):owner(FileLockOwner(process)) {}
    ~FileLockCloseContext() { RecordLockFree(retired); }
    static void Closing(const FdEntry& entry,void* context);
};
}
