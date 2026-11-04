# Task capacity and Steam

Brook currently supports 1,024 task ID slots, including ID zero and kernel
threads. The allocator can issue a high ID after cumulative task churn even
when relatively few tasks are live. Scheduler, process and PMM capacities now
share `TaskIdCapacity`; the PTE format statically limits it to 1,024. Mapping
and physical allocator entry points reject out-of-range owners rather than
truncating the owner or indexing beyond a table.

The intended interim target is 5,120 slots (5,119 nonzero IDs). This is not yet
enabled. A safe increase requires:

* A wider **per-mapping** owner representation preserved across fork, COW,
  detach, restoration, hidden mappings and teardown. Physical-frame ownership
  cannot substitute: fork can map the same frame with different leaf owners.
  PTE tag bits are also used for borrowed-device teardown.
* Scheduler policy storage and ABI changes with matching rebuilt modules. Host
  size probes at 5,120 IDs measured 297,072 bytes for EEVDF, 122,952 for MLFQ
  and 30,728 for RR, compared with the current 65,536-byte arena.
* Moving capacity-sized temporary arrays out of interrupt-sensitive stacks.
  A 5,120-entry pointer array is 40 KiB, while AP bootstrap stacks are 64 KiB.
  Individual tasks currently allocate 256 KiB of kernel stack, so 5,119 live
  tasks could consume roughly 1.25 GiB in stacks alone. The blocked-wakeup
  scan's scratch array is now outside the interrupt stack and protected by its
  existing scan handoff; group signal, fatal and group exit still need work.
* Reviewing diagnostics and independent resource limits, including VMALLOC
  registration, task snapshot size and debug-job budgets.

Before enabling the increase, test PIDs 1023, 1024, 4096 and 5119 through
mapping, fork, COW and recovery, then test allocation, exhaustion, reuse,
wakeup and concurrent teardown on a disposable SMP VM. Dell validation should
follow with a recoverable kernel and working maintenance path.

One candidate for wider owner storage reserves the inline owner value 1023 as
an escape and stores the full owner in a sparse sidecar per affected leaf-table
page. The sidecar must be admitted before publishing a leaf, keyed to the leaf
table rather than to a shared data frame, and reclaimed only after table
retirement. Detach/restore, move, fork, COW and pageable eviction must carry the
full owner as part of the transaction; copying the raw PTE alone is insufficient.
The range-detach callback and single-page detach API carry a typed
raw-leaf/owner snapshot, and file invalidation saves the captured owner for
restoration. Range move and protection walkers also carry the typed snapshot;
move preflight rejects owners that cannot yet be encoded inline. Owners are
still read from the existing inline bits. A standalone
sidecar directory now supports per-table storage, but the live VMM does not yet
use it. The mapping transaction's optional snapshot is an observation, not an
additional frame reference.
The pageable-memory experiment retains the detached owner across eviction,
failed-write rollback and fault-in; fork assigns the child owner to its stored
or recovery entry. The hosted VMM fixture exercises those paths with a mapping
owner different from the address-space owner.
The direct map, try-map, `VmmReadLeaf` and detach paths now select the kernel
page-table lock for the kernel sentinel, explicit kernel root and shared upper-half slots;
user-only range operations reject the explicit kernel root. Host tests race the
sentinel against the explicit root and a kernel root against a user-root upper
alias, and check independent lower-half user roots. A disposable two-vCPU TCG
boot reached the scheduler and compositor and passed its TLB tests. Sidecar
integration still needs the remaining owner-transfer and failure rollback work.
