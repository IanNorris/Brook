#pragma once
#include "mapping_transaction.h"

namespace brook {
// Device/file preparation happens outside AS.mutation and must stage every
// requested page before returning success. The caller retains range.backing
// and cancellation protection until this request returns.
struct UserMappingRequest {
    MappingRange range;
    uint64_t hint, length, preparedPages;
    bool fixed;
    int (*prepare)(MappingTransaction&,const UserMappingRequest&,void*);
    void* context;
};
int64_t UserMappingCreate(Process* actor, AddressSpaceState* space, const UserMappingRequest& request);
int64_t UserMappingRemove(Process* actor, AddressSpaceState* space, uint64_t address, uint64_t length);
int64_t UserMappingProtect(Process* actor, AddressSpaceState* space, uint64_t address, uint64_t length, uint32_t protection);
// Destination must be a fresh, unpublished user AS. Failure leaves its partial
// clone owned by destination teardown, which the caller must destroy. Parent
// CoW downgrades may remain after failure; they are acknowledged before return.
bool UserMappingClone(Process* actor, AddressSpaceState* source, AddressSpaceState* destination, uint16_t pid);
// Source allocation must remain owned while the snapshot translates and pins
// its pages. The returned descriptor owns those pins independently of the source.
MappingBacking* UserMappingBufferSnapshot(uint64_t kernelAddress,uint64_t length);
bool UserMappingConfigureBuffer(UserMappingRequest& request,MappingBacking* buffer);
// Publishes a coherent process framebuffer source, retaining the new descriptor
// and releasing the old one outside the process framebuffer source lock. Caller owns the source pixels.
void UserFramebufferPublish(Process* process,MappingBacking* buffer,uint32_t* pixels,uint32_t bytes,
                             uint32_t width,uint32_t height,uint32_t stride);
int64_t UserMappingBufferAlias(Process* actor,AddressSpaceState* space,MappingBacking* buffer,uint32_t protection);
// Caller must hold a strong descriptor reference through return.
// Removes only ranges carrying this retained descriptor, including protection
// splits/remaps, while preserving newer MAP_FIXED replacements. Retires after ACK.
int64_t UserMappingRemoveBacking(Process* actor,AddressSpaceState* space,MappingBacking* backing);
// Copies a kernel-owned argument buffer into prepared NX stack frames before
// any user leaf is published. Returns stack base and the optional argument VA.
int64_t UserMappingStack(Process* actor, AddressSpaceState* space, uint64_t size,
                          const void* arguments, uint64_t argumentLength, uint64_t* argumentAddress);
enum class UserPageAccess { Ready, Missing, Denied, OutOfMemory };
UserPageAccess UserMappingPageAccess(Process* actor, AddressSpaceState* space, uint64_t address,
                                     uint32_t access, bool resolveCow);
int64_t UserMappingBreak(Process* actor, AddressSpaceState* space, uint64_t requested);
using UserRemapPreparation = int (*)(const MappingRange& range,uint64_t newLength,void* context);
int64_t UserMappingResize(Process* actor, AddressSpaceState* space, uint64_t source,uint64_t oldLength,
                          uint64_t newLength,uint64_t flags,uint64_t destination,
                          UserRemapPreparation prepare,void* context);
}
