# Memory Subsystem Documentation

## Overview

The Brook kernel memory subsystem provides three layers:

1. **Physical Memory Manager (PMM)** — bitmap allocator for 4KB page frames
2. **Virtual Memory Manager (VMM)** — x86-64 page table management, virtual address space allocators
3. **Kernel Heap** — boundary-tag allocator for dynamic kernel allocations

Dependencies: PMM → VMM → Heap (each layer depends on the previous).

---

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `address.h` | 76 | Strongly-typed `PhysicalAddress`, `VirtualAddress`, `PageTable` wrappers |
| `physical_memory.h` | 101 | PMM public API declarations |
| `physical_memory.cpp` | 602 | PMM implementation |
| `virtual_memory.h` | 158 | VMM public API, address space layout constants, PTE flag definitions |
| `virtual_memory.cpp` | 724 | VMM implementation |
| `heap.h` | 64 | Kernel heap public API declarations |
| `heap.cpp` | 506 | Kernel heap implementation |

---

## address.h — Strongly-Typed Address Wrappers

### Purpose
Prevents silently mixing physical and virtual addresses by wrapping both in
distinct types. This is one of the most common OS-dev footguns.

### Types

| Type | Fields | Description |
|------|--------|-------------|
| `PhysicalAddress` | `uint64_t value` | Physical memory address |
| `VirtualAddress` | `uint64_t value` | Virtual memory address |
| `PageTable` | `PhysicalAddress pml4` | Wraps the physical address of a PML4 |

### Design Notes
- Constructors are `constexpr explicit` — no implicit conversions.
- `operator bool()` is explicit — prevents accidental use in arithmetic.
- `raw()` extracts the underlying integer for hardware register writes.
- `KernelPageTable` is a sentinel (null pml4) resolved by VMM functions to the boot CR3.
- Arithmetic operators allow offset calculations without breaking type safety.

### Quality Assessment
✅ Well-designed, clean, no issues found.

---

## Physical Memory Manager (PMM)

### Purpose
Manages physical page frame allocation using a bitmap. Supports per-page
ownership tracking for process resource accounting and COW.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `PAGE_SIZE` | 4096 | x86-64 page size |
| `MAX_PHYS_GB` | 128 | Max supported physical RAM |
| `MAX_PHYS_PAGES` | 33,554,432 | 128GB / 4KB |
| `BITMAP_WORDS` | 524,288 | 4MB bitmap in BSS |
| `PMM_NULL_PAGE` | 0xFFFFFFFF | Linked list sentinel |
| `PMM_MAX_PIDS` | 1024 | Max simultaneous PIDs |

### Data Structures

**Bitmap** (`g_bitmap[BITMAP_WORDS]`): Bit = 0 means free, 1 means used.
In BSS (zeroed), then explicitly set to all-ones in `PmmInit()` before
selectively freeing usable regions from the boot memory map.

**PageDescriptor** (12 bytes per page):
- `next`/`prev`: doubly-linked list indices within a PID's page list
- `pid`: owning PID
- `tag`: `MemTag` value (KernelData, Heap, PageTable, UserData, etc.)
- `refCount`: COW reference count (0=untracked, 1=exclusive, 2+=shared)

**PidList** (per PID): head/tail indices + page count.

### Functions

