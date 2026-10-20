#pragma once

#include "module_abi.h"
#include <stdint.h>

// Brook kernel module loader.
//
// Modules are relocatable ELF64 objects (.mod files) compiled with:
//   clang -r --target=x86_64-elf -ffreestanding -mcmodel=kernel -mno-red-zone
//
// Loading sequence:
//   1. Read ELF data (e.g. via VfsOpen/VfsRead)
//   2. ModuleLoad(path) — finds, parses, relocates, and calls module init
//   3. Module init calls DeviceRegister() / VfsMount() etc.
//
// Unloading:
//   ModuleUnload(handle) — rejects permanent providers; otherwise exits/frees
//
// Discovery:
//   ModuleDiscoverAndLoad(dir) — scans a VFS directory for *.mod files

namespace brook {

struct ModuleHandle {
    const ModuleInfo* info;       // Points into the module's .modinfo section
    uint64_t          baseVirt;   // VMM base address of the loaded module
    uint64_t          pageCount;  // Number of VMM pages allocated
    bool              active;
    bool              loading;
    bool              unloading;
    bool              resident;   // Permanent callback registration pins these pages.
    bool              initFailed; // Failed init may still own exported callbacks.
};

// Load a module from a VFS path.
// Returns a handle on success, nullptr on failure (prints reason to serial).
ModuleHandle* ModuleLoad(const char* path);

// Atomically retain all owning modules for permanent callbacks/data. Addresses
// must belong to a published module mapping or the built-in kernel image.
// Loading mappings are published before init. Unloading mappings reject retain.
// Retention is permanent until reboot; rejected registrations may conservatively
// retain their module too. No locks may be held across init/exit/free operations.
bool ModuleRetainAddresses(const uintptr_t* addresses, uint32_t count);

// Unload only if active and not permanently retained. Refusal does not call exit.
bool ModuleUnload(ModuleHandle* handle);

// Find a loaded module by name. Returns a borrowed registry handle, not a lease;
// callers must not retain it across unrelated unload/reload operations.
ModuleHandle* ModuleFind(const char* name);

// Print all loaded modules to serial.
void ModuleDump();

// Scan a VFS directory for *.mod files and load each one.
// Returns the number of successfully loaded modules.
uint32_t ModuleDiscoverAndLoad(const char* dirPath);

// Module enumeration for procfs.
struct ModuleSnapshot {
    char        name[64];
    char        version[32];
    char        description[128];
    bool        resident;
    bool        initFailed;
    uint64_t    sizeBytes;  // pageCount * 4096
    bool        active;
};
uint32_t ModuleMaxSlots();
ModuleSnapshot ModuleSnapshotAt(uint32_t idx);

} // namespace brook
