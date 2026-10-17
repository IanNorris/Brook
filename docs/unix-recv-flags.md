# Unix receive flags and Firefox audio isolation

Linux accepts `MSG_NOSIGNAL` on Unix `recvmsg`; receives do not generate
SIGPIPE. Brook's Unix receive mask rejected this flag with `EOPNOTSUPP`.
[Mozilla's audio IPC transport](https://github.com/mozilla/gecko-dev/blob/master/third_party/rust/audioipc2/src/sys/unix/mod.rs)
passes it together with `MSG_CMSG_CLOEXEC`. The kernel now accepts that
combination without changing descriptor delivery, nonblocking reads or EOF.
Other unsupported receive flags remain rejected.

## Independent reproducer

`scripts/workloads/unix_recv_flags.py` runs on Linux and Brook's installed
Python. Its four controls cover an ordinary receive, `MSG_NOSIGNAL`, the audio
IPC combination, and that combination with `MSG_DONTWAIT`. Each transfers a
memfd with real contents through SCM_RIGHTS, checks epoll readiness, verifies
close-on-exec behavior, checks empty nonblocking receive and observes EOF.

The Linux control passes 4/4. The unmodified Brook candidate passes only the
ordinary receive; the other three fail at `recvmsg` with error 95. The revised
kernel passes all four as UID 1000. The release build, supported compiled-entry
audit and all 31 hosted suites pass. This is concrete receive compatibility
evidence, not proof that Firefox audio works end to end.

The reproducer uses `_socket.socketpair` C objects directly. Python's higher
level wrapper rewraps each descriptor and first encounters a separate Brook
Unix address-query error (`EBADF`). This test isolates receive flags and does
not establish that the high-level wrapper works.

## Browser runs and local evidence

The baseline normal Firefox wrapper renders the local H.264/AAC fixture and,
after a native Play click, reports advancing media time and `paused=false`.
Five seconds of silent-backend QEMU WAV capture contains zero PCM frames.
The log reports cubeb initialization failure. Native close returns exit zero
and no remaining descendant tasks, with the desktop back at ten tasks. Checked
shutdown and all three offline filesystem checks pass.

Evidence and frozen artifacts are retained under ignored paths:

- Baseline: `build/workloads/firefox-audio-register-fix-20261017/`.
- Candidate: `build/workloads/firefox-audio-recvmsg-fix-20261017/`.
- Build: `build/recvmsg-nosignal-build-20261017.log`.
- Hosted tests: `build/recvmsg-nosignal-tests-20261017.log`.

Both guests use eight CPUs, 4 GiB RAM, software composition at 1080p and the
silent host audio backend. Normal launcher preferences and audio backend
selection are unchanged in the first candidate run.

## Candidate browser result

The normal wrapper now reaches the parent's AudioIPC Server RPC thread and
attempts PulseAudio initialization. It reports a PulseAudio context error,
missing local daemon executable and a pipe-wakeup error. Two five-second PCM
captures contain zero frames and the fixture remains at media time zero after
Play. Native close releases all descendants with exit zero. These observations
do not identify the cause of the PulseAudio pipe-wakeup error.

A separate newly created Brook-owned profile selects `media.cubeb.backend=alsa`
and otherwise retains the normal first-run preferences. Audio IPC and content
sandbox preferences are not disabled. The log reports `media.cubeb.sandbox:
true`, stream initialization and STARTED, followed by callbacks on the AudioIPC
Client Callback thread. A five-second capture contains 220,521 stereo PCM
frames at 44.1 kHz with nonzero amplitude. This is progress beyond the prior
IPC initialization failure, but not usable-audio acceptance: 681 of 800
first-channel peak bins are exactly silent, versus 89 of 800 in the decoded
source. Firefox repeatedly reports 3,386 or 4,410 lost frames. The intervals
span a looping source and different offsets; these counts are a coarse silence
comparison, not sample alignment or an exact loss measurement.

The existing ALSA route uses a file PCM over a null slave. Its device-clock and
delay reporting remain a separate investigation; selecting ALSA alone does not
fix playback. No launcher preferences changed. The first Play click in this
second window missed its shifted position; only `play-corrected-input.json`
and subsequent evidence demonstrate playback.

Native close eventually returns exit zero with no remaining descendant tasks
and ten desktop tasks. A two-second capture after the close request contains
zero PCM frames, showing inactive output. Checked shutdown and all three
offline filesystem checks pass. **The candidate fails serial/device
acceptance**: an earlier virtio-blk response timeout and recovered queue reset
are preserved in `shutdown/serial-check.json` and the final serial log. Do not
promote this run as a clean workload image or infer that the receive-flag change
caused or resolved the device fault.
