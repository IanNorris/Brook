# Loading throughput controls — 26 October 2026

Stardew gameplay is user-verified and marked compatible on main, with slow
loading explicitly retained as a limitation. The earlier hard-link/inode-index
change reduced one request-to-window observation from 333.5 to 128.3 seconds;
subsequent same-kernel observations varied substantially. This pass repairs the
test harness, adds cache/mount-fanout controls, and implements conservative
mount-lookup hints. Application measurements and their limits follow below.

## Reproducible small I/O gate

The synthetic harness had silently depended on privileged bridge launches.
Ordinary launches now default to the Brook user, so creating fixtures directly
under root-owned `/data` failed with `EACCES`. The probes now explicitly request
UID 0 and record that identity. This is a test-fixture fix, not evidence of a
new filesystem regression or of unprivileged app compatibility.

All runs use the same ABI-55 kernel SHA256
`3fb62053bca2733caf4a136ac91f3bd8de965147a68863f6a86682570cb3c33f`,
8 vCPUs, 2 GiB guest RAM, eight workers, 8 MiB per worker and 64 small files per
worker. RAM and disk gates pass mixed-append, boundary/fragmentation, content
verification after reboot, cleanup and offline checks, returning free blocks
and inodes exactly to their starting values.

The new `--backend disk --host-cache cold-cleanup` control waits for the first
VM's owner to reap QEMU, finishes offline checks, fsyncs only the private image
inodes, requests `POSIX_FADV_DONTNEED`, and measures their page residency before
and after. It rejects incomplete eviction. It does not evict host filesystem
metadata or device caches and never changes global cache settings.

Two consecutive runs with durable controller ownership both exit zero:

- Normal cache: whole guest gate 32.58 s; sequential write 50.6 MiB/s,
  sequential read 85.9 MiB/s; reboot verification of 64 MiB takes 338.95 ms.
- Cold cleanup: whole gate 34.13 s; sequential write 60.7 MiB/s,
  sequential read 68.1 MiB/s; reboot verification takes 333.00 ms. All three
  images have zero resident host file pages immediately before the second boot.

Only the reboot/cleanup stage follows eviction. Initial write/read phases cannot
be attributed to that cache control. Boot itself can repopulate pages before
verification. These results do not prove equivalence for large application loads.

Host samples cover the entire controller interval, including Nix setup and
offline checks, rather than exactly aligned guest phases. CPU `some` pressure
accounts for 15.2% and 16.8% of those intervals; host load ranges from 0.93–7.23
and 9.60–16.16 respectively. Global I/O `some` pressure is 0.52% and 0.40%.
These are shared-host measurements, not workload-exclusive counters. The runs
are useful correctness/cache controls, not a quiet-host A/B performance claim.

Earlier small cached runs reached 238–354 MiB/s on sequential reads. Another
cold-control attempt completed guest checks but its outer controller exited
143; it is retained as incomplete controller acceptance and excluded from the
successful pair. Its initial phases were already slower before eviction.

## What to measure next

The small gate does not reproduce the real load: Stardew startup previously
issued roughly 334–414 thousand data-image reads, with around 1.0–1.3 GiB read.
Aggregate QEMU request service times overlap; they are not an additive wall-time
breakdown. Neither these counters nor the cache controls justify blaming one
guest function for the full delay.

The next bounded experiment should separate Steam runtime preparation from game
initialization, then compare metadata/path lookup cost at low and high bind-mount
counts. Prior captures include retained mount/path walking and synchronous
virtio block I/O. A mount-fanout probe with unchanged file content would test
that lead cheaply before changing mount indexing. Pair this with actual request
size/count measurements on the existing application images; throughput of fresh
small images is insufficient evidence about their layout or cold behavior.

Raw reports, cache residency, original-controller exit receipts and host samples
remain under ignored `build/workloads/loading-throughput-20261026/`. No game,
account, save or authentication data is published.

## Mount fanout experiment and candidate

The added `--mount-fanout-probe` uses the same checked one-byte file throughout
three stages: before, during and after 128 unrelated bind mounts. Each stage
contains three batches of 500 absolute lookups and 500 directory-FD-relative
lookups, including content/EOF verification on every iteration. It separately
checks overmount restoration and retained-FD identity. Linux passes the same
fixture in a private user/mount namespace, without the roughly doubled cost
observed in Brook's short samples.

