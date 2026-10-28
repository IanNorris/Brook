# Chromium clocked audio wrapper

The optional `downstreamStaging = true` parameter selects the downstream-aware
OSS bridge. The minimal release profile uses it together with the launcher's
`opt/audio-staging-frames=4096` boot configuration. Both are required: the older
plugin charges physical downstream backlog against its logical ALSA ring and
cannot use the extra allowance to keep producing. The application still uses
its normal negotiated ring and period sizes. Other package callers retain the
existing default unless they opt in.

Build with `nix-build tools/chromium-brook-pkg --no-out-link`.

This wraps the pinned stock Chromium without rebuilding its engine. Like the
Firefox package, it replaces an unset ALSA configuration or Brook's inherited
`/nix/etc/asound.conf` with the packaged clocked OSS route. An explicitly different
configuration is preserved. Absolute store references retain the configuration
and plugin in the package closure. Existing browser profiles are untouched.

The older file-over-null route turns an output-file `EPIPE` into ALSA `EIO`.
Its prepare/reset operations do not reset the output DSP descriptor. A real
device discontinuity can therefore leave that open unusable until reopened.
The clocked plugin exposes the device error and implements DSP reset during
prepare. This corrects the recovery path; it does not identify or remove every
cause of the initial discontinuity.

The experiment image and `tools/workload-pkg/chromium.nix` select this wrapper.
The generic public package index continues to list stock Chromium. The existing
Brook launcher still supplies its experimental Wayland/software-rendering flags;
Chromium's sandbox remains disabled. See [audio evidence](../../docs/audio-clock.md)
for guest checks and remaining limits.
