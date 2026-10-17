#pragma once
#include "address.h"
#include "sync/kmutex.h"

namespace brook {
struct Vnode;
struct FileObjectPage;
struct FileMappingRegistration;
// Embedded in a filesystem's retained live-inode object, shared across opens.
// Every caller must retain its vnode/inode through the operation. Mappings use
// that lifetime instead of inode-reuse cache identities. The writeback queue
// explicitly retains a vnode until a successful flush after all mappings retire.
// Ordinary writes are write-through. Writable mappings remain disabled while
// resize coordination, msync and background writeback are incomplete.
struct FileObject {
    KMutex gate;
    uint64_t generation,orderId;
    FileMappingRegistration* mappings;
    // Includes retiring tokens until TLB ACK/unlink; changed by the registry.
    uint64_t mappingRegistrations;
    Vnode* writebackVnode;
    FileObject* writebackNext;
    FileObjectPage* pages[64];
};
void FileObjectInit(FileObject* object);
// Quiescent last-inode release only: no sleeping locks or I/O; IRQ-close safe.
// Frame/heap release still uses their short internal spinlocks.
void FileObjectDispose(FileObject* object);
// Returned page owns a frame reference. Page preparation takes the file gate
// outside AS mutation; backend operations never recursively call the VFS layer.
PhysicalAddress FileObjectGetPage(FileObject* object,Vnode* vnode,uint64_t index,
                                  bool load,int* error,bool* loaded = nullptr);
// One gate acquisition: load primary if necessary, pin only resident neighbors.
// Caller bounds count by its VMA and EOF snapshot; publication must still
// revalidate mapping/file generations. For valid count (1..16), failure clears
// all count slots; invalid count returns -22 without accessing the array.
// on success each nonempty slot is an owned reference, including cache hits.
static constexpr uint32_t FILE_OBJECT_PAGE_BATCH = 16;
int FileObjectGetPages(FileObject* object,Vnode* vnode,uint64_t index,uint32_t count,
                       PhysicalAddress* pages,bool* loaded = nullptr);
int FileObjectRead(FileObject* object,Vnode* vnode,void* buffer,uint64_t length,uint64_t* offset);
int FileObjectWrite(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset);
// Kernel-staged buffer only. Fresh EOF selection, backend write and resident
// cache publication share one inode gate, including across independent opens.
int FileObjectAppend(FileObject* object,Vnode* vnode,const void* buffer,uint64_t length,uint64_t* offset);
// Publication hook: caller owns the file gate and retains its vnode. Mark only
// the canonical frame that will be exposed, before making any writable alias.
// Exposure is conservative and survives flushes while registrations remain.
bool FileObjectExposeWritableLocked(FileObject* object,Vnode* vnode,uint64_t index,PhysicalAddress physical);
// Flush exposed resident pages intersecting [begin,end), clipped to current EOF.
// Uses bounded snapshots, retries short progress, preserves exposure on errors
// and success. Does not itself flush the backend device or enable MAP_SHARED.
// Caller retains vnode; no AS/PT lock or user buffer may span this operation.
int FileObjectWriteback(FileObject* object,Vnode* vnode,uint64_t begin,uint64_t end);
// Finite pass over exposed objects. Successful objects with no registrations
// release their retained vnode outside all locks. Errors keep backing/pages for
// retry; other objects are still attempted. Does not flush backend devices.
int FileObjectWritebackAll();
// Initial integration: caller's mapping-pin gate has established no mappings.
// Coordinated mapped resize/invalidation must replace that restriction later.
int FileObjectTruncateUnmapped(FileObject* object,Vnode* vnode,uint64_t length);
}
