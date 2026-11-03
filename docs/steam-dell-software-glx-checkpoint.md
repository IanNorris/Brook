# Dell Steam software GLX checkpoint

On the XPS 13 9343, Valve's updater completed outside the inner SteamRT
container. Its package moves cross separate mounts inside that container and
fail with `EXDEV`; bypassing the VFS mount boundary is not a suitable fix.
The updated client opted into SteamRT 64-bit after restoring the appropriate
client flag and patching the updated executable interpreters.

The normal software Xwayland session did not expose GLX. An isolated Xephyr
server, running inside that session with `+extension GLX`, `+iglx`, Mesa's
software driver path and the Mesa vendor library available, reported GLX 1.4
and returned a visual from `glXChooseVisual`. Its private mount namespace
provided `/bin/sh` and a disk-backed temporary directory for its keyboard map
and lock; these experiments did not change the system-wide library cache.
Steam's private host library directory also needed the Nix GnuTLS-compatible
Nettle and Hogweed versions to avoid mixing them with older runtime libraries.

One client launched against the nested display got past the earlier GLX and
TLS errors and started its web helper. Shortly afterward the Dell stopped
responding to input and its debug connection was lost. The saved log does not
establish whether the cause was process exhaustion, graphics, or another
failure. A usable login window was not confirmed. Do not repeat this launch
without a bounded way to capture resource use and stop it before another
unrecoverable hang. The subsequent forced power-off required an offline ext2
repair; the SSD and filesystem identities were verified before preen, and a
separate full read-only check passed afterward.

This nested setup is a diagnostic path, not a finished desktop integration.
It uses software rendering, a separate X desktop and incomplete audio/session
integration. The ordinary Xwayland/GLX path and Steam's long-term launcher
still need proper fixes.

## Bounded retry

The Dell's original SSD boot pair was preserved while A/B slots were
initialized. A QEMU-tested kernel with opt-in debug-job task budgets then
booted from the new slot. On the Dell, the four-task acceptance probe passed
fork, unreaped-child, `exec`, `setsid`, thread and reclamation checks.

A single new Steam attempt used a 256-task budget. The last prompt live
sample, 14.6 seconds after launch, showed 35 reservations, a peak of 35,
zero budget refusals, 59 system processes and about 7.55 GB free. Subsequent
statistics requests timed out and the Dell stopped answering ARP. These are
the last observed values, not proof of the state at the eventual hang: a
resource spike or another failure could have occurred between samples. No
further Steam retries should be made until there is a way to capture or
recover from the failure. A task budget does not constrain memory or protect
against a kernel or device deadlock.

## Software-rendered QEMU comparison

A private clone of an older, previously clean Steam review image was booted
with the same kernel as the Dell task-budget candidate, four virtual CPUs and
software rendering. The Steam client/profile and device environment were not
identical to the Dell. The first attempt stopped before launching because the
clone had less than the launcher's required 1 GiB of unreserved space; after a
checked shutdown, the private data filesystem was enlarged offline.

A second attempt reached the client and web helper through a nested GLX-capable
Xephyr display. It was limited to 256 job tasks and paused deliberately at a
30-second host deadline for a QMP register capture. At the last successful
sample, 28.6 seconds in, it had 51 reserved tasks, a peak of 117, and no budget
refusals; the guest was still responding. The captured CPU instruction
pointers all resolved to the scheduler idle loop, a point-in-time observation
rather than evidence of a stall. The VM was resumed and later shut down with
its checked filesystem and device flush.

The client logs include repeated ANGLE/Vulkan initialization failures and,
after further runtime, repeated `Too many open files` errors from attempts to
set up PulseAudio. Brook currently reports an RLIMIT_NOFILE ceiling of 1024;
these messages do not establish whether the process hit that ceiling, leaked
descriptors, or encountered another FD allocation limit. Capture per-process
FD usage and allocation failures before choosing a fix. This run does not
establish the cause of the Dell's lost network or prove that the login works.

### PulseAudio control

A subsequent private-VM run inspected `/proc/<pid>/fd` after Steam's audio
errors. Its `steam` process had 1,024 open descriptors, with descriptors
sampled at 100–109, 400–409 and 1000–1023 all reported as `memfd`. This is
consistent with the earlier PulseAudio investigation in
`steam-pulse-shm-checkpoint.md`; it does not establish a general memfd leak.

In a fresh boot of the same private VM, an authenticated PulseAudio 17 null
sink was checked with `pactl info` before launching the same Steam client.
The Steam process had 91, 94, 93 and 93 open descriptors across four samples
about five seconds apart. This supports using the existing explicit audio
server for further Steam diagnostics rather than merely raising the descriptor
limit. A bounded 60-second run remained responsive, but the nested X display
was black and no login was shown. Its log included shared-memory corruption,
ANGLE/Vulkan initialization errors, and unavailable desktop services. These
are leads rather than a diagnosis, and the older QEMU Steam image is not an
exact reproduction of the Dell. The VM subsequently completed a checked flush
and shutdown.

