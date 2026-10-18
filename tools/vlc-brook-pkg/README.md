# VLC guest compatibility build

This overrides the repository's pinned Nixpkgs VLC 3.0.23-2 package. Source
and dependencies remain Nix builds; game packs and runtime images are unrelated.

The patches:

- Honor explicit `--avcodec-hw=none` before negotiating opaque hardware formats.
- Use Linux memfd buffers before temporary filesystem files, allowing Brook's
  shared-memory Wayland path to share decoder pixels with the compositor.
- Build native OSS output against OSS3 Linux headers, without mandatory OSS4
  enumeration/format constants. OSS3 pause drains already queued audio; it does
  not implement the OSS4 silence/skip operations.
- Keep the Qt-owned Wayland surface alive during native close until VLC's
  existing playlist shutdown has joined the video renderer and released its
  window. Ignore the Qt close event instead of forcibly releasing/hiding the
  widget before asking VLC to quit.
- Clear Wayland buffer user data before releasing its picture in release
  builds too. Otherwise pool teardown can release an already-freed clone.
- Refresh the borrowed Wayland surface after Qt's initial fullscreen, stack
  and sizing changes, before returning the blocking window request to the
  renderer. Those transitions can hide and recreate the native surface.
  Reject a missing final handle and release the widget reservation.
- Return from OSS playback on a persistent write error or zero progress and
  request VLC's deferred output restart. Short writes retain their unwritten
  suffix; interrupted writes retry. The previous callback could spin forever on
  a faulted DSP descriptor and prevent playback or shutdown from progressing.

