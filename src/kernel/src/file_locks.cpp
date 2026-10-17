#include "file_locks.h"
#include "process.h"
#include "vfs.h"
#include "memory/file_object.h"
namespace brook {
RecordLockOwner FileLockOwner(Process* process) {
    return {process->isThread?process->leaderIncarnation:process->incarnation,
            process->tgid?process->tgid:process->pid};
}
void FileLockCloseContext::Closing(const FdEntry& entry,void* context) {
    if(entry.type!=FdType::Vnode || !entry.handle)return;
    auto* vnode=static_cast<Vnode*>(entry.handle);
    // file_object is a borrowed, nonblocking inode accessor, not a stat/I/O op.
    auto* object=vnode->ops->file_object?vnode->ops->file_object(vnode):nullptr;
    if(!object)return;
    auto& close=*static_cast<FileLockCloseContext*>(context);
    RecordLockReleaseOwner(object->recordLocks,close.owner,close.retired);
}
}
