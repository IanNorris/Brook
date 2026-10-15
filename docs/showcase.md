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

## Live one-take tour

The target is a real OS demonstration in one continuous take, without edits.
Start Brook, launch a simple text editor with comfortably readable type, and
enter:

> Brook - a from scratch OS with Linux compatibility
>
> A quick tour. One take, no edits.

Then launch progressively more demanding applications, keeping earlier apps
running. Bring each window forward on a musical cue. Background or minimized
launches will be needed so readiness can be established before a window's cue;
that behavior still needs implementation and verification. The OS itself is
the demonstration, including real input and visible application responses.

Potential scenes are terminal editing and live Rust/C++ compilation, browser
navigation, a Writer document, 720p media playback, Quake's recorded Installation
demo, SuperTuxKart and GLtron. A full editor, Blender and Firefox/Chromium join
the tour only after they are usable. Simultaneous-app stability, readable UI
text, predictable focus, safe app shutdown and performance come before a final
recording. A loading screen or launcher icon does not establish a working app.

The selected soundtrack is **Brook 2000.mp3**, supplied separately: 3:09.213,
stereo at 48 kHz. Keep the MP3 outside Git. Its beat markers and suitable scene
cues still need measurement; no tempo has been assumed. Play and capture the
music in synchronization with the continuous take, rather than assembling
separate clips to disguise startup or stability issues.

Use the VM bridge to rehearse repeatable actions and inspect readiness. For
continuous recording, investigate a private QEMU D-Bus display/audio listener,
including GPU DMA-BUF updates where available. This captures the VM directly
and avoids the user's host desktop. QEMU documents those listener interfaces;
the recorder has not yet been implemented or validated.
[QEMU D-Bus display documentation](https://www.qemu.org/docs/master/interop/dbus-display.html).

Record a native 1080p master with timestamps, audio synchronization and
frame-drop counters. Preserve the unedited take and the action/cue log so its
behavior can be reproduced. A final movie has not been produced yet.


### SuperTuxKart launcher and verified run

SuperTuxKart 1.5 installed through guest `nix-install` in about 119 seconds,
including its roughly 718 MB asset package, on the validated eight-core,
4 GiB kernel `af3d143c36e7b93a1e5d0c38df466deeb46973cd94cf527538cd8971f5a97948`.
The native 1920×1080 capture shows an 800×600 Lighthouse AI race through virgl,
OpenGL 4.3 and Mesa 26.0.6 on the host Intel UHD Graphics 630.

The tracked `/nix/stk-play.sh` wrapper redirects HOME and all writable XDG
state to `/data/home/supertuxkart-$UID`, on ext2. User and root launches have
separate state directories; the Apps entry and STK scenario scripts launch as
uid 1000. Fresh experiment images and `update_nix_disk.sh --tools` deploy the
wrapper and `/nix/bin/brook-supertuxkart` alias. The stock STK closure is unchanged.
Its OpenAL backend is null for silent testing. Launch manually with
`run --uid 1000 /nix/stk-play.sh`, or add `--race-now --track=lighthouse
--numkarts=4 --profile-time=180` for the controlled race.

The first successful timed race removed every app thread, completed guest sync,
and left both ext2 images with offline filesystem-check status 0. It also emitted
mapping accounting warnings during teardown; [RUN-06](backlog.md#run-06--gpu-resource-mapping-accounting-during-supertuxkart-teardown)
records the evidence. A later window-resize test produced native 1280×719 buffers
but bridge frame requests timed out. The selected screenshot predates that resize;
it establishes real gameplay, rather than resize or teardown stability.
