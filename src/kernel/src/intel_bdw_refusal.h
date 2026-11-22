#pragma once
#include <stdint.h>
namespace brook {
// Diagnostic only: the most recent reason a GPU submission or handoff
// operation was refused, set at each refusal site immediately before the
// error is returned (all such sites run under the submission or handoff
// mutex). The syscall layer logs it, rate-limited, when the error reaches
// userspace - hasvk turns any execbuffer error into VK_ERROR_DEVICE_LOST, so
// without this the renderer only sees "device lost".
inline const char* g_bdwRefusal=nullptr;
inline void BdwNoteRefusal(const char* why){__atomic_store_n(&g_bdwRefusal,why,__ATOMIC_RELEASE);}
inline const char* BdwTakeRefusal(){return __atomic_exchange_n(&g_bdwRefusal,static_cast<const char*>(nullptr),__ATOMIC_ACQ_REL);}
// Log the first 16 refusals, then every power of two.
inline bool BdwRefusalLogDue(uint64_t& counter){const uint64_t n=++counter;return n<=16 || !(n&(n-1));}

// Execbuffer decode/validation refusals, one per check, each counted since
// boot. A refusal also records the offending value, the limit or expected
// value it was compared with, and the exec object index (or NoObject).
enum class BdwExecReject : uint8_t {
    VideoEngine,            // I915_EXEC_BSD while the VCS executor is not ready
    ObjectCount,            // buffer_count zero or above the per-file handle limit
    ObjectsPointer,         // buffers_ptr null
    Flags,                  // execbuffer flags outside the supported set (value=flags, limit=supported mask)
    NoRelocMissing,         // I915_EXEC_NO_RELOC not set (relocations unsupported)
    Engine,                 // engine selector other than default/render/BSD
    LegacyFields,           // DR1/DR4 nonzero
    ContextId,              // rsvd1 context above 32 bits
    InOutFence,             // rsvd2 (sync-file in/out fence) nonzero
    ExtensionShape,         // USE_EXTENSIONS with fence array, num_cliprects or null chain
    FenceArrayShape,        // FENCE_ARRAY count zero/above limit or null pointer
    Cliprects,              // cliprects without FENCE_ARRAY/USE_EXTENSIONS
    BatchAlignment,         // batch_start_offset or batch_len not 8-byte aligned
    ObjectsFault,           // buffers_ptr range wraps or cannot be read
    NoMemory,               // kernel copy allocation failed
    Relocations,            // exec object relocation_count/relocs_ptr nonzero
    ObjectReserved,         // exec object rsvd1/rsvd2 nonzero
    HandleInvalid,          // handle zero, out of range or not open in this file
    HandleDuplicate,        // the same handle listed twice
    ObjectFlags,            // EXEC_OBJECT_* outside WRITE|48B|PINNED|ASYNC|CAPTURE
    ObjectNotPinned,        // EXEC_OBJECT_PINNED missing (kernel placement unsupported)
    ObjectAddress,          // offset not canonical 48-bit
    ObjectAddressAlignment, // offset not page aligned, or not aligned to alignment
    ObjectAlignment,        // alignment nonzero and below 4096 or not a power of two
    ObjectBacking,          // buffer has no pages
    PageLimit,              // total pages of the buffer set above the worker limit
    ObjectBeyond48,         // offset+size beyond 2^48
    ObjectBeyond4G,         // offset+size beyond 4 GiB without SUPPORTS_48B_ADDRESS
    BatchProtected,         // batch object is protected or marked WRITE
    BatchRange,             // batch start/length outside the batch object
    ObjectOverlap,          // two objects overlap in GPU address space
    Context,                // context id not found (ENOENT) or not bindable
    FenceChain,             // timeline extension chain/name/flags/reserved invalid
    FenceFault,             // fence or timeline point array unreadable
    FenceEntry,             // fence entry handle zero or flags not WAIT|SIGNAL
    FenceSync,              // fence syncobj handle not found
    FenceWait,              // WAIT on a syncobj (point) with no fence
    FenceSignal,            // signal fence could not be created or added
    Count
};
inline const char* BdwExecRejectName(BdwExecReject r){
    static const char* const names[]={
        "execbuffer decode: I915_EXEC_BSD video engine submission not available",
        "execbuffer decode: buffer_count zero or above limit",
        "execbuffer decode: buffers_ptr null",
        "execbuffer decode: unsupported execbuffer flags",
        "execbuffer decode: I915_EXEC_NO_RELOC required",
        "execbuffer decode: unsupported engine",
        "execbuffer decode: DR1/DR4 nonzero",
        "execbuffer decode: context id above 32 bits",
        "execbuffer decode: in/out fence (rsvd2) unsupported",
        "execbuffer decode: USE_EXTENSIONS shape",
        "execbuffer decode: FENCE_ARRAY count or pointer",
        "execbuffer decode: cliprects without fence array",
        "execbuffer decode: batch start/len not 8-byte aligned",
        "execbuffer decode: exec object array fault",
        "execbuffer decode: out of kernel memory",
        "execbuffer decode: relocations unsupported",
        "execbuffer decode: exec object reserved fields nonzero",
        "execbuffer decode: invalid handle",
        "execbuffer decode: duplicate handle",
        "execbuffer decode: unsupported EXEC_OBJECT flags",
        "execbuffer decode: EXEC_OBJECT_PINNED required",
        "execbuffer decode: non-canonical object offset",
        "execbuffer decode: object offset misaligned",
        "execbuffer decode: invalid object alignment",
        "execbuffer decode: object has no backing pages",
        "execbuffer decode: buffer set pages above limit",
        "execbuffer decode: object beyond 48-bit address space",
        "execbuffer decode: object beyond 4 GiB without SUPPORTS_48B_ADDRESS",
        "execbuffer decode: batch object protected or written",
        "execbuffer decode: batch start/len outside batch object",
        "execbuffer decode: objects overlap in GPU address space",
        "execbuffer decode: context lookup or bind",
        "execbuffer decode: timeline fence extension invalid",
        "execbuffer decode: fence array fault",
        "execbuffer decode: fence entry handle or flags invalid",
        "execbuffer decode: fence syncobj not found",
        "execbuffer decode: wait syncobj has no fence",
        "execbuffer decode: signal fence create/add failed",
    };
    static_assert(sizeof(names)/sizeof(names[0])==unsigned(BdwExecReject::Count),"one name per reject reason");
    return unsigned(r)<unsigned(BdwExecReject::Count)?names[unsigned(r)]:"execbuffer decode: unknown";
}
constexpr uint32_t BdwRefusalNoObject=~uint32_t{0};
struct BdwRefusalDetail {const char* why;uint64_t value,limit;uint32_t object;};
inline BdwRefusalDetail g_bdwRefusalDetail{};
inline uint64_t g_bdwExecRejects[unsigned(BdwExecReject::Count)]{};
// Records the specific failed check. The detail is paired with its reason
// pointer, so a later plain BdwNoteRefusal does not inherit stale values.
inline void BdwNoteExecReject(BdwExecReject r,uint64_t value=0,uint64_t limit=0,uint32_t object=BdwRefusalNoObject){
    if(unsigned(r)>=unsigned(BdwExecReject::Count))return;
    __atomic_add_fetch(&g_bdwExecRejects[unsigned(r)],1,__ATOMIC_RELAXED);
    const char* why=BdwExecRejectName(r);
    g_bdwRefusalDetail={why,value,limit,object};
    BdwNoteRefusal(why);
}
inline uint64_t BdwExecRejectCount(BdwExecReject r){
    return unsigned(r)<unsigned(BdwExecReject::Count)?__atomic_load_n(&g_bdwExecRejects[unsigned(r)],__ATOMIC_RELAXED):0;
}
// The recorded detail, if the most recent specific refusal was why.
inline bool BdwRefusalDetailFor(const char* why,BdwRefusalDetail& out){
    if(!why)return false;
    const BdwRefusalDetail d=g_bdwRefusalDetail;
    if(d.why!=why)return false;
    out=d;return true;
}
}
