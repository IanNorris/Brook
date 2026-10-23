# Steam shared-memory budget and Linux control — 23 October 2026

Steam login remains the active application goal. This checkpoint records
successful infrastructure tests separately from failed client attempts.

## Shared-memory policy

Tmpfs already allocates storage on demand. The previous hardcoded 256 MiB
aggregate ceiling was too small for desktop experiments, independently of each
mount's byte quota. Boot now configures a budget from available physical pages;
ordinary mounts retain the 16 MiB default unless size= is supplied, and the
desktop /dev/shm receives half the aggregate budget. Configuration is rejected
while any tmpfs instance is live. No up-front reservation is made.

statfs availability is now the smaller of per-mount and aggregate remaining
capacity. Earlier per-mount-only readings did not exclude global exhaustion;
the earlier 256 MiB comparison therefore did not establish a distinct mapping
bug. Bounded page-in diagnostics identify ENOSPC (-28), including on the small
three-page mapping that subsequently receives SIGBUS. Diagnostic formatting
was corrected to use Brook's supported single-l length modifier; the first
malformed records are retained and are not accepted error-code evidence.

An intermediate budget used PMM's total-page count, which is actually the
highest tracked address and includes address-space holes. The next candidate
uses free pages at boot instead. The policy is a charged-memory ceiling, not a
promise about all future applications' physical working sets. Metadata and the
current separate backing/canonical mapping pages are charged.

Host tests cover configuration validation, refusal during live mounts,
per-mount ENOSPC and a second mount observing aggregate exhaustion. The complete
102-test suite and tmpfs ASan/UBSan pass. Exact-ELF entry and scheduler mutation
audits and native socket/PI/procfs/packet-pipe/robust-futex controls pass.
A new portable three-pool mapping control creates three 25 MiB+16-byte files,
touches every page and the partial tail in a child, checks the data through
independent reads, then removes its fixtures. It passes on host Linux and
Brook. The Linux host control uses /tmp; Brook uses its canonical /dev/shm.

## Capacity is a symptom, not the whole client failure

Larger quotas postpone the client failure. Brook creates up to thirteen
25 MiB+16-byte pools before exhausting a 640 MiB intermediate quota; most space
is released after the client exits. A repeat records the growing file-size
histogram without exposing names. These runs exit 135 and are not login passes.
One RAM-budget review records a scheduler exit_group timeout; its controller
correctly fails serial acceptance even though checked shutdown, original VM
wait and all four offline filesystem checks finish with exit zero.

The isolated Linux reference VM runs Steam without strace under a fresh user
profile. It reaches the QR-code login page and remains at two 25 MiB+16-byte
pools through 100 samples: 58,414,784 total logical bytes in fourteen files.
Its tracked service stops successfully, original QEMU wait returns zero and
its offline filesystem check passes. No account is authenticated. The Linux
reference uses its existing six-GiB image; Brook uses eight GiB, so this is a
startup resource-shape comparison, not a normalized performance benchmark.
The prior archived Linux traces have no continuous post-detach view of this
resource growth, and are not used as a substitute for this fresh control.

## Graphics and diagnostic scope

The helper failure contains standard EGL initialization failures and Vulkan
result -3. A minimal ANGLE probe initially fails because its library path omits
the installed Mesa vendor directory. With the complete Steam library path,
ANGLE OpenGL initializes both outside and inside SteamRT. Default, hardware
Vulkan and SwiftShader initialization pass outside the container; the OpenGLES
backend reports its own unsupported-driver diagnostic. These initialization
checks are not windowed rendering or full helper acceptance.

The GPU-enabled full-client comparison also exits 135. The helper still needs
a focused diagnosis; no graphics environment override is shipped as a fix.
Only public source basenames/line counts, known flags, immutable ANGLE error
strings and Vulkan result codes are exported from client logs. Raw logs stay
private. Procfs currently lacks full command lines, environ and file-backed
map details, so its empty graphics introspection results are not evidence that
the helper has no flags or libraries. The failed early trace controller lacked
a committed PID; recovery kept the same original job. No useful descendant
trace was captured in that attempt.

Fork now preserves the selected syscall trace filter along with the tracing
booleans; otherwise a filtered parent could accidentally enable unfiltered
logging in its children. This is diagnostic correctness, not a Steam fix.

Ignored evidence directories: build/workloads/steam-filefault-20261023,
steam-shm-budget-20261023, steam-helper-retry-20261023 and
steam-linux-footprint-20261023, plus their separate review directories.
Steam software, profiles, raw logs, cookies and images remain untracked.

The hardware Vulkan control succeeds outside SteamRT but fails inside it;
forcing the installed canonical virtio ICD does not resolve that difference.
A direct render-node control establishes that /dev/dri and renderD128 are both
absent inside SteamRT, while native enumeration/open/DRM version ioctl pass.
OpenGL initialization can use the X server connection and succeeds inside the
same runtime. A separately identified private helper-wrapper comparison adds
--use-gl=angle and --use-angle=gl, retaining a hash-checked original. The initial
hash guard correctly rejects the original archive hash because the guest has
a required Nix bash shebang. The actual small script is inspected, its hash
verified, and a separate corrected fixture applies the reversible change.
No Valve wrapper contents or downloaded libraries are committed.

The explicit canonical ICD control still fails inside SteamRT. The render-node
probe reports ENOENT for both /dev/dri and renderD128 in that container. The
OpenGL-wrapper client attempt exits 135 and does not reach login; the original
wrapper is restored with its hash checked. All original helper-review jobs are
drained, PulseAudio and Xwayland exit zero, heap/checked shutdown and the
retained original VM wait complete. Its serial controller fails acceptance
because an exit_group timeout is present; this remains a failed stability run.
All four offline filesystem checks pass. The next implementation targets only
the proven DRM/sysfs broker visibility gap across genuine binds.
