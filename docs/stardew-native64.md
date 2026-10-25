# Stardew Valley native64 startup — 24 October 2026

The default native64 Linux depot now reaches an interactive title/menu through
Steam. Load shows an empty isolated test home, Back returns to the title, and
the in-game Exit button removes the game and its runtime helpers. This is a
startup/menu result, not full gameplay or audible-output acceptance. No existing
save was loaded. Game files and account evidence stay in ignored private reviews.

## 25 October: container source FD reporting

The live Steam launch ends with wrapper status 1 after roughly four minutes of
Soldier runtime preparation. The fatal message is bubblewrap's inability to
open a graphics-library bind source (`EOPNOTSUPP`). Earlier failures to execute
the i386 capsule helper produce warnings and are not sufficient to explain this
exit. Hard-link `EPERM` copying fallbacks explain a concrete preparation cost;
that optimization remains separate from this compatibility fix.

The [upstream safe-open fallback](https://github.com/containers/bubblewrap/blob/main/safe_openat.c)
opens the source, then reads its proc-fd link to verify containment beneath
`/oldroot`. On the unchanged Brook kernel, both ordinary and `O_PATH` opens of
the exact library succeed, while both FD readlinks fail with error 95. The
parent directory reports correctly. Ext2 lacked non-directory object-path
reporting; successful stat/open outside the container missed this later check.

The candidate records each retained file entry's pinned parent and leaf,
separately from the shared inode. Reporting reconstructs the parent's current
path and validates the entry, without a cached absolute label or reverse inode
search. Rename and unlink update exact entries, including hard-link aliases.
Namespace changes update reporting even when a later metadata write returns
`EIO`. Incomplete rename rollback permanently invalidates uncertain records.
Deleted leaves retain a deleted marker; deleted ancestors and inode-only legacy
conversions remain explicitly unreportable. Bubblewrap's containment check is
unchanged.

All 109 host tests pass, including production ext2 callbacks, concurrent
report/rename/reference retirement, allocation unwind, post-commit metadata
failure and both destination/source rollback failures. The public file-bind
probe adds ordinary/unbound `O_PATH` FD readback and a real bind/proc/pivot-root
`/oldroot` check. Linux passes (204 fixture checks, 5 parent checks); the candidate
passes on ext2 (190 fixture checks, 5 parent checks). The exact library's two FD
readbacks also pass natively. Release entry, scheduler-start and lock/wait
mutation audits pass.

Candidate kernel SHA-256:
`9381109685423a1ebaf85cb97ba829aac23c48d179f79acf4178b6b1b6f933c3`.
All 13 driver modules remain matched at ABI 53. Native evidence is private in
`build/reviews/stardew-fd-report-20261025/`. Through-Steam game acceptance is
still pending; the passing file probe alone does not establish it.

### Next container gate: mkdir on existing read-only parents

The actual retry gets beyond the former FD-reporting error and then fails in
bubblewrap's destination-parent preparation with `EROFS`. A focused Linux/Brook
comparison confirms an independent error-ordering bug: `mkdir` on an existing
directory in a read-only bind returns `EEXIST` on Linux but `EROFS` on Brook.
This prevents normal mkdir-p preparation of already-present `/nix/store` parents.

The candidate looks up the final entry before requiring writable admission for
`mkdir`. Existing entries return `EEXIST`; missing entries still encounter
`EROFS` before backend creation. Other mutation operations retain their previous
ordering. Parent search permission checks and mountpoint handling remain in the
same namespace transaction.

All 109 host tests and compiled entry/scheduler audits pass. The expanded
read-only-bind probe passes the Linux tmpfs control (62 fixture checks, 8 parent
checks). The separate Linux disk/mapping control encounters an inherited host
mount restriction; it is not a passing full Linux run. The fixed Brook candidate
passes both tmpfs and ext2 controls (124 fixture checks, 8 parent checks), then
passes the 190-check file-bind/pivot fixture again. The unchanged candidate
before this fix fails the first existing-directory assertion with error 30.

Kernel `37f3215728af26e17eae2dd8a8d69fd9857c11fe844a73b25943674aa45d2c4d`
and matched ABI-53 modules are in the private `stardew-mkdir-20261025` review.
The preceding FD-reporting review exits Steam normally with no remaining
client/runtime/private-Pulse helpers, acknowledges checked sync, and passes all
four offline filesystem checks. Its 33 original save-directory files retain
identical content. Only `steam_autocloud.vdf` has a refreshed mtime; all other
save-directory timestamps are unchanged. No original saved game is loaded.

### Next container gate: exact file-descriptor bind operands

The following Steam attempt gets beyond both earlier failures and reaches the
bind mount itself, which returns `ENOENT`. A public FD-to-FD mount succeeds on
Linux but fails at its first mount on unchanged Brook. Final proc magic-link
following retains the exact file object but intentionally has no lexical
parent/leaf; the file-bind preparation previously assumed those were present.

An optional backend callback now supplies an independently retained parent and
leaf for a live exact entry. Ext2 validates its existing pinned entry under the
filesystem lock and rejects detached or uncertain records. Tmpfs supplies its
existing live node parent/name under the filesystem mutex. VFS retains the
original attachment; it never reopens a printed pathname. File-mount roots use
their covered parent/name, and older target FDs follow the effective covering
mount before metadata recovery. Existing type/link-count/namespace/mutation
restrictions remain enforced. This extends module ABI to 54, requiring all 13
drivers to be rebuilt and staged with the kernel.

Host tests cover renamed entries, allocation unwind, magic-link type mismatch,
stacking through an old covered FD, detached mounts and unlink/recreate. The
production Ext2 fixture checks exact parent ownership alongside existing
rename, overwrite, unlink and hard-link reporting cases. All 109 host tests and
compiled entry/scheduler mutation audits pass. Expanded Linux native controls
pass, including a source FD beneath a later overmount and FD-to-FD bind after
pivot. The first native candidate passed Ext2 rename/stack/FD binds and exposed
the missing tmpfs counterpart at the pivot destination. The final Brook candidate passes 228 fixture checks plus 5 parent checks,
including the tmpfs pivot destination. The read-only suite still passes all
124 fixture checks plus 8 parent checks. Actual Steam retry is pending.

Kernel SHA-256:
`bad7d5727931bb5116c29fc3fe70188db5721fb7789c109d496de553021e154a`,
with all 13 matched ABI-54 modules in private review
`build/reviews/stardew-proc-bind-tmpfs-20261025/`.

The preceding mkdir review exits Steam normally, completes checked sync and
passes all four offline filesystem checks. Original save contents remain
unchanged, with only the Steam cloud marker timestamp refreshed.

### FAT initrd bind sources

The actual ABI-54 retry passes the earlier library bind failures, then exits
with `EOPNOTSUPP` binding `/etc/machine-id`. That source is a real FAT initrd
file. FAT already pins retained file directory-entry coordinates against
rename/unlink/reuse, but did not expose the new entry-parent callback.

The counterpart factors the existing exact sector/offset enumeration from
file-path reporting and returns the interned parent object under the FAT mutex.
It does not reopen a printed pathname or select files by content cluster. Host
coverage includes long and empty file names, renamed parents and short output
buffers. All 109 host tests and compiled entry/scheduler mutation audits pass.
The public native probe now binds the real machine-id FD across pivot into a
tmpfs target. The Linux control opens this FD before pivot because its machine-id
is an absolute host symlink; this keeps the control about the retained object.
Native Brook passes both complete suites: 228 file-bind fixture checks plus
5 parent checks (including the separate pivot child), and 124 read-only fixture
checks plus 8 parent checks. The next actual Steam retry remains pending.
Kernel `b52588deb9d34fc03e9b430cf8ee772ab76b5742844c98805d8f19ad0e1a6fca`
and 13 matched ABI-54 drivers are preserved in private review
`build/reviews/stardew-proc-bind-fat-20261025/`.

The preceding review's game launch reaches the next bind gate after about
258 host seconds without a game window. Steam exits normally, all helpers
drain, checked sync succeeds and all four offline filesystem checks pass.
Original saved-game contents and non-cloud-marker timestamps are unchanged.

### Pathname socket FD following

The FAT-fixed Steam retry advances to binding the Wayland socket, then returns
`EOPNOTSUPP` for `/tmp/wayland-0`. This is a real Ext2 pathname socket with one
link and working FD readback. The existing socket-bind probe passes textual
operands but, when changed to proc-FD operands with recursive bind, reproduces
this failure. Linux passes the descriptor variant.

Proc's exact vnode-FD follow gate allowed only directories and regular files.
It now also permits retained filesystem socket nodes. Existing FD/process
identity and access checks remain; transport sockets retain their separate
metadata policy, and devices, textual symlinks and FIFOs are not broadened.
Normal open still reaches the filesystem socket's `ENXIO`; O_PATH/stat/bind
can retain its real inode. The probe checks those boundaries and performs real
bidirectional socket exchange through the bind.

All 109 host tests and compiled entry/scheduler mutation audits pass. Linux
passes 176 fixture checks plus 5 parent checks. Brook passes the same 176 plus
5 checks, then the prior file-bind (228 plus 5) and read-only (124 plus 8) suites.
Kernel `a44a3823f14b0631682f2902c23cc97d5f7c0125d8c7840b3ae36bd9ef1382c8`
with all 13 ABI-54 drivers is retained in private
`build/reviews/stardew-proc-socket-20261025/`. Actual Steam retry is pending. The preceding review reaches its next bind gate after about
250 host seconds, drains Steam normally, completes checked sync and passes all
four offline filesystem checks. Original save contents remain unchanged.

### Provider loader cache location

With pathname socket following fixed, SteamRT reaches `pv-adverb` and command
loading. Its `/bin/bash` then cannot load `libGL.so.1`. A static control inside
the normal mutable Soldier setup establishes that the GL files are mounted,
the regenerated cache is valid and contains their x86-64 SONAMEs, and plain
bash loads. The 64-bit Steam overlay fails until the override directory is
prepended. Read/mmap controls agree on the preserved cache bytes.

The provider loader copied into initrd is Nix glibc 2.42. It is compiled to read
its store-prefix cache instead of `/etc/ld.so.cache`. Runtime logs confirm that
SteamRT replaces the container interpreter with this `/lib64` provider copy.
A same-container A/B records the original loader searching its Nix cache and
failing; a separately adapted loader searches `/etc/ld.so.cache` and loads the
same overlay/bash without the override prepend. The two normal mutable controls
take about 154 and 151 host seconds; direct-wrap and no-copy controls encounter
different setup paths and are not equivalent game controls.

Provider provisioning now relocates only the unique standalone cache-path
literal in the copied loader's read-only, non-executable ELF load data. It
preserves file size, layout, executable bytes, the separate help-text occurrence
and the build ID. It records original/output SHA-256, offset, span and old/new
paths in the manifest. The installed Nix package remains unchanged. Five tests
cover exact byte preservation and rejection of missing/duplicate literals,
malformed headers, writable/code data and partial executable-segment overlap.
The native explicit-loader A/B passes. Its unchanged help text/build ID must
not be used to identify this modified compatibility artifact; use the manifest
hash. The compatibility output SHA-256 is
`2d544fecc1c8c402834451c37c61d74a193241e3e7968c327940e5e60e57d96a`.

The fresh initrd contains the verified adapted-loader hash. All three native
regression probes pass again: read-only binds (124 plus 8 checks), file binds
(228 plus 5) and pathname socket binds (176 plus 5). The real Steam launch now
passes command loading and reaches game initialization after roughly 290 host
seconds of runtime preparation. It then exits before mapping a game window:
OpenAL cannot connect to PipeWire and the fallback ALSA file backend aborts on
an audio-write assertion after `Bad file descriptor`. This is not yet title/menu
acceptance. Kernel and all 13 ABI-54 drivers remain the previously checked
`a44a3823...` build. The preceding
review drains Steam and all diagnostic helpers normally, completes checked sync
and passes all four offline filesystem checks. Original save contents and
non-cloud-marker timestamps remain unchanged.

### Companion audio environment

A probe through the real `steam-runtime-launch-client --alongside-steam`
service confirms that its environment has neither `PULSE_SERVER` nor
`PULSE_COOKIE`. The public launcher previously supplied these only to the inner
Steam command, after creating the companion service. It now supplies the explicit
SteamRT-visible endpoint and cookie path to both branches. No cookie contents
are exported. The new companion reports both settings, and a real `pactl info`
request through it succeeds against PulseAudio 17.0. This diagnostic server uses
a null sink; its successful connection does not establish speaker output.

### Through-Steam title/menu acceptance

With that environment fix, the actual AppID launch reaches a rendered game
window after about 310 host seconds. Runtime preparation occupies roughly the
first 290 seconds. The smaller initial window clips the menu; maximizing makes
the full title and its buttons visible. Absolute tablet input does not move the
captured game pointer. Switching the active virtual mouse to PS/2 and injecting
relative motion/button pairs opens **No Saved Games Found**, returns with Back,
and activates Exit. No overlay-disable workaround or game-library modification
is needed. Smaller-window geometry and cooperative absolute/relative input
remain follow-up work.

After Exit, the game window, reaper and game-runtime helpers disappear and Steam
removes the AppID from its running list. The outer game launch reports status 1
and the game shell reports its child as `Killed`; this is not a verified zero
exit. The earlier direct launch's scoped trace showed the game killing itself
with SIGKILL, but that cause was not independently retraced in this run. Steam
API initialization/IPC warnings also remain in the private logs. Broader Steam
integration and gameplay remain unaccepted.

Steam's normal shutdown completes with its tracked launcher job exiting 0 and
no remaining client or private-Pulse helpers. All 33 original save files retain
identical content; non-cloud-marker timestamps are unchanged. Checked guest
metadata/device sync succeeds. The original host exec controller ended with
143 before recording its child's terminal wait, so that wrapper status remains
unavailable and its receipt is preserved. Independent exact-QEMU identity and
open-disk checks establish that the images are offline. All four read-only
filesystem checks (data, nix, tmp, boot) return 0. Private evidence is retained
in `build/reviews/stardew-loader-cache-20261025/`.

### Sequential startup profiles

A fresh live-review launch maps its window after 333.5 host seconds. A subsequent
launch with sequential captures maps it after 348.7 seconds, about 4.6% longer.
Snapshots every roughly 20 seconds put pressure-vessel setup through 300 seconds,
graphics capture around 300 seconds, container command loading around 320 seconds
and the game process/window in the final 20–30 seconds. These are observation
bounds, not exact exclusive phase timings.

Eighteen capture windows cover setup and game initialization. The initial busy
four-second capture drops 3,019 events; the two-second transition capture drops
163. Both are excluded from aggregate CPU shares. The other 16 windows contain
24,260 CPU samples with no producer drops or event-count mismatch. About 76.6%
of these samples are idle across the eight CPUs. One pressure-vessel thread
occupies most of a core during preparation. Timer sampling underrepresents
interrupt-masked time, and file export adds observer overhead.

QEMU counters on `data.img` increase by 4,171,635,200 read bytes and
2,031,413,760 written bytes: about 3.89 GiB read and 1.89 GiB written over
1,698,884 read and 1,173,669 write requests. Average request sizes are only about
2.4 KiB and 1.7 KiB. Profile output is on the separate boot image and contributes
about 35 MiB of writes there. Host block-service times can overlap and must not
be presented as an additive wall-time partition.

Prominent preparation stacks include retained path walking, `Ext2HoldInode`,
file writes and the synchronous virtio block submission/completion loop. Module
addresses are resolved using the recorded module bases and the retained ELF
section layout. `Ext2HoldInode` allocates a candidate before searching a global
linked list, even on a hit. This provides an indexing/allocation target alongside
hard-link support; it does not establish an exclusive percentage of wall time.
Both `link` and `linkat` currently return `EPERM` unconditionally. Avoiding the
runtime-copy fallback is therefore a concrete next experiment, followed by the
same launch/counter controls. No kernel or game binaries changed for this run.

Private raw/Speedscope captures, process snapshots, disk counters and aggregate
JSON are under `live-user-20261025/startup-profile/` in the review directory.
The game is left open and maximized after capture. Its private PulseAudio null
sink remains silent. A reversible attempt to load `module-oss` for speaker output
fails at `FIONBIO` with `ENOTTY`, before changing the existing stream's sink.

## Memory barriers

The original direct startup reports CoreCLR HRESULT 0x8007FF02. A PID-scoped
trace shows membarrier QUERY returning ENOSYS, followed by one-page mlock
returning ENOSYS and process exit. This matches the initialization choice in
[.NET's GC platform code](https://raw.githubusercontent.com/dotnet/runtime/v6.0.36/src/coreclr/gc/unix/gcenv.unix.cpp):
use registered private expedited barriers when available, otherwise lock a
helper page for the protection-change fallback. This source comparison does
not identify the shipped binary's exact source revision.

The candidate implements only QUERY, REGISTER_PRIVATE_EXPEDITED and
PRIVATE_EXPEDITED. Registration belongs to the address space: threads sharing
CLONE_VM share it, fork copies it and fresh exec clears it. Other commands
and nonzero flags return EINVAL; use before registration returns EPERM.
Full mlock semantics remain unimplemented.

The first implementation broadcasts a separate memory-barrier epoch to every
online CPU, a stronger scope than the process-private contract. It shares the
existing TLB interrupt delivery and timer service points, but neither the TLB
request slot/lock nor the PMM quarantine epoch. The caller fences before
publication and after acknowledgements. Each CPU fences before publishing its
acknowledgement, with interrupts disabled across CPU identification and ACK
publication. Concurrent epochs coalesce; epoch wrap is rejected. The caller
waits with interrupts enabled and no request/address-space lock. Missing ACKs
cause failure after a bounded wait, never off-CR3 forgiveness or false success.

An earlier, untested draft sharing the TLB request lock was discarded: a CPU
holding that lock while waiting for all CPUs can form a transitive cycle with
other IRQ locks. The independent epoch removes that lock dependency.
Narrowing targets later would require explicit scheduler/migration ordering.
See [Linux's ordering scenarios](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/sched/membarrier.c).

## Validation so far

- Release kernel builds. Supported compiled entry, scheduler-start and
  scheduler lock/wait audits pass with mutation checks. The entry audit admits
  scalar MFENCE in the GPR-only TLB leaf; vector instructions, calls and
  external branches remain rejected. Its external-branch mutation now targets
  beyond the actual function size rather than assuming a rel8 displacement
  cannot land inside a larger function.
- `tools/at-syscall-probe/membarriercheck.c` passes on Linux and the eight-vCPU
  Brook T review: 15 checks, including QUERY, invalid flags/combined commands,
  EPERM before registration, idempotent registration, inherited fork state,
  exec reset, shared-thread use and 10,000 StoreLoad litmus rounds with the
  fast side using only a compiler barrier. No forbidden outcome occurred.
- A strengthened probe also passes on both systems, with two concurrent
  issuers explicitly alternating CPU affinity while a third worker performs
  mmap/mprotect/munmap. These are empirical ordering/stability controls, not
  exhaustive proof or performance acceptance.

The T review uses kernel capsule v17, 12 GiB RAM, eight vCPUs, MLFQ and the
existing Intel hardware graphics configuration. The S staging attempt stopped
before VM launch because its manifest incorrectly included compiler temporary
objects; the corrected capsule inventories only kernel, symbols and 13 modules.
No running VM was overwritten or rebuilt.

## Next startup failure

With the candidate, the traced direct game apphost observes QUERY=24 and
successful registration, then creates runtime threads. The earlier CoreCLR
initialization error is absent. Two launches instead end with a null-adjacent
SIGSEGV in dynamically mapped executable code, at the same page-relative
offset. After Brook delivers that fault to .NET's handler, a user-mode #GP
occurs inside a different mapped code region (CS 0x2b, error zero). Signal
context handling is the next diagnostic target; the current Linux-compatible
signal frame explicitly supplies a null fpstate pointer. Its role in the
handler fault has not yet been established. No game window is reached.
A scoped trace also finds getcpu ENOSYS;
that is a compatibility lead, not an established cause of the fault.

The original Steam route independently remains blocked by its i386 reaper and
the later game-container bind-source failure. A previously tested private
64-bit reaper was restored to stock after diagnosis; no replacement is shipped.
Successful direct startup alone will not complete Steam launch acceptance.

## Signal FP-state baseline

Local inspection of the public CoreCLR component identifies the nested #GP at
module-relative `0x519b2f`: `fxrstor [rdi+0x100]`. A breakpoint at an earlier
valid invocation is not sufficient evidence; its MXCSR is legal. A subsequent
capture at the actual `ExceptionStub13` entry uses the selected CPU's CR3 to
read just the failing operand's alignment/control metadata. The operand is
16-byte aligned, but MXCSR is 406284485, with reserved bits set. The failing
instruction and invalid control word explain the #GP. The debugger's first
virtual-memory read failed; the physical page-table walk corrected that
observation problem without changing the guest.

Brook's synchronous and syscall-return signal delivery both zero the signal
frame, leaving `mc.fpstate` null; sigreturn restores only GPRs. The public
`tools/at-syscall-probe/signalfpcheck.c` passes eight Linux cases: XMM/YMM
export, handler clobber followed by restoration, handler-edited saved state,
asynchronous versus synchronous delivery, and normal versus alternate stack.
On the unchanged T candidate, the first case fails both valid-image and
restoration checks. This independently reproduces the ABI gap. It does not
yet prove that repairing it is sufficient for the game to start.

The next implementation must use the interrupted XSAVE slots already owned by
the exception/syscall entry wrappers, not a potentially stale process save or
a C-level save after kernel SSE use. Pass the slot explicitly after XSAVE,
which clobbers EDX. Build a zeroed, standard Linux signal image from defined
x87/SSE/AVX components, normalizing absent components and reserved bytes;
never expose all 1088 kernel-slot bytes. Supply aligned FP storage and Linux
XSAVE metadata. On sigreturn, validate copied user state (including MXCSR,
header, bounds and trailer) before replacing the wrapper's final XRSTOR image.
Support legacy and null reset deliberately, and reject malformed images
without a kernel fault. Both delivery paths and their compiled entry audits
need coverage. The current nested-handler bookkeeping is a separate limit.

## Signal FP-state candidate and native acceptance

The U candidate implements the save/restore contract above in both delivery
paths. The third assembly-to-C argument is set after XSAVE clobbers EDX.
Export uses a zeroed, field-wise image; init x87/SSE components are materialized,
AVX presence follows the saved header, and no reserved kernel-slot bytes are
published. XCR0 and CPUID verify the supported standard x87/SSE/AVX layout;
the MXCSR mask comes from hardware, never from a user-supplied mask field.
Handlers begin with initial live FP state while ucontext retains interrupted
state. Sigreturn validates a copied image before replacing the authoritative
entry slot restored by the assembly wrapper.

Discovery and validation are separate. Missing magic or invalid discovery
sizes select legacy state; a readable missing trailer also selects legacy,
matching the Linux control. An unreadable trailer is an error. Recognized
extended state still rejects invalid MXCSR, unsupported components and
compacted/reserved headers. Null state resets x87/SSE/AVX; legacy state restores
x87/XMM and initializes upper YMM halves.

Validation on Linux and the eight-vCPU U review:

- Eight `signalfpcheck` cases verify nondefault x87 control and ST0, MXCSR,
  XMM/YMM state, handler edits and live clobber, default handler controls,
  synchronous versus syscall delivery, and normal versus alternate stacks.
- Ten `signalfpreturncheck` cases verify legacy/null restoration, discovery
  fallback, invalid MXCSR/XCOMP/XSTATE/pointer rejection, unreadable trailer
  rejection, and invalid MXCSR combined with legacy fallback. Invalid contexts
  terminate the child with SIGSEGV without taking down the VM.
- Focused host normalization/privacy tests pass, including poisoned init-state
  storage, x87 padding, unused tail, reserved headers and malformed images.
  The release build and supported entry/scheduler mutation audits pass. New
  entry checks reject missing/wrong FP-slot arguments in full exception stubs
  and the syscall signal-check call site; the broader dispatcher remains an
  unsupported full-return proof, as before.

The actual game still faults at CoreCLR `FXRSTOR`. Its failing context now has
capture flags `0x0010000f`; the restore caller is module offset `0x4ddac2`,
identifying the alternate-stack returnPoint after the signal worker, rather
than the converted exception context. A breakpoint after that returnPoint's
capture observes valid MXCSR `0x1f80` and mask `0xffff`. At the later restore,
the mask is zero and MXCSR invalid. Compare context identity and track the
intervening write next. A first bounded raw write-watch attempt did not yield
a writer; its packet acceptance was not verified, and its timeout-side read
used an idle CPU's CR3, so it is not evidence that no write occurred.
No title/menu or game-launch success is claimed.

## Alternate-stack lifecycle defect

The verified breakpoint/watch comparison uses the same private returnPoint
address at capture and restore: its control fields change while CoreCLR's
signal worker runs. A verified raw hardware-watch packet was accepted, but no
writer stop occurred before the restore fault, so this is not evidence against
an overwrite.

Source inspection identifies a concrete prerequisite failure: fresh Brook
processes returned `ss_flags=0` with no configured alternate stack. CoreCLR's
`EnsureSignalAlternateStack` allocates only when its query returns exactly
`SS_DISABLE`; Brook's response skips that allocation. CoreCLR can then treat
the handler as already on an alternate stack and switch its worker onto the
interrupted stack, overlapping the still-live returnPoint. This overlap is a
source-supported explanation, pending the repaired game retry.

The static `sigaltstackcheck` passes Linux and fails unchanged Brook at the
fresh-query/CoreCLR precondition. The candidate canonicalizes fresh disabled
state, resets configuration for new shared-address-space threads and committed
exec, preserves fork inheritance, and determines `SS_ONSTACK` from the saved
user RSP rather than the generic handler flag. Input is safely copied before
output, including aliased structures. Both signal paths save configured stack
flags in ucontext and select alternate-stack entry from the interrupted RSP.
SS_AUTODISARM remains unsupported; configurations exceeding the existing
32-bit size field fail instead of silently truncating. Sigreturn restoration
of user-edited uc_stack and nested-handler bookkeeping remain separate gaps.

The eight-vCPU V review (kernel capsule v19) passes the full alternate-stack
probe plus all eight FP-state and ten FP-return cases. Supported compiled
entry, scheduler-start and scheduler-lock/wait mutation audits pass. The U
source review completed orderly shutdown and all four offline filesystem
checks passed before V was staged. Game startup acceptance remains separate.

The first V game retry progresses past the prior CoreCLR FXRSTOR #GP. It
handles the initial generated-code null fault and resumes to a later instruction
in that generated function, where another null fault occurs. Brook then refuses
handler delivery because its blanket `inSignalHandler` flag remains set; no
rt_sigreturn occurred between the faults. CoreCLR's direct context restoration
can bypass sigreturn, making that flag unsuitable for delivery eligibility.
Signal-mask-based delivery and nested/nonlocal-return tests are the next step.
No game window has been reached.

## Nonlocal return and nested signal acceptance

A five-case static Linux control reproduces the next fault independently:
unchanged V dies with SIGSEGV on the second synchronous recovery through
`sigsetjmp`/`siglongjmp`. W (kernel capsule v20) passes it, the alternate-stack
lifecycle probe, all eight FP-state cases and all ten FP-return cases. Focused
host user-copy/frame-bound and FP-normalization tests pass, as do supported
compiled entry and scheduler mutation audits.

Delivery now follows per-signal masks instead of a persistent handler-active
boolean; nonlocal user context restoration can bypass sigreturn without
stranding kernel delivery state. The boolean prerequisite on sigreturn is
removed while copied frame/return/FP validation remains. Both delivery paths
honor SA_NODEFER without discarding explicit sa_mask bits. A blocked or ignored
synchronous hardware fault remains fatal. Frames allocated while already on
an alternate stack must stay wholly inside that configured stack, including
the FP reservation; adjacent mapped pages do not extend it.

The control covers two sequential nonlocal SIGSEGV recoveries, nested
SIGUSR1/SIGUSR2 on the alternate stack with independent MXCSR and mask
restoration, synchronous same-signal SA_NODEFER nesting, a fatal blocked
hardware fault, and alternate-stack exhaustion. This is focused empirical
coverage, not complete Linux signal conformance. Deprecated handler fields
remain diagnostic only, preserving the current process/module ABI.

The W game retry gets past both generated-code null faults. It now reaches a
managed MonoGame SDL initializer and aborts with a missing macOS
`/usr/lib/libSystem.dylib`, rather than a corrupted signal return.
[MonoGame's platform detector](https://github.com/MonoGame/MonoGame/blob/v3.8.1/MonoGame.Framework/Platform/Utilities/CurrentPlatform.cs)
defaults Unix to macOS unless uname returns exactly `Linux`. A scoped game
trace confirms both uname calls succeed. Brook currently supplies `Brook` as
sysname, so the Linux ABI personality must identify as `Linux`; the candidate
retains Brook branding in the uname version and existing OS metadata. No game
assembly or library was modified.

X (capsule v21) verifies `sysname=Linux`, `version=#1 Brook`, `machine=x86_64`.
The unmodified game advances through SDL initialization to graphics-device
creation; the macOS library failure is absent. Release build and compiled entry/
scheduler audits pass. The authenticated profile contains existing save files,
so subsequent game tests use a separate empty HOME/XDG tree. A private hash and
metadata inventory of the original saves is retained for before/after checks;
no existing save is loaded. The direct diagnostic must also match the installed
Mesa GLX vendor path already included by the Steam launcher.

With the installed Mesa GLX vendor directory in its loader path, X reaches
Stardew's title menu and a mouse click opens the empty acceptance home's
`No Saved Games Found` screen. A separate SDL/OpenGL control creates a 640x360
window and reports matching logical/drawable sizes and initial viewport with
Mesa 4.6 compatibility rendering. The game initially draws a desktop-sized
menu into a smaller window; maximizing improves visibility but the lower edge
remains clipped. Escape navigation was not observed. These presentation/input
limits remain open, rather than being counted as full gameplay acceptance.
The native title-bar close ends the game with status137, not clean exit; an
in-game Exit-button test is still required. Original-save content hashes and
timestamps remain unchanged for all 33 files after the first title trial.
No through-Steam game-launch success is claimed.

The Exit-menu trial also ends with signal9. A scoped process trace attributes
this to the game itself calling `kill(getpid(), SIGKILL)`; the synthetic control
for nonleader `exit_group(37)` exits37 on both Linux and Brook. Thus signal9
is not, by itself, evidence that Brook forcibly closed a normally exiting game.
The game-initiated exit drains the tracked group completely. Full startup
strace heavily delays this workload; reducing it to process calls allows it to
reach its window again. Original-save hashes and timestamps remain identical
after all X trials.

## Native64 Steam reaper prerequisite (candidate)

The public 64-bit reaper needs real PR_SET/GET_CHILD_SUBREAPER semantics, rather
than the prior success stub. The candidate stores settings per group incarnation
under the scheduler registry lock: threads share, fork starts clear, exec
preserves. Final group death adopts live and zombie child groups into the nearest
live ancestor subreaper, with existing SIGCHLD/waitability/robust-exit gates.
The reaper fallback must not bypass adoption. User copies occur outside locks.

Review identified three important cases: exec's thread-birth latch does not mean
the surviving subreaper is dead; ancestry must come from the canonical retained
leader instead of an arbitrary copied thread; thread publication refreshes its
parent after concurrent reparenting. Nonleader exec currently detaches in Brook
rather than performing Linux de_thread/PID preservation. The bounded candidate
keeps children and the setting with any surviving old-group peers, copies the
setting to the detached exec process, and transfers ownership only when that
process is the final live member. This prevents a new ownership regression but
does not claim to repair Brook's pre-existing nonleader-exec ABI difference.

The static `subreapercheck` passes Linux: actual GET output, EFAULT, nonzero SET
and ignored unused arguments, sibling sharing, fork-clear/exec-preserve, twelve
running/zombie adoption rounds with exit status, nearest/cleared ancestor and
SIGCHLD. Unchanged X fails the initial GET-write check, independently confirming
the stub. Production-code host fixtures cover exec-latch eligibility, stale
member exclusion, PID reuse, live-old-group retention, final-member transfer
and ancestry refresh at publication. Native candidate acceptance is pending.

Y (capsule v22) passes the complete native subreaper control, including running
and zombie adoption while the original leader is dead and a sibling survives.
The actual unmodified public 64-bit reaper also passes two pipe-controlled
orphan tests on Linux and Brook: it remains alive while an adopted grandchild
is blocked, then propagates grandchild success or failure after release. All
nested/alternate-stack and 18 FP-state/return cases pass again on Y. All 108
host tests pass with assertions active in Release, along with compiled-entry
and scheduler mutation audits. General SIGCHLD is observed; the public probe
does not independently isolate the already-zombie adoption notification from
other coalescing SIGCHLD events.

The fresh Release build exposed disabled assertions in host fixtures, leading
to unused-variable errors and potentially removed fixture operations. Host
tests now centrally undefine NDEBUG so their assertions run in every build
type. This corrects test execution, not production-kernel assertion policy.

## Reproducible helper and Steam launch gate

`tools/reaper-pkg` packages the unchanged MIT-licensed upstream implementation
as a static native64 executable. Its Nix build runs the two orphan lifetime and
exit-status controls on Linux. The resulting package binary also passes both
controls on Y, with its digest verified before execution and the tracked job
fully drained. This is separate from the earlier manually built helper check.

`steam_client_check.py --launch-app 413150` queues the installed game through
Steam after startup. The current private acceptance profile redirects only this
game's HOME and XDG directories to an empty test home; existing saves and the
original Steam configuration are backed up privately. No account or game assets
are included in the repository.

Steam receives the launch request but displays a confirmation that launching
here would disconnect another computer's active game session. That confirmation
has been left untouched pending the owner's decision. A queued request and an
authenticated client do not yet establish successful through-Steam game launch.

A subsequent read-only inventory confirms all 33 original save files still have
identical content hashes and sizes, with no added or removed files. One file's
mtime changed during the Steam launch attempt. The prior byte-and-timestamp
invariant therefore no longer passes, even though save content is intact.
Private before/after inventories and Cloud logs are preserved. Steam Cloud
activity is present; causation for the timestamp change remains unproven.
Redirecting the game's HOME alone does not establish isolation from the Steam
client's Cloud synchronization. Before completing acceptance, verify that Cloud
cannot read or write the original save location during the controlled test.
