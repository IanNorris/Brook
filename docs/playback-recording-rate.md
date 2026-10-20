# Historical playback recording: measured audio tempo

The user's October 20 recording supports accelerated, nonuniform audio playback
at roughly 1.3× relative to a local reference performance of *Thriller*. This
quantifies the historical symptom; it does not reproduce it on the current
kernel or identify the responsible clock/device path.

The supplied file is 17.5667 seconds, 1280×720 at 30 fps, with 44.1-kHz stereo AAC.
The browser's playhead is hidden and the desktop clock shows minutes only, so
image inspection cannot establish an exact media or guest-clock rate. The local
*Number Ones* audio track contains the same performance heard in the recording,
allowing signal matching without downloading a reference or publishing media.
Different edits/mastering remain a limitation of that comparison.

`scripts/workloads/analyze_playback_recording.py` decodes both local sources
into memory, computes 32 normalized log spectral bands and matches the recording
to the reference over pitch-preserving time-scale factors 0.85–1.65. It writes
only JSON; neither input nor decoded media belongs in git. The match searches
both offset and rate rather than assuming the song's nominal BPM.

| Query | Best rate | Reference start | Similarity |
| --- | ---: | ---: | ---: |
| Whole recording | 1.310× | 189.289 s | 0.619 |
| First ~6 s | 1.310× | 189.289 s | 0.831 |
| ~5–11 s | 1.310× | 195.837 s | 0.829 |
| ~10–16 s | 1.240× | 202.432 s | 0.791 |

The best whole-recording match constrained to normal speed has similarity 0.125,
substantially below the accelerated match. Known-rate controls generated from
the local reference through FFmpeg `atempo` recover 1.000× and 1.300×, with
similarities 0.988 and 0.950 respectively. The 0.005 search step is not an error
bar; the later slower section and possible stalls make one exact constant rate
inappropriate. The analysis does not distinguish browser time stretching,
skipped audio chunks, guest clock errors or host device pacing.

Ignored evidence, input hashes, the complete parameter set, per-window alternatives
and control results are under
`build/workloads/timing-scheduler-20261020/speedup-recording/validated-rate.json`.
Source file hashes identify the private inputs without including their contents.
The first signal-analysis attempt failed on a missing host NumPy zlib dependency;
using the existing 64-bit Nix zlib library resolved it. No package installation or
user-file change was needed.

For another local recording, use a Python environment containing NumPy and an
existing FFmpeg executable:

```sh
python3 scripts/workloads/analyze_playback_recording.py \
  --recording /absolute/path/to/recording.mp4 \
  --reference /absolute/path/to/matching-local-audio.mp3 \
  --output build/playback-rate/result.json --controls
```

Reserve host resources while a VM benchmark is running. Our analysis uses host
CPUs 14–15 with single-threaded numerical operations while the VM is restricted
to CPUs 0–13.

The controlled scheduler fixtures so far use private PipeWire output and HDA
`use-timer=off`. The normal launcher defaults to `use-timer=on`. Testing that
separate device-pacing cohort is still required before connecting the historical
recording to any current repair. The [per-vCPU clock fix](pvclock-percpu.md)
addresses a different, directly measured pause/resume defect.