### Nested software GLX and shared-pool controls

On a fresh software QEMU boot, `glx_check.py` targeted nested Xephyr :8 with
llvmpipe. It successfully created a GLX context, made it current and read
`[255, 0, 0, 255]` from a red clear without X or GL errors. The same check
also passed inside the SteamRT client container. The probe explicitly loads
Brook's Nix GL libraries; it does not test CEF's EGL/ANGLE implementation or
establish that the actual login window rendered. The SteamRT wrapper emitted
warnings about unsupported i386 helpers and missing ancillary utilities; the
64-bit probe itself exited zero.

The existing static synthetic shared-pool benchmark also exited zero both in
the guest and inside that SteamRT container. In the container's single run,
two simultaneously live 26,214,416-byte pools took approximately 96 ms and
326 ms for their respective cold writes. These are individual, non-normalized
measurements on the QEMU guest; they do not measure the Dell or prove that
Steam's parent obtained both pools before its inferred handshake deadline. The
benchmark binary had SHA-256
`c3fc9d68f4da202470e174063000363fb4720e491ddb3d627a78b04bdac200e4`.
The test VM was stopped with a checked guest and device flush.

`process_fd_summary.py` is a bounded guest-side follow-up for a known PID. It
reports counts by descriptor class without printing link targets, and reports
unreadable or raced entries rather than treating them as zero. Its snapshots
are non-atomic and a PID can be reused between samples; the caller must bind a
result to an independently observed process incarnation. A controlled Brook VM
process opened three memfds and the helper observed three; after closure the
count fell by three. The test VM completed a checked flush and shutdown.

### Handoff and proc-descriptor stat

A 60-second, metadata-instrumented QEMU run used the verified private Pulse
server and software Xephyr. The main Steam process mapped 18 distinct
26,214,416-byte objects; nine helper processes each mapped two of the same
objects as the main process. The trace had 1,180 rows, maximum per-process
sequence 574 of its 1,024-row cap, and no reported mapping-tracker miss. This
shows matching objects were mapped; it does not by itself prove their contents
were consumed successfully or that the deadline was met. Steam's main
process was sent SIGTERM after observation, its debug job drained, and the
original helper wrapper was restored and hash-checked before guest flush.

The trace's CEF stderr had repeated EGL initialization failures and fatal
`proc_util.cc(97)` diagnostics with EOPNOTSUPP. Chromium's published sandbox
implementation follows entries under `/proc/self/fd` with `fstatat` while
checking open directories, though the precise source revision corresponding
to this shipped CEF is not established. The new `procfdstatcheck.c` native
control passes directory, memfd, both pipe ends, Unix socket and eventfd cases
on Linux. On the Dell-candidate kernel under QEMU the directory case passes,
while the other five fail the followed-stat operation with errno 95. This is a
real compatibility defect and a plausible cause of the CEF failures, not yet
proof that fixing it alone produces a login window.

Following proc FD links currently creates retained targets for socket metadata
and selected vnode-backed descriptors, while other descriptors return
EOPNOTSUPP. A complete fix must respect process/descriptor lifetime,
authorization, dynamic memfd metadata, anonymous inode identity, pipe
reader/writer lifetime and O_PATH semantics. It must also handle the actual
FD classes encountered by CEF without incorrectly treating synthetic
directories as ordinary files. The regression is deliberately left failing
until those contracts are implemented and tested; a fabricated stat snapshot
is not a general proc FD implementation. The test binary SHA-256 was
`2572fe52818a283d1b20b36c8d1e3c61ef2c8802dc414b56f13c9ae509f90ed0`.

### Anonymous descriptor stat candidate

The kernel now follows selected same-thread-group anonymous proc FD links to
an ephemeral stat-only snapshot. It captures a memfd reference under the FD
lock, snapshots its size and allocated blocks after releasing that lock, and
then drops the reference. Pipe and other selected descriptors use copied scalar
metadata; no reader/writer endpoint is retained. The snapshot object rejects
all opens, including `O_PATH`, so it does not claim persistent proc FD reopen
semantics. Other unsupported descriptor kinds, including the synthetic DRM
directory and render-device descriptors, remain separate work. Namespace,
incarnation and existing filesystem authority checks are preserved.

