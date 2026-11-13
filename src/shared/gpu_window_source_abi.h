#pragma once
#include <stdint.h>
/* Validate that a connected, accepted provider socket originated from the
 * current owner incarnation of this OS window. Arguments: socket fd, window
 * slot, nonzero window incarnation, 0, 0, 0. No fd or lease is transferred. */
#define BROOK_SYS_GPU_WINDOW_VALIDATE 0xB1D
/* On a connecting provider endpoint, AUTH pins the currently attached desktop
 * renderer's peer identity. DRAIN is valid only after AUTH: it returns 0 once
 * that exact renderer group is fully reaped and the retained buffer has no
 * GPU uses; EAGAIN means retry, EIO means uncertain/failed. The renderer must
 * not export producer fds to other processes or retain them after its lifetime. */
#define BROOK_SYS_GPU_WINDOW_PEER_RECOVERY 0xB1E
#define BROOK_GPU_WINDOW_PEER_AUTH 0
#define BROOK_GPU_WINDOW_PEER_DRAIN 1
/* Provider atomically publishes its completed CPU VFB with the nonzero
 * surface commit it contains; only the owner of the exact window may tag it.
 * A renderer can then query that tag only for an exact window incarnation and
 * captured publication generation. Untagged or superseded content fails. */
#define BROOK_SYS_WM_PUBLISH_SOURCE_COMMIT 0xB1F
#define BROOK_SYS_WM_QUERY_SOURCE_COMMIT 0xB20
/* Invalidate the previous proof before modifying a shared VFB. A crash during
 * writing leaves it untagged until the owner publishes a completed snapshot. */
#define BROOK_SYS_WM_BEGIN_SOURCE_WRITE 0xB21
/* A content identity, never a window title or a reusable slot alone. The
 * provider obtains the incarnation from the OS and the commit from its
 * retained surface tree. Device/inode identifies the retained DMA-BUF across
 * fd transfer; zero values are invalid. This identity does not transfer an fd,
 * prove GPU completion, or grant a producer/consumer ownership lease. */
struct BrookGpuWindowIdentity {
    uint32_t window,reserved;
    uint64_t incarnation,commit,device,inode;
};
static inline int brook_gpu_window_identity_valid(const struct BrookGpuWindowIdentity *id){
    return id && id->window && !id->reserved && id->incarnation && id->commit && id->inode;
}
static inline int brook_gpu_window_identity_equal(const struct BrookGpuWindowIdentity *a,const struct BrookGpuWindowIdentity *b){
    return brook_gpu_window_identity_valid(a) && brook_gpu_window_identity_valid(b) &&
        a->window==b->window && a->incarnation==b->incarnation && a->commit==b->commit &&
        a->device==b->device && a->inode==b->inode;
}
