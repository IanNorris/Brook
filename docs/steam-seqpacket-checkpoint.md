# Sequenced-record Unix IPC checkpoint

Brook previously reported SOCK_SEQPACKET while implementing stream semantics.
Sending two records and receiving two bytes returned those bytes without
MSG_TRUNC; the next receive combined the first record's remainder with the
second record. The same synthetic test on Linux discards the truncated tail and
preserves the second record. Chromium's zygote uses this socket type, making it
a relevant independently reproducible compatibility gap.

The transport now retains one record per send/write/sendmsg, stages whole
scatter/gather messages outside its spinlock, and discards only a truncated
record's remaining bytes. Empty records participate in readiness and backpressure.
Descriptor ownership and sender credentials follow record identities, including
adjacent empty and nonempty records at the same byte offset. Plain read discards
ancillary rights; recvmsg can install them with close-on-exec or report ancillary
truncation. Connected peers must agree on sequenced framing. Existing stream
transports continue using their byte/descriptor-group boundaries.

The fixed transport uses the existing 65,536-byte directional ring, reserving one
byte: maximum record size is 65,535 bytes, with at most 63 queued record metadata
entries. Oversized writes return EMSGSIZE and a full queue returns EAGAIN for a
nonblocking operation, without publishing a prefix. MSG_PEEK is unsupported.
The change does not implement Unix datagram sockets.

## Validation

Candidate ELF SHA256:
`eaf81ea15a3cc498bc9b650de49cb6b7edeb5085ea781ae0408b980c5b4c7e6a`.

The public `scripts/workloads/unix_seqpacket_check.py` passes on Linux (88 checks)
and Brook (186 checks). Counts depend on how many empty records fit before
backpressure. It covers truncation and MSG_TRUNC length reporting, vector I/O,
empty sends, zero-byte read, descriptor grouping and close-on-exec, ancillary
truncation, plain-read capability release, queue-full atomicity, PID-oracle
endpoint passing plus fork credentials, shutdown, and listen/accept. The rights
release cases verify EPIPE after every receiver capability has been closed or
discarded. The first Linux harness expected empty output flags even when
MSG_CMSG_CLOEXEC was echoed; that harness expectation was corrected before the
accepted Linux/guest comparison.

All 102 assertion-enabled host tests pass. Exact-ELF entry-state and scheduler
mutation audits pass. Native socket type, unsupported PI futex, proc task, packet
pipe, robust futex and retained DRM probes pass; quota and PulseAudio write
fallback controls pass. Xwayland and the private null-sink PulseAudio server
start normally and pactl connects.

## Steam result

The first unmodified-helper Steam rerun still exits 135 after shared-memory quota
exhaustion. A prelaunch baseline verifies the pristine helper wrapper, an empty
/dev/shm, and the CEF log's byte offset/inode. The footprint monitor sees ten
25 MiB plus 16-byte pools before exhaustion; this is not accepted login. The
record-boundary bug is fixed independently, and no quota increase is used to
hide the remaining failure. Original job/group/EOF evidence and raw client logs
remain private ignored artifacts.


The failed Steam job and the monitor both finish with empty groups and EOF on
both streams. PulseAudio and Xwayland exit normally. Heap integrity passes;
checked shutdown, final serial acceptance, the original owned launcher wait,
and all four offline filesystem checks pass. No helper wrapper or shared-memory
quota changes are carried into this acceptance run.
