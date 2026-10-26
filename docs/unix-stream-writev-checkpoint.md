# Large Unix-stream writev and presentation (26 October 2026)

The [software present-ID fix](steam-api-and-present-wait-checkpoint.md) gets
Goose to interactive menus, but scene loading remains unaccepted. A saved
native stack puts the presentation worker inside outbound
`xcb_put_image_checked → xcb_send_request → writev`, before the checked-request
reply wait. The lossy profile also records this worker in
`sys_writev → UserBufferReadable → UserMappingPageAccess`, sometimes blocking
on the address-space mutation mutex. That establishes a sampled path, not its
time share or a lost-wakeup diagnosis.

## Discriminator

The stream ring accepts at most 65,535 bytes at once. Previously each stream
writev vector went through sys_write, which validated its entire remaining
range before discovering a short write or EAGAIN. A multi-MiB X11 image could
therefore repeatedly validate its shrinking tail, including page-level mapping
mutex acquisitions. Requested bytes are not bytes copied or sent.

The owned [Unix-stream control](../tools/at-syscall-probe/unixwritecheck.c)
transfers the same patterned 8 MiB in either large shrinking vectors or bounded
32 KiB vectors. A forked reader verifies every byte and its exit is collected.
The exact static binary, SHA256
`8032ca05fd7c2040fac0006f962c414844ae640d08b5715bfba2de68df6162b1`,
passes on Linux and Brook. The baseline Brook large shape takes 48.926 ms,
including 47.140 ms in write calls, while the bounded shape takes 6.949 ms.
Large retries offer a cumulative 1,005,907,768 bytes while sending 8,388,608.
The direct-host Linux controls both take about 5.8 ms. That Linux run is not
the matched Linux VM performance baseline and is not an absolute VM comparison.

## Bounded stream staging

Only AF_UNIX SOCK_STREAM writev dispatch changes. The existing FdRef pins the
endpoint, descriptors are snapshotted, summed lengths are overflow checked,
and at most one ring-sized prefix is safely copied to resident kernel memory
before transport locks. A short transport write returns only its actual count;
uninspected later user bytes remain the caller's responsibility on retry.
Copy/metadata/allocation failures publish no staged bytes. Nonblocking and
transport errors propagate. Sequenced packets, SCM message handling, pipes,
ordinary write and other descriptor paths retain their existing dispatch.

The host fixture compiles the production helper and covers a bounded prefix
with an unreadable later tail, large advertised lengths, descriptor mutation
after snapshot, copy faults/no publication, empty vectors, overflow,
allocation cleanup, EAGAIN, blocking flag propagation and a transport accepting
only 17 of the staged bytes. Astra's read-only review finds no blocker.
All 115 host tests and both compiled entry/scheduler audits pass. Module ABI
remains 56. Native PI ownership/contention/robust/signal and netlink/interface/
descriptor controls pass again on the candidate.

Candidate kernel SHA256:
`4e47f74440f0a2f5da43a1ad228da5c54368cff3a61dd5a379fb72c169fa4826`.

## Native results

In large/chunk/chunk/large order on the candidate, all four 8 MiB integrity
checks pass. Large transfers take 6.915 and 7.530 ms; bounded transfers take
6.739 and 6.328 ms. Large write-call time is 3.864 and 3.204 ms. This removes
the large-shape penalty in this control without shifting that cost to the
bounded shape. It does not establish all application startup behavior.

The expanded Vulkan WSI control performs 120 serialized 1920×1080 frames,
checking exact XGetImage colors, present IDs and zero-time completed-ID waits.
Both kernels use the same candidate Mesa, eight vCPUs, 8 GiB and a rendered
Steam Store background in separate private reviews. All frames pass.

- Baseline: 18.228 seconds total; median 138.655 ms, p95 201.515 ms.
- Candidate: 10.058 seconds total; median 78.597 ms, p95 91.042 ms.

These include acquire, GPU commands/idle, X11 readback and present completion.
They measure this serialized control, not game FPS or an isolated syscall's
cost. Host load and client content can vary. Game scene-loading acceptance
still requires a real retry; this measurement does not claim it fixed.

Private evidence is retained under the followup and unix-vector reviews and
`build/workloads/pi-futex-20261026`. Licensed data, raw account-bearing frames
and original saves are not committed.