The complete Brook baseline passes with original controller exit zero, clean
offline filesystem checks and exact allocation restoration. Median time per
checked absolute / directory-FD lookup is 26.49 / 29.84 microseconds before
fanout, 53.18 / 57.98 during fanout and 24.71 / 30.17 after removal. These
include open, byte/EOF reads and close, not just the pathname walk.

The candidate adds two conservative per-attachment hints: whether an attachment
may contain a covered mount, and whether it may parent a virtual mount ancestor.
False permits skipping the corresponding global registry scan; true retains
the existing exact lookup. Hints only transition to true. They never cache a
mount result or make a detached entry authoritative. Virtual chains mark every
actual parent attachment; namespace clone, recursive-bind publication and
pivot-root preparation also record new edges. Detach/failure may retain a true
hint, costing a scan without changing behavior.

Candidate kernel SHA256:
`e30a5e2940a6191319e0eba7c74665e1debb99d5e00dbfbb3713211eb089d2ae`.
The same native gate passes with controller exit zero and exact allocation
restoration. Its medians are 24.33 / 29.02 microseconds before fanout, 42.48 /
45.55 during fanout, and 20.54 / 24.97 after removal. The high-fanout reductions
are about 20% and 21%; this is not a measured game-loading improvement.

The existing production-VFS host fixture passes namespace copies, recursive
binds, virtual ancestors, pivot-root, file-bind aliases, failure rollback and
retirement/lifetime checks. Both compiled entry/scheduler mutation audits pass.
All 109 host tests pass across the validation: 108 in the full suite and the
unchanged FD stress test separately in 5.52 seconds. The latter was interrupted
in the first full-suite attempt after saturating the shared host while a review
VM was open. That attempt is not a passing full-suite result, and the contended
VM interval is excluded from performance comparisons. Future stress runs should
be separated from application timing.

Two earlier fixture attempts are excluded from acceptance: one used unsupported
`unshare` instead of Brook's supported mount-namespace `clone` path; another's
legacy launch reply timed out even though the probe completed. The latter was
recovered with checked shutdown and clean offline checks. The corrected gate
waits longer for that one launch reply and never retries a launch after timeout.
An incomplete candidate capsule failed before any VM launch and was replaced by
one containing its matching symbol table. Successful controls are named
`io-mount-fanout-owned-baseline` and `io-mount-fanout-candidate-complete` in the
private evidence directory.

## Real Steam/Stardew acceptance and first matched loading pair

The candidate opens the signed-in Steam client and renders its Library and
Store. One warm-client observation reaches Stardew's process after 41.28 s and
window after 53.04 s without a profiler during that launch. That observation is
not a matched comparison with earlier 128–212 s runs. It reads 1,049.8 MiB from
the data image in 336,019 requests, with 29.94 s of overlapping aggregate read
service time. The game renders its menu and has an active clocked audio stream.
After normal game close, a one-second QEMU mixed-output capture contains only
zero samples; no stale last-buffer playback is detected in that capture.

The subsequent matched pair clones both runs from the same checked, stopped
private disks, using 8 vCPUs, 8 GiB RAM, the same Intel graphics/display/audio
configuration and ordinary Steam launch path. All six private image inodes have
zero measured file-page residency before each launcher starts. The launcher can
repopulate pages; filesystem metadata and device caches remain uncontrolled.
There is one run per kernel, in baseline-then-candidate order.

- Baseline: Steam main window observed after 72.32 s; game process after
  157.20 s and game window after 169.38 s from its launch request. Data-image
  reads are 1,306.87 MiB in 406,137 requests; writes 161.74 MiB; aggregate read
  service 53.75 s.
- Candidate: Steam main window observed after 61.10 s; game window after
  75.78 s from its request. Data-image reads are 1,285.89 MiB in 400,036
  requests; writes 158.42 MiB; aggregate read service 42.98 s.

