# Direct VM video and audio recording

The bridge's `vm_record` tool records the selected VM through a private QEMU
VNC connection passed over QMP. It captures neither the host desktop nor a
microphone, opens no new listening port, and sends no input through the display
connection. Still screenshots retain the separate completed-frame lease API.

## Setup and use

Prepare the pinned host encoder before starting a VM:

```sh
nix-build tools/vm_bridge/recording-runtime.nix -o build/vm-recording-runtime
```

Launch a bridge-enabled VM with the existing experiment scripts. Use
`BROOK_AUDIODEV=none` for silent speakers with guest audio capture; `--no-audio`
removes the guest device and is unsuitable for testing guest sound. Reload an
already running MCP server to discover the new tool. Source CLI calls work
immediately:

```sh
python3 tools/vm_bridge/server.py --call vm_record --arguments '{"session":"SESSION_ID","action":"start","duration_s":30,"fps":30,"audio":true,"width":1920,"height":1080}'
python3 tools/vm_bridge/server.py --call vm_record --arguments '{"session":"SESSION_ID","action":"status"}'
python3 tools/vm_bridge/server.py --call vm_record --arguments '{"session":"SESSION_ID","action":"stop"}'
```

Start acknowledges an asynchronous worker; poll status until `complete` or
`failed`. Stop requests early finalization. One recorder may own a session at a
time. Bounds are 1–600 seconds, 1–60 requested frames per second and an even
canvas up to 4096×2160. Smaller guest modes are centered without scaling;
exceeding the selected canvas fails explicitly.

Artifacts stay under ignored `build/vm-bridge/SESSION_ID/artifacts/`: a Matroska
H.264/AAC master, frame timeline JSONL and result JSON. Incomplete output retains
its `.partial.mkv` suffix. `vm_artifacts` returns video path, size, checksum and
MIME metadata rather than embedding large video data in a tool response.

## Timing and limits

Video timestamps describe host-monotonic arrival of complete display updates.
Audio uses QEMU's PCM stream, its sample clock and a bounded receive queue;
idle periods and known missing samples are filled with silence. A long audio
arrival gap without an END event is reported as an estimated discontinuity.
The encoder holds the latest video image between updates and through stop.

Metadata reports coalesced display updates, missed encoder slots, locally
queued audio drops, late samples, silence and estimated discontinuities.
QEMU supplies no source audio timestamps or upstream drop counters: unknown
upstream losses remain unknown. These observations do not establish exact
physical A/V timing. Capture and encoding consume host CPU and memory.

CPU and GPU headless QEMU and bounded CPU-compositor/native-SDL recording were
exercised. GPU native-window operation, boot/mode transitions, 4K and reset-during-live-recording still
need live acceptance. No final showcase production has begun.

## Acceptance on 17 October 2026

All 70 bridge tests passed using the pinned PyAV runtime. Coverage includes
fragmented RFB messages, bounds, real file-descriptor transfer, audio timelines,
stale worker ownership, generation changes and actual codec finalization at
1, 2 and 30 FPS. Live tests covered duplicate-start refusal, killed-worker
recovery, early stop and a deliberately injected 1.2-second encoder stall.

A 20-second GPU recording captured Brook Player playing the 1280×720 Big Buck
Bunny source in an 800×600 window on a native 1920×1080 desktop. The decoded
master contained 513 video frames, lasted 20.021 seconds including AAC padding,
and had no local audio queue drops, late samples or estimated discontinuities.
There were 44 coalesced display updates and 12 missed encoder slots. Guest
playback reported 24 FPS. Host encoder cost was 18.55 CPU seconds over the
20-second take, with about 163 MiB peak resident memory; the file was 17 MiB.

The player closed through its native close button and returned to ten tasks.
A separate two-second recording decoded to zero audio amplitude after close.
Checked shutdown and all three offline filesystem checks passed. CPU and GPU
marker fixtures also produced video and audio; observed onset differences
varied, including a roughly 260 ms CPU case, so one-frame synchronization is
not an accepted guarantee.

