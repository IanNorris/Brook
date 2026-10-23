# Steam procfs metadata checkpoint (2026-10-23)

This checkpoint fixes two procfs metadata inconsistencies. The measured CEF
single-thread check no longer traps in the link-count-only comparison. Steam's
main process still aborts separately; login and authentication are not accepted.
Final candidate acceptance and teardown results are recorded below.

## CEF task-directory link count

A private loader audit probe collected only selected library basenames, PIDs and
load addresses. Correlating libcef base 0x10014000 with the kernel breakpoint RIP
0x183c2acf identifies ELF offset 0x83aeacf. Its preceding code calls __fxstatat64
on a proc directory descriptor and the constant `self/task/`, then traps if
st_nlink is at most two. The ELF also contains the surrounding function name
IsSingleThreadedImpl and source path sandbox/linux/services/thread_helpers.cc.
This replaces the earlier guessed library offset, which was not a diagnosis.

The baseline directly demonstrates the inconsistency: stat/fstatat report two
links for both one and two threads; fstat on an open task-directory descriptor
correctly reports three and four. Retained ProcObjectStat returned the generic
directory link count, while the older opened-directory path included live tasks.
ProcObjectStat now reports two plus the incarnation-validated snapshot's live
thread count for task directories. Other directory link counts are unchanged.

## Reported inode stability

The extended existing proctaskcheck exposed another inconsistency. A worker's
/proc/thread-self and /proc/TGID/task/TID status fields agree, but sequential
unpinned stats returned inode 96 versus 105. ProcObjectMake intentionally interns
only live retained objects; recreation after the last reference gets a new
internal identity. Using that allocation identity as st_ino leaked object
lifetime into reported filesystem metadata.

Reported proc inode IDs are now separate from transient VfsObject::identity.
Global entries occupy a fixed low domain. Task entries occupy a reserved high
domain and encode the captured process incarnation plus role, task-member flag
and entry index. The 65,536-slot layout reserves 32 roles and 1,024 indexes per
member class; compile-time checks tie the index capacity to MAX_FDS. Checked
arithmetic returns EOVERFLOW on exhaustion rather than truncating or hashing.
Both retained path stats and opened vnode stats use this reported identity.
Existing object ownership, interning, authorization, process-incarnation checks
and namespace-object device identities remain intact. No Process references or
persistent caches are added, and module ABI remains 50.

## Validation

All 102 rebuilt host tests pass. The new host fixture checks 196,608 global/task
role/member/index combinations for uniqueness and deterministic repeats, plus
boundary/overflow cases. Proc-inode and VFS-lifetime fixtures pass ASan/UBSan.
Astra reviewed snapshot validation, inode-domain injectivity, lifetime and both
stat paths. Entry-state and scheduler mutation audits run against the exact ELF.

The portable tools/at-syscall-probe/proctaskcheck.c passes on Linux and the final
Brook VM. It compares stat/fstatat/fstat and inode equality before worker birth,
while a worker lives and after join. The worker repeats unpinned alias lookups
32 times. Existing status, enumeration, membership, foreign-task rejection and
32 retained-directory retirement checks remain. No actual PID-number reuse was
observed in this run; the host identity test covers distinct generations.
Packet-pipe and robust-futex native regressions also pass on the final kernel.

The link-count-only intermediate run preserved a native failure in the worker
inode-alias check; that result prompted the inode fix rather than weakening the
assertion. It also showed no recurrence of the measured CEF breakpoints while
Steam's main process still exited 134. Intermediate artifacts remain failures or
diagnostic comparisons, not full acceptance.

Private loader probes were not shipped. Early attempts wrote outside the visible
SteamRT tree and collected no useful library bases. A controlled directory/auxv
check established the missing path and AT_SECURE=0; moving output into the visible
client tree yielded the useful bases. The earlier inference that SteamRT had
stripped LD_AUDIT was not established. Steam redirects CEF logs into client/logs;
wrapper stderr alone does not characterize the CEF failure. Only probe-owned
metadata, source filename/line counters and fixed error-category counts were
exported; raw client logs remain inside the preserved guest disks.

Ignored evidence: build/workloads/steam-cef-20261023/ and the steam-cef and
steam-cef-fixed review directories. Final frozen ELF SHA-256:
`e624523d9ed521a8d9d8b7d36e9bdc3ec06d1d5116dcbc2ef7cd32984bfef40f`.

The final uninstrumented Steam attempt exits 134 with its original group empty
and both output streams at EOF. Its serial capture contains no user-exception,
killed-process or breakpoint-exit markers; the measured CEF trap is removed.
The independent main-client abort remains the next diagnosis, and no login UI
acceptance is claimed. Hardware Intel virgl GLX pixel readback and canonical
/dev/shm alias/cancellation/remap regressions pass on this same final kernel.
Entry-state and scheduler mutation audits also pass.

All guest commands drained, the exact Xwayland process exited 0, heap integrity
passed and checked sync completed. The original VM launcher was harvested with
exit 0. Boot, data, Nix and temporary disks each passed offline fsck with exit 0.
No kernel build or staging took place while QEMU ran. Raw client logs, private
probes, downloaded binaries and disk images remain outside Git.