This pair observes about 55% less request-to-window time, with similar data
traffic. It is encouraging application evidence, not a repeated statistical
benchmark or a promise of the same reduction on other loads. Shared-host CPU
pressure, order effects and uncontrolled lower-level cache state remain limits;
service times overlap and cannot form an additive wall-time breakdown.
Whole-controller CPU `some` pressure is materially different: 16.43% for the
baseline and 0.35% for the candidate. These intervals include startup and
shutdown, not just the game load, and do not attribute pressure to a particular
process. Therefore the pair does **not** establish a causal 55% code speedup.
A reverse-order control is required before strengthening that claim.

That reverse-order pair is now complete, using fresh copies of the same seed
and the same pre-launch file-page control. The candidate reaches the game
process at 66.52 s and window at 77.78 s; the baseline reaches them at 85.31 s
and 98.12 s. Steam main-window observations are 61.99 and 60.76 s respectively.
Whole-controller CPU `some` pressure is 0.34% in both runs, versus the substantial
imbalance in the first pair. The candidate's data reads are 1,287.29 MiB in
400,416 requests; baseline reads 1,307.42 MiB in 405,973. Their overlapping
aggregate read-service times are 40.30 and 42.87 s.

The quieter reverse pair observes about **21% less game request-to-window
time**. Candidate window times are consistent across the two pairs (75.78 and
77.78 s); the first baseline's 169.38 s remains a contended observation rather
than the basis for a 55% optimization claim. Much of the quiet-pair difference
precedes creation of the game process, consistent with the runtime-preparation
target. These remain two observations per kernel on a shared host, not a broad
application benchmark. No further performance change is pursued in this pass.
Whole-QEMU process CPU observations in the reverse pair are 532.90 CPU-seconds
for the candidate and 544.99 for baseline, spanning most of the VM interval
rather than an exclusive game-load phase.

Both games close through their normal WM_DELETE protocol. Both Steam groups
drain with exit zero, audio streams drain, checked filesystem/device sync
completes, original VM owners exit zero, and all four offline filesystem checks
pass. All 33 original save content hashes and non-marker mtimes remain unchanged.
Cloud-marker mtime changes remain separate from gameplay save writes. The
candidate's earlier standalone review also passes these checks. Private pair
evidence is under `build/reviews/stardew-loading-ab-20261026/`.
Both reverse-pair runs pass the same acceptance checks; their evidence is under
`build/reviews/stardew-loading-ba-20261026/`.

## Off-screen window scaling explained the installer clicks

The installer was not blocked in its installation callback. Steam created a
1,920 × 1,036 window at (100, 100) on the 1,920 × 1,080 desktop. The GPU
compositor shrank its destination to the visible 1,820 × 980 rectangle while
sampling the complete texture. X11 input retained the original coordinates.
A native X11 readback and the composed frame measured a 500-pixel modal bar
shrunk to 474 pixels, matching the horizontal clipping ratio. Maximizing Steam
removed the clipping; checkbox, Cancel and Install then worked, and Graveyard
Keeper downloaded successfully.

The fix preserves the full source-to-destination mapping and lets the GPU
viewport clip it. Desktop draw origins are signed throughout the ABI-56
kernel/driver interface. Endpoint arithmetic uses 64 bits and rejects overflow;
NDC conversion retains large off-screen coordinates without narrowing the Q23
quotient to 32 bits. Chrome and screen-space backdrop helpers retain their
existing matching source/destination clipping.

The matched kernel and all 13 modules build. Compiled entry and scheduler
mutation audits pass, as do all 110 host tests (14.79 seconds). A focused test
covers signed bounds, all edges/corners, unequal texture/window dimensions and
extreme NDC coordinates. Native Xwayland color-grid readback additionally
checks 18,466 actual composed pixels across all four edges and two corners,
with zero mismatches after moving the cursor away. The first corner capture
had three cursor-covered pixels; its unobstructed repeat passes. Evidence remains private
under `build/reviews/steam-games-clip-20261026/modal-control/`.

## New-game acceptance remains open

