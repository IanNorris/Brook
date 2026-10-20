# Linux workload reference (under development)

A separate NixOS image for comparing Brook with Linux on the same QEMU hardware.
The configuration evaluates, but boot, Weston, browser, game and measurement
acceptance are still pending. The existing diskless Vulkan control remains
separate; passing its probe is not browser/game acceptance.

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
