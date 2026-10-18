# Brook clocked ALSA playback slave

This is intended as a compatibility path for applications using ALSA. The
synthetic playback/capture probes are diagnostic tools and regressions. It is
selected by Firefox in the default experiment image. The global ALSA default
for other applications remains separate.

The pinned ALSA plugin source is patched to propagate I/O/control errors,
advertise native 44.1-kHz stereo S16_LE playback and negotiate bounded software
and physical buffers. An outer ALSA `plug` handles client conversion. Native clock availability
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
It also opens the production plugin against a synthetic `/dev/null` endpoint
through real ALSA `plug` negotiation: 8, 22.05, 44.1, 48 and 96 kHz, integer and
float samples, and 50/100/200-ms requested latencies (30 cases).

Logical periods and buffers can have arbitrary frame geometry. Requiring both
power-of-two periods and an integer count of periods rejected rate conversion,
including Firefox's 48-kHz YouTube audio. Physical OSS fragments remain 256
bytes, with enough whole fragments to cover the software ring. The unused
physical capacity is less than 64 stereo frames; transfer/pointer accounting
continues to use accepted application frames and actual per-stream delay.
Hardware capacity never substitutes for the logical ring size. The kernel's
16,384-frame capacity limit is preserved. The old plugin failed 20/30 host
negotiations; the corrected plugin passes all 30.

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

The default experiment profile now includes this Firefox package and Chromium.
The fresh-image check in build/workloads/fresh-browsers-20261018 passes normal
menu launches and controlled 48-kHz Opus audio without manual package/profile
setup. Chromium and a synthetic home file retain state across VM restart;
Firefox local-storage writes fail with a quota error, so Firefox persistence
remains unaccepted. See docs/experiments.md for this checkpoint and its limits.
