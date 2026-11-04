# Individual Xwayland windows

Brook now has an opt-in owned rootless Xwayland path. Native controls show two
independent xeyes windows, with no enclosing X desktop. Authenticated Steam
Store and Library render in a native window, and Friends opens and moves as a
separate window. Longer Steam acceptance, broader transient policy and
shaped-window rendering remain open.

## Native geometry API

`WM_WINDOW_STATE` (`0xB08`) snapshots one calling provider's window without
mapping pixels or exposing foreign windows. Its shared 40-byte ABI is
`src/shared/wm_window_state.h`. It reports client coordinates, extent,
focus/visibility flags and creation incarnation. Zero discovers the identity;
subsequent queries can require it. Missing/foreign windows return `ENOENT`;
owned recycled slots return `ESTALE`.

`WM_PLACE_WINDOW` (`0xB09`) accepts the owned window's nonzero incarnation and
signed client coordinates, preserving size and focus. Both operations serialize
window access under the native mutex. Query output is prefaulted before that
mutex and copied after releasing it. Provider buffer sizes now preserve their
exact requested extents; interactive desktop resize minima remain separate.

The production window lifecycle host fixture passes ownership, incarnation,
placement, scaled decoration and signed-coordinate controls. The release kernel
builds; entry-state audits pass for the supported templates, together with
scheduler-start and lock/wait mutation checks. This is not universal handler
coverage. Frozen kernel SHA-256:
`f1092252021847281bf229169b2f9f2e99bb4a937c69c05c7a5c1eeb704367cd`.
`scripts/workloads/window_state_check.py --brook-guest` passes 23 native checks,
including inaccessible output, foreign ownership, placement and a 32x24 buffer.

## Owned server and private XWM

Launch with:

```sh
waylandd --xwayland /nix/profile/bin/Xwayland --xwm /nix/bin/brook-xwm --xdisplay :0
```

The launcher creates private Wayland, XWM, control and display-readiness socket
pairs. Only the exact compositor-created `wl_client` can see or bind
`xwayland-shell-v1`; UID and client-supplied names do not grant access. The shell
associates committed 64-bit serials with `WL_SURFACE_SERIAL` events in either
arrival order, scoped to a server generation. Destroying the short-lived shell
object preserves pending and committed surface association state.

`brook-xwm` accepts inherited descriptors, never ambient `DISPLAY`. XCB startup
and synchronous replies run in this worker rather than Wayland dispatch.
Composite manual redirection is required for real rootless Xwayland surfaces.
Normal X windows use Brook chrome; override-redirect windows use client pixels
without taking native keyboard focus. Root, support and InputOnly windows are
excluded from native presentation; InputOnly X map requests are still honored.
X borders are forced to zero to keep backing pixmaps and client extents aligned.

Follow-up controls honor an explicit `_MOTIF_WM_HINTS` decorations=0 request,
including a mapped-window change, while missing/malformed hints retain Brook
chrome. Client chrome remains focusable. The bridge forwards pointer-driven
`_NET_WM_MOVERESIZE`, maximization requests and `WM_CHANGE_STATE` iconification
to the existing native window operations. Maximize/minimize state flows back
through X properties. This implements full-window maximization rather than
independent horizontal/vertical modes; fullscreen is not implemented here.

The fixed-message stream has bounded queues, framing for partial/coalesced
reads, and fail-closed generation retirement on EOF, malformed messages or queue
exhaustion. XID incarnations reject stale commands. Transient parent references
carry resolved identities. Native geometry commands carry serials so an older
X acknowledgement cannot undo a newer native drag/resize. Old-size frames wait
for matching extents, and NULL-buffer unmapping clears native incarnation state.

Focus uses input hints and `WM_TAKE_FOCUS` with an asynchronously acquired X
server timestamp. Close uses `WM_DELETE_WINDOW` when supported, otherwise the
ordinary X client-close fallback. Reparenting away from the root retires the
native association; returning creates a fresh identity. Generation retirement
removes owned X windows without disconnecting native Wayland clients.

Native maximize/restore preserves unrelated `_NET_WM_STATE` atoms, and a
minimize-only update does not rewrite that property. Malformed or oversized
properties are left intact. An isolated X server fixture covers these cases;
concurrent direct property writers and fresh Steam/Dell acceptance remain
outside that test.

