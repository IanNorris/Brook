# Memfd sealing and pinned sendfile

Memfds implement F_GET_SEALS and F_ADD_SEALS for SEAL, SHRINK, GROW and WRITE.
Creation without MFD_ALLOW_SEALING starts sealed against new seals. Checks run
under the existing memfd lock, including write/pwrite, truncation, allocation
and mapping admission. Failed combined requests leave the seal set unchanged.
F_ADD_SEALS follows Linux's 32-bit argument and error precedence.

Each mapping backing holds a counted memfd lease until its final reference is
released, including fork, split, remap and rollback paths. First WRITE sealing
conservatively returns EBUSY for any live mapping. Shared mappings admitted
after WRITE sealing cannot later gain write protection. Mapped shrink also
returns EBUSY until cross-address-space invalidation is implemented. Unmapped
shrink zeros the retained page tail before regrowth. Private writable memfd
mapping remains unsupported. No public module layout changed; ABI remains 44.

The native probe exposed an existing sendfile bug: memfd inputs appeared as
EOF and output errors were swallowed. sendfile now pins both descriptors,
reads memfd or vnode input, dispatches to the exact pinned output, propagates
errors and advances the input offset only by bytes delivered. Explicit offsets
preserve the input descriptor position. Append output is rejected before read.
Tests include sealed output, successful copying, readonly descriptors, partial
nonblocking-pipe transfer and append rejection for memfd and ordinary files.

The identical portable probe passed 61,782 checks on Linux and 61,790 on Brook,
including conservative mapping cases. All 101 host tests passed; the seal
policy passed ASAN/UBSan. This sanitizer result covers the policy, not the full
kernel syscall or mapping callbacks. Final-ELF entry/scheduler audits and
mutation checks, all existing container probes, nine native regression programs
and Mousepad passed. All jobs/groups ended empty. Heap, checked shutdown,
original launcher wait and four offline filesystem checks passed.

Accepted evidence: `build/workloads/memfd-seals-v2-20261023/completion-audit.json`.
Session: `2fc4f0726e1040dc9e42b4cabc62afb8`.
Kernel, ABI 44 with 13 modules:
`64dbc71cdeea718091c7071f3eaf4184d12e3214ca3e8d8269f2873f88891669`.
The first native sendfile failure is retained separately and is not acceptance.

Unchanged SteamRT now passes sealing and reaches graphics library collection.
Its complete stderr was saved and checked against the guest-recorded SHA256.
The capsule helper fails because Brook has no provider loader cache. A direct
replay with the real prepared runtime root and CAPSULE_DEBUG confirmed ENOENT
for every provider cache path. Generating a real cache exposed a further bug:
getdents64 reports symlinks as regular files. Python scandir versus lstat and a
scoped ldconfig trace reproduce it. That correction is the next checkpoint;
no SteamRT child or login acceptance is claimed here.
