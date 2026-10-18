# Firefox clocked-audio integration

Build with `nix-build tools/firefox-brook-pkg --no-out-link`.

This rebuilds the pinned Firefox wrapper, not the browser engine. It adds
`defaultPref("media.cubeb.backend", "alsa")` through Firefox's existing
autoconfiguration mechanism. Existing user preferences take priority; no profile
files or sandbox preferences are changed.

The wrapper replaces Brook's inherited legacy
`ALSA_CONFIG_PATH=/nix/etc/asound.conf` with a packaged clocked OSS configuration.
An explicitly different caller configuration is preserved. The configuration
names the packaged OSS module by absolute store path, so no diagnostic
`ALSA_PLUGIN_DIR` is needed. The Nix closure retains both configuration and module.

Host build, wrapper syntax checks and ALSA configuration parsing pass. The
package was exported and installed through guest `nix-install` on isolated
disks. The existing `brook-firefox` launcher then played a controlled H.264/AAC
clip without test ALSA environment variables or a backend preference in its
fresh profile. The previous cloned profile was preserved separately.

Acceptance remains incomplete. Before the eventfd fix, one ten-second capture
had a 370 ms silent interval and a replay had none. With kernel
`244ee8e0...52a0`, two consecutive ten-second captures have respectively no
silent 10 ms bins and a longest silent run of 480 ms. Normal close exits zero
with no surviving group tasks; a subsequent two-second capture reports no
active output. These results do not establish continuous, glitch-free playback.
Pause/resume and preservation of an existing explicit backend preference still
need guest checks. At that checkpoint this candidate was not selected by the image's package
profile or public index; global ALSA defaults remain unchanged.

Evidence is retained in ignored
`build/workloads/epoll-cache-diagnostics-20261018/` and
`build/workloads/eventfd-edges-candidate-20261018/`. See
[workload results](../../docs/workloads.md) for the accompanying eventfd
regressions and checked-shutdown/filesystem gates.

Export for that test with:

```sh
python3 scripts/export_guest_package.py --expression tools/firefox-brook-pkg \
  --name firefox-brook --version 150.0.1 --output build/package-cache
```

See [audio evidence](../../docs/audio-clock.md) for the preceding isolated
playback checkpoint and its limitations, including short-video loop gaps.

## YouTube audio checkpoint — 18 October 2026

The corrected OSS plugin permits arbitrary logical ALSA periods/buffers while
reserving sufficient whole physical fragments. This fixes a real rate-conversion
negotiation failure: the previous wrapper accepted 44.1-kHz integer/float
streams but rejected both 48-kHz variants with EINVAL. Firefox logged
OpenCubeb stream-initialization failure and produced no PCM on YouTube.
The plugin's Nix checks reproduce the failure through real ALSA and a synthetic
OSS endpoint; the original fails 20/30 format/rate/latency cases and the fix
passes all 30. The same four-case setup matrix passes in the corrected guest.

The exported candidate was installed through guest nix-install as Brook on
fresh isolated disks. The installed-app UI selected its normal Firefox wrapper;
no backend/sandbox user preference or diagnostic environment override was added.
YouTube Crab Rave then produced PCM and a tab speaker indicator. Three consecutive
ten-second captures contained 441126, 443780 and 441212 frames. The first had a
longest silent run of 290 ms; the following two had no silent 10-ms blocks.
Pause produced zero frames with an inactive stream, and resume produced 441484
frames over 10.007 host seconds with no silent 10-ms blocks. These are bounded
checks, not continuous glitch-free playback acceptance. That run preceded
base-image promotion; the public index still does not select this package.

Evidence: build/workloads/firefox-rate-youtube-20261018, session
3076e2a5810a4a4e8b47e74a80ee0874. Tested wrapper:
/nix/store/mdir5smk71dplawml4nj7s7skgh72qz5-firefox-150.0.1. Its plugin was
y3g2ig30a9c72q8r0vdk44fj14wc8bfl-alsa-plugins-oss-brook-1.2.12. A subsequent
build with explicitly enabled test assertions produced
shpjif3sglmshri03qqgvgd7k2lvjxjl-alsa-plugins-oss-brook-1.2.12; its runtime
PCM shared library is byte-identical (SHA-256
3478024b73ff6d045c70e88b9404be3e56f33932416eb8ff168f07c8cb4d4218).

Normal close drained all browser lineage tasks, post-exit audio was inactive,
heap integrity passed, sync was acknowledged, the launcher exited zero and
all three offline filesystem checks passed. General shared-profile ownership,
base-image promotion and broader application/audio regressions remain open.

The default experiment profile now includes this Firefox package and Chromium.
The fresh-image check in build/workloads/fresh-browsers-20261018 passes normal
menu launches and controlled 48-kHz Opus audio without manual package/profile
setup. Chromium and a synthetic home file retain state across VM restart;
Firefox local-storage writes fail with a quota error, so Firefox persistence
remains unaccepted. See docs/experiments.md for this checkpoint and its limits.
