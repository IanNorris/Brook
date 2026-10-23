#pragma once
#include "address.h"
#include "sync/kmutex.h"
#include "record_locks.h"
#include "mapping_fault.h"

namespace brook {
struct Vnode;
struct FileObjectPage;
struct FileMappingRegistration;
// Embedded in a filesystem's retained live-inode object, shared across opens.
// Every caller must retain its vnode/inode through the operation. Mappings use
// that lifetime instead of inode-reuse cache identities. The writeback queue
// explicitly retains a vnode until a successful flush after all mappings retire.
// Ordinary writes are write-through. Writable shared mappings use canonical
// pages, conservative exposure, msync/fsync and periodic writeback. Mapped
// truncate remains rejected until resize coordination is enabled.
struct FileObject {
    RecordLockTable recordLocks;
    KMutex gate;
    // Offset reservation/copyout serialization, ordered before gate. Fault page
    // preparation uses backend ops directly and never acquires this mutex.
    KMutex ioOffsetGate;
    uint64_t generation,orderId;
    // Gate-owned unknown-EOF quarantine. Size writers remain blocked until
    // recovery first observes EOF and reconciles every retained private alias.
    int mappingError;
    uint64_t privateValidPrefix;
    FileMappingRegistration* mappings;
    // Includes retiring tokens until TLB ACK/unlink; changed by the registry.
    uint64_t mappingRegistrations;
    Vnode* writebackVnode;
    FileObject* writebackNext;
    FileObjectPage* pages[64];
    // Optional backend quota for canonical frames and their metadata. Installed
    // before inode publication. Release must be non-sleeping (IRQ final-close).
    void* pageBudgetContext;
    bool (*reservePageBytes)(void*,uint64_t);
    void (*releasePageBytes)(void*,uint64_t);
    // Under inode gate, before canonical allocation/exposure. Backends may
    // materialize sparse storage so later shared writeback cannot need quota.
    int (*prepareBackingPage)(void*,uint64_t);
};
void FileObjectInit(FileObject* object);
// Quiescent last-inode release only: no sleeping locks or I/O; IRQ-close safe.
// Frame/heap release still uses their short internal spinlocks.
void FileObjectDispose(FileObject* object);
// Returned page owns a frame reference. Page preparation takes the file gate
// outside AS mutation; backend operations never recursively call the VFS layer.
PhysicalAddress FileObjectGetPage(FileObject* object,Vnode* vnode,uint64_t index,
                                  bool load,int* error,bool* loaded = nullptr);
// Pure identity lookup with the inode gate already owned; safe under a PT
// spinlock. Never allocates, loads a page or acquires another lock.
bool FileObjectIsCanonicalLocked(FileObject* object,uint64_t index,PhysicalAddress physical);
// Caller owns inode gate and has detached/ACKed every alias before marking.
// Canonical data MUST have flushed successfully before the resize attempt;
// orphan retirement can discard that stale cache without writing it again.
// Preserve private data hidden; never treat a failed backend attempt as rollback.
void FileObjectQuarantineLocked(FileObject* object,int error,uint64_t lastKnownPrefix);
// Caller has re-statted EOF, reconciled all private aliases and ACKed stale
// canonical mappings; old canonical data was flushed BEFORE the resize attempt.
// Drop stale cache, clear quarantine and advance epoch. No AS/PT lock held.
void FileObjectCompleteMappingRecoveryLocked(FileObject* object);
// One gate acquisition: load primary if necessary, pin only resident neighbors.
// Caller bounds count by its VMA and EOF snapshot; publication must still
// revalidate mapping/file generations. For valid count (1..16), failure clears
// all count slots; invalid count returns -22 without accessing the array.
// on success each nonempty slot is an owned reference, including cache hits.
static constexpr uint32_t FILE_OBJECT_PAGE_BATCH = 16;
int FileObjectGetPages(FileObject* object,Vnode* vnode,uint64_t index,uint32_t count,
                       PhysicalAddress* pages,bool* loaded = nullptr);
// Fault preparation: primary EOF classification and neighbor bounds are sampled
// under the same inode gate as loading. All slots are cleared on entry; owned
// references returned on success. Caller must revalidate its VMA/file epoch.
MappingFaultResult FileObjectFaultPages(FileObject* object,Vnode* vnode,uint64_t index,
                                       uint32_t capacity,PhysicalAddress* pages,bool* loaded=nullptr);
int FileObjectRead(FileObject* object,Vnode* vnode,void* buffer,uint64_t length,uint64_t* offset);
int FileObjectWrite(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset);
// Kernel-staged buffer only. Fresh EOF selection, backend write and resident
// cache publication share one inode gate, including across independent opens.
int FileObjectAppend(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset);
// Publication hook: caller owns the file gate and retains its vnode. Mark only
// the canonical frame that will be exposed, before making any writable alias.
// Exposure is conservative and survives flushes while registrations remain.
bool FileObjectExposeWritableLocked(FileObject* object,Vnode* vnode,uint64_t index,PhysicalAddress physical);
// Conservative exposure before mprotect grants write permission. The file
// gate is owned; marks resident canonical pages only, without I/O or allocation.
bool FileObjectExposeRangeLocked(FileObject* object,Vnode* vnode,uint64_t begin,uint64_t end);
// Flush exposed resident pages intersecting [begin,end), clipped to current EOF.
// Uses bounded snapshots, retries short progress, preserves exposure on errors
// and success. Does not itself flush the backend device or enable MAP_SHARED.
// Caller retains vnode; no AS/PT lock or user buffer may span this operation.
int FileObjectWriteback(FileObject* object,Vnode* vnode,uint64_t begin,uint64_t end);
// Finite pass over exposed objects. Successful objects with no registrations
// release their retained vnode outside all locks. Errors keep backing/pages for
// retry; other objects are still attempted. Quarantined objects report their
// error but retire stale cache/queue ownership once every mapping is gone.
// Quarantine requires a successful pre-attempt flush. No backend-device flush.
int FileObjectWritebackAll();
// Kernel boot/shutdown integration. Stop waits for the active pass before
// device write admission closes; explicit shutdown sync then performs the final
// flush. Neither call is used by the standalone/hosted fixtures.
void FileObjectStartWriteback();
void FileObjectStopWriteback();
// Initial integration: caller's mapping-pin gate has established no mappings.
// Coordinated mapped resize/invalidation must replace that restriction later.
int FileObjectTruncateUnmapped(FileObject* object,Vnode* vnode,uint64_t length);
}
