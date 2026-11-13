#pragma once
#include <stdint.h>
namespace brook {
struct BdwGpuBuffer;
struct BdwGpuFile;
struct BdwGpuContext;
struct BdwGpuSync;
struct BdwFence;
uint32_t BdwGpuFileCreateSync(BdwGpuFile* file,bool signaled);
BdwGpuSync* BdwGpuFileGetSync(BdwGpuFile* file,uint32_t handle);
uint32_t BdwGpuFileImportSync(BdwGpuFile* file,BdwGpuSync* sync);
bool BdwGpuFileDestroySync(BdwGpuFile* file,uint32_t handle);
void BdwGpuSyncRef(BdwGpuSync* sync);
void BdwGpuSyncUnref(BdwGpuSync* sync);
BdwFence* BdwGpuSyncAcquire(BdwGpuSync* sync);
bool BdwGpuSyncReplace(BdwGpuSync* sync,BdwFence* fence);
// Internal timeline API only. A caller must not expose a prepared fence until
// it has serialized publication with queue admission and handled rollback.
BdwFence* BdwGpuSyncPrepareTimeline(BdwGpuSync* sync,uint64_t point,BdwFence* dependency=nullptr);
bool BdwGpuSyncPublishTimeline(BdwGpuSync* sync,BdwFence* fence);
BdwFence* BdwGpuSyncFindTimeline(BdwGpuSync* sync,uint64_t point);
uint64_t BdwGpuSyncQueryTimeline(BdwGpuSync* sync,bool lastSubmitted);
// Context references survive handle destruction and file close. The worker
// exclusively owns an active context until Finish proves the engine stopped.
uint32_t BdwGpuFileCreateContext(BdwGpuFile* file);
BdwGpuContext* BdwGpuFileGetContext(BdwGpuFile* file,uint32_t id);
BdwGpuContext* BdwGpuFileGetDefaultContext(BdwGpuFile* file);
bool BdwGpuFileDestroyContext(BdwGpuFile* file,uint32_t id);
void BdwGpuContextRef(BdwGpuContext* context);
void BdwGpuContextUnref(BdwGpuContext* context);
bool BdwGpuContextBegin(BdwGpuContext* context,uint32_t (&saved)[20*1024],bool& present);
bool BdwGpuContextFinish(BdwGpuContext* context,const uint32_t (*saved)[20*1024],bool safelyStopped);
void BdwGpuContextMarkLost(BdwGpuContext* context,bool pending);
uint32_t BdwGpuContextLoss(const BdwGpuContext* context);
// Internal per-open handle table. Callers must hold a file reference across
// every operation. Returned buffers carry their own reference. Closing a GEM
// handle does not revoke a retained mapping or GPU submission reference.
BdwGpuFile* BdwGpuFileCreate();
void BdwGpuFileRef(BdwGpuFile* file);
void BdwGpuFileUnref(BdwGpuFile* file);
uint32_t BdwGpuFileCreateBuffer(BdwGpuFile* file,uint64_t requested,uint64_t& rounded);
// Import a retained buffer into this file, returning an existing handle when
// already present. The slot acquires its own reference on first import.
uint32_t BdwGpuFileImportBuffer(BdwGpuFile* file,BdwGpuBuffer* buffer);
bool BdwGpuFileCloseBuffer(BdwGpuFile* file,uint32_t handle);
BdwGpuBuffer* BdwGpuFileGetBuffer(BdwGpuFile* file,uint32_t handle);
// Resolve a copied handle array atomically under the file lock. On success
// every output owns one buffer reference; on failure outputs are unchanged.
// Input and output must be disjoint kernel-owned arrays.
bool BdwGpuFileResolveBuffers(BdwGpuFile* file,const uint32_t* handles,uint32_t count,BdwGpuBuffer** out);
uint64_t BdwGpuFileMapToken(BdwGpuFile* file,uint32_t handle);
BdwGpuBuffer* BdwGpuFileGetMapping(BdwGpuFile* file,uint64_t offset,uint64_t length,uint64_t& bufferOffset);
}
