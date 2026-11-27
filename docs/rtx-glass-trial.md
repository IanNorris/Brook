# RTX Glass trial

Glass runs as a boot service on one active NVIDIA output. The standard RTX
profile keeps the CPU compositor. For the first trial, boot with only the
primary monitor attached. An unavailable renderer, CE path, or second active
head leaves the CPU compositor in control.

## Build the trial and control

Build from this branch combined with the current desktop integration. Use
the same signed USB-system layout and already admitted profile plan as the
working desktop set. Choose the exact Mesa output already staged in that
profile. Check that all paths under `nv_glass.runtime_store_paths` in the
candidate manifest exist on the SSD before activating the trial.

Add these flags to the desktop's existing RTX builder command. Keep its
operator public key, update signer, GSP firmware, storage and network options.

```sh
--session desktop --display-max 3840x2160 \
  --nv-display --nv-gsp --nv-drm \
  --waylandd-nv-dmabuf --waylandd "$WAYLANDD" --xwm "$XWM" \
  --nv-glass --nv-glass-icd "$NVK_MESA/share/vulkan/icd.d/nouveau_icd.x86_64.json"
```

The builder includes VKSUP and VKRENDER and pins the supplied NVK ICD. It
adds `/etc/nv-glass` and `/etc/waylandd-nv-no-layer`. The latter keeps NVK
Wayland buffers on the fenced import and VFB-snapshot path so the same
textures are visible to either compositor. For a CPU control set omit the
two Glass flags. Use the same monitor and boot arguments for both sets.

## Check the trial

- Confirm `DESKTOP_NV_GLASS_ATTACHED renderD130` in the boot log and growing
  `glass_frames` and `glass_bytes` in `bridge hardware nv-disp driver desktop stats`.
  Check `bridge hardware nv-gsp ce-present status` and `glass_quarantined=0`.
- Capture the front while moving and overlapping windows. Check color,
  geometry, cursor, taskbar, chrome and backdrop blur. Check a plain Wayland
  SHM app, an NVK Vulkan Wayland app through `vkdirect`, Chromium and an
  Xwayland app. Use an ordinary window for the capture. The trial withholds
  desktop captures after the capture-exclusion policy has protected a window,
  pending frame-specific mask metadata.
- Compare the same window geometry and workload for at least 20 seconds in
  each set. Record the change in completed presents, renderer and compositor
  CPU time, and the display copy and error counters. The window upload
  path uses CPU snapshots, so successful GPU rendering and presentation do
  not establish a window-ingest speedup.
- Restart into the control set and verify the CPU desktop again.

## Handoff

The boot renderer captures the existing WM scene, rasterizes quads and blur
into an optimal Vulkan image, then copies the complete result into a linear
VRAM PRIME buffer. It waits for Vulkan completion before asking the kernel
to copy the buffer into the existing scanout ring. The nv-disp flipper still
owns each scanout address and accepts the result only after CE completion.
The CE maps the validated VRAM range using Video-aperture PTEs and the
existing ordered BAR1 invalidate path. An uncertain CE completion retains
the source and excludes the affected scanout buffer from CPU fallback.

The new kernel entry points cover opt-in renderer attach, present and
detach. The NVIDIA-specific hooks query a linear PRIME backing, copy to the
head-0 scanout ring and read its completed front. No GSP boot changes are
required. Physical NVK, CE scanout, blur and multi-application pacing are
trial acceptance items.