The XWM now drops synthetic X11 surface-serial events and ignores zero,
duplicate and stale serials without retiring the entire session. Its per-window
high-water mark survives an unmap. The isolated Xvfb tests exercise rejection
with the production helper and lifecycle handling with a test-only helper;
Xvfb cannot generate Xwayland's direct server-originated event. The change
still needs a real Xwayland association/remap run with the packaged version
before being deployed to the Dell.

The first live Steam review exposed a Friends input defect: native presentation
put Friends above the main window, while the X root stack put the main window
above Friends. Native activation now raises the matching X window as well as
focusing it. An overlapping-window control fails on the live baseline helper
and passes on the corrected helper. The same review confirms both Steam windows
explicitly request decorations=0; ignoring that property caused double chrome.
The restarted native O review verifies Friends' close button while it overlaps
Steam, plus Steam's own minimize button and taskbar restore, with no duplicate
chrome. The Q review additionally verifies client maximize/restore and Friends'
client-driven title drag and bottom-right resize: it moves independently from
(1070,432) to (752,234), then resizes from 300×650 to a matching 475×710 native
window and buffer. The maximized main client stays at 1920×1036.

The launcher uses a private `-displayfd` channel and checks the chosen display,
plus a bounded startup deadline. The first native attempt used `-nolock`, which
Xserver ignores for non-root callers. That attempt remains recorded; the
corrected ordinary-user launch succeeds. The new worker is deployed with the
compositor by fresh-image, image-update and private offline-staging scripts.

## Evidence and remaining acceptance

The Nix check phase exercises the actual shell and bridge adapters. It covers
both association orders, protocol errors, retained pixels, independent windows,
geometry ACK ordering, stale-size frames, NULL-buffer remap, focus/close and EOF
isolation. A mapped native xdg toplevel keeps its native identity and retained
pixels through helper EOF and server disconnect. A replacement generation can
reuse XIDs/serials while rejecting delayed messages from the retired generation.
Native review N additionally terminates the owned Xwayland server: its X
windows disappear while the same native Mousepad window retains its identity,
receives focus and accepts newly typed text. Replacement-server acceptance
was subsequently exercised with Steam open as described below; Dell acceptance
remains open.
A fresh headless Xvfb control additionally exercises the real XCB
worker: InputOnly maps, border suppression, serial association, configure,
globally active client focus, cooperative close, reparent and XID reuse. The
stale-XID control also rejected a deliberately broken worker during development.
These controls do not replace native Xwayland or Steam acceptance.

In private review `steam-rootless-xwayland-n-20261024`, real xeyes windows have
exact 160x100 and 240x140 client sizes. Dragging one leaves the other stationary.
Maximize reaches a matching 1916x996 buffer; restore returns to 240x140 at the
moved position. Cropped captures show pupils responding to left/right pointer
positions. Both window buttons close their X clients. Mousepad runs as a native
Wayland client on the same compositor. The corrected compositor/worker were
hash-verified runtime overrides in this first review; repeat fresh-boot acceptance
with the corrected packaged launcher succeeds in review
`steam-client-container-o-20261024`: the service compositor starts its own
Xwayland and worker on display `:0`, without runtime executable overrides.

The same review preserves the authenticated profile and runs Steam without a
loader audit or preload. Store renders at 1280x800; native maximize produces a
1916x996 Library with game artwork. Friends opens at 300x650 and a drag moves it
independently while Library stays maximized. The native Wayland editor remains
on the same compositor. These private captures establish individual-window
presentation and real application rendering; they do not yet establish all
dialogs, successful installation or long-term stability. Friends' own client
close button is verified after the focus/stack correction; the later Q review
adds client-driven movement and resizing controls.

Known gaps and remaining gates:

- Finish Steam dialogs/navigation, input-alignment and a small-download
  acceptance run through the rootless path. Initial authenticated Store,
  Library, Friends and independent movement are verified.
- Validate replacement-server behavior on the Dell and test Steam relaunch
  after a server loss. A private VM test with Steam open is described below.
- Transient identities currently affect initial placement; later parent motion,
  minimize and stacking need broader policy/acceptance. Existing native popup
  raising also groups by provider, which can include unrelated Wayland clients.