Graveyard Keeper downloaded 553,248,832 bytes and installed 1,893,434,515 bytes.
The actual Linux launcher and UnityPlayer are ELF64 x86-64. The first launch on
the old control kernel created no game window: Player.log stopped just after
display discovery, and saved kernel stacks showed the main and worker threads
in futex waits. The main user stack reaches UnityPlayer's pthread_mutex_lock
wrapper. This is a diagnosis lead, not proof of a futex defect. The adjacent Unity
initializer requests recursive + priority-inheritance attributes and ignores
the initialization result. A reduced public control with hash-matched soldier
libc/pthread 2.31 succeeds on Linux (kind 33) but returns ENOTSUP/95 on Brook
(kind 0). Brook intentionally reports PI futexes unsupported, as recorded in
[the PI checkpoint](steam-pi-futex-checkpoint.md); returning fake success is
not a fix. A passive observer then delegates the real game-container pthread functions
without changing their results. It records ENOTSUP for mutex `0xc009a680`
at UnityPlayer offset `0x1bb5756`, followed by a lock of that same mutex at
`0x1d54395`, the captured blocked call site. This confirms the ignored
initialization failure. The observer uses the actual container override libc;
the soldier-2.31 reduced test is an additional control, not a claim that every
container selects that libc. This diagnostic launch is distinct from normal
Steam acceptance and is terminated after saving the evidence. SIGTERM and
normal Steam shutdown did not clear the game; targeted SIGKILL drained it,
then checked VM shutdown, original-owner exit zero and all four filesystem
checks succeeded. No game save was opened.

Neither Graveyard Keeper nor Untitled Goose Game is marked compatible yet.
Untitled Goose Game's Win64 metadata is preflight evidence; its game payload
has since been downloaded and its Unity executable, crash handler and DLLs
are confirmed PE AMD64. Its default Proton Experimental 11 Wine and wineserver
are ELF64. The first launch faults in the optional Wine preloader after its
low-address reservation covers its own 2 MiB image, reproducing the loader
placement limitation already recorded in [the Proton checkpoint](proton.md).
A logging-only repeat confirms the same fault; the supported
`WINELOADERNOEXEC=1` setting alone still executes this build's preloader.
The previous optional-preloader staging workaround also fails here: Runtime
4 explicitly selects that path and exits 127 when it is absent. The original
preloader is restored with the same SHA256 before testing the loader fix.
The preloader is verified ET_DYN without PT_INTERP, entry `0x1000`, load span
`0x6000` and maximum PT_LOAD alignment 4096. Its original bytes remain unchanged. Both titles' assets, account data and saves remain in
ignored private images.

