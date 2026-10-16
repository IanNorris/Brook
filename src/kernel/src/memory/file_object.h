#pragma once
#include "address.h"
#include "sync/kmutex.h"

namespace brook {
struct Vnode;
struct FileObjectPage;
struct FileMappingRegistration;
// Embedded in a filesystem's retained live-inode object, shared across opens.
// Every caller must retain its vnode/inode through the operation. This avoids
// a vnode -> object -> vnode ownership cycle and inode-reuse cache identities.
// No mapped writable pages/writeback yet: ordinary writes are write-through.
struct FileObject {
    KMutex gate;
    uint64_t generation,orderId;
    FileMappingRegistration* mappings;
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
// Initial integration: caller's mapping-pin gate has established no mappings.
// Coordinated mapped resize/invalidation must replace that restriction later.
int FileObjectTruncateUnmapped(FileObject* object,Vnode* vnode,uint64_t length);
}
