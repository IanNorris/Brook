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

The validated playback configuration currently uses `BROOK_SMP=1`. Four-vCPU
startup can still fail descriptor duplication. Closing VLC during a concurrent
package installation also produced a worker fault; keep installs and playback
separate for now. See REC-03/REC-04 in `docs/backlog.md` for the evidence.