A transient CEF reason-213 failure preceded recovery of the Library; a normal
client restart subsequently renders the Store. In Chromium 126.0.6478.183,
reason 213 is `INVALID_INITIATOR_ORIGIN`, a semantic origin authorization check.
The visible `data:` URL alone does not explain it. No kernel IPC repair or
cache reset is justified by that observation.
[Exact-version check](https://github.com/chromium/chromium/blob/126.0.6478.183/content/browser/renderer_host/ipc_utils.cc).

The fresh-modal ordering control, recursive X11 tree/focus inspection and
native/composed pixel comparison resolved the installer issue without changing
Steam IPC or launcher permissions. On the matched ABI-56 image, Steam's 1,920 × 1,036 window moved to (100, 100)
retains its modal geometry. The desktop-shortcut checkbox toggles at its
displayed coordinates and toggles back before confirming the owned Goose
installation. This verifies actual input alignment as well as pixel mapping.
Further game changes require evidence from the new launch.

## Interpreter-less PIE placement

The restored preloader needs a loader-policy repair rather than an executable
mapping exception. Linux distinguishes interpreted PIE programs from ET_DYN
loaders without PT_INTERP when choosing placement.
[Linux ELF loader](https://github.com/torvalds/linux/blob/master/fs/binfmt_elf.c).
The selected Proton 11 preloader's fixed reservations cover the DOS/low-memory
arena, a 32-bit allocation band and a high allocation band near the stack;
`0x600000000000` lies outside those ranges.
[Proton 11 preloader](https://github.com/ValveSoftware/wine/blob/proton_11.0/loader/preloader.c).

Brook now prefers that high address only for interpreter-less ET_DYN main
images. Interpreted PIEs retain their 2 MiB preferred base, and ET_EXEC retains
its linked addresses. Bias rounding preserves the largest PT_LOAD alignment;
unusual interpreted PIEs with nonzero linked addresses or large alignments
can therefore move slightly higher than before to satisfy that constraint.
The common preflight/load calculation retains the image-size bound, 128 MiB
brk reservation and 16 MiB mmap guard below `USER_MMAP_END`. The separately
loaded PT_INTERP address is unchanged. This is deterministic placement, not
ASLR or permission protection against a program overwriting itself.

The updated real-loader host tests cover sparse/reversed/overlapping segments,
COW ownership, collision rejection, TLS template relocation, entry/PHDR/brk,
unchanged interpreted PIE/ET_EXEC placement, nonzero-address bias alignment
and oversized-alignment rejection before allocation. All 110 host tests pass
(14.28 s); exact compiled entry and scheduler mutation audits pass.
Astra's read-only review found no blocking correctness issue.

The public [staticpiecheck probe](../tools/at-syscall-probe/staticpiecheck.c)
has no libc or interpreter and no dynamic relocations. The same ELF passes
on Linux. On the old Brook policy it exits 42 with entry `0x201040`, safely
reporting low placement before making a fixed reservation. On the new kernel, the same SHA256
`8856775d2eefe0a7b519ce5cd155919333f2b2333863d0871826f76a1711155c`
passes through both direct creation and guest execve: entry
`0x600000001040`, brk `0x600000006000`, mmap `0x600009006000`, and exit zero
with `STATICPIE_RESERVE_PASS`. Auxv, initialized data/BSS, brk grow/restore,
anonymous mmap and execution after the low fixed reservation all pass.
The original unmodified Wine preloader is the subsequent workload control.

## Restored Proton launch and presentation controls

With the high-placement kernel, the original Proton 11 preloader runs without
modification. Untitled Goose Game reaches Unity 2018.4 initialization, creates
its D3D11 device on Intel UHD 630 through Venus, initializes input, loads assets
and connects to Steam. This validates progress past the earlier loader fault;
it does not establish game compatibility.

The default X11 presentation path repeatedly fails Vulkan swapchain creation
with `VK_ERROR_INITIALIZATION_FAILED`. A separate control explicitly sets
`MESA_VK_WSI_DEBUG=sw,noshm` on the Steam client. That removes the swapchain
errors and creates a native game window, but the game remains white and silent.
The effective game environment is recorded before interpreting this result:
putting the setting only in Proton's `user_settings.py` did not override Brook's
existing empty environment key, because that Proton mechanism supplies defaults
only for absent keys. No production environment policy was changed.

Stock Proton Experimental 11 in this image has no Wine Wayland driver. The
attempted Wayland setup failed before changing the configuration; a subsequent
X11 launch is explicitly excluded as a Wayland test. The helper now propagates
guest setup failures to prevent dependent launches after such failures.

The presenting X11 control remains white after explicit X11 focus and native
maximize. Direct X11 image reads from both the game window and its render child
also return white, placing that symptom before Brook composition. A three-second
audio capture is silent. The main thread waits in Wine's reply-pipe readv; worker
stacks mostly wait in pipes or futexes, with wineserver waiting in epoll. A later
check finds the address-space mutex unlocked, so transient mapping-lock waits
are not evidence of a kernel deadlock. There is no justified pipe or compositor
repair from these stacks alone. An inventory found no installed native Vulkan
probe at the known paths, so no additional high-address Vulkan control was run.

The game ignores its final normal WM_DELETE request; a targeted termination is
required. Neither a playable scene nor clean game exit is accepted. These
presentation and stall issues remain separate follow-up work, alongside real
PI-futex support for Graveyard Keeper. The bounded investigation stops here
rather than introducing a speculative synchronization or graphics rewrite.

Final save checks retain all 33 original Stardew save-file contents and every
non-marker file's timestamp; Steam's cloud marker timestamp changed. Goose's
existing cloud save retains both contents and timestamp. No existing game save
was opened during these new-game tests. All game files, logs, account details
and unredacted screenshots remain private and excluded from git.

The final Steam operation completes with exit zero and zero live tasks. The
audio mixer reports no streams. Checked VM shutdown confirms filesystem and
device flush; the original launcher exits zero and all four offline filesystem
checks (boot, data, nix and tmp) return zero. The work remains on
`wip/loading-throughput-20261026`; these new-game results do not promote either
title to the compatibility list.