Evidence, frozen artifacts and harnesses are retained locally under
`build/workloads/vm-recording-20261017/`. The player session was
`f146cac24aa5496995a094ab25c710d4`, recording
`6c677de42ccc457cabc5b9987bc44a63`. Movie and recording media are not tracked.
Big Buck Bunny: (c) copyright 2008, Blender Foundation / www.bigbuckbunny.org,
[CC BY 3.0](https://peach.blender.org/about/).

## Three-minute rehearsal — 18 October 2026

A 180.000-second headless CPU-compositor take at native 1920×1080 captured
Firefox playing a controlled 640×360 Opus/video fixture while Chromium and
VSCodium were launched and exercised. The original Matroska master contains
3,972 H.264 frames and stereo 48-kHz AAC. An independent full decode verified
every video frame, native dimensions and monotonic presentation timestamps,
plus 8,438 AAC frames. The MP4 copy is a container-only remux of the whole take.
Selected frames at 10, 80 and 160 seconds were inspected at full resolution.

The recorder counted 4,246 incoming display updates, 282 coalesced updates and
28 missed encoder slots. It received 8,634,317 audio sample frames and filled
5,689 with silence; queue drops, late samples and estimated discontinuities
were all zero. Host cost was about 92 CPU seconds and 174 MiB peak resident
memory. Requested capture cadence was 30 FPS; the encoded frame count does not
establish 30 distinct guest frames per second. Upstream loss and physical A/V
synchronization remain unknown, and silent host output was used.

This is technical rehearsal footage: overlapping windows and an editor
completion mistake are visible. It is not a final showcase or proof of smooth
concurrent browser playback. Native-window capture and boot/display transitions
remain separate acceptance work. Local evidence:
`build/workloads/stability-f99e267d-20261018/review/`, session
`1ebd6b51256e49949dfcba5471e8bed6`, recording
`62880e51b2e2427caca3f169b50bc1ac`.

## Native SDL window — 18 October 2026

Two 90-second CPU-compositor recordings ran with QEMU's actual `-display sdl`
window active and its host audio backend set to `none`. Capture used the VM's
private RFB/audio stream; it did not capture the host desktop. VLC decoded the
1280×720 Big Buck Bunny source on the native 1920×1080 desktop. All video and
AAC frames decoded, with monotonic timestamps and the expected canvas size.

| Take | Video frames | Coalesced updates | Missed encoder slots | Received audio frames | Silence filled |
| --- | ---: | ---: | ---: | ---: | ---: |
| Initial playback | 2,419 | 117 | 35 | 3,593,247 | 726,760 |
| Recovery-package window test | 2,117 | 60 | 31 | 3,413,907 | 906,099 |

Both reported zero local audio queue drops, late samples and estimated
discontinuities. Requested cadence was 30 FPS; these counts do not certify
application or display frame rate. Silence includes idle/pause/exit intervals;
upstream losses and physical A/V synchronization remain unknown. Encoder cost
was respectively about 85 and 55 CPU seconds, with 171 and 165 MiB peak RSS.
Full-resolution frames were inspected from both takes.

The second take includes pause/resume, a measured 55-second minimize, restore
and normal close. It exposed an application limitation: audio recovered, but
VLC's video area stayed black after restore. Ordinary playback and recording
work; video restoration is not accepted. The first run also exposed an OSS
write-error loop, addressed by the [VLC recovery patch](../tools/vlc-brook-pkg/README.md).
Recording completion must not be confused with full application acceptance.

Evidence: `build/workloads/media-regression-bed237cb-20261018/`. Sessions
`e93e9fc9d43f4402a88651e132acadf8` and `d284671b1e0b41d5a779d8d9362748f4`,
recordings `03849c21d4cd4e1e98e626ca7a5e2084` and
`57383b5f2a5d4060bfccb674bd78c995`. Boot/mode transitions, GPU native-window
recording, 4K and reset during capture remain open. No final showcase has begun.

## Boot and display modes — 18 October 2026

A native-SDL CPU run started paused with QEMU `-S`. The recorder produced its
first frame before CPUs resumed, then captured 150 seconds across source modes
640×480, 1280×800 and 1920×1080. The experiment launcher now forwards additional
QEMU arguments, including `scripts/run_experiment.sh desktop -S`; a test checks
argument boundaries and the unchanged default invocation.

All 1,956 H.264 and 7,033 AAC frames decoded with monotonic timestamps. Inspected
frames show the initial black display, Brook boot logo/progress, desktop and VLC
playback. The recorder counted 700 coalesced updates and 903 missed encoder
slots; it received 3,694,606 audio sample frames and filled 3,505,400 with silence,
including boot/idle intervals. Local audio queue drops, late samples and
estimated discontinuities were zero. Host cost was about 103 CPU seconds and
176 MiB peak RSS. These results verify capture continuity through mode changes,
not smooth playback or source frame rate. The user observed VLC black flashes
and corruption during this diagnostic run.

VLC's full launch lineage exited, audio became inactive, the original launcher
returned zero, all three offline filesystem checks passed, and nine saved files
retained their original hashes and ownership. Evidence:
`build/workloads/boot-recording-7c72adad-20261018/review/`, session
`2cab1de4c4e347c5b92792e88a89a6a2`, recording
`d7913c6603e24dcbbf9f21f490c72558`. GPU native-window recording, 4K and reset
during capture remain open. No final showcase has begun.

## Native OpenGL display compatibility — 18 October 2026

The user review launch exposed a QEMU frontend restriction: adding even a
private `-vnc none,audiodev=hda0` alongside `-display sdl,gl=on` prevents startup
with “Display vnc is incompatible with the GL context.” The bridge now leaves
VNC disabled for native SDL/GTK OpenGL displays. An explicit conflicting VNC
selection is rejected before QEMU starts. CPU SDL and EGL-headless displays
retain their private RFB/audio transport.

Session metadata reports private-RFB availability. Video recording requests on
the native-GL configuration report the unsupported transport before dispatch;
kernel frame capture and QMP WAV capture remain independent. Native GPU video
recording still needs another capture path; this fix does not complete it.

All 25 recording/transport tests pass with the media runtime, including actual
socket descriptor handoff. The user's restarted review VM was verified running
with `sdl,gl=on`, PipeWire and no VNC option. Live application acceptance and
checked shutdown of that user-owned session remain separate.
