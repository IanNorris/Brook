# Steam packet-pipe checkpoint (2026-10-23)

Steam gets past its pipe-creation assertion but still does not show a login
screen. The staged client now exits 134, and the kernel records breakpoint
exceptions in two steamwebhelper processes. This is a later unresolved failure;
it is not authentication, updater or complete Steam acceptance.

## Implementation

The preceding checkpoint identified a steamclient constructor calling pipe2
with flags 0x84000 (O_CLOEXEC | O_DIRECT). Brook rejected O_DIRECT with EINVAL,
causing the m_rgPipe assertion. Pipe creation now accepts the flag and implements
packet semantics instead of silently treating the pipe as a byte stream.

Each write is framed, with writes larger than 4096 bytes split into packets.
A read stops after one packet, and a short read discards that packet's remaining
bytes. Zero-size reads leave packets untouched. Packet admission is atomic under
backpressure; nonblocking writes report EAGAIN or previously delivered packets.
Allocation, signal and reader-close failures retain partial-write accounting.
Stream pipes keep their existing write policy.

PipeBuffer lazily allocates a uint16 length entry per ring offset when packet
mode is enabled. For a 64 KiB pipe this costs 128 KiB; stream-only pipes do not
allocate it. Allocation happens outside the spinlock, publication and boundary
updates happen under the pipe lock, and duplicate speculative allocations are
freed. Metadata remains until final pipe destruction, preserving queued packets
when F_SETFL disables O_DIRECT. Mode selection belongs to the writing descriptor.
Module ABI is now 50 because PipeBuffer's layout changed.

Packet writev gathers across vector boundaries before framing; pipe readv
scatters one read across vectors. The scalar/vector read staging size respects
the caller's capacity up to pipe capacity, avoiding hidden 4 KiB truncation when
a stream prefix precedes a packet. User-copy failure reports bytes already
copied. Pipe access checks reject writing the read end while preserving the
legacy bidirectional socketpair representation. Existing broader open-file
status-sharing behavior across duplicated descriptors is not redesigned here.

## Validation and remaining failure

The portable source probe is `tools/at-syscall-probe/packetpipecheck.c`.
Linux control reports 99 checks and Brook reports 96, covering framing,
short-read discard, zero-length read, large writes, vectored I/O, mode changes,
mixed stream/packet reads, full-buffer atomicity, flags and endpoint access.
The count varies with pipe capacity: Linux uses page slots while Brook retains
its byte ring; their small-packet/full-buffer capacity is not claimed identical.

The actual hosted write branch additionally tests metadata allocation failure,
allocation failure after one delivered packet, atomic admission under pressure
and preservation of stream/socketpair behavior. All 101 rebuilt host tests pass
on the final source. Pipe-write, pipe-poll and VFS lifecycle fixtures pass
ASan/UBSan. Entry-state and scheduler mutation audits pass against the frozen ELF.
Astra reviewed metadata lifetime, staging limits, partial progress and user-copy
accounting; its reported issues were corrected before native acceptance.

Inside the private Brook VM, the robust-futex lifecycle probe still passes.
Intel hardware virgl GLX context/pixel readback passes with no GL/X errors,
and the canonical /dev/shm 19-case alias/isolation/cancellation/remap suite passes
again as UID 1000.

The original staged Steam job exits 134 after about 52 seconds, with all of its
descendants subsequently drained and both output streams at EOF. Fixed diagnostic
counts for m_rgPipe, Fatal assert, shared-memory allocation, /dev/shm and out of
space are zero. The prior pipe assertion is removed; the later CEF breakpoint
cause is not yet established. Simultaneous CPU exception output is interleaved,
so a guessed instruction address has not been treated as a verified diagnosis.
Full raw client logs remain in the guest; only controlled metadata and fixed
error-category counts were exported. Existing interpreter patches and skipped
bootstrap remain part of this diagnostic setup.

The exact Xwayland process exited 0, all tracked jobs drained, heap integrity
passed and the VM completed checked sync/shutdown. Its original launcher exited
0; boot/data/Nix/temporary disks each passed offline fsck with exit 0. No kernel
build or staging occurred while QEMU was running. Earlier candidate builds are
retained but were not booted as acceptance runs.

Ignored evidence: `build/workloads/steam-packet-20261023/` and
`build/reviews/steam-packet-off-gpu-1ms-fixed-mlfq-a-20261023/`.
Frozen ELF SHA-256:
`4e4f727f51bdf717484cdec4d44350bfc1ddd55acf75563e2d38c7f0309023d9`.

Linux interface reference: <https://man7.org/linux/man-pages/man2/pipe.2.html>.
