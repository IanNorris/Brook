# Brook OS — Roadmap

## Recently Completed
- **pipe() + dup2()** — 4KB ring buffer pipes, FD duplication, refcounting
- **Busybox working** — echo, uname, cat-via-pipe, ash shell all pass
- **76 syscalls** implemented (was 51): poll, readlink, pipe2, kill, chdir, getrlimit, sysinfo, gettid, sigaltstack, statfs, select, sendfile, etc.
- **Linux open() flag translation** — O_LARGEFILE etc. properly mapped to VFS flags
- **syscheck test** — 28/28 comprehensive syscall compatibility tests pass
- **Lua 5.4 interpreter** — runs arithmetic, strings, tables, fibonacci, factorial, coroutines
- **Stress test** — 40/40 fork+pipe+wait cycles pass under concurrent load
- **fork + waitpid + execve** — full POSIX process lifecycle

## Current Branch: fix/smp-scheduler-races
Latest commit: a088da4 (stress test)

---

## Phase: Dynamic Driver Module Loading

## Problem & Approach

We want to take drivers (virtio-blk, PS/2 keyboard, future AHCI/USB)
out of the kernel binary and load them at runtime from disk.

**Format chosen: relocatable ELF kernel modules (`.mod` files)**
- Same format Linux uses for `.ko` kernel modules
- Compiled with `-r` (partial link) or as a single relocatable `.o`
- No GOT/PLT needed — we apply R_X86_64_64 / R_X86_64_PC32 / R_X86_64_PLT32
  relocations directly, resolving against the kernel symbol table
- All module code runs in ring 0, same address space as the kernel
- Modules must be in the ±2GB range from the kernel (PC32 relocs)
  → allocate module memory from high-address VMM space (already above 2GB)
  → if that becomes a problem we switch to R_X86_64_64-only (-mcmodel=large)

Shared objects (.so with -fPIC) deliberately avoided:
they require PLT/GOT machinery that's complex and unnecessary in kernel space.

## Component Overview

```
kernel symbol table      ELF module on disk (/boot/drivers/virtio_blk.mod)
        │                         │
        │      ModuleLoad(path)   │
        └──────────┬──────────────┘
                   │
              ELF loader
              ├── parse sections (.text, .rodata, .data, .bss, .rela.*)
              ├── VMM allocate pages (writable + executable)
              ├── copy/zero sections
              ├── apply relocations (R_X86_64_64, PC32, PLT32)
              └── resolve undefined symbols → kernel symbol table
                           │
                    call ModuleInfo.init()
                           │
                    module calls DeviceRegister(), etc.
```

## Module ABI (module_abi.h — shared between kernel and modules)

```cpp
struct ModuleInfo {
    const char* name;
    const char* version;
    int (*init)();      // returns 0 on success
    void (*exit)();
};

// Each module defines exactly one of these in a known ELF section.
#define DECLARE_MODULE(n, i, e) \
    __attribute__((section(".modinfo"), used)) \
    static const brook::ModuleInfo _module_info = { n, "1.0", i, e }
```

## Kernel Symbol Export

```cpp
// In any kernel header/source file:
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(DeviceRegister);
// ... etc.

// Expands to a KernelSymbol entry in a dedicated .ksymtab ELF section.
// The linker script collects them into a contiguous table bounded by
// __start_ksymtab / __stop_ksymtab.
```

## Build System Changes

- New CMake function `add_driver_module(name sources...)`:
  - Compiles sources to a relocatable ELF with `-r`
  - Uses same kernel flags: `--target=x86_64-elf -ffreestanding -mcmodel=kernel -mno-red-zone`
  - Output: `build/drivers/<name>.mod`
- `scripts/install_modules.sh`: copies `.mod` files into `build/esp/drivers/`
  so QEMU serves them at `/boot/drivers/`

## Todos

1. `ksymtab`      — kernel symbol table (EXPORT_SYMBOL macro + linker script)
2. `elf-structs`  — ELF64 struct definitions (just what we need, no libc)
3. `elf-loader`   — parse + relocate + resolve ELF module into kernel VA space
4. `module-abi`   — ModuleInfo struct, DECLARE_MODULE macro, module_abi.h
5. `module-mgr`   — ModuleLoad/Unload + module registry (up to 64 modules)
6. `module-build` — CMake add_driver_module() + build plumbing
7. `first-module` — extract virtio_blk as the first driver module
8. `kbd-module`   — extract PS/2 keyboard as second module
9. `module-disco` — scan /boot/drivers/ at boot, load all .mod files

## Open Questions / Design Choices

- **Module isolation**: for now, a crashing module crashes the kernel (same
  ring 0). True isolation requires ring 3 + IPC which is a later milestone.
- **Inter-module symbols**: initially modules can only import kernel symbols,
  not each other's. Add a second pass (load-order dependency resolution) later.
- **Versioning**: ModuleInfo.version is a string for now. A proper ABI version
  number check (magic + abi_version field) should be added before loading.
- **Memory model**: if modules > 2GB from kernel base become an issue, switch
  module compilation to `-mcmodel=large` and only use R_X86_64_64 relocations.