`VnodeStat` carries optional exact descriptor blocks, device and already-
reported IDs; its layout change raises the kernel module ABI to 64. The
matched release kernel and all 13 modules were rebuilt and staged together.
Three focused hosted tests pass, including reference acquisition, a simulated
close/reuse, group-incarnation denial and terminal-pipe classification. The
native static proc FD test passes on Linux and on the final ABI 64 QEMU kernel
for a normal process and a `CLONE_NEWUSER` child, including sparse memfd
size, blocks and ownership comparisons. A four-thread close/stat stress test
also passes 300 iterations per worker. The earlier `unshare`-based fixture
failed with ENOSYS; the passing namespace fixture uses clone instead.

With the earlier source-equivalent ABI 63 candidate, an authenticated private
VM profile rendered the Steam client instead of the black screen; its Store
reported a content-load failure. The rebuilt, matched ABI 64 kernel repeated
the bounded software-Xephyr/Pulse run and rendered the client and Store
artwork. During that captured interval there were no `proc_util.cc(97)` or
GPU-process fatal messages, though EGL initialization warnings remained.
The profile, client build and virtual hardware differ from the Dell, and no
fresh QR sign-in or Dell stability is implied. The screenshots and account
state remain private. Both candidate VMs completed checked guest and device
flushes before stopping. The final ABI 64 kernel SHA-256 is
`942500681ee1b8111d4694e7a99f40c74a6e90c4f2361320e4bd6cb21125496a`.

### Dell candidate packaging

A private Dell kernel/initrd candidate is prepared from the last deployed
initrd with the ABI 64 kernel and all three drivers present in that initrd
replaced from the matched build. `prepare_hardware_kernel_pair.py` pins the
chosen kernel hash, enumerates visible and hidden initrd drivers, verifies
module ABI metadata and checks the copied bytes. It preserves the other
initrd configuration and does not touch a device. The requested module ABI is
not independently extracted from the kernel: compatibility is established for
this candidate by a separate exact-pair QEMU boot, not by the packager alone.

The exact final packaged kernel/initrd booted in QEMU and exposed the expected
hardware debug network and payload identity. The kernel hash remains
`942500681ee1b8111d4694e7a99f40c74a6e90c4f2361320e4bd6cb21125496a`;
the packaged initrd hash is
`f3bc21ef5c8b11eb78f8cdf3edc1354de2d8bc9e022679f720cdd05ab281e3d9`.
The packager's six host tests include hidden modules, ABI mismatch, a missing
module, an unexpected kernel and real ELF metadata parsing. The candidate was
subsequently activated in SSD slot B (generation 4) after a separate upload-only
check, retaining the prior slot. A checked restart reached the expected payload,
Ethernet and the 255,791,009,792-byte persistent desktop. On the physical Dell,
the unprivileged, bounded native proc-FD probe exited zero and passed directory,
memfd, pipe-read, pipe-write, socket, eventfd and sparse-memfd cases. This
verifies those cases on the candidate kernel, not the cause of the Steam freeze.

### Fresh-client software VM follow-up

After the Dell SSD was manually repaired, a reflinked private QEMU clone used
the matched ABI 64 kernel, software Xephyr and a separately verified PulseAudio
null sink. A new client was downloaded from Valve into a new guest home; no
signed-in profile was copied into that home. The download manifest identified
version 1792618285, matching the retained comparison client's manifest.

The first direct launch exited 42 because the bootstrap had not created
Steam's `.steam-enable-steamrt64-client` marker. After adding it, the web helper
repeatedly failed on an incompatible Hogweed symbol: the profile's GnuTLS was
being mixed with the runtime's older Nettle libraries. Pairing Nettle and
Hogweed from GnuTLS's own RPATH eliminated those symbol errors. On the next
launch after Steam had recorded its installed manifest, the login UI and QR
code appeared; a test string typed into the account field was visible, and a
clear command was sent without signing in. No credentials were used. The 60-second
bounded run peaked at 163 of 256 reserved job tasks and recorded no denied
births; a QMP capture was taken, and the client was subsequently terminated
and its tracked job drained. The comparison's screenshots and client data are
private build artifacts. It demonstrates this VM login path, not Dell stability
or physical graphics support; the Dell's prior hard freeze remains unexplained.

### Attended Dell retry

With the ABI 64 candidate running on the Dell, a private PulseAudio null sink
passed an authenticated connection check. A nested software Xephyr display
using the Dell's installed Mesa exposed GLX 1.4; an unprivileged probe created
a llvmpipe context and read back a red pixel with no reported GL or X error.
The Steam job was then started with a 256-task limit while a host collector
recorded kernel logs and memory/process statistics. The Dell stopped answering
the network approximately 22 seconds after dispatch. The last live sample
reported 72 processes, 7,499,501,568 bytes free and an idle OOM state. No new
kernel log lines were captured immediately before contact was lost. These
figures are not observations of the eventual failure state and do not explain
the freeze. In the last eight live seconds completed-process and address-space
counts increased while cleanup queues remained empty, and compositor p95 stayed
at 18 ms. That makes a gradual buildup in those observed queues or compositor
intervals less apparent, but does not rule out a sudden lockup. The exact-kernel
panic scanner was pinned to the deployed kernel. The user confirmed the hang
and triggered a manual panic; the resulting capture shows three other CPUs
spinning while acquiring the indirect-block cache lock. It does not identify
the lock owner or prove a common mount. The cache previously held that lock
across allocation and potentially blocking disk reads, a credible starvation
mechanism. A candidate moves those operations outside the spinlock; a host
regression gates concurrent misses and checks failures and eviction. Physical
Steam validation remains outstanding. Raw diagnostic samples remain private.

