# Brook clocked ALSA playback slave

This is intended as a compatibility path for applications using ALSA. The
synthetic playback/capture probes are diagnostic tools and regressions. It is
not yet selected by the default image or Firefox launcher.

The pinned ALSA plugin source is patched to propagate I/O/control errors,
advertise native 44.1-kHz stereo S16_LE playback and negotiate a bounded exact
buffer. An outer ALSA `plug` handles client conversion. Native clock availability
is required during prepare; the kernel's unclocked legacy delay estimate is not
accepted as a playback clock.

The pointer uses ALSA's boundary mode. Frames actually accepted through transfer,
minus this stream's incomplete frames, determine completion. An expected
application-pointer origin prevents software-only forward/rewind operations from
creating completion. A drained zero-origin reset can rebase; reset with pending
data requires prepare recovery. Configuration operations on one PCM handle must
be serialized by its caller. This is not acceptance of the entire ALSA API.

Build and exercise the patched production callbacks without an audio device:

```sh
nix-build tools/alsa-oss-pkg --no-out-link
```

The build's check phase uses real ALSA null-PCM parameters plus injected device
operations. It covers errors, short/zero transfers, native-clock rejection,
whole-ring completion, boundary wrapping and invalid origin/queue snapshots.

Export for an isolated guest installation:

```sh
python3 scripts/export_guest_package.py --expression tools/alsa-oss-pkg \
  --name brook-alsa-oss --version 1.2.12 --output build/package-cache
```

Use a private loopback cache server and the guest's normal `nix-install` workflow.
Do not change a user's package index/profile for a diagnostic run; use cloned
disks. Select the exported output's `lib/alsa-lib` with `ALSA_PLUGIN_DIR` and a
separate `ALSA_CONFIG_PATH` containing:

```conf
pcm.brook_oss {
    type oss
    device "/dev/dsp"
}
pcm.brook_plug {
    type plug
    slave {
        pcm "brook_oss"
        format S16_LE
        rate 44100
        channels 2
    }
}
```

Run `scripts/workloads/alsa_oss.py` inside the VM with the libasound path, PCM
name and `clock`, `boundaries` or `exact`. Exact mode emits the same 9,660 known
nonzero frames as `dsp_short_tail.py`; capture the entire source interval and
compare the samples, not just RMS or a success message. Keep host audio silent.

See [audio evidence](../../docs/audio-clock.md) for accepted identities, limits
and remaining Firefox integration. Client-rate/format conversion, browser IPC,
normal-launcher behavior and wider application regressions remain separate
acceptance work. The default file-over-null route remains unchanged.
