# Linux workload reference

A separate NixOS image for comparing Brook with Linux on the same QEMU hardware.
The first private image boots Weston at 1920×1080, runs the buffered Firefox
workload and Quake II GL3 at 1280×720, and passes installed-closure verification,
clean shutdown and offline ext4 checks. This is a first warm reference, not a
statistical or cold-start acceptance suite. The diskless Vulkan control remains
separate.

The pinned image includes Firefox, Quake II's engine, SuperTuxKart, GLtron and
the same `scheduler_mix.c` workload. Applications live on a persistent ext4
root image, with no swap, rather than in the initrd or a shared host Nix store.
The benchmark user is `brook` (UID 1000). Use a disposable SSH public key;
password authentication and root SSH are disabled. Membership in wheel does
not bypass sudo authentication. The launcher must bind SSH forwarding to
`127.0.0.1`, use a private known-hosts file and never forward the host agent.

For example, from the repository root, create an ignored private output folder
and disposable key, then build with bounded parallelism while no benchmark VM
is running:

```sh
mkdir -m 700 build/linux-reference
ssh-keygen -q -t ed25519 -N '' -f build/linux-reference/id_ed25519
nix-build tools/linux-workload-reference \
  --argstr sshPublicKey "$(cat build/linux-reference/id_ed25519.pub)" \
  --max-jobs 1 --cores 2 --out-link build/linux-reference/result
```

The output contains `root.img`, `bzImage`, `initrd` and `manifest.json`. The
image has no bootloader: a launcher must use the supplied kernel/initrd and
pass the manifest's `init` path as `init=...`, along with its kernel parameters.
Always clone `root.img` into a private writable raw image before booting. Do not
attach Brook disks, the user's current VM disks or a writable host Nix store.
Locally owned Quake II data must be staged separately after boot and must never
be added to this derivation or git.

Match eight vCPUs, six GiB RAM, host CPU affinity 0–13, QEMU/renderer libraries,
Intel render node, Venus/virtio GPU, 1920×1080 output and silent 44.1-kHz audio.
The configuration uses Weston on DRM and an unprivileged Wayland session. Record
actual guest app/renderer versions and effective display/audio settings before
measuring. Different compositors, filesystems, Mesa builds and cache state are
comparison limits. Cold guest profiles do not imply a cold host filesystem cache.
Only one benchmark VM may run at a time; wait for clean process exit and check
the stopped writable filesystem before interpreting a trial as accepted.

Start with the generated buffered video/input/CPU fixture and report startup
separately from steady state. Then compare the Quake II GL3 demo and a free game.
Use the Linux results to guide the optional Brook EEVDF trial after its runtime
accounting, lifetime and SMP safety prerequisites, retaining RR and MLFQ controls.


## Launch and control

Use the same host ICD and isolated null-sink daemon as the Brook control. The
bounded helper creates that daemon without loading device-discovery/ALSA modules:

```sh
python3 scripts/run_silent_audio.py --output build/linux-reference/audio --timeout 3600
```

It writes `daemon.json`; create its output folder's `stop` file to stop only its
owned child. The helper also stops the child on timeout. For example (substitute
the exact host ICD from your checked run):

```sh
python3 scripts/linux_workload_reference.py start \
  --image build/linux-reference/result --output build/linux-reference/run-0 \
  --ssh-key build/linux-reference/id_ed25519 \
  --host-icd /nix/store/EXACT-MESA/share/vulkan/icd.d/intel_icd.x86_64.json \
  --audio-daemon build/linux-reference/audio/daemon.json \
  --timeout 3600
```

The launcher stays attached until its owned QEMU exits. It fixes eight vCPUs,
six GiB, CPU affinity 0–13, 1-GiB GPU hostmem, 1080p, callback HDA and the selected
null-sink backend. Without `--audio-daemon`, it uses the `none` backend; that is
a different audio cohort. PID incarnation and the actual Nix-wrapped QEMU
executable are checked for every control operation. A bounded timeout is a
failed trial, even if the subsequent shutdown succeeds. No Brook or user disk
is attached and no host desktop input/capture API is used.

In another terminal, use `status`, `exec`, `screenshot` and `shutdown` with
`--output build/linux-reference/run-0`. `exec` takes a guest command after `--`.
Screenshots first try QMP; GL scanout may have no CPU-readable surface, in which
case Weston's normal Super+S authorizes its own screenshot client. The tool
transfers that guest PNG without enabling compositor debug access. After
shutdown, wait for the original launcher, then run `check` to verify authoritative
exit, serial powerdown and the offline private filesystem. An acknowledgment of
`system_powerdown` alone is not clean-shutdown evidence.

## First measured control

The October 20 image uses Linux 6.18.26, Firefox 150.0.1, Mesa 26.0.6 and Quake II
8.60. The selected Linux clocksource is `kvm-clock`; its clock-event device is
`lapic-deadline`. Brook uses the same clocksource with periodic LAPIC delivery.
The reference loads the exact host QEMU/virglrenderer libraries used by Brook.

The first default Firefox profile decoded video and delivered audio but stalled
its visual callbacks under Weston. A 12-second independent Wayland presentation
probe delivered 703 frames, narrowing this to the Firefox path. A separate
profile with `widget.wayland.vsync.enabled=false` restored visual updates; the
known [Mozilla vsync diagnostic](https://bugzilla.mozilla.org/show_bug.cgi?id=1753774)
is a reference configuration workaround, not a Brook fix. Record this option
explicitly; do not pool it with default-profile results.

For the buffered fixture, the Linux controller is
`scripts/workloads/run_linux_browser_sched_mix.py`; it requires the existing
private VM and identical static `scheduler-mix` binary. It defaults to an
800×600 browser window to match Brook, with `--software-vsync` selecting the
separate working reference configuration. It records input, video, silent PCM,
CPU and deadline evidence and closes the known fresh single-window Firefox
profile through its quit-confirmation flow. The caller still shuts down and
checks the VM. Unknown/failed jobs are retained, never automatically replaced.

In the first matched-size 24-worker/30-second pilot, video advances 29.620 seconds
across 29.605 page seconds with 23.07 video callbacks/s, no sampled dropped frames,
and continuous nonzero captured audio. CPU work is 6.56 million batches and
periodic sleeper p95 is 0.45 ms (worst 66.72 ms). This is one warm pilot with Linux
compositor, cache, autogroup and per-thread policy differences, not proof that
EEVDF alone causes the difference. Batch-count max:min is not CPU-time fairness.
Evidence is in `build/linux-workload-reference-20261020/`.

For Quake II, the shared asset-free timing probe accepts `--engine` for the Linux
binary plus a fresh `--home` and locally staged `--pak-dir`. It preserves the
same demo packet selection and nominal 15.1-second interval. The first GL3/720p
run observes 15.440 seconds, exits zero and reports hardware virgl on Intel UHD
630. This is a timing/smoke control, not an uncapped rendering-throughput score.
The private control has only the owned pak, without OST staging; do not equate it
with a music-matched native audio run. All licensed data remains local/ignored.
