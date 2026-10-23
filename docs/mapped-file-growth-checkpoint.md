# Mapped file growth checkpoint

CEF's real startup capture attempts ftruncate64 from 8,192 to 270,336 bytes
while retaining a mapping. Brook previously rejected every resize of a mapped
vnode with EBUSY, including growth. This checkpoint enables the observed growth
operation without dropping canonical pages or invalidating borrowed PTEs.

Ext2 and tmpfs explicitly opt their canonical file objects into a strict-growth
callback. The callback contract preserves all bytes below page-aligned old EOF,
even when an error is returned, and never shrinks the backing. Other backends
remain rejected. The file-object path re-stats EOF under its inode gate so a
concurrent ordinary write cannot turn a stale growth request into shrink. It
retains all canonical frames, exposure leases and aliases, propagates errors,
and advances the generation even if a failed attempt committed a larger EOF.
The existing VFS mutation gate still excludes first-pin publication.

Mapped shrink, same-size truncate and growth from partial-page old EOF remain
rejected. Those cases are not a general mapped-resize implementation. No public
module layout changes; ABI remains 50. Memfd behavior is unchanged.

## Verification

Production ELF SHA256:
`3812b40f33d9cb3a08eb4edf60982b07e4d6c06deecd8c9c19e42114c2ace751`.

The actual file-object host fixture checks existing frame identity, shared
stores, retained writeback, zero-filled new pages, disabled backends, invalid
resize requests, current-size revalidation, backend errors before growth and
errors after a committed growth. File-object and tmpfs ASan/UBSan fixtures pass.
The full assertion-enabled host suite passes 101 of 102 in its first run; the
unrelated audio-delivery fixture times out during parallel kernel compilation
and passes in isolation afterward. Both results are retained. Production
kernel, drivers and standalone tests build; exact-ELF entry-state and scheduler
mutation audits pass.

`scripts/workloads/mapped_file_growth_check.py` passes on Linux and through real
Brook syscalls on tmpfs, the data filesystem and the runtime disk. The parent
and child keep the old mappings alive during growth, verify old mapped stores
through independent readonly aliases and pread, check zero-filled new pages,
write the new region in the child, then verify fsync and reopened contents.
All native jobs end with empty groups and both streams at EOF. Native Unix IPC,
DRM namespace binds, tmpfs quotas and PulseAudio write fallback also pass.

## Steam result

The first unmodified-helper run still exits 135 before login. Its footprint
reaches ten 25 MiB plus 16-byte pools and the newly appended CEF log contains
two persistent-memory corruption warnings. Thus safe aligned growth fixes a
demonstrated compatibility gap, but does not establish the cause of the pool
buildup or corruption. The next comparison uses a fresh private profile and a
delegating probe that records successful CEF-sized growth as well as failures.
The original failed client and monitor drain completely; PulseAudio and
Xwayland exit normally. Heap integrity, checked shutdown, final serial checks,
the original owned launcher wait and all four offline filesystem checks pass.

Private evidence:
`build/reviews/steam-mapped-growth-off-gpu-1ms-fixed-mlfq-a-20261023` and
`build/workloads/steam-mapped-growth-20261023/`.