- XShape bounding-region clipping is not implemented. Shaped xeyes buffers can
  expose undefined pixels outside the eye shapes; do not treat these captures as
  proof of clean shaped-window rendering.
- MCP semantic close rejects X windows with unknown client-origin metadata.
  Native chrome close dispatch works. Do not invent PID-based signal authority
  from untrusted X properties to bypass this limitation.
- Steam authenticated evidence, profiles and downloaded software remain private.
  The previous SDL3 host crash review disks are preserved independently.

The compositor now retries an owned Xwayland generation after server or worker
loss, with capped exponential backoff. It waits for both retired children to
be reaped, escalating a stuck child after a grace period, and avoids spawning
during shutdown. The host fixture covers a stopped worker and preservation of
an unrelated native surface. In a private software-rendered VM, terminating
the real server and then the replacement worker each produced a new generation;
a fresh xeyes window opened after each. That VM run preceded the final bounded
shutdown-cleanup adjustment, which has host-test coverage. Existing X clients
disconnected as expected. This is not a Steam-load or Dell stability result.

The final compositor build was subsequently exercised in a private,
software-rendered VM with a null-output PulseAudio server. Steam reached its
rootless main window, and compositor maximize and restore actions completed.
Terminating Xwayland while Steam was open left the compositor running; a new
server and worker appeared, and a fresh xeyes window opened through them. The
guest was stopped after a checked filesystem and device flush. Existing X
clients disconnect on server loss; this test does not show Steam automatically
reconnecting, establish long-term Steam stability, or validate the Dell.

## Xwayland interval-timer lead

The serial log of the earlier rootless Steam run reports over 50,000
`ENOSYS` entries for syscall 38 from Xwayland. X server smart scheduling uses
`setitimer(ITIMER_REAL)`; this is a concrete missing interface, but the log
alone does not establish that it caused a freeze or quantify performance.
`scripts/workloads/itimer_real_check.c` provides an externally time-bounded
Linux oracle for shared-thread cancellation, fork, exec, alarm interoperability
and blocked periodic delivery. It passed ten runs on the Linux host, and its
statically linked musl build also passed there. The same static executable in a
disposable two-CPU QEMU/TCG guest failed its first `setitimer` call with
`ENOSYS`; Brook does not yet implement the interface. A correct implementation
must share the process-wide alarm, preserve it across exec (including nonleader
exec), avoid inheriting it on fork, and handle pending signals and periodic
rearming. Ordinary POSIX timers have different semantics and cannot simply be
substituted. VIRTUAL and PROF need separate CPU accounting.

Upstream's [smart-scheduling timer](https://raw.githubusercontent.com/XQuartz/xorg-server/master/os/utils.c)
explains the repeated REAL timer calls. Whether a `-dumbSched` diagnostic A/B
improves this particular workload remains untested. For that comparison, start
the compositor with `BROOK_XWAYLAND_DUMB_SCHED=1`; it passes `-dumbSched` to its
owned Xwayland, while leaving the default invocation unchanged. Xwayland 24.1.14
disables both smart-scheduling timer signals and threaded input for that option,
so any performance change cannot be attributed to the timer alone. Rebuilding
the compositor and its host suite passed. A private software-rendered VM clone
of the Xwayland-capable review was booted once in each mode with the same
compositor binary. In both, a 240x140 xeyes client appeared as an individual
rootless window. The normal run's sampled serial log reached at least 256
unimplemented syscall-38 calls from Xwayland; the diagnostic run showed none
through the xeyes check. The runs were not duration-matched benchmarks, and
this does not establish input correctness, performance or Steam stability.
Steam A/B and hardware acceptance are still required. This diagnostic does not
implement `setitimer`.

An initial Steam attempt in that private clone was invalid because it omitted
the earlier review's validated provider-loader payload. With the current
kernel, matching modules and the original payload restored, SteamRT progressed
and launched approximately 90 tasks, but no Steam window appeared. The private
runtime log reported repeated `Too many open files` errors. A later run on the
same private configuration showed Steam's shared descriptor table at the
kernel's 1,024-entry capacity: `/proc/89/fd` contained 945 `memfd` descriptors,
39 paths and 40 other descriptors. Several Steam threads exposed the same full
table. The log also reported repeated software Vulkan/EGL initialization
failures; this host has no available render node, so a failure/retry loop is a
plausible lead, not an established cause. Descriptor names and allocation stacks
were not captured. No matching kernel FD-exhaustion diagnostic was observed;
several descriptor installation paths return EMFILE without that log. The VM
was synced and stopped. This is not a matched Steam A/B or Dell result.

