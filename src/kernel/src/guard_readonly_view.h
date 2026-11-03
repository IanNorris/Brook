#pragma once
namespace brook {
struct Device;
struct Vnode;
// Kernel-only, unpublished file view. The caller must later bind it to an
// authenticated, lifetime-tracked checker session before exposing it to user
// space. Creating this view does not authorize mounting or repair.
Vnode* GuardCreateReadOnlyView(Device* device,int flags,int* error);
}
