# Isolated Venus software X11 presentation candidate

Build with `nix-build tools/mesa-venus-brook-pkg --no-out-link --cores 4`.
This uses the repository's Mesa pin with VirGL/Venus drivers. It does not
replace the host driver, default desktop profile or guest launcher.

The patch addresses the software X11 PutImage path used with
`MESA_VK_WSI_DEBUG=sw,noshm`. Venus still renders on the host GPU. PutImage has
no X Present completion event, so the unpatched path never advances its
present ID. A client waiting indefinitely for that ID can stall despite
correct displayed pixels.

Checked PutImage requests establish post-draw server processing and detect
request errors. Connection errors remain surface loss. Only successful
processing advances the ID under the existing progress mutex, before returning
the image for reuse. No kernel import/type/fence guard is bypassed.

The patch also compares the condition helper's result with `thrd_timedout`,
rather than errno ETIMEDOUT, to preserve Vulkan's timeout result.

The conservative checks add an X roundtrip per draw request. The damaged-region
path can issue one request per row; this is a correctness candidate, not a
performance acceptance claim. A later optimization must retain error detection,
post-draw ordering, monotone completion and image-lifetime safety.

Build `tools/vulkan-probe-pkg` and run its `vulkan-wsi-probe` using the candidate's
`share/vulkan/icd.d/virtio_icd.x86_64.json` in `VK_DRIVER_FILES`:

- No arguments: four exact XGetImage color readbacks.
- `--present-wait`: eight readbacks with distinct IDs across image reuse,
  finite completion waits and zero-time waits after completion.
- `--present-wait-lost-window`: destroy the window before presentation and
  require surface-lost/out-of-date rather than false successful completion.
- `--large`: 120 full-HD frames with present-ID completion and exact pixels,
  reporting guest monotonic per-frame durations. This serializes GPU idle,
  X11 readback and present waits; it is not a game FPS benchmark.

Keep an external observer/deadline. Probe success does not establish game
rendering, audio or clean-exit acceptance. See the
[checkpoint](../../docs/steam-api-and-present-wait-checkpoint.md).
