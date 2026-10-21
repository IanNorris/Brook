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

The sustained-playback fixture additionally runs the unchanged production
callbacks through real libasound `plug` conversion, `writei`, availability,
delay, automatic start and drain. Sixteen cases cover 22.05/44.1/48/96-kHz
integer/float clients, whole-buffer and oversized writes, partial admissions,
and delayed completion. It compares the original physical 4416/64-frame
geometry with the experimental 6016/128 geometry while retaining ALSA's
separate logical ring. Accepted native frames must drain exactly and the
logical queue must never overflow. Both geometries pass this fixture; a larger
physical ring alone has not reproduced Firefox's native stall. Descriptor
readiness, scheduling, audible continuity and Firefox are separate gates:
this fixture opens only `/dev/null` and advances completion explicitly.

Logical periods and buffers can have arbitrary frame geometry. Requiring both
power-of-two periods and an integer count of periods rejected rate conversion,
including Firefox's 48-kHz YouTube audio. By default physical OSS fragments are
256 bytes, with enough whole fragments to cover the software ring. The unused
physical capacity is then less than 64 stereo frames. The opt-in kernel refill
experiment can enlarge the physical geometry; transfer/pointer accounting
continues to use accepted application frames and actual per-stream delay.
Hardware capacity never substitutes for the logical ring size. The kernel's
16,384-frame capacity limit is preserved. The old plugin failed 20/30 host
negotiations; the corrected plugin passes all 30.

An optional logical-buffer experiment can be built separately:

```sh
nix-build --expr 'import ./tools/alsa-oss-pkg { minimumBufferFrames = 6174; }' --no-out-link
```

The parameter is a native 44.1-kHz stereo frame constraint (64 through 16384),
not a physical-device capacity override. The default remains 64. The 6174-frame
candidate requests at least 140 ms of actual logical storage. It passes all 30
negotiation and 16 sustained-playback cases; native rate conversion can round
its logical size up by a frame. The existing default fixture retains its exact
4410–4412 logical and 4416/64 or 6016/128 physical assertions. Candidate tests
separately require the negotiated logical minimum, ordinary rounded physical
capacity, exact accepted-frame drain and no logical overfill.

This is not a general guarantee for every parameter value. Isolated host probes
found six negotiation failures at 6016 and 8192 frames, and two at 6000 and
6144 frames, predominantly float clients requiring rate conversion. Those
failures are retained under
`build/workloads/timing-scheduler-20261021/logical-buffer-probe/`.
A physical reserve alone does not enlarge ALSA's ring; a larger logical ring
also increases possible audio backlog. Native mixed playback and source-credit
latency must be evaluated before promotion. Readiness and scheduler accounting
remain separate checks.

An **opt-in, incomplete** readiness experiment is available with:

```sh
nix-build --expr 'import ./tools/alsa-oss-pkg { lowWaterReadiness = true; }' --no-out-link
```

It uses the OSS4 `SNDCTL_DSP_LOW_WATER` byte threshold so raw descriptor polling
can block instead of waking every 64 native frames. Brook's native-rate setter
changes readiness without changing queued data or the completion clock, and
reset preserves the threshold. The
[FreeBSD OSS implementation](https://cgit.freebsd.org/src/tree/sys/dev/sound/pcm/dsp.c)
documents the same select/poll byte contract. Linux ioctl encoding is used.

The fixture uses the real ALSA plugin, libasound and `poll`, with an eventfd
modeling device readiness. Aligned-period tests pass at 8, 22.05, 44.1, 48 and
96 kHz, including threshold boundaries, live `avail_min` changes, reprepare and
unsupported-device errors. These are not general converted-client acceptance.
The optional `test-oss-readiness PLUGIN 8000 --partial` gate fails: accepting
799/800 client frames leaves a partial period in the rate layer. After one
native frame advances, raw poll is ready with only 1/200 client frames free.
The static slave threshold cannot observe that pending client data. The normal
package therefore leaves this experiment disabled. A correct general solution
must incorporate the outer rate layer's pending state and verify irregular
refills across ring wrap.

The exploratory native Firefox pair is retained under
`build/reviews/alsa-low-water-pair-2-20261021`. Its selected audio-thread ioctl
count fell from 2,634 to 857 for similarly sized accepted audio payloads, but
this same-boot control/candidate order is not balanced. Page progress telemetry
was missing during the capture windows, and PCM samples are not aligned to a
decoded reference. This is not acceptance of playback pace or smoothness.
The first pair failed at a guest-script substitution before playback; its clean
shutdown and failure evidence are retained separately.

An optional callback recorder can be built separately:

```sh
nix-build --expr 'import ./tools/alsa-oss-pkg { callbackTrace = true; }' --no-out-link
```

It wraps the production playback callbacks and preserves their results and
origin/pointer guards. It adds no ALSA or device queries. Callback timings,
thread/PCM identity, requested/returned frames and logical pointer state go to
a bounded ring, with a separate recent-write history. A positive `first_write`
snapshot establishes playback binding; a device-probe close with zero buffer
size does not. Up to 32 periodic snapshots, eight non-`EAGAIN` error dumps and eight
stop/close/drain dumps are appended to `/tmp/brook-alsa-trace-PID.log`.
This preserves evidence when a browser child exits without closing its plugin.
Snapshots retain a tail rather than every call. Limits are per process, and
periodic snapshots require another callback; missing records cannot prove a
backend exited or remained blocked.

The ring uses fixed storage, but timestamps, a private mutex and file dumps
perturb execution. Dumps can run while ALSA holds its PCM lock.
It is a diagnostic build, with separate native acceptance, and remains disabled
in the normal package. Collect all process files from the private VM with byte
counts/hashes and verify actual playback activation before interpreting an
absence of errors. An entry into `close` is not proof that close returned.

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
