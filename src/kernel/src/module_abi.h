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
static constexpr uint32_t MODULE_ABI_VERSION = 1;

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
