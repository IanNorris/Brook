# Firefox clocked-audio integration candidate

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
need guest checks. This candidate is not selected by the image's package
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
