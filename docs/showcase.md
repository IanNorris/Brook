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

The bridge now leases a completed-frame snapshot, avoiding mutable render-buffer
captures; see [capture acceptance](completed-frame-capture.md). Its GPU readback
and brief VM pause are useful for stills.
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
launches let readiness be established before a window's cue. The bridge now
offers launch, focus, minimize, maximize, restore and close actions; rehearsing
their timing with the chosen application set remains necessary. The OS itself is
the demonstration, including real input and visible application responses.

Potential scenes are terminal editing and live Rust/C++ compilation, browser
navigation, a Writer document, 720p media playback, Quake's recorded Installation
demo, SuperTuxKart and GLtron. Firefox, Chromium and VSCodium already have real
workload checks; selection for the tour also needs a stable concurrent rehearsal.
Blender remains optional because its missing capabilities are outside the small-fix
scope. Simultaneous-app stability, readable UI
text, predictable focus, safe app shutdown and performance come before a final
recording. A loading screen or launcher icon does not establish a working app.

The selected soundtrack is **Brook 2000.mp3**, supplied separately: 3:09.213,
stereo at 48 kHz. Keep the MP3 outside Git. Its beat markers and suitable scene
cues still need measurement; no tempo has been assumed. Play and capture the
music in synchronization with the continuous take, rather than assembling
separate clips to disguise startup or stability issues.

Use the VM bridge to rehearse repeatable actions and inspect readiness. Direct
VM recording now uses a private QEMU display/audio stream and a pinned host
encoder. CPU/GPU headless captures, real player audio, early stop and worker
recovery passed bounded tests; see [recording setup and limits](vm-recording.md).

The recorder writes a native-canvas H.264/AAC master with frame timestamps and
local loss counters. Source timestamps and upstream drop counts are unavailable.
Boot transitions and native-window capture still need acceptance.
Preserve the unedited take and action/cue log. Final showcase production remains
on hold; no final movie has been produced.

### Rough rehearsal sequence

These are provisional wall-clock slots, not measured musical cues. The 3:09
soundtrack remains separate until the sequence and recording are reliable.

| Time | Visible action | Readiness gate |
| --- | --- | --- |
| 0:00–0:25 | Boot, open a text editor, type the two introductory lines | Stable display transition; readable text and native input |
| 0:25–0:55 | Compile and run a small C++ or Rust example in a terminal | Exact output, successful exit, no pending build |
| 0:55–1:20 | Bring up the saved Writer document and GIMP image | Existing files open correctly; keep earlier windows available |
| 1:20–1:55 | Use one browser on a real site, then play a short video | Working input, advancing video and audio; use the browser that passes the rehearsal |
| 1:55–2:20 | Play the local 720p clip in the media player | Stable playback; pause the browser first to avoid competing audio |
| 2:20–2:55 | Bring up Quake II's recorded demo | Verified GPU mode, mouse/focus behavior and normal demo progress |
| 2:55–3:09 | Return to the desktop, show the running apps and end | Responsive focus changes and an intact continuous recording |

Use one browser in the proposed take. Adding both browsers, Codium, Blender or
a second game requires another simultaneous-workload check and a memory budget;
minimizing a window does not release its resources. Slow launches stay visible
or happen within the continuous take before their cue. Do not hide waits with edits.

A technical 180-second headless rehearsal has been captured at 1920×1080 with
Firefox, Chromium and VSCodium. All 3,972 encoded video frames and the AAC stream
decode successfully. This is test footage, including scripted-edit mistakes and
overlapping windows, rather than the proposed showcase sequence. Its counters
and remaining recording limitations are in [recording evidence](vm-recording.md).


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