An opt-in, globally bounded `opt/memfd-trace=1` diagnostic then identified the
repeated allocations as Steam-created `pulseaudio` memfds when no Pulse server
was configured. With a private null-output Pulse server and its cookie passed
into SteamRT, the runaway stopped and a rootless Steam sign-in window appeared.
That establishes a provisioning problem in this clone, not the cause of the
Dell freeze or evidence that scheduler mode improved stability. The trace can
include application-provided names and is disabled by default. A two-CPU VM
exercise checked invalid and kernel-space names, enabled/disabled behavior,
and the 159-line lifetime cap after 4,100 creations.

The same private clone was subsequently booted with Xwayland's default scheduler
and a fresh null-output Pulse server. Steam reached a rootless main window;
one Steam process held 119 descriptors, including two memfds, at inspection.
Xwayland's sampled unimplemented syscall-38 count reached at least 14,848,
while the diagnostic-scheduler run logged none. The sequential runs reused a
writable clone and differed in persisted runtime state, so neither timing nor
window differences establish a scheduler performance effect. Neither run
establishes long-term Steam stability or reproduces the Dell freeze.

The fixture also checks caught and default `SIGALRM` delivery to a child
spinning without syscalls; these checks pass on Linux. Brook's current caught
signal delivery occurs at syscall return, so the timer must remain disabled
until IRQ return can deliver it too. An isolated `RealIntervalTimer` state
machine models pending, periodic phase and reprogramming; it is not connected
to the syscall or production alarm. Group identity, locking, signal disposition,
lifecycle and IRQ integration remain.

An independent `scripts/workloads/signal_irq_delivery.c` isolates caught
signal delivery from timers: a masked sibling sends `tgkill` after the target
enters a userspace-only loop. It passes on Linux and fails in a disposable
two-CPU Brook QEMU/TCG guest, confirming that the return-path gap is real.
The IRQ register-layout adapter is not connected to the wrappers yet. Its
host tests check the distinct syscall and IRQ register mappings; the existing
11-case guest signal-return suite passed after extracting the syscall mapping.

A QEMU-only `opt/irq-signal-probe=1` experiment now tries caught-signal
delivery at LAPIC timer and reschedule-IPI return. On a disposable two-CPU TCG
guest, the no-syscall-loop fixture passed with the option and failed on the same
kernel without it. The existing 11-case signal-return guest fixture also passed
with the option enabled. Caught-only claims now select the handler while holding
the source notification lock; hosted contention tests cover both ordinary and
POSIX timer sources. The IRQ fixture passed again with that change. This is not
production acceptance: default dispositions remain on their existing path.
The static `signal_irq_registers.c` fixture passed on Linux and an opt-in
two-CPU QEMU/TCG guest: it checks RAX/RCX/R11, carry, a red-zone sentinel,
YMM15 round-trip, normal and alternate stacks, and an edited return RIP. This
does not inspect the exported FP image, cover all registers, or prove which IRQ
wrapper delivered the signal. A separate static fixture passed on Linux and
opt-in QEMU for private alternate-stack write isolation across fork and an
unwritable alternate stack; the guest logged the IRQ frame copy failure, the
child died with SIGSEGV and the parent survived. The userspace isolation check
alone does not distinguish COW from eager copying. In a separate two-CPU TCG
boot, `opt/irq-signal-probe=2` disabled timer-return delivery; a privileged
test-kernel-only control requested reschedule IPIs while the register fixture
ran. Two target-PID `source=ipi` records and the passing fixture establish
both stack cases went through the reschedule wrapper. They do not prove which
individual IPI caused each delivery or establish physical SMP behavior.
The compiled entry audit covers both hook arguments, source and target, with
mutation checks; it remains a structural check. Concurrent termination and
broader SMP coverage remain. The option defaults off and has not been deployed
to Dell.
