# Steam PI-futex capability checkpoint (2026-10-23)

Steam's independent main-client abort is traced to a false-success PI-futex
implementation. The fix reports the unsupported capability honestly, allowing
PulseAudio's existing ordinary-mutex fallback. This does not implement priority
inheritance or certify Steam login.

## Diagnosis and Linux comparison

A private delegated abort probe identifies libpulsecommon-17.0.so offset
0x60d9a, immediately after the abort call in pa_mutex_unlock's pthread error
path. The exact public Nix PulseAudio library is identified from guest staging;
other builds of the same version have different offsets. A second private probe
delegates real pthread operations and records only failed-operation numeric
state and caller metadata. The failing mutex kind is 0x21: recursive with PI.
A small PulseAudio threaded-mainloop control reproduces the same ownership
failure independently of Steam. No assertion, lock or abort is bypassed.

Brook's FUTEX_LOCK_PI implementation acquired an uncontended word, but returned
success when acquisition failed. FUTEX_UNLOCK_PI unconditionally cleared the
word, without checking its owner. A contender could therefore believe it owned
another thread's mutex, or fail its later pthread ownership check and abort.

The same reduced raw-syscall probe gives this concrete comparison:

- Linux strace: self-owned LOCK_PI returns EDEADLK; unowned UNLOCK_PI returns
  EPERM; uncontended lock/unlock transfers and clears ownership correctly.
- Old Brook: both invalid ownership operations return success.
- Fixed Brook: PI operations return ENOSYS without changing the word.

An isolated Linux strace of the exact public PulseAudio library's threading
control passes. Linux's glibc PI capability probe receives EPERM from an unlock
of a zero-valued word. Brook must return ENOSYS so glibc can report ENOTSUP from
PI mutex initialization. [PulseAudio 17's mutex implementation](https://raw.githubusercontent.com/pulseaudio/pulseaudio/v17.0/src/pulsecore/mutex-posix.c)
explicitly retries that initialization with PTHREAD_PRIO_NONE on ENOTSUP.

The earlier full Linux SteamRT setup traces remain useful reference evidence
under build/workloads/steamrt-feasibility-20261022. They are not an uninterrupted
trace through login: keeping strace attached changed CEF behavior; detaching
before CEF allowed the login screen. Brook has built-in syscall tracing, not
ordinary ptrace/strace support. This checkpoint compares a reduced reproducer,
plus bounded built-in tracing of the later Brook main process; it does not claim
identical full-client traces or identical host-library selection across systems.

## Implementation and validation

All six PI operation classes are rejected before touching user memory or
resolving futex keys: LOCK_PI, UNLOCK_PI, TRYLOCK_PI, WAIT_REQUEUE_PI,
CMP_REQUEUE_PI and LOCK_PI2. Private/realtime variants receive the same capability
result. Existing ordinary and robust non-PI wait/wake paths are unchanged.
Proper PI ownership, queue handoff, owner death and scheduler priority donation
remain future implementation work. Returning ENOSYS is a real capability limit,
not a successful emulation or a special case for Steam.

The portable pifutexcheck probe requires explicit supported/unsupported modes.
Linux passes 15 supported-mode checks. Fixed Brook passes 219 unsupported-mode
checks covering word preservation, flag combinations, null-address probes and
ordinary WAIT/WAKE regression. Baseline observations retain the old wrong
success results rather than treating them as acceptance.

All 102 host tests pass. Exact-ELF entry-state and scheduler mutation audits
pass. Native procfs, packet-pipe (96 checks), robust-futex (223 checks), Intel
virgl GLX red-pixel readback and canonical /dev/shm alias/cancellation/remap
regressions pass. The unmodified PulseAudio threaded-mainloop control now
completes successfully without a preload probe. The shared-memory regression
passes both root and desktop-user checks; native, GLX, PulseAudio and Steam
also ran as UID 1000.

Frozen ELF SHA-256:
`9b58fe2b88266e72fd1351b2aac00f7af810b142656567d99cfb3b316e6bf493`.
Module ABI remains 50. Ignored evidence is under
build/workloads/steam-abort-20261023 and the steam-abort/steam-pi-fixed reviews.

## Application result and remaining work

The first uninstrumented fixed-kernel Steam run remains alive beyond the old
51-second abort point and records no pthread_mutex_unlock failure. It does not
display login. A later two-second trace of its main process shows repeated exec
attempts for a missing pulseaudio executable and EMFILE from openat, socket and
memfd_create. A descriptor-class count finds 1,006 descriptors, including 943
memfds. Neither known PulseAudio endpoint nor PulseAudio/PipeWire program is
present in this image. The missing-daemon autospawn loop is a separate concrete
lead; descriptor exhaustion is not proof of the first CEF failure's cause.

That original run is preserved as unsuccessful: it ignores a graceful SIGTERM
request, then its identity-checked main process is killed. The wrapper reports
137, the original group drains and both output streams reach EOF. A separately
identified explicit-server configuration comparison follows; it is not a replay
or replacement pass for that run. The explicit-server comparison still reaches
1,024 descriptors (943 memfds), with 41,659 fixed-string EMFILE messages at the
sample. It does not show login and later exits 139 with one guest page fault.
A subsequent stop action correctly refuses the already-retired PID rather than
signaling another process. Its original group drains and both streams reach
EOF. The descriptor accumulation and that separate page fault are unresolved;
no explicit-server setting is shipped as a fix.

The baseline PulseAudio control aborts in a worker and its waiting Python parent
requires identity-checked termination. That failed receipt is retained. The
baseline VM later completes checked shutdown, its original owner exits zero,
and boot/data/Nix/temporary disks all pass offline checks. Private libraries,
raw client logs, downloaded software and images are not committed.

The fixed-kernel VM also completes heap verification and checked shutdown; its
original VM wrapper's retained wait reports exit 0 and all four offline disk
checks report 0. The enclosing nix-shell tool receipt reports 143 after the
controller prints its stopped marker; that separate host-tool result is retained
and is not relabeled as a successful tool exit. No VM remains running. Kernel
build/staging occurs only between stopped, checked runs. A final comment-only
wording correction distinguishes glibc's zero-valued-word probe from a null
address; executable behavior is the frozen, accepted candidate described above.

Follow-up: [PulseAudio socket fallback and shared-memory capacity](steam-pulse-shm-checkpoint.md)
records the memfd investigation and its next bounded application results.
