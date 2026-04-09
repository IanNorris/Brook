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
//   ModuleUnload(handle) — calls exit(), frees VMM pages
//
// Discovery:
//   ModuleDiscoverAndLoad(dir) — scans a VFS directory for *.mod files

namespace brook {

struct ModuleHandle {
    const ModuleInfo* info;       // Points into the module's .modinfo section
    uint64_t          baseVirt;   // VMM base address of the loaded module
    uint64_t          pageCount;  // Number of VMM pages allocated
    bool              active;
};

// Load a module from a VFS path.
// Returns a handle on success, nullptr on failure (prints reason to serial).
ModuleHandle* ModuleLoad(const char* path);

// Unload a previously loaded module.  Calls exit() if defined, frees pages.
void ModuleUnload(ModuleHandle* handle);

// Find a loaded module by name.  Returns nullptr if not found.
ModuleHandle* ModuleFind(const char* name);

// Print all loaded modules to serial.
void ModuleDump();

// Scan a VFS directory for *.mod files and load each one.
// Returns the number of successfully loaded modules.
uint32_t ModuleDiscoverAndLoad(const char* dirPath);

} // namespace brook
