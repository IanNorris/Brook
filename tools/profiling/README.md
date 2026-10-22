# Optional client GPU timing probes

These probes run inside a Linux-compatible application. They do not measure
physical scanout or isolate CPU time from scheduling. Build with a compiler and
libc compatible with the guest, and use fresh absolute output paths:

```sh
cc -O2 -shared -fPIC sdl_swap_trace.c -ldl -o swap.so
cc -O2 -shared -fPIC drm_ioctl_trace.c -ldl -o drm.so
```

Inject into the application being measured, for example with these environment
variables supplied through `vm_guest_run`:

```text
LD_PRELOAD=/tmp/swap.so:/tmp/drm.so
BROOK_SWAP_TRACE=/tmp/swaps.csv
BROOK_DRM_TRACE=/tmp/drm.csv
BROOK_DRM_TRACE_SKIP_SWAPS=50
BROOK_SWAP_SETTINGS=/tmp/swap-settings.json
```

The swap probe uses the **SDL2 ABI**, including `SDL_GL_GetSwapInterval(void)`.
It also works with SDL2-compat's SDL2 interface. Do not inject it into a native
SDL3 application. The optional settings file reports the actual getter's
minimum/maximum over recorded swaps; a saved game cvar alone does not prove
that API state. Missing getters or zero queries do not establish VSync state.

The DRM probe groups `drmIoctl` calls by thread, request number and phase:

- `0`: unscoped, including startup, skipped swaps and non-swap threads.
- `1`: between swaps on the swap thread, including shutdown after its last swap.
- `2`: inside swap on that thread.

Phases begin after the configured number of skipped swaps. They do not align
automatically with game demo markers. Without the SDL hooks all calls remain
unscoped. Aggregates have a fixed capacity; elapsed times include waits and
preemption. Logarithmic histogram percentiles are bounds, not exact quantiles.
Concurrent thread totals must not be added as frame wall time. In particular,
bound the shutdown tail before subtracting outside-swap DRM totals from the
between-swap intervals.

Both probes avoid per-frame allocation and output. Normal process teardown
writes files with exclusive creation; existing files are never overwritten.
An abnormal exit can leave no complete trace. DRM trace admission closes at
teardown, and any admitted call still in flight invalidates the capture.
Overflow, clock errors, saturation and inconsistent histograms also invalidate
it. Keep failures as evidence, rather than interpreting them as zero-cost work.

After a clean private VM shutdown and offline filesystem checks, extract the
files and verify their sizes/hashes against guest metadata. Avoid bulk stdout
transfer through the bridge. Analyze with:

```sh
python3 scripts/workloads/analyze_swap_trace.py /path/to/swaps.csv
python3 scripts/workloads/analyze_drm_trace.py /path/to/drm.csv
python3 scripts/test_drm_ioctl_trace.py
```

Only the opt-in preload environment activates these probes. They do not change
normal application launchers or VM settings. Keep instrumented results separate
from uninstrumented throughput controls.

## Vulkan timestamp capability probe

Build `nix-build tools/vulkan-probe-pkg --no-out-link` and run its
`bin/vulkan-probe --expect-driver venus` inside a private Venus VM with
`BROOK_VULKAN_TIMESTAMPS=1`. This opt-in mode brackets upload, compute and
readback in the existing three-round correctness workload. It reads query
availability after the existing completion wait, without `WAIT_BIT` or a new
queue-idle operation. Default mode has no query pool or timestamp writes.
`BROOK_VULKAN_SYNC_FD=1` additionally exercises the existing external sync-file
completion path with data checked before the ordinary fence wait.

The final JSON retains raw GPU ticks, queue valid-bit width, timestamp period,
CPU timestamps around submit and completion return, and two calibration samples
when `VK_EXT_calibrated_timestamps` advertises device and `CLOCK_MONOTONIC`
domains. Missing calibration is explicit; unsupported timestamp queues fail the
requested probe. Inspect the complete exit/result before analysis:

```sh
python3 scripts/workloads/analyze_vulkan_timestamps.py /absolute/path/result.json
python3 scripts/test_vulkan_timestamps.py
```

These are instrumented GPU intervals, including stalls and dependencies, not
exclusive hardware-engine costs. Boundary timestamps can affect pipeline
overlap. CPU calls include descheduling. Calibrated estimates retain each
sample's advertised maximum deviation; the guest monotonic domain does not
establish host clock alignment. This short, offscreen capability test does not
measure cube pacing, the compositor, WSI or physical presentation. Use bounded
query rings and deferred reads when extending it to real frame workloads.

## Timestamped Vulkan cube

`nix-build tools/vulkan-cube-timing-pkg --no-out-link` builds a separate
instrumented version of the pinned Khronos `vkcube` (the C executable).
Set `BROOK_CUBE_TRACE=/tmp/unique-cube.csv` in its guest environment and use a
bounded run such as `--wsi wayland --c 600`. The trace is written exclusively at
normal teardown. Without that variable it allocates no timestamp pool.

The query pool has two pairs, matching the cube's existing two frame slots.
Results are read after each slot's existing fence wait and before reuse; final
slots are collected after its existing device-idle wait. An 8,192-row fixed
array avoids per-frame allocation and output. The CSV records CPU wait, acquire,
prepare, submit and present markers alongside GPU start/end ticks. Metadata
retains errors and overflow, and each row retains submission/availability state.
After clean shutdown, offline filesystem checks and extraction/hash validation:

```sh
python3 scripts/workloads/analyze_cube_timing.py /absolute/path/cube.csv
```

The cube defaults to FIFO presentation, so normal acquire blocking is expected.
Investigate outliers rather than treating every acquire wait as wasted time.
Its GPU intervals can include semaphore dependencies; CPU/GPU durations overlap
and must not be summed. Cube traces currently have no clock calibration or
scanout timestamp. Keep an uninstrumented control and assess observer overhead
before using these results as a throughput benchmark.
