# Live process argument data — 24 October 2026

Brook returned the 32-byte task name for `/proc/<pid>/cmdline`. A Steam feature
flag audit exposed this independently of the remaining Steam startup failure.
The implementation now reads the live NUL-separated exec argument strings,
including empty arguments, through an incarnation-bound procfs descriptor.

Initial argument and environment strings are packed in vector order. The
address space owns the argument extent; fork copies it and threads share it.
Exec replaces the address-space pointer under the registry lock used to pin a
foreign reader's reference. Reads hold the AS mutation mutex while copying
readable resident user leaves into a bounded kernel buffer, then release the
foreign AS before copying to the caller. Device mappings are excluded. A kernel
operation guard protects references and staging storage across cancellation.
An existing open descriptor observes in-place edits and a later exec; zombies
return EOF and a departed incarnation returns ESRCH rather than another PID's
arguments. Stat continues to report size zero.

The reader is limited to the readable resident exec argument extent.
PR_SET_MM/proctitle extension into environment storage and faulting remapped
argument pages remain unsupported.

## Validation

`scripts/workloads/proc_cmdline_check.c` passes the same 11 checks on Linux and
Brook: forged argv[0], empty arguments, contiguous argument order, small offset
reads, live edits through an existing descriptor, PROT_NONE EOF, a task-directory
thread view, fork/CoW isolation, exec through an existing descriptor, zombie EOF,
and departed-process ESRCH. Its output contains only check counts.

The assertion-enabled host suite passed 102/102 before the final device-range
exclusion. The refreshed exact-source run passed 100/102 with timeouts in the
unchanged scheduler-lock-owner and audio-delivery stress tests; both passed
when rerun alone (0.01 s and 7.34 s). The current mapping test also passes under
ASan/UBSan, including cross-page argument reads, permission changes, inherited
extents and CoW frame separation. Both exact-ELF entry-state and scheduler
mutation audits pass.

Frozen production ELF:
`be9735656ed90bc99855f9d4def34df393f26be88b9d5060bc9e7997ece44961`,
with 13 matched ABI-50 modules. In review
`steam-mapped-growth-off-gpu-1ms-fixed-mlfq-h-20261023`, native IPC, DRM,
seqpacket, quota, Pulse and mapped-growth controls pass, followed by the new
11-check native argument regression. The subsequent Steam attempt uses a fresh
profile, pristine wrapper, 16 GiB RAM and unrestricted tmpfs byte quotas, with
no preload probe or IPC feature override. Steam still failed to reach login;
the browser and zygote argument views were readable, confirming neither feature
override was present. A later short kernel profile does not change the absence
of application preloading. Heap integrity passed. Metadata sync exceeded the
bridge’s initial 30-second observation window, then completed on a longer
request; the original VM owner exited normally and all four offline filesystem
checks passed. This fixes argument visibility, not the remaining Steam failure.
