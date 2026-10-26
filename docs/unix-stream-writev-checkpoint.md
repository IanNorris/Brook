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
cost. Host load and client content can vary. These native measurements alone
do not establish game scene-loading acceptance.

## Original-game retry

Goose is launched normally through Steam with the original executable, API DLL
and restored Proton settings. The new-game test explicitly verifies **Save 02,
Empty** before confirmation, leaving the existing Save 01 untouched. It reaches
the rendered tutorial, responds to the prompted Space honk, and accepts
mouse-directed movement out of the starting bush onto the path. Camera motion,
the goose model and the movement tutorial render correctly.

A passive 44.1 kHz stereo HDA capture records three honk keypresses. The capture
contains 5.053 seconds of samples over 5.055 host seconds. Before input its RMS
is 0.00247 and peak 309/32768; the three response windows have RMS
0.02710/0.02390/0.02467 and peaks 8957/6958/8979. This establishes audio delivery
and input-correlated responses, not a subjective stutter-free playback verdict.

The game's pause-menu Quit dialog reports progress saved and returns to the
title. The title-menu Quit and its Yes confirmation remove the window and
known main PID 561. Steam removes the app from its running list; its tracked
launcher PID 410 returns zero. Individual Wine statuses remain unknown (-1 in
Steam's tracking log), not independently collected direct process exit codes.
Original Goose save contents/timestamp, the 33 original Stardew file contents
and non-cloud-marker timestamps, and both retained Graveyard save contents
remain unchanged after the gameplay save-and-quit path.

Loading is still slow. The earlier loading attempt did not verify the selected
slot, so this successful new-save run does **not** prove that the stream change
alone fixed the same old-save scenario. Older Goose saves, long sessions and
full-game coverage remain unverified. No licensed binary or save is modified
or submitted to obtain this result.

The review uses a relative PS/2 pointer. An attempted absolute QMP movement
correctly reports no handler; relative input and a guest XQueryPointer readback
allow the movement test. Bridge coordinate actions should discover this
capability and provide a relative fallback or an explicit unsupported result.

## Native Graveyard renderer control

On the preceding kernel, normal Steam launches at the same 1280×720 size give
different results: OpenGL renders only the title background, while
`-force-vulkan -screen-width 1280 -screen-height 720 -screen-fullscreen 0`
initializes hardware Venus and renders the full title logo and menu buttons.
Both runs close with normal WM_DELETE and their native tasks drain before
the next control. No existing save is opened. This narrows the UI issue to
the renderer path; it does not claim the OpenGL or resize bug fixed. Vulkan
input/audio/resize acceptance was checked next on kernel
`4e47f74440f0a2f5da43a1ad228da5c54368cff3a61dd5a379fb72c169fa4826`.

The full Vulkan menu renders again. A passive HDA capture contains 135,808
stereo frames at 44.1 kHz (3.080 seconds), peak 3757/32768 and RMS 0.01820.
That proves sample delivery, not subjective stutter-free sound. Pointer Options
and keyboard menu attempts do not establish a successful menu transition;
input remains unaccepted.

Maximizing does not reproduce the earlier fully black GL window, but the
composited image initially occupies only its old-size area. After restoring,
the rendered content has the wrong scale/extent for the restored window.
XGetGeometry reports the maximized X11 drawable at 1916×996, with nonblack
source samples across it. This distinguishes source drawing from the displayed
geometry, but does not identify the faulty stage or fix the resize problem.

Normal WM_DELETE eventually removes the window and native PID 760. Steam
removes app 599140 from its running list and its tracked launcher 697 exits
zero. Individual native process statuses are reported as unknown (-1), not
independently collected zero exit codes. Existing saves were not opened:
both original Graveyard save contents, original Goose save contents/timestamp,
and all 33 Stardew contents/non-cloud-marker timestamps remain unchanged.

## Final review closure

After the platform-depot architecture checks, normal Steam shutdown completes
with collected exit zero, zero live descendants and both output streams at EOF.
The final original-save checks retain the guarantees above. Checked VM stop
acknowledges filesystem and device flush; the original launcher exits zero.
The serial acceptance check and all four offline filesystem checks (boot,
data, Nix and temporary disks) pass. No forced stop was needed for this review.

This closes the bounded PI/presentation investigation. Remaining native
Graveyard input/resize and OpenGL UI issues are explicit follow-up work;
the Goose result uses the isolated Mesa candidate and software X11 presentation,
not repaired direct cross-capset PRIME import or a default-driver deployment.

Private evidence is retained under the followup and unix-vector reviews and
`build/workloads/pi-futex-20261026`. Licensed data, raw account-bearing frames
and original saves are not committed.
