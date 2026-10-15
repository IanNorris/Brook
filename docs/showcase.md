# Brook screenshots and a scene demo

Screenshots are captured from the VM, never from the host desktop. The current
target is native **1920×1080**, following the decision to prioritise readable
1080p images before adding 4K display support. Capture resolution is checked;
upscaling a smaller framebuffer does not count as a higher-resolution capture.

The older gallery files are mostly 1920×1080 too. Their README previews were
800 pixels wide, reducing a 16-pixel UI glyph to about 7 pixels. Full-size links
now preserve access to the original image. Making the desktop larger would
make fixed-size text even smaller in the same README preview.

## Clean desktop and capture

The normal desktop starts the window manager, wallpaper and Wayland server.
It does not automatically open Kernel Console, Bash or Calculator. Open a
terminal with the taskbar **+** button or the Terminal shortcut; launch
Calculator and either console from **Apps**. The Kernel Console shortcut runs
the `kconsole` kernel-shell command. Serial logs, debug TCP, the VM bridge and
panic inspection continue to work without any console window.

Use isolated writable experiment images with at least eight vCPUs. Hardware
Quake needs `BROOK_GPU=gl BROOK_COMPOSITE=gpu`; hardware availability alone
does not ensure that a dmabuf application's contents appear in the desktop.
Host audio can stay silent with `BROOK_AUDIODEV=none`.

```sh
BROOK_VM_BRIDGE=1 BROOK_SMP=8 BROOK_AUDIODEV=none \
  BROOK_GPU=gl BROOK_COMPOSITE=gpu scripts/run_experiment.sh desktop
```

Inside the development shell, capture the session printed by the launcher:

```sh
python3 scripts/capture_showcase.py SESSION brook-quake2-gl3-1080 \
  --caption 'Quake II Installation demo through hardware virgl GL3'
```

The helper writes a PNG and JSON metadata under ignored `build/showcase/`,
including native dimensions, kernel hash, session, frame source and image
checksum. It requests neither scaling nor cropping and rejects the wrong
resolution or a panic frame. It does not certify that an application is
interactive: visually inspect each image and exercise the application before
publishing it. Copy only selected PNGs into `docs/images/`; do not copy disk
images, game packs, soundtracks, guest memory or private session metadata.

The bridge's GPU readback briefly pauses the VM and is useful for stills.
Repeated readbacks are unsuitable for measuring rendering performance or
recording a smooth video.

## Proposed scene demo

A short, scripted reel would work well: a boot logo and quiet desktop, a
browser interaction, a Writer edit, 720p media playback, then a hardware Quake
demo and a return to the desktop. Add SuperTuxKart and GLtron after their
current launch/rendering paths are verified. A failing app should not be
represented by a launcher icon or a fabricated screenshot.

Use the VM bridge to prepare each scene and perform repeatable input actions.
Warm up each application before recording its clip; use readiness events or
visible checkpoints rather than assuming it loads at a fixed time. Quake's
recorded Installation demo gives a repeatable moving scene without requiring
mouse capture.

For continuous capture, the promising next step is a private QEMU D-Bus
display listener: receive VM display updates and audio directly, including
the GPU's DMA-BUF path where available. This avoids capturing the user's
desktop. QEMU documents both display and audio listener interfaces; choosing
an interface does not yet implement or validate the recorder.
[QEMU D-Bus display documentation](https://www.qemu.org/docs/master/interop/dbus-display.html).

Record a native 1080p master with timestamps and frame-drop counters. Keep
real app audio as a separate track, then assemble warmed clips to music
offline. For example, 120 BPM gives eight seconds per four-bar phrase in 4/4;
a 96-second reel can use twelve phrases. Cut on phrase boundaries, hold UI
interactions long enough to read them, and reserve fast cuts for game footage.
The selected soundtrack is **Brook 2000.mp3**, supplied separately: 3:09.213,
stereo at 48 kHz. Keep that source audio outside Git. Beat markers and scene
cuts still need to be mapped to the track; the 120 BPM example above is a
planning example, not a measured tempo for this music. A final movie has not
been produced yet.
