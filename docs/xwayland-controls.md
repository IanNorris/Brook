# Isolated Xwayland controls

Steam login is not yet accepted. Testing stock X11 drawing separately from
Steam exposes a smaller cross-context graphics failure.

The subsequent [PRIME context repair](steam-prime-contexts.md) fixes the
illegal-resource/whole-window-blackening regression; presentation and the
later fence-memory crash are still under investigation.

## Reusable guest helpers

Install stock `xeyes` and its closure using the normal guest package installer.
The test uses pinned xeyes 1.3.1, store output
`6cxs97yslzr72khjgql9f7zb1n5np4j4-xeyes-1.3.1`. When adding a different pinned
package to an older guest index, use a unique index name: an existing exact
`xeyes` entry takes precedence over a new fuzzy match. Review W installs this
output as `brook-xeyes`; the executable is still `xeyes`.

Start [xwayland_check.py](../scripts/workloads/xwayland_check.py) inside the
guest, using `--glamor off` for software or `--glamor gl` for acceleration.
Its rootful `-nolock` diagnostic requires root; an unprivileged server attempt
fails explicitly. It is not the normal desktop launch policy.

Run [xeyes_check.py](../scripts/workloads/xeyes_check.py) as Brook. It explicitly
selects the guest display, defaults to `320x200+40+40`, and accepts `--no-shape`
and `--classic` (core X11 instead of XRender). The stock program owns rendering
and pointer tracking; the helper only constructs its arguments and execs it.
The outer 800-by-600 Brook window belongs to the rootful X server, while the
geometry applies to the X11 child. This does not test rootless integration or
desktop transparency. Review W uses `--no-shape` and default XRender drawing.

## Review W, 24 October

The unchanged ABI-50 kernel is
`d1891f156df9254ba66a0fe882f5cd2729a3546755b038074e9be079f2b31a57`.
The private review uses eight vCPUs, 12 GiB RAM, Intel hardware rendering,
MLFQ and the same private 1 ms QEMU fence-poll build as review V.

Stock xeyes draws correctly and tracks the cursor with both software and GL
Xwayland. The user verifies both visible runs. Direct X11 GetImage captures
the white eyes, their hashes change after pointer movement, and composed
bridge frames visibly show the corresponding pupil positions. Both snapshot
groups report no X11 errors. On the GL server, the explicit green rectangle
also reads back all 12,800 pixels correctly. The existing
[GLX check](../scripts/workloads/glx_check.py) reports virgl / Intel UHD 630,
OpenGL 4.6, exact red `[255, 0, 0, 255]` readback, and no GL/X errors.

Those positive controls do not certify subsequent presentation. A later
capture records red bars and an existing mapped xeyes window becoming entirely
black. The xeyes process remains live. QEMU reports context 256 (Xwayland)
trying to create a surface for illegal resource 65537, from the separate GLX
client's context namespace. The bridge captures the red bars in this run;
the earlier near-simultaneous user/bridge discrepancy is still unresolved.

After Steam and PulseAudio drain and the pristine Steam wrapper is restored,
restarting Xwayland and xeyes in the same VM gives working eyes again. Running
the short GLX check alone then reproduces a CREATE_OBJECT context error and
turns the still-mapped 320-by-200 xeyes drawable entirely black (64,000 black
pixels). The GLX client's own red-pixel check passes. No Steam process is
running during this repetition. This isolates an application-context failure;
a fresh-boot repetition remains useful because the VM itself is reused.

Source review finds that PRIME_FD_TO_HANDLE creates a canonical GEM alias but
does not attach the resource to the importing GPU context. GL EXECBUFFER also
ignores its BO dependency list. Astra confirms both gaps. Existing retirement
fences the producer and kernel compositor, not importing application contexts,
so adding an attachment alone is insufficient. The next fix needs explicit
attachment lifetime, safe detach/reimport, and execution/retirement
synchronization, with a two-context rendered-pixel regression after closing
the exporting file. This is a concrete graphics lead, not proof that it is
the only remaining Steam login issue.

Private evidence and controllers remain under
`build/workloads/xwayland-isolation-20261024` and review W. No proprietary
payload, raw application logs, disk images or authentication data belong in git.

W finishes with all tracked groups drained, the pristine Steam wrapper restored,
heap integrity and checked sync passing, and the original VM owner exiting zero.
Final serial acceptance and all four offline filesystem checks pass. Earlier
installer/index and unprivileged-server setup failures remain recorded as
failures; they are not counted as successful launches.