| Function | Signature | Contract |
|----------|-----------|----------|
| `PmmInit` | `(const BootProtocol*)` | Must be called first. Builds bitmap from memory map. Marks low 1MB, kernel image, framebuffer, and boot data as used. |
| `PmmEnableTracking` | `()` | Call after HeapInit(). Allocates PageDescriptor array via VmmAllocPages. Backfills all used pages to KernelPid. |
| `PmmAllocPage` | `(MemTag, uint16_t pid) → PhysicalAddress` | Single page allocation. Uses search hint for O(1) average case. Returns null on OOM. |
| `PmmAllocPages` | `(count, MemTag, pid) → PhysicalAddress` | Contiguous allocation. Linear scan for free run. Returns null if no run found. |
| `PmmFreePage` | `(PhysicalAddress)` | Free one page. No-op on null/unaligned/OOB/already-free. Respects refcount: shared pages are just decremented. |
| `PmmSetOwner` | `(PhysicalAddress, MemTag, pid)` | Change page ownership. **⚠️ BUG (BRO-080): no lock held; also dead code — never called.** |
| `PmmGetTag` | `(PhysicalAddress) → MemTag` | Read-only tag query. No lock held (acceptable for single-word reads). |
| `PmmGetPid` | `(PhysicalAddress) → uint16_t` | Read-only PID query. Same note. |
| `PmmRefPage` | `(PhysicalAddress)` | Increment COW refcount. Legacy pages (refCount=0) jump to 2. Capped at 255. |
| `PmmUnrefPage` | `(PhysicalAddress)` | Decrement refcount. If last ref (0 or 1), actually frees the page. |
| `PmmGetRefCount` | `(PhysicalAddress) → uint8_t` | Read current refcount. |
| `PmmKillPid` | `(uint16_t pid)` | Free all pages owned by PID. No-op for KernelPid. Handles COW: shared pages are decremented, exclusive pages freed. |
| `PmmFreeByTag` | `(uint16_t pid, MemTag tag)` | Free pages matching specific tag. Inline list surgery (doesn't use ListRemove helper). |
| `PmmEnumeratePid` | `(pid, callback, ctx)` | Walk PID's page list, call callback for each. |
| `PmmDumpPidStats` | `()` | Print per-PID page counts to serial. |
| `PmmGetFreePageCount` | `() → uint64_t` | Return free page count. |
| `PmmGetTotalPageCount` | `() → uint64_t` | Return total tracked page count. |

### Locking
All mutating operations hold `g_pmmLock` (SpinLock, interrupt-disabling).
Read-only queries (`PmmGetTag`, `PmmGetPid`, `PmmGetRefCount`) do NOT hold
the lock. This is generally safe for single-field reads but not for atomic
multi-field consistency.

### Known Issues
- **BRO-080**: `PmmSetOwner()` doesn't hold `g_pmmLock`. Dead code — never called.
- `PmmFreeByTag` does inline list surgery instead of using `ListRemove`, duplicating
  logic. Should use the helper for consistency.
- `PmmGetTag`/`PmmGetPid` don't hold the lock. Safe for reads but could return stale
  data under concurrent modification. Acceptable for current use (diagnostics).
- `refCount` is `uint8_t` — max 255 sharers. Sufficient for current workload but
  would overflow if a page is shared across 256+ processes.

---

## Virtual Memory Manager (VMM)

### Purpose
Manages x86-64 4-level page tables, provides VMALLOC and module-space bump
allocators, and handles per-process user page table lifecycle.

### Address Space Layout

| Range | PML4 Index | Purpose |
|-------|-----------|---------|
| `0x0000_0000_0000_0000..0x0000_0000_0000_FFFF` | 0 | NULL guard (64KB, unmapped) |
| `0x0000_0000_4000_0000..0x0000_7FFF_FFFF_FFFF` | 0-255 | User space |
| `0xFFFF_8000_0000_0000..0xFFFF_80FF_FFFF_FFFF` | 256 | Direct physical map (16GB, 2MB pages) |
| `0xFFFF_C000_0000_0000..+32GB` | 384 | VMALLOC region |
| `0xFFFF_C080_0000_0000..+508GB` | 385 | Kernel heap |
| `0xFFFF_FFFF_9000_0000..+256MB` | 511 | Module space |
| `0xFFFF_FFFF_8000_0000..0xFFFF_FFFF_FFFF_FFFF` | 511 | Kernel image |

### PTE Encoding
Bits [51:12] = physical address. Bits [11:0] = hardware flags. Custom fields:
- Bits [11:9] = MemTag (3 bits, 8 values)
- Bit 52 = COW bit (page was writable, now read-only shared)
- Bits [62:53] = PID (10 bits, max 1024)
- Bit 63 = NX (no-execute)
- Bit 62 = VMM_FORCE_MAP (internal flag, stripped before PTE write)

### Functions

| Function | Signature | Contract |
|----------|-----------|----------|
| `VmmInit` | `()` | Called after PmmInit(). Captures kernel CR3. Builds 16GB direct physical map via 2MB pages at PML4[256]. |
| `VmmMapPage` | `(PageTable, VirtualAddress, PhysicalAddress, flags, MemTag, pid) → bool` | Map single 4KB page. Rejects < VIRTUAL_NULL_GUARD (unless VMM_FORCE_MAP). Creates intermediate page tables as needed. Returns false on failure. |
| `VmmUnmapPage` | `(PageTable, VirtualAddress)` | Clear PTE, issue INVLPG. No-op if not present. |
| `VmmVirtToPhys` | `(PageTable, VirtualAddress) → PhysicalAddress` | Walk page table, return physical address (including page offset). Null if unmapped. |
| `VmmGetPte` | `(PageTable, VirtualAddress) → uint64_t*` | Return pointer to leaf PTE. Null if not present. |
| `VmmAllocPages` | `(pageCount, flags, MemTag, pid) → VirtualAddress` | Bump-allocate from VMALLOC region. Maps backing physical pages. Full rollback on failure. Registers in allocation table. |
| `VmmAllocKernelStack` | `(pageCount, MemTag, pid) → VirtualAddress` | Like VmmAllocPages but with guard pages. Returns base of usable region. Layout: [guard][usable*N][guard]. |
| `VmmFreeKernelStack` | `(VirtualAddress, pageCount)` | Delegates to VmmFreePages. Guard page VA is leaked (acceptable for long-lived stacks). |
| `VmmAllocModulePages` | `(pageCount, flags, MemTag, pid) → VirtualAddress` | Bump-allocate from MODULE region. Same pattern as VmmAllocPages. |
| `VmmFreePages` | `(VirtualAddress, pageCount)` | Free physical pages and clear PTEs. Clears allocation registry. |
| `VmmKillPid` | `(uint16_t pid)` | Free all VMALLOC allocations for PID, then call PmmKillPid. |
| `VmmCreateUserPageTable` | `() → PageTable` | Allocate PML4, zero user half, copy kernel upper half (PML4[256..511]). |
| `VmmDestroyUserPageTable` | `(PageTable)` | Recursively free all page table pages in user half. Uses PmmUnrefPage for leaf data pages (handles COW). |
| `VmmKernelMarkReadOnly` | `(VirtualAddress, size) → bool` | Clear VMM_WRITABLE on a range of kernel pages. Used to harden dispatch tables after init. |
| `VmmSwitchPageTable` | `(PageTable)` | Write CR3, flush TLB. |

### Locking
- `g_kernelPtLock`: Protects kernel page table walks with `create=true`.
- `g_userPtLock`: Protects ALL user page table walks. Single global lock — correct
  but sub-optimal for SMP. Per-tgid locking noted as future optimization.
- `g_vmmLock`: Protects VMALLOC/module bump allocators and allocation registry.

### Known Issues / Observations
- **Bump allocators never reclaim**: VMALLOC and module-space are bump-only. Freed
  pages release physical memory but the virtual range is never reused. With a 32GB
  VMALLOC region this is practically fine, but module space (256MB) could exhaust
  under heavy module load/unload cycles.
- **Allocation registry is 1024 static slots**: No warning when full — the allocation
  just isn't tracked. `FindFreeSlot` returns null and the allocation succeeds but
  is invisible to `VmmGetAllocation`/`VmmKillPid` VMALLOC scan.
- **2MB page splitting** in `WalkToPtr`: Correctly implemented with full TLB flush of
  the 2MB range. Good defensive code.
- **VmmVirtToPhys** includes page offset in result (`| (virtAddr & 0xFFF)`) — correct
  for byte-level translation, documented implicitly.
- **`ZeroPageIdentity`** is only safe during early init before identity map is modified.
  No runtime check enforces this — relies on caller discipline.

---

## Kernel Heap

### Purpose
Dynamic kernel memory allocation (`kmalloc`/`kfree`/`krealloc`). Boundary-tag
allocator with first-fit search, auto-expansion, poison fill, and integrity checking.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `HEADER_MAGIC` | `0xB10CBEEF` | Block header sentinel |
| `FOOTER_MAGIC` | `0xB10CBEE2` | Block footer sentinel |
| `ALIGN` | 64 | All allocations 64-byte aligned |
| `INITIAL_PAGES` | 256 | 1MB initial heap |
| `EXPAND_PAGES` | 256 | 1MB expansion increments |
| `HEAP_VIRT_BASE` | `0xFFFFC08000000000` | PML4[385] |
| `HEAP_VIRT_MAX` | `0xFFFFC0FF00000000` | 508GB max |
| `POISON_ALLOC` | `0xCD` | Fresh allocation fill |
| `POISON_FREE4` | `0xDFDFDFDF` | Freed memory fill (32-bit pattern) |

### Block Layout
```
[BlockHeader: 64B]  magic, size, free, padding
[User data]         64-byte aligned
[BlockFooter: 64B]  size, magic, padding
```
Total overhead per block: 128 bytes (HEADER_SIZE + FOOTER_SIZE).
Minimum block size: 192 bytes (OVERHEAD + ALIGN).

### Functions

| Function | Signature | Contract |
|----------|-----------|----------|
| `HeapInit` | `()` | Must call after VmmInit(). Maps 1MB at HEAP_VIRT_BASE. Creates one large free block. |
| `kmalloc` | `(uint64_t size) → void*` | First-fit, 64B-aligned. Returns null on 0 or failure. Auto-expands on exhaustion (one retry). Poison-fills on alloc. Spot-checks free-poison on alloc (detects write-after-free). |
| `kfree` | `(void* ptr)` | Validates header magic. No-op on null or already-free. Coalesces with adjacent free blocks in both directions. Poison-fills on free. |
| `krealloc` | `(void* ptr, uint64_t newSize) → void*` | In-place if block is large enough. Otherwise alloc+copy+free. Null ptr → kmalloc. Zero size → kfree+null. |
| `HeapFreeBytes` | `() → uint64_t` | **⚠️ BUG (BRO-076): returns inflated values due to g_freeBytes tracking drift.** |
| `HeapCheckIntegrity` | `() → bool` | Walk all blocks, verify magic/size consistency. **⚠️ BUG (BRO-079): doesn't hold g_heapLock.** |
| `HeapSetPoison` | `(bool enable)` | Toggle poison fill. Disabling improves performance. |
| `HeapGetStats` | `(HeapStats* out)` | Snapshot heap state. Takes lock internally. Thread-safe. |
| `HeapDumpStats` | `()` | Print stats to serial. |

### Locking
`g_heapLock` (SpinLock) protects all heap metadata. Held during the entire
kmalloc search and kfree coalesce. Never held across a sleep (spinlock =
interrupt-disabling).

### Known Issues
- **BRO-076**: `g_freeBytes` tracking is broken — inflates monotonically with
  splits. The alloc path double-counts the remainder block's contribution without
  removing the original block's contribution. Diagnostic-only impact.
- **BRO-079**: `HeapCheckIntegrity` doesn't hold `g_heapLock` — can see torn state.
- **ExpandHeap walks entire heap** to find the last block: O(n_blocks). Could be
  optimized by maintaining a tail pointer. Not a correctness issue.
- **krealloc doesn't try in-place expansion** by coalescing with the next free block.
  Always falls back to alloc+copy+free if the current block is too small. This is
  correct but wasteful for hot realloc patterns.
- **First-fit search** can fragment the heap under adversarial allocation patterns.
  Acceptable for a kernel heap with relatively few allocation sizes.
- **BlockHeader/BlockFooter are 64 bytes each** (mostly padding). This wastes memory
  but simplifies alignment — every block boundary is 64-byte aligned without extra
  logic. The 128B overhead means small allocations (1–64 bytes) have 67–100% overhead.

---

## Cross-Cutting Observations

1. **Init ordering is critical**: PmmInit → VmmInit → HeapInit → PmmEnableTracking.
   This is documented in each header but not enforced programmatically.
2. **Memory leak path on process exit**: `VmmKillPid` frees VMALLOC allocations (kernel
   stacks, etc.), then `PmmKillPid` frees the PID's tracked physical pages. User page
   tables are freed via `VmmDestroyUserPageTable` in process.cpp. This three-step
   teardown is correct but fragile — if a page is allocated by the kernel on behalf of
   a user process but tagged KernelPid, it won't be freed when the process exits.
3. **No ASLR**: User space addresses, kernel VMALLOC, and module loading are all
   deterministic (bump allocators from fixed bases). Not a concern for a hobby OS but
   worth noting.
4. **Direct map cap**: 16GB direct physical map. If QEMU is given >16GB RAM, pages above
   the direct map will fault when the kernel tries to write them via `PhysToVirt()`.
   A comment in VmmInit notes this must be >= QEMU -m size.