The callback ownership regression can be checked against the matching VLC
source directory (the package derivation's `src`):

```sh
python3 tools/vlc-brook-pkg/check-buffer-release.py /path/to/vlc-source
```

It extracts the actual release/detach callbacks, applies the package patch and
compiles five ownership cases in debug/release modes. The model keeps clones
distinct from their backing pictures and detects stale releases without using
freed host memory. Unpatched release code reproduces two duplicate releases;
patched code passes all cases. This does not replace real playback/quit tests.

The OSS write regression likewise compiles the actual `Play` callback:

```sh
python3 tools/vlc-brook-pkg/check-oss-write.py /path/to/vlc-source
```

Ten cases cover full/short writes, interruption, broken pipe, zero progress,
partial write followed by error, unavailable-device errors and an empty block.
Six bounded no-progress loops reproduce in the original callback. All patched
cases return and release their block once; fatal cases request output restart.
The Nix check phase runs the patched cases against the source being built.
This proves callback termination and ownership, rather than physical-device
reopening or glitch-free audio.

Build and export its closure plus a guest index entry:

```sh
nix-shell --run 'python3 scripts/export_guest_package.py'
python3 -m http.server 18473 --bind 127.0.0.1 --directory build/package-cache
```

In Brook's terminal, append the generated index (once) and install normally:

```sh
run /nix/profile/bin/bash -c "curl -fsS 10.0.2.2:18473/vlc-brook-native.idx >> /nix/index/packages.idx"
run /nix/profile/bin/env NIX_CACHE_URL=http://10.0.2.2:18473 /nix/bin/nix-install vlc-brook-native
run --uid 1000 /nix/profile/bin/brook-vlc /data/videos/big-buck-bunny-720p.mov
```

Wait for each command to finish before the next. The cache server is needed
for installation only. The installed package launches from the guest's own
store afterwards. `BROOK_AUDIODEV=none` on the host launcher keeps speakers
silent while retaining the guest HDA device and bridge WAV capture.

An eight-vCPU, 4 GiB run on 17 October installed this package through Gallery,
played 720p video, captured nonzero OSS audio, paused and resumed. This improves
on the earlier one-vCPU baseline; it does not resolve intermittent SMP startup
failures across repeated runs. Close still reproduced the known worker fault
at RIP `0x1e45703f`, now without a concurrent install, leaving peer tasks alive.
Maintenance SIGKILL reclaimed the group before checked shutdown and clean
filesystem checks. Keep installs and playback separate; see REC-03/REC-04 in
`docs/backlog.md` and the [fresh Gallery acceptance](../../docs/app-gallery.md).

The worker-fault containment fix was subsequently verified in session
`72f2260bb6cf4742bfff86664d5fbe5b`: native playback still faults on close in
`wl_proxy_marshal_flags`, but the kernel now reclaims the entire application
group automatically. No windows or VLC tasks remained, the audio stream became
inactive, and checked shutdown plus all three offline filesystem checks passed.
This fixes leaked peers after a fatal worker fault; it does not fix VLC's
underlying shutdown crash.

The first package candidate with only the close-ordering fix got past the
marshal fault but aborted with `free(): double free detected in tcache 2`
after playing the 720p fixture (`97f83c58727c4a2f9b6eb056fc3280ef`). This exposed
the independent release-build buffer ownership bug above. Two combined-build runs
then failed during startup with an invalid surface argument to `get_viewport`,
followed by a marshal fault (`6a022ab5ba4f43b59f56e76d8d6f0c9d` and
`f92beebdde45481ca1597c4af4b91c78`). The startup handoff patch refreshes the
borrowed handle after Qt's stack changes. Pointer-only debug logging compares
saved values without dereferencing the old surface; it cannot exclude address
reuse or prove which specific transition replaced a surface.

Final candidate `3x6wrd699l6kmnr9gr5jkq8xlvsh4yi4-vlc-brook-3.0.23-2` passed
four launches across two eight-vCPU/4-GiB boots. Session
`e3695842f6024fd181e857e04309b60d` covered debug-logged playback/native close,
normal playback/paused native close, and fullscreen-request/return followed by
Ctrl+Q. The two normal shell-supervised runs returned exit status zero. PCM
capture contained 88,477 stereo frames at 44,100 Hz (2.006 s, RMS 0.01076);
after Quit the stream was inactive with zero captured frames. This is audio
activity/cleanup evidence, not a stutter or A/V-sync measurement.

After preserving the test user's VLC settings and rebooting, session
`0f33c099192c4d4084a878ad8269e318` created fresh settings, rendered the movie
and closed natively with exit status zero. Every close returned to ten service
tasks with no windows, no fatal fault, double-free abort or group-exit timeout.
Both checked shutdowns and all three filesystem checks passed. Evidence is
under ignored `build/workloads/gallery-fresh-20261017/`, including
`vlc-case-window-*.log`, PCM metadata and completed-frame PNGs.

Fullscreen requests changed client controls but retained the native window
bounds, so full-screen geometry support is not accepted. Startup-time close,
long sessions, playlist switching and additional later hide/reparent transitions
still need coverage. The test index used distinct package names to avoid the
installer's existing-registration repair behavior, which retains its store hash;
a general package upgrade workflow remains separate work.

## OSS write recovery checkpoint — 18 October 2026

A native-SDL regression with the preceding package initially played 720p video
and passed pause/resume, then reported repeated DSP `EPIPE` with silent output
after a prolonged minimize/restore. Its `Play` callback retried the same buffer
forever. That run was retained as a negative and required SIGKILL; it is not a
normal-quit pass.

Package `45m0jgbaciv7zmp1avpkvsf108dispdh-vlc-brook-3.0.23-2` adds the bounded
write/restart path above. It built with all ten callback cases passing and was
installed through guest `nix-install` as UID 1000. The deployed OSS module matches
the host build: SHA-256
`6d45c887d741a86f39affbd9394e0ba7c5eb4b39ae45504c0332baa7b8b1c0aa`.

The normal launcher played the movie, paused/resumed and restored nonzero audio
after a measured 55-second minimize. Three temporary-backpressure write errors
were logged, with replacement audio streams and subsequent nonzero output,
rather than the preceding repeated-error loop. Normal close drained the complete
launch lineage and left inactive audio with zero captured frames. A separate
active-playback SIGKILL case also drained its full lineage and output. A full
cold boot retained the new module and passed ordinary playback/audio and normal
close again. Checked shutdown, original launcher exit zero, heap checks and all
three read-only filesystem checks passed after each boot; nine saved fixtures
retained exact hashes and Brook ownership.

**Video restoration remains broken:** the video area was black after the long
minimize even though audio recovered. The reference movie at that timestamp is
not black. This checkpoint accepts bounded callback recovery and exit cleanup,
not video restoration, uninterrupted audio or controlled guest `EPIPE` injection.
Evidence: `build/workloads/media-regression-bed237cb-20261018/`.