After offline e2fsck repair and a separate clean read-only check, the candidate
was tested in private QEMU and staged to the Dell's alternate SSD boot slot.
The preserved previous slot remains available. With the same isolated software
Xephyr, null audio sink and 256-task cap, the physical Dell stayed responsive
past the earlier failure and displayed the Steam QR sign-in screen. The
attended frame and logs are private; no login success is claimed until the
user verifies it. This successful run supports the lock-scope diagnosis but
does not prove every possible hang is fixed.

### Emulated USB network comparison

A separate four-CPU software QEMU run used the same kernel and timer fallback,
but replaced virtio networking with emulated xHCI and USB ECM. A fresh Valve
client reached the QR sign-in screen and remained reachable through the
120-second capture. This exercises Brook's USB network path but not the Dell's
Intel xHCI, Ethernet adapter or physical SSD. After a checked guest sync and
QEMU exit its disposable disk copies were removed.

### Timer-fallback comparison

A new reflinked software QEMU clone used the same ABI 64 kernel, four virtual
CPUs, a fresh Valve client and a new unsigned-in home. QEMU's KVM CPU identity
was hidden; the serial log explicitly reported `APIC: elapsed clock delivered
IRQ fallback` rather than pvclock. The updated bootstrap completed without
manually adding its opt-in or crypto links. With software Xephyr and a verified
Pulse null sink, Steam displayed its QR login and the guest remained responsive
until a host QMP capture at 120 seconds. The job peaked at 160 of 256 reserved
tasks with no observed refusals; the client and test services were then stopped
and the guest completed a checked shutdown. This tests more than a clock switch:
hiding KVM changes guest-visible hypervisor features as well. It reduces the
likelihood that the fallback timer alone explains the Dell failure, but does
not rule out hardware-specific timing, USB, storage, or other driver interactions.
The disposable disk copies were removed after shutdown; private diagnostic
captures remain under `build/reviews/steam-dell-fallback-20261103`.

### Post-authentication observation

The user authenticated on the Dell, but the client remained at the loading
screen. In a subsequent recovery-boot retry, the same software display and
null audio setup remained responsive and peaked at 176 of 256 job tasks, with
no denied births. A point-in-time sample showed 119 descriptors for the main
client and 179 for its browser helper. These samples do not establish the
resource state of the prior failed session or rule out a later spike.

The physical LAPIC-based syscall timing probe passed a controlled sleep test
with no invalid spans. During the retry the main process continued polling;
the browser helper repeatedly made short `recvmsg` calls returning negative
results. This alone does not distinguish expected nonblocking retries from an
error. The new opt-in failure summary captures the first 16 distinct negative
dispatch syscall/result pairs with the latest first argument and an explicit
overflow count. It is bounded and disabled until a thread is selected. Raw
results can include internal restart codes; the first argument can be a
pointer, and samples do not prove peer progress. Host model tests and a kernel
build pass; the extended response and physical Steam behavior still require
runtime verification. Account details and screenshots remain private.

The failure summary was subsequently checked end to end in a disposable VM:
repeated `close(-1)` calls appeared as syscall 3, result `-EBADF`, with the
expected argument and no overflow. The matched Dell kernel/initrd also booted
in a separate QEMU fixture before physical deployment; that does not test the
physical SSD or xHCI hardware. On the Dell, the web helper's repeated
`recvmsg` failures were `-EAGAIN` on a Unix descriptor, consistent with a
nonblocking read with no data. The Steam connection log recorded successful
authentication, but its browser log then reported `/dev/shm` ENOSPC. A live
sample reported 3,792,896 bytes available from a 495,853,568-byte mount;
after the Steam job drained, 482,508,800 bytes were available. This points to
live shared-memory pressure, while leaving open whether a larger quota alone
will produce a usable client or whether usage would continue growing.

A first attempt to test an 800 MiB tmpfs in an unprivileged nested namespace
failed with EPERM; a root bubblewrap attempt stopped earlier at its unsupported
`capget` syscall. Neither experiment changed the desktop's `/dev/shm` mount.
The next comparison needs a verified private mount or a separately tested,
boot-derived budget candidate; preserving aggregate memory headroom matters.
