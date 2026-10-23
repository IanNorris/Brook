#pragma once

#include <stdint.h>

// module_abi.h — Brook OS kernel module ABI.
//
// This header is included by BOTH the kernel and driver module sources.
// Keep it dependency-free (no kernel-internal headers).
//
// A minimal driver module looks like:
//
//   #include "module_abi.h"
//   #include "device.h"       // DeviceRegister etc.
//
//   static int MyDriverInit() {
//       // ... set up device, call DeviceRegister() ...
//       return 0; // 0 = success
//   }
//   static void MyDriverExit() { /* cleanup */ }
//
//   DECLARE_MODULE("my_driver", MyDriverInit, MyDriverExit);

namespace brook {

// Current module ABI version.  The loader rejects modules built against
// a different ABI version.
// Version 6 makes filesystem sync/unmount callbacks return checked errors.
// Version 5 added filesystem device identity to VnodeStat.
// Version 4 added real link counts, typed removal and busy-unmount checks to VFS.
// Version 3 added timestamps and creation/chmod modes. Version 2 belonged to
// an archived, incompatible GPU experiment and must never be reused.
// ABI 7 appends VnodeOps::truncate and adds per-thread file-fault ownership
// to Process; rebuild all modules against both updated layouts.
// ABI 8 adds cooperative cancellation state to Process and identity to KMutex.
// ABI 9 adds current/maximum access permissions to Process::MemFdMap.
// ABI 10 adds per-thread user GS base to SavedContext in Process.
// ABI 11 adds child stop/continue event state to Process.
// ABI 12 moves task mappings into a shared AddressSpaceState.
// ABI 13 appends a stable file-object accessor to VnodeOps.
// ABI 14 adds file registrations to range tables and file identity to mapping backings.
// ABI 15 carries the requested creation mode through VfsFsOps::mkdir.
// ABI 16 appends native submission/progress/restart callbacks to AudioDriver.
// ABI 17 appends VfsFsOps permission-policy and opaque-link fields.
// ABI 18 adds retained-vnode access and real-ID filesystem permission callbacks.
// ABI 19 adds owned vnode directory paths and retained-inode path lookup.
// ABI 21 preserves full resource-box coordinates and byte layout in GpuAppOps transfers.
// ABI 22 adds verified resource retirement to GpuAppOps.
// ABI 23 adds host-owned captured textures and verified copy completion.
// ABI 24 removes the unleased raw external-resource blit operation.
// ABI 25 passes canonical source formats to retained GPU capture.
// ABI 26 adds bounded offscreen premultiplied scene composition.
// ABI 27 adds verified temporary-backing uploads into owned images.
// ABI 28 appends retained pathname socket creation to VfsFsOps.
// ABI 29 adds the retained vnode identity to file mapping backing descriptors.
// ABI 30 types app GPU context identity and app capability reporting.
// ABI 31 adds owned HOST3D blobs and typed Venus ring submissions.
// ABI 32 appends validated Venus DMA-BUF image import to GpuAppOps.
// ABI 33 appends fixed-point source / buffer-transform flags to GpuSceneLayer.
// ABI 34 versions SchedOps and adds measured-service/runnable/preemption hooks.
// ABI 35 adds retained VFS mount ownership to Vnode.
// ABI 36 adds retained backend-object factories and Vnode identity pins.
// ABI 37 appends retained-parent mutation and metadata callbacks to VfsObjectOps.
// ABI 38 appends Process filesystem-context ownership and replaces cwd/mask authority.
// ABI 39 assigns per-task no_new_privs state in Process (previously padding).
// ABI 40 adds independent per-task mount-namespace membership to Process.
// ABI 41 adds private user-namespace/capability state and credential snapshot fields.
// ABI 42 changes retained magic-link callbacks and adds path/namespace FD kinds.
// ABI 43 appends explicit vnode mapping policy for the bounded tmpfs backend.
// ABI 44 adds per-vnode mount data admission for read-only transitions.
// ABI 45 appends exact retained-entry path reporting to VfsObjectOps.
// ABI 46 widens public mount snapshot paths to the existing 512-byte VFS bound.
// ABI 47 adds FIFO creation and vnode-owned stream state.
// ABI 48 appends backend canonical-page quota hooks to FileObject.
// ABI 49 adds per-thread robust-futex registration and deferred exit state.
static constexpr uint32_t MODULE_ABI_VERSION = 49;

// Magic bytes at the start of every valid ModuleInfo.
static constexpr uint32_t MODULE_MAGIC = 0x4B524F42; // 'BROK'

struct ModuleInfo {
    uint32_t    magic;       // Must equal MODULE_MAGIC
    uint32_t    abiVersion;  // Must equal MODULE_ABI_VERSION
    const char* name;        // Short name: "virtio_blk", "ps2_kbd"
    const char* version;     // Semantic version string: "1.0.0"
    const char* description; // Human-readable one-liner
    int  (*init)();          // Called after relocations are applied; 0 = success
    void (*exit)();          // Called before the module is unloaded (may be nullptr)
};

} // namespace brook

// ---- DECLARE_MODULE macro ----
//
// Place exactly ONE of these in each module's source.
// The loader finds the .modinfo section to locate the ModuleInfo.
//
// Example:
//   DECLARE_MODULE("virtio_blk", VirtioBlkInit, VirtioBlkExit,
//                  "Legacy virtio block device driver");

#define DECLARE_MODULE(mod_name, init_fn, exit_fn, desc_str)             \
    __attribute__((section(".modinfo"), used))                           \
    static const brook::ModuleInfo _module_info = {                      \
        brook::MODULE_MAGIC,                                             \
        brook::MODULE_ABI_VERSION,                                       \
        (mod_name),                                                      \
        "1.0.0",                                                         \
        (desc_str),                                                      \
        (init_fn),                                                       \
        (exit_fn),                                                       \
    }

// ---- MODULE_IMPORT_SYMBOL ----
//
// Documents that this module depends on a specific kernel symbol.
// Not enforced at compile time — the loader will fail at link time if
// the symbol isn't in the kernel's export table.
// Useful as self-documentation and for future dependency tooling.
//
// Example:
//   MODULE_IMPORT_SYMBOL(kmalloc);
//   MODULE_IMPORT_SYMBOL(DeviceRegister);

#define MODULE_IMPORT_SYMBOL(sym) \
    static_assert(true, "import: " #sym) // no-op marker, documents dependency
